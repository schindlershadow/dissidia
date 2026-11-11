// Blocking HTTP/1.1 client with TLS (mbedTLS): see psp_http.h.
#include "psp_http.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kBadSock = INVALID_SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kBadSock = -1;
static void close_sock(sock_t s) { ::close(s); }
#endif

#ifdef PSPRECOMP_HAVE_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
extern const unsigned char psp_cacert_pem[];
extern const unsigned long psp_cacert_pem_len;
#endif

namespace psp_http {

namespace {

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(uint8_t(c))); return s; }

#ifdef _WIN32
void net_init() {
    static std::once_flag f;
    std::call_once(f, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}
std::string last_net_error() { return "error " + std::to_string(WSAGetLastError()); }
#else
void net_init() {}
std::string last_net_error() { return std::strerror(errno); }
#endif

void set_timeouts(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = DWORD(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

// TCP connect with a timeout; blocking with I/O timeouts afterwards.
sock_t connect_to(const std::string& host, int port, int timeout_ms, std::string* err) {
    net_init();
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string ps = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), ps.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        *err = "cannot resolve " + host;
        return kBadSock;
    }
    std::string last = "cannot connect to " + host;
    sock_t out = kBadSock;
    for (addrinfo* a = res; a && out == kBadSock; a = a->ai_next) {
        sock_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kBadSock) continue;
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int r = ::connect(s, a->ai_addr, int(a->ai_addrlen));
        bool pending = r != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
        bool ok = r == 0;
        if (pending) {
            fd_set w, e;
            FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) ok = true;
            else last = "connection to " + host + " timed out or was refused";
        } else if (!ok) last = "cannot connect to " + host + ": " + last_net_error();
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
#else
        const int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int r = ::connect(s, a->ai_addr, socklen_t(a->ai_addrlen));
        bool ok = r == 0;
        if (!ok && errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            if (poll(&p, 1, timeout_ms) > 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                ok = soerr == 0;
                if (!ok) last = "cannot connect to " + host + ": " + std::strerror(soerr);
            } else {
                last = "connection to " + host + " timed out";
            }
        } else if (!ok) {
            last = "cannot connect to " + host + ": " + last_net_error();
        }
        fcntl(s, F_SETFL, fl);
#endif
        if (!ok) { close_sock(s); continue; }
        set_timeouts(s, timeout_ms);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        out = s;
    }
    freeaddrinfo(res);
    if (out == kBadSock) *err = last;
    return out;
}

#ifdef PSPRECOMP_HAVE_MBEDTLS
struct TrustStore {
    mbedtls_x509_crt crt;
    bool ok = false;
    TrustStore() {
        mbedtls_x509_crt_init(&crt);
        psa_crypto_init();
        // positive return = some certificates were skipped (unsupported algorithm, expired entry): the rest are usable
        ok = mbedtls_x509_crt_parse(&crt, psp_cacert_pem, size_t(psp_cacert_pem_len)) >= 0;
        if (const char* extra = std::getenv("PSPRECOMP_CA_FILE")) {
            std::ifstream f(extra, std::ios::binary);
            std::string pem((std::istreambuf_iterator<char>(f)), {});
            if (!pem.empty()) {
                pem.push_back('\0');
                mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(pem.data()), pem.size());
            }
        }
    }
};
TrustStore& trust() { static TrustStore t; return t; }

std::string tls_error(int rc) {
    char b[160];
    mbedtls_strerror(rc, b, sizeof b);
    return std::string(b);
}

int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    const sock_t s = *static_cast<sock_t*>(ctx);
#ifdef _WIN32
    const int r = ::send(s, reinterpret_cast<const char*>(buf), int(len), 0);
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    const ssize_t r = ::send(s, buf, len, flags);
    if (r < 0) return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
#endif
    return int(r);
}
int bio_recv(void* ctx, unsigned char* buf, size_t len) {
    const sock_t s = *static_cast<sock_t*>(ctx);
#ifdef _WIN32
    const int r = ::recv(s, reinterpret_cast<char*>(buf), int(len), 0);
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;     // includes the receive timeout
#else
    const ssize_t r = ::recv(s, buf, len, 0);
    if (r < 0) return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;   // EAGAIN = timeout
#endif
    return int(r);
}
#endif

