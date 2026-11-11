// WebDAV and Google Drive providers: see psp_cloud_providers.h.
#include "psp_cloud_providers.h"

#include "psp_http.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace psp_cloud {
namespace {

using psp_http::Bytes;
using psp_http::Headers;
using psp_http::Request;
using psp_http::Response;

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(uint8_t(c))); return s; }

std::string http_error(const Response& r, const std::string& what) {
    if (r.status == 0) return what + ": " + (r.error.empty() ? "no answer" : r.error);
    std::string t = r.text();
    if (t.size() > 160) t.resize(160);
    for (char& c : t) if (c == '\n' || c == '\r') c = ' ';
    return what + ": HTTP " + std::to_string(r.status) + (t.empty() ? "" : " " + t);
}

// ---------------------------------------------------------------- a small JSON reader (objects, arrays, strings, numbers)
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json* get(const std::string& k) const {
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& k) const {
        const Json* j = get(k);
        if (!j) return "";
        if (j->type == Str) return j->s;
        if (j->type == Num) return std::to_string(static_cast<long long>(j->n));
        return "";
    }
    long long num(const std::string& k) const {
        const Json* j = get(k);
        if (!j) return 0;
        if (j->type == Num) return static_cast<long long>(j->n);
        if (j->type == Str) return std::atoll(j->s.c_str());      // Drive sends sizes as strings
        return 0;
    }
};

struct JsonParser {
    const std::string& t;
    size_t p = 0;
    int depth = 0;
    bool ok = true;
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) p++; }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t* v) {
        if (p + 4 > t.size()) return false;
        *v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = t[p++];
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string* out) {
        if (p >= t.size() || t[p] != '"') return false;
        p++;
        while (p < t.size()) {
            const char c = t[p++];
            if (c == '"') return true;
            if (c != '\\') { *out += c; continue; }
            if (p >= t.size()) return false;
            const char e = t[p++];
            switch (e) {
                case 'n': *out += '\n'; break;
                case 't': *out += '\t'; break;
                case 'r': *out += '\r'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'u': {
                    uint32_t u;
                    if (!hex4(&u)) return false;
                    if (u >= 0xD800 && u < 0xDC00 && p + 1 < t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(&lo)) return false;
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(*out, u);
                    break;
                }
                default: *out += e;
            }
        }
        return false;
    }
    Json value() {
        Json j;
        ws();
        if (p >= t.size() || ++depth > 32) { ok = false; return j; }
        const char c = t[p];
        if (c == '{') {
            j.type = Json::Obj; p++; ws();
            if (p < t.size() && t[p] == '}') { p++; depth--; return j; }
            for (;;) {
                ws();
                std::string k;
                if (!string(&k)) { ok = false; break; }
                ws();
                if (p >= t.size() || t[p++] != ':') { ok = false; break; }
                Json v = value();
                if (!ok) break;
                j.o.emplace_back(std::move(k), std::move(v));
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == '}') { p++; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            j.type = Json::Arr; p++; ws();
            if (p < t.size() && t[p] == ']') { p++; depth--; return j; }
            for (;;) {
                Json v = value();
                if (!ok) break;
                j.a.push_back(std::move(v));
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == ']') { p++; break; }
                ok = false; break;
            }
        } else if (c == '"') {
            j.type = Json::Str;
            if (!string(&j.s)) ok = false;
        } else if (t.compare(p, 4, "true") == 0) { j.type = Json::Bool; j.b = true; p += 4; }
        else if (t.compare(p, 5, "false") == 0) { j.type = Json::Bool; p += 5; }
        else if (t.compare(p, 4, "null") == 0) { p += 4; }
        else {
            char* e = nullptr;
            j.n = std::strtod(t.c_str() + p, &e);
            if (e == t.c_str() + p) ok = false;
            else { j.type = Json::Num; p = size_t(e - t.c_str()); }
        }
        depth--;
        return j;
    }
};

bool parse_json(const std::string& text, Json* out) {
    JsonParser jp{text};
    *out = jp.value();
    return jp.ok && out->type != Json::Null;
}