// One connection, plain or TLS.
class Conn {
public:
    ~Conn() { close(); }
    bool open(const std::string& scheme, const std::string& host, int port, int timeout_ms, std::string* err) {
        sock_ = connect_to(host, port, timeout_ms, err);
        if (sock_ == kBadSock) return false;
        if (scheme == "https") {
#ifdef PSPRECOMP_HAVE_MBEDTLS
            if (!trust().ok) { *err = "the embedded CA certificates could not be loaded"; return false; }
            mbedtls_ssl_init(&ssl_); mbedtls_ssl_config_init(&conf_);
            mbedtls_entropy_init(&entropy_); mbedtls_ctr_drbg_init(&drbg_);
            tls_ = true;
            int rc = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, nullptr, 0);
            if (rc == 0) rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                          MBEDTLS_SSL_PRESET_DEFAULT);
            if (rc != 0) { *err = "TLS setup failed: " + tls_error(rc); return false; }
            mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&conf_, &trust().crt, nullptr);
            mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
            if ((rc = mbedtls_ssl_setup(&ssl_, &conf_)) != 0 || (rc = mbedtls_ssl_set_hostname(&ssl_, host.c_str())) != 0) {
                *err = "TLS setup failed: " + tls_error(rc);
                return false;
            }
            mbedtls_ssl_set_bio(&ssl_, &sock_, bio_send, bio_recv, nullptr);
            for (;;) {
                rc = mbedtls_ssl_handshake(&ssl_);
                if (rc == 0) break;
                if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                *err = "TLS handshake with " + host + " failed: " + tls_error(rc);
                if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                    char info[256];
                    mbedtls_x509_crt_verify_info(info, sizeof info, "", mbedtls_ssl_get_verify_result(&ssl_));
                    std::string v(info);
                    while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
                    *err += " (" + v + ")";
                }
                return false;
            }
#else
            *err = "this build has no TLS support (https is unavailable)";
            return false;
#endif
        }
        return true;
    }
    // >0 bytes; 0 = the peer closed; <0 = error (err set)
    long read(uint8_t* buf, size_t n, std::string* err) {
#ifdef PSPRECOMP_HAVE_MBEDTLS
        if (tls_) {
            for (;;) {
                const int r = mbedtls_ssl_read(&ssl_, buf, n);
                if (r > 0) return r;
                if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
                if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                *err = "read failed (timed out or the connection dropped): " + tls_error(r);
                return -1;
            }
        }
#endif
#ifdef _WIN32
        const int r = ::recv(sock_, reinterpret_cast<char*>(buf), int(n), 0);
#else
        long r;
        do { r = ::recv(sock_, buf, n, 0); } while (r < 0 && errno == EINTR);
#endif
        if (r < 0) { *err = "read failed (timed out or the connection dropped)"; return -1; }
        return r;
    }
    bool write_all(const uint8_t* p, size_t n, std::string* err) {
        while (n) {
            long w;
#ifdef PSPRECOMP_HAVE_MBEDTLS
            if (tls_) {
                const int r = mbedtls_ssl_write(&ssl_, p, n);
                if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (r < 0) { *err = "write failed: " + tls_error(r); return false; }
                w = r;
            } else
#endif
            {
#ifdef _WIN32
                w = ::send(sock_, reinterpret_cast<const char*>(p), int(n), 0);
#else
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                do { w = ::send(sock_, p, n, flags); } while (w < 0 && errno == EINTR);
#endif
                if (w < 0) { *err = "write failed (timed out or the connection dropped)"; return false; }
            }
            p += w; n -= size_t(w);
        }
        return true;
    }
    void close() {
#ifdef PSPRECOMP_HAVE_MBEDTLS
        if (tls_) {
            if (sock_ != kBadSock) mbedtls_ssl_close_notify(&ssl_);
            mbedtls_ssl_free(&ssl_); mbedtls_ssl_config_free(&conf_);
            mbedtls_ctr_drbg_free(&drbg_); mbedtls_entropy_free(&entropy_);
            tls_ = false;
        }
#endif
        if (sock_ != kBadSock) { close_sock(sock_); sock_ = kBadSock; }
    }
private:
    sock_t sock_ = kBadSock;
#ifdef PSPRECOMP_HAVE_MBEDTLS
    bool tls_ = false;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
#endif
};

// Decodes a chunked body from `in` starting at `pos`. true when the terminating chunk was seen (body complete);
// false when more bytes are needed. `*bad` is set for a malformed stream.
bool decode_chunked(const Bytes& in, size_t pos, Bytes* body, bool* bad, size_t max_body) {
    body->clear();
    for (;;) {
        size_t eol = pos;
        while (eol + 1 < in.size() && !(in[eol] == '\r' && in[eol + 1] == '\n')) eol++;
        if (eol + 1 >= in.size()) return false;
        const std::string line(reinterpret_cast<const char*>(&in[pos]), eol - pos);
        char* endp = nullptr;
        const unsigned long long n = std::strtoull(line.c_str(), &endp, 16);
        if (endp == line.c_str()) { *bad = true; return false; }
        pos = eol + 2;
        if (n == 0) return true;                                   // trailers are ignored
        if (n > max_body || body->size() + n > max_body) { *bad = true; return false; }
        if (in.size() < pos + n + 2) return false;
        body->insert(body->end(), in.begin() + long(pos), in.begin() + long(pos + n));
        pos += size_t(n);
        if (in[pos] != '\r' || in[pos + 1] != '\n') { *bad = true; return false; }
        pos += 2;
    }
}

Response once(const Request& rq, const Url& u) {
    Response out;
    Conn c;
    if (!c.open(u.scheme, u.host, u.port, rq.timeout_ms, &out.error)) return out;

    const bool dflt = (u.scheme == "http" && u.port == 80) || (u.scheme == "https" && u.port == 443);
    const bool v6 = u.host.find(':') != std::string::npos;
    std::string head = rq.method + " " + u.path + " HTTP/1.1\r\nHost: " + (v6 ? "[" + u.host + "]" : u.host) +
                       (dflt ? "" : ":" + std::to_string(u.port)) +
                       "\r\nUser-Agent: psprecomp-cloudsync\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    bool has_len = false;
    for (auto& h : rq.headers) {
        if (h.first.find_first_of("\r\n") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos) {
            out.error = "bad header";
            return out;
        }
        const std::string ln = lower(h.first);
        if (ln == "host" || ln == "connection") continue;
        if (ln == "content-length") has_len = true;
        head += h.first + ": " + h.second + "\r\n";
    }
    if (!has_len && (!rq.body.empty() || rq.method == "POST" || rq.method == "PUT"))
        head += "Content-Length: " + std::to_string(rq.body.size()) + "\r\n";
    head += "\r\n";
    if (!c.write_all(reinterpret_cast<const uint8_t*>(head.data()), head.size(), &out.error)) return out;
    if (!rq.body.empty() && !c.write_all(rq.body.data(), rq.body.size(), &out.error)) return out;

    // read until the headers are complete, then the body by its framing
    Bytes in;
    uint8_t tmp[16384];
    size_t hdr_end = std::string::npos;
    bool eof = false;
    auto fill = [&]() -> bool {
        const long r = c.read(tmp, sizeof tmp, &out.error);
        if (r < 0) return false;
        if (r == 0) { eof = true; return true; }
        if (in.size() + size_t(r) > rq.max_body + 65536) { out.error = "response too large"; return false; }
        in.insert(in.end(), tmp, tmp + r);
        return true;
    };
    while (hdr_end == std::string::npos) {
        for (size_t i = 3; i < in.size(); i++)
            if (in[i - 3] == '\r' && in[i - 2] == '\n' && in[i - 1] == '\r' && in[i] == '\n') { hdr_end = i + 1; break; }
        if (hdr_end != std::string::npos) break;
        if (eof) { out.error = "the server closed the connection before answering"; return out; }
        if (!fill()) return out;
    }
    {
        const std::string hs(reinterpret_cast<const char*>(in.data()), hdr_end);
        size_t p = hs.find("\r\n");
        const std::string status = hs.substr(0, p);
        if (status.compare(0, 5, "HTTP/") != 0 || status.size() < 12) { out.error = "not an HTTP response"; return out; }
        out.status = std::atoi(status.c_str() + status.find(' ') + 1);
        while (p != std::string::npos && p + 2 < hs.size()) {
            const size_t q = hs.find("\r\n", p + 2);
            const std::string line = hs.substr(p + 2, q == std::string::npos ? std::string::npos : q - p - 2);
            p = q;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) v++;
            out.headers.emplace_back(line.substr(0, colon), line.substr(v));
        }
    }
    const bool no_body = rq.method == "HEAD" || out.status == 204 || out.status == 304 || (out.status >= 100 && out.status < 200);
    const std::string te = lower(out.header("Transfer-Encoding"));
    const std::string cl = out.header("Content-Length");
    if (no_body) return out;
    if (te.find("chunked") != std::string::npos) {
        for (;;) {
            bool bad = false;
            if (decode_chunked(in, hdr_end, &out.body, &bad, rq.max_body)) return out;
            if (bad) { out.status = 0; out.error = "malformed chunked response"; return out; }
            if (eof) { out.status = 0; out.error = "the response was cut off"; return out; }
            if (!fill()) { out.status = 0; return out; }
        }
    }
    if (!cl.empty()) {
        const unsigned long long n = std::strtoull(cl.c_str(), nullptr, 10);
        if (n > rq.max_body) { out.status = 0; out.error = "response too large"; return out; }
        while (in.size() - hdr_end < n) {
            if (eof) { out.status = 0; out.error = "the response was cut off"; return out; }
            if (!fill()) { out.status = 0; return out; }
        }
        out.body.assign(in.begin() + long(hdr_end), in.begin() + long(hdr_end + n));
        return out;
    }
    while (!eof) if (!fill()) { out.status = 0; return out; }
    out.body.assign(in.begin() + long(hdr_end), in.end());
    return out;
}

}  // namespace