// ---------------------------------------------------------------- WebDAV
std::string percent_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(uint8_t(s[i + 1])) && std::isxdigit(uint8_t(s[i + 2]))) {
            o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string xml_unescape(std::string s) {
    static const std::pair<const char*, const char*> e[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (auto& kv : e) {
        size_t p = 0;
        const size_t n = std::strlen(kv.first);
        while ((p = s.find(kv.first, p)) != std::string::npos) { s.replace(p, n, kv.second); p += std::strlen(kv.second); }
    }
    return s;
}

// The text of the first <...:name>text</...:name> in `block` (any or no namespace prefix, any case), "" when absent.
std::string xml_text(const std::string& block, const std::string& name, bool* found = nullptr) {
    if (found) *found = false;
    size_t pos = 0;
    while ((pos = block.find('<', pos)) != std::string::npos) {
        const size_t gt = block.find('>', pos);
        if (gt == std::string::npos) break;
        std::string tag = block.substr(pos + 1, gt - pos - 1);
        pos = gt + 1;
        if (tag.empty() || tag[0] == '/' || tag[0] == '?' || tag[0] == '!') continue;
        const bool selfclose = tag.back() == '/';
        const size_t sp = tag.find_first_of(" \t\r\n/");
        std::string nm = lower(tag.substr(0, sp));
        const size_t colon = nm.find(':');
        if (colon != std::string::npos) nm = nm.substr(colon + 1);
        if (nm != name) continue;
        if (found) *found = true;
        if (selfclose) return "";
        const size_t close = block.find("</", pos);
        return close == std::string::npos ? "" : xml_unescape(block.substr(pos, close - pos));
    }
    return "";
}

class WebDavProvider : public Provider {
public:
    explicit WebDavProvider(WebDavConfig c) : c_(std::move(c)) {
        while (!c_.url.empty() && c_.url.back() == '/') c_.url.pop_back();
    }
    std::string label() const override { return "WebDAV " + c_.url; }

    std::string list(std::vector<RemoteItem>* out) override {
        out->clear();
        Request r = req("PROPFIND", c_.url + "/");
        r.headers.emplace_back("Depth", "1");
        r.headers.emplace_back("Content-Type", "application/xml");
        const std::string body = "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:getcontentlength/>"
                                 "<d:resourcetype/></d:prop></d:propfind>";
        r.body.assign(body.begin(), body.end());
        const Response res = psp_http::perform(r);
        if (res.status == 404) return "";                      // nothing uploaded yet
        if (res.status != 207 && !res.ok()) return http_error(res, "WebDAV list");
        const std::string xml = res.text();
        size_t pos = 0;
        for (;;) {                                              // one block per <...response>
            size_t a = std::string::npos;
            for (size_t i = pos; i < xml.size(); i++) {
                if (xml[i] != '<') continue;
                const size_t gt = xml.find('>', i);
                if (gt == std::string::npos) break;
                std::string tag = lower(xml.substr(i + 1, gt - i - 1));
                const size_t sp = tag.find_first_of(" \t\r\n/");
                tag = tag.substr(0, sp);
                if (tag == "response" || (tag.size() > 9 && tag.compare(tag.size() - 9, 9, ":response") == 0)) { a = i; break; }
            }
            if (a == std::string::npos) break;
            size_t b = std::string::npos;
            for (size_t i = a + 1; i < xml.size(); i++) {
                if (xml.compare(i, 2, "</") != 0) continue;
                const size_t gt = xml.find('>', i);
                if (gt == std::string::npos) break;
                std::string tag = lower(xml.substr(i + 2, gt - i - 2));
                if (tag == "response" || (tag.size() > 9 && tag.compare(tag.size() - 9, 9, ":response") == 0)) { b = gt + 1; break; }
            }
            if (b == std::string::npos) break;
            const std::string block = xml.substr(a, b - a);
            pos = b;
            std::string href = percent_decode(xml_text(block, "href"));
            bool coll = false;
            xml_text(block, "collection", &coll);
            if (href.empty() || coll || href.back() == '/') continue;
            const size_t slash = href.rfind('/');
            const std::string name = slash == std::string::npos ? href : href.substr(slash + 1);
            if (name.empty()) continue;
            RemoteItem it;
            it.name = name;
            it.size = uint64_t(std::strtoull(xml_text(block, "getcontentlength").c_str(), nullptr, 10));
            out->push_back(it);
        }
        return "";
    }

    std::string get(const std::string& name, Bytes* out) override {
        const Response r = psp_http::perform(req("GET", item(name)));
        if (!r.ok()) return http_error(r, "WebDAV download of " + name);
        *out = r.body;
        return "";
    }

    std::string put(const std::string& name, const Bytes& data) override {
        for (int attempt = 0; attempt < 2; attempt++) {
            Request r = req("PUT", item(name));
            r.body = data;
            r.headers.emplace_back("Content-Type", "application/octet-stream");
            const Response res = psp_http::perform(r);
            if (res.status == 200 || res.status == 201 || res.status == 204) return "";
            if ((res.status == 409 || res.status == 404) && attempt == 0) {     // the folder does not exist yet
                const Response mk = psp_http::perform(req("MKCOL", c_.url + "/"));
                if (mk.status == 201 || mk.status == 405) continue;               // created, or it was there after all
                return http_error(mk, "WebDAV cannot create the folder");
            }
            return http_error(res, "WebDAV upload of " + name);
        }
        return "WebDAV upload failed";
    }

    std::string remove(const std::string& name) override {
        const Response r = psp_http::perform(req("DELETE", item(name)));
        if (r.ok() || r.status == 404) return "";
        return http_error(r, "WebDAV delete of " + name);
    }

private:
    std::string item(const std::string& name) const { return c_.url + "/" + psp_http::url_encode(name); }
    Request req(const std::string& method, const std::string& url) const {
        Request r;
        r.method = method;
        r.url = url;
        r.max_redirects = 3;
        r.timeout_ms = 30000;
        if (!c_.user.empty() || !c_.password.empty())
            r.headers.emplace_back("Authorization", "Basic " + psp_http::base64(c_.user + ":" + c_.password));
        return r;
    }
    WebDavConfig c_;
};

// ---------------------------------------------------------------- Google Drive
class DriveProvider : public Provider {
public:
    explicit DriveProvider(GoogleConfig c) : c_(std::move(c)) {}
    std::string label() const override { return "Google Drive"; }

    std::string list(std::vector<RemoteItem>* out) override {
        out->clear();
        ids_.clear();
        std::string page;
        for (int guard = 0; guard < 50; guard++) {
            std::string url = c_.api_base + "/drive/v3/files?spaces=appDataFolder&pageSize=1000&fields=" +
                              psp_http::url_encode("nextPageToken,files(id,name,size)");
            if (!page.empty()) url += "&pageToken=" + psp_http::url_encode(page);
            Response r;
            if (std::string e = call("GET", url, {}, &r); !e.empty()) return e;
            Json j;
            if (!r.ok() || !parse_json(r.text(), &j)) return http_error(r, "Google Drive list");
            if (const Json* f = j.get("files"))
                for (const Json& file : f->a) {
                    RemoteItem it;
                    it.name = file.str("name");
                    it.size = uint64_t(file.num("size"));
                    const std::string id = file.str("id");
                    if (it.name.empty() || id.empty()) continue;
                    if (ids_.count(it.name)) continue;      // a duplicate name (two devices created it at once): keep the first
                    ids_[it.name] = id;
                    out->push_back(it);
                }
            page = j.str("nextPageToken");
            if (page.empty()) break;
        }
        return "";
    }

    std::string get(const std::string& name, Bytes* out) override {
        std::string id;
        if (std::string e = find(name, &id); !e.empty()) return e;
        if (id.empty()) return name + " is not in Google Drive";
        Response r;
        if (std::string e = call("GET", c_.api_base + "/drive/v3/files/" + psp_http::url_encode(id) + "?alt=media", {}, &r); !e.empty())
            return e;
        if (!r.ok()) return http_error(r, "Google Drive download of " + name);
        *out = r.body;
        return "";
    }

    std::string put(const std::string& name, const Bytes& data) override {
        std::string id;
        if (std::string e = find(name, &id); !e.empty()) return e;
        Response r;
        if (!id.empty()) {
            Bytes body = data;
            if (std::string e = call("PATCH", c_.api_base + "/upload/drive/v3/files/" + psp_http::url_encode(id) + "?uploadType=media",
                                     {{"Content-Type", "application/octet-stream"}}, &r, &body); !e.empty()) return e;
            if (!r.ok()) return http_error(r, "Google Drive upload of " + name);
            return "";
        }
        const std::string boundary = "psprecomp_boundary_7f3a91c2";
        std::string head = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{\"name\":\"" + json_escape(name) +
                           "\",\"parents\":[\"appDataFolder\"]}\r\n--" + boundary + "\r\nContent-Type: application/octet-stream\r\n\r\n";
        const std::string tail = "\r\n--" + boundary + "--\r\n";
        Bytes body(head.begin(), head.end());
        body.insert(body.end(), data.begin(), data.end());
        body.insert(body.end(), tail.begin(), tail.end());
        if (std::string e = call("POST", c_.api_base + "/upload/drive/v3/files?uploadType=multipart&fields=id",
                                 {{"Content-Type", "multipart/related; boundary=" + boundary}}, &r, &body); !e.empty()) return e;
        if (!r.ok()) return http_error(r, "Google Drive upload of " + name);
        Json j;
        if (parse_json(r.text(), &j) && !j.str("id").empty()) ids_[name] = j.str("id");
        return "";
    }

    std::string remove(const std::string& name) override {
        std::string id;
        if (std::string e = find(name, &id); !e.empty()) return e;
        if (id.empty()) return "";
        Response r;
        if (std::string e = call("DELETE", c_.api_base + "/drive/v3/files/" + psp_http::url_encode(id), {}, &r); !e.empty()) return e;
        if (r.ok() || r.status == 404) { ids_.erase(name); return ""; }
        return http_error(r, "Google Drive delete of " + name);
    }

private:
    static std::string json_escape(const std::string& s) {
        std::string o;
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
            else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
        return o;
    }

    // The file id for `name` (cached from the last list or lookup); "" = not there.
    std::string find(const std::string& name, std::string* id) {
        auto it = ids_.find(name);
        if (it != ids_.end()) { *id = it->second; return ""; }
        const std::string q = "name = '" + name + "' and 'appDataFolder' in parents and trashed = false";
        Response r;
        if (std::string e = call("GET", c_.api_base + "/drive/v3/files?spaces=appDataFolder&fields=" + psp_http::url_encode("files(id,name)") +
                                 "&q=" + psp_http::url_encode(q), {}, &r); !e.empty()) return e;
        Json j;
        if (!r.ok() || !parse_json(r.text(), &j)) return http_error(r, "Google Drive lookup of " + name);
        id->clear();
        if (const Json* f = j.get("files"))
            if (!f->a.empty()) { *id = f->a[0].str("id"); ids_[name] = *id; }
        return "";
    }

    std::string refresh_access() {
        if (c_.refresh_token.empty()) return "not signed in to Google Drive";
        Request r;
        r.method = "POST";
        r.url = c_.auth_base + "/token";
        r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
        const std::string form = psp_http::form_encode({{"client_id", c_.client_id}, {"client_secret", c_.client_secret},
                                                         {"refresh_token", c_.refresh_token}, {"grant_type", "refresh_token"}});
        r.body.assign(form.begin(), form.end());
        const Response res = psp_http::perform(r);
        Json j;
        if (!parse_json(res.text(), &j)) return http_error(res, "Google sign-in refresh");
        if (j.str("access_token").empty()) {
            const std::string err = j.str("error");
            if (err == "invalid_grant") return "Google sign-in expired or was revoked: sign in again";
            return "Google sign-in refresh failed: " + (err.empty() ? std::to_string(res.status) : err);
        }
        token_ = j.str("access_token");
        return "";
    }

    // One API call with the bearer token; on a 401 the token is refreshed and the call repeated once.
    std::string call(const std::string& method, const std::string& url, Headers extra, Response* out, const Bytes* body = nullptr) {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (token_.empty()) if (std::string e = refresh_access(); !e.empty()) return e;
            Request r;
            r.method = method;
            r.url = url;
            r.timeout_ms = 30000;
            r.headers = extra;
            r.headers.emplace_back("Authorization", "Bearer " + token_);
            if (body) r.body = *body;
            *out = psp_http::perform(r);
            if (out->status == 401 && attempt == 0) { token_.clear(); continue; }
            break;
        }
        if (out->status == 0) return "Google Drive: " + (out->error.empty() ? "no answer" : out->error);
        return "";
    }

    GoogleConfig c_;
    std::string token_;
    std::map<std::string, std::string> ids_;     // object name -> Drive file id
};

}  // namespace

std::unique_ptr<Provider> make_webdav_provider(const WebDavConfig& cfg) { return std::make_unique<WebDavProvider>(cfg); }
std::unique_ptr<Provider> make_google_drive_provider(const GoogleConfig& cfg) { return std::make_unique<DriveProvider>(cfg); }

DeviceCode google_start_device_flow(const GoogleConfig& cfg) {
    DeviceCode d;
    Request r;
    r.method = "POST";
    r.url = cfg.auth_base + "/device/code";
    r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    const std::string form = psp_http::form_encode({{"client_id", cfg.client_id}, {"scope", "https://www.googleapis.com/auth/drive.appdata"}});
    r.body.assign(form.begin(), form.end());
    const Response res = psp_http::perform(r);
    Json j;
    if (!parse_json(res.text(), &j) || j.str("device_code").empty()) {
        d.error = http_error(res, "Google sign-in");
        if (parse_json(res.text(), &j) && !j.str("error").empty())
            d.error = "Google sign-in: " + j.str("error") + (j.str("error_description").empty() ? "" : " (" + j.str("error_description") + ")");
        return d;
    }
    d.device_code = j.str("device_code");
    d.user_code = j.str("user_code");
    d.verification_url = j.str("verification_url");
    if (d.verification_url.empty()) d.verification_url = j.str("verification_uri");
    d.expires_in = int(j.num("expires_in"));
    d.interval = std::max(1, int(j.num("interval") ? j.num("interval") : 5));
    return d;
}

TokenResult google_poll_device_flow(const GoogleConfig& cfg, const std::string& device_code) {
    TokenResult t;
    Request r;
    r.method = "POST";
    r.url = cfg.auth_base + "/token";
    r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    const std::string form = psp_http::form_encode({{"client_id", cfg.client_id}, {"client_secret", cfg.client_secret},
                                                     {"device_code", device_code},
                                                     {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});
    r.body.assign(form.begin(), form.end());
    const Response res = psp_http::perform(r);
    Json j;
    if (!parse_json(res.text(), &j)) { t.error = http_error(res, "Google sign-in"); return t; }
    if (!j.str("refresh_token").empty()) {
        t.state = TokenResult::Done;
        t.refresh_token = j.str("refresh_token");
        return t;
    }
    const std::string err = j.str("error");
    if (err == "authorization_pending") t.state = TokenResult::Pending;
    else if (err == "slow_down") t.state = TokenResult::SlowDown;
    else if (err == "access_denied") { t.state = TokenResult::Denied; t.error = "sign-in was declined"; }
    else if (err == "expired_token") { t.state = TokenResult::Expired; t.error = "the code expired: start again"; }
    else { t.state = TokenResult::Error; t.error = "Google sign-in: " + (err.empty() ? "unexpected answer" : err); }
    return t;
}

}  // namespace psp_cloud