std::string Response::header(const std::string& name) const {
    const std::string n = lower(name);
    for (auto& h : headers) if (lower(h.first) == n) return h.second;
    return "";
}

bool tls_available() {
#ifdef PSPRECOMP_HAVE_MBEDTLS
    return true;
#else
    return false;
#endif
}

Url parse_url(const std::string& url) {
    Url u;
    const size_t se = url.find("://");
    if (se == std::string::npos) return u;
    u.scheme = lower(url.substr(0, se));
    if (u.scheme != "http" && u.scheme != "https") return u;
    size_t p = se + 3;
    size_t pe = url.find_first_of("/?#", p);
    std::string auth = url.substr(p, pe == std::string::npos ? std::string::npos : pe - p);
    std::string rest = pe == std::string::npos ? "" : url.substr(pe);
    const size_t hash = rest.find('#');
    if (hash != std::string::npos) rest.erase(hash);
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;      // "http://h?x" -> "/?x"
    if (auth.find('@') != std::string::npos) return u;           // credentials in URLs are not supported
    u.port = u.scheme == "https" ? 443 : 80;
    if (!auth.empty() && auth[0] == '[') {                       // [v6]:port
        const size_t rb = auth.find(']');
        if (rb == std::string::npos) return u;
        u.host = auth.substr(1, rb - 1);
        if (rb + 1 < auth.size()) {
            if (auth[rb + 1] != ':') return u;
            u.port = std::atoi(auth.c_str() + rb + 2);
        }
    } else {
        const size_t colon = auth.rfind(':');
        if (colon != std::string::npos) {
            u.host = auth.substr(0, colon);
            u.port = std::atoi(auth.c_str() + colon + 1);
        } else {
            u.host = auth;
        }
    }
    if (u.host.empty() || u.port <= 0 || u.port > 65535) return u;
    u.path = rest;
    u.valid = true;
    return u;
}

Response perform(const Request& r0) {
    Request rq = r0;
    for (int hop = 0;; hop++) {
        const Url u = parse_url(rq.url);
        if (!u.valid) {
            Response e;
            e.error = "bad URL: " + rq.url;
            return e;
        }
        Response res = once(rq, u);
        const bool redirect = res.status == 301 || res.status == 302 || res.status == 303 || res.status == 307 || res.status == 308;
        if (!redirect || hop >= rq.max_redirects) return res;
        const std::string loc = res.header("Location");
        if (loc.empty()) return res;
        std::string next;
        if (loc.compare(0, 7, "http://") == 0 || loc.compare(0, 8, "https://") == 0) next = loc;
        else if (loc[0] == '/') next = u.scheme + "://" + (u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host) + ":" + std::to_string(u.port) + loc;
        else { res.error = "unsupported relative redirect"; res.status = 0; return res; }
        const Url nu = parse_url(next);
        if (!nu.valid) return res;
        if (nu.host != u.host) {                                   // never send credentials to another host
            Headers kept;
            for (auto& h : rq.headers) if (lower(h.first) != "authorization") kept.push_back(h);
            rq.headers = kept;
        }
        if (res.status == 303 || ((res.status == 301 || res.status == 302) && rq.method == "POST")) {
            rq.method = "GET";
            rq.body.clear();
            Headers kept;
            for (auto& h : rq.headers) {
                const std::string n = lower(h.first);
                if (n != "content-type" && n != "content-length") kept.push_back(h);
            }
            rq.headers = kept;
        }
        rq.url = next;
    }
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') o += char(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string o;
    for (auto& p : kv) {
        if (!o.empty()) o += '&';
        o += url_encode(p.first) + "=" + url_encode(p.second);
    }
    return o;
}

std::string base64(const Bytes& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        const uint32_t v = uint32_t(d[i]) << 16 | uint32_t(d[i + 1]) << 8 | d[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == d.size()) {
        const uint32_t v = uint32_t(d[i]) << 16;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == d.size()) {
        const uint32_t v = uint32_t(d[i]) << 16 | uint32_t(d[i + 1]) << 8;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
    }
    return o;
}
std::string base64(const std::string& s) { return base64(Bytes(s.begin(), s.end())); }

}  // namespace psp_http
