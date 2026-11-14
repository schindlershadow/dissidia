// Cloud sync of save folders: see psp_cloudsync.h.
#include "psp_cloudsync.h"

#include "psp_savemgr.h"

#include <thread>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace psp_cloud {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) { return p.u8string(); }

bool read_file(const fs::path& p, Bytes* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out->resize(size_t(n));
    if (n) f.read(reinterpret_cast<char*>(out->data()), n);
    return bool(f);
}

// Written next to the target and renamed over it, so a reader never sees half a file.
bool write_file_atomic(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (n) f.write(static_cast<const char*>(data), std::streamsize(n));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}
bool write_file_atomic(const fs::path& p, const Bytes& b) { return write_file_atomic(p, b.data(), b.size()); }

// A plain file name: what a save folder and the remote store may hold. No separators, no "..", not empty.
bool plain_name(const std::string& n) {
    if (n.empty() || n.size() > 64 || n == "." || n == "..") return false;
    for (unsigned char c : n)
        if (c == '/' || c == '\\' || c == ':' || c < 0x20) return false;
    return true;
}
bool plain_suffix(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    for (unsigned char c : n)
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
}

// ---------------- SHA-256 ----------------
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t nbuf = 0;
    uint64_t total = 0;

    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += n;
        while (n) {
            const size_t k = std::min(n, sizeof(buf) - nbuf);
            std::memcpy(buf + nbuf, p, k);
            nbuf += k; p += k; n -= k;
            if (nbuf == 64) { block(buf); nbuf = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (nbuf != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

void put_u16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put_u32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); }
void put_u64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i))); }

const char kMagic[8] = {'P', 'S', 'C', 'S', '1', 0, 0, 0};

// ---------------- folder provider ----------------
class FolderProvider : public Provider {
public:
    explicit FolderProvider(std::string dir) : dir_(std::move(dir)) {}
    std::string label() const override { return "Folder " + dir_; }
    std::string list(std::vector<RemoteItem>* out) override {
        out->clear();
        std::error_code ec;
        if (!fs::exists(P(dir_), ec)) return "";     // nothing uploaded yet
        for (fs::directory_iterator it(P(dir_), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const std::string n = S(it->path().filename());
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) continue;   // an upload in progress
            out->push_back({n, uint64_t(it->file_size(e2))});
        }
        if (ec) return "cannot list " + dir_ + ": " + ec.message();
        std::sort(out->begin(), out->end(), [](const RemoteItem& a, const RemoteItem& b) { return a.name < b.name; });
        return "";
    }
    std::string get(const std::string& name, Bytes* out) override {
        if (!plain_name(name)) return "bad name";
        if (!read_file(P(dir_) / P(name), out)) return "cannot read " + name;
        return "";
    }
    std::string put(const std::string& name, const Bytes& data) override {
        if (!plain_name(name)) return "bad name";
        if (!write_file_atomic(P(dir_) / P(name), data)) return "cannot write " + name + " in " + dir_;
        return "";
    }
    std::string remove(const std::string& name) override {
        if (!plain_name(name)) return "bad name";
        std::error_code ec;
        fs::remove(P(dir_) / P(name), ec);
        return ec ? "cannot delete " + name + ": " + ec.message() : "";
    }
private:
    std::string dir_;
};

// ---------------- state file ----------------
struct State {
    std::string device;
    std::map<std::string, std::string> base;   // suffix -> hash as of the last sync
};

State load_state(const std::string& path) {
    State s;
    std::ifstream f(P(path));
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string k, a, b;
        is >> k >> a >> b;
        if (k == "device" && !a.empty()) s.device = a;
        else if (k == "base" && plain_suffix(a) && b.size() == 64) s.base[a] = b;
    }
    return s;
}
bool save_state(const std::string& path, const State& s) {
    std::string t = "device " + s.device + "\n";
    for (auto& kv : s.base) t += "base " + kv.first + " " + kv.second + "\n";
    return write_file_atomic(P(path), t.data(), t.size());
}

std::string new_device_id() {
    std::random_device rd;
    char b[17];
    std::snprintf(b, sizeof b, "%08x%08x", unsigned(rd()), unsigned(rd()));
    return b;
}

// ---------------- one sync run ----------------
struct Run {
    Provider& p;
    const Options& o;
    State st;
    std::map<std::string, psp_savemgr::SaveInfo> local;   // by suffix
    std::map<std::string, std::string> local_hash;
    std::map<std::string, Meta> remote;
    std::set<std::string> remote_blobs;

    Run(Provider& p_, const Options& o_) : p(p_), o(o_) { st = load_state(o.state_path); }

    bool included(const std::string& sfx) const { return !o.include || o.include(sfx); }

    void scan_local() {
        local.clear(); local_hash.clear();
        for (const psp_savemgr::SaveInfo& s : psp_savemgr::scan(o.saves_root)) {
            if (!plain_suffix(s.suffix) || !included(s.suffix)) continue;
            auto it = local.find(s.suffix);
            if (it != local.end() && it->second.disc == o.disc_id) continue;   // keep this install's own ID's save
            if (it != local.end() && s.disc != o.disc_id) continue;
            local[s.suffix] = s;
        }
        for (auto& kv : local) {
            const std::string h = hash_save(kv.second.path);
            if (h.empty()) continue;
            local_hash[kv.first] = h;
        }
        for (auto it = local.begin(); it != local.end();)
            it = local_hash.count(it->first) ? std::next(it) : local.erase(it);
    }

    std::string scan_remote() {
        remote.clear(); remote_blobs.clear();
        std::vector<RemoteItem> items;
        if (std::string e = p.list(&items); !e.empty()) return e;
        for (const RemoteItem& it : items) {
            auto ends = [&](const char* ext) {
                const size_t n = std::strlen(ext);
                return it.name.size() > n && it.name.compare(it.name.size() - n, n, ext) == 0;
            };
            if (ends(".save")) remote_blobs.insert(it.name.substr(0, it.name.size() - 5));
        }
        std::vector<std::pair<std::string, std::string>> wanted;   // (suffix, object name) of every metadata file to read
        for (const RemoteItem& it : items) {
            if (it.name.size() <= 5 || it.name.compare(it.name.size() - 5, 5, ".meta") != 0) continue;
            const std::string sfx = it.name.substr(0, it.name.size() - 5);
            if (!plain_suffix(sfx) || !included(sfx) || !remote_blobs.count(sfx)) continue;
            wanted.push_back({sfx, it.name});
        }
        // Each download is its own connection (a TLS handshake each): with a provider that allows it, read them side by side.
        std::vector<Bytes> bodies(wanted.size());
        std::vector<char> got(wanted.size(), 0);
        auto fetch = [&](size_t i) { got[i] = p.get(wanted[i].second, &bodies[i]).empty() ? 1 : 0; };
        if (p.concurrent_get() && wanted.size() > 1) {
            std::atomic<size_t> next{0};
            auto worker = [&] { for (size_t i; (i = next.fetch_add(1)) < wanted.size();) fetch(i); };
            std::vector<std::thread> th;
            const size_t n = std::min<size_t>(6, wanted.size());
            for (size_t i = 1; i < n; ++i) th.emplace_back(worker);
            worker();
            for (auto& t : th) t.join();
        } else {
            for (size_t i = 0; i < wanted.size(); ++i) fetch(i);
        }
        for (size_t i = 0; i < wanted.size(); ++i) {
            if (!got[i]) continue;
            const Meta m = meta_from_text(std::string(bodies[i].begin(), bodies[i].end()));
            if (m.valid()) remote[wanted[i].first] = m;
        }
        return "";
    }

    std::string push(const std::string& sfx, bool keep_old_remote) {
        const psp_savemgr::SaveInfo& s = local.at(sfx);
        Bytes blob;
        if (std::string e = pack_save(s.path, &blob); !e.empty()) return e;
        if (keep_old_remote && remote_blobs.count(sfx)) {
            Bytes old;
            if (p.get(sfx + ".save", &old).empty()) p.put(sfx + ".bak", old);   // best effort: a one-slot undo
        }
        if (std::string e = p.put(sfx + ".save", blob); !e.empty()) return e;
        Meta m;
        m.hash = local_hash.at(sfx);
        m.blob = sha256_hex(blob);
        m.device = st.device;
        m.disc = s.disc;
        m.mtime = s.mtime;
        const std::string t = meta_to_text(m);
        if (std::string e = p.put(sfx + ".meta", Bytes(t.begin(), t.end())); !e.empty()) return e;
        remote[sfx] = m;
        remote_blobs.insert(sfx);
        st.base[sfx] = m.hash;
        return "";
    }

    std::string pull(const std::string& sfx) {
        const Meta& m = remote.at(sfx);
        Bytes blob;
        if (std::string e = p.get(sfx + ".save", &blob); !e.empty()) return e;
        std::string disc = psp_savemgr::valid_disc_id(m.disc) ? m.disc : o.disc_id;
        const fs::path staging_root = P(o.state_path).parent_path() / "cloud_staging";
        std::error_code ec;
        fs::remove_all(staging_root, ec);
        const fs::path dir = staging_root / P(disc + sfx);
        struct Cleanup { fs::path d; ~Cleanup() { std::error_code e; fs::remove_all(d, e); } } cleanup{staging_root};
        if (!m.blob.empty() && sha256_hex(blob) != m.blob) return "downloaded save does not match its checksum (damaged or half-uploaded)";
        if (std::string e = unpack_save(blob, S(dir)); !e.empty()) return "downloaded save is damaged: " + e;
        if (hash_save(S(dir)) != m.hash) return "downloaded save does not match its checksum (damaged or half-uploaded)";
        psp_savemgr::SaveInfo info;
        if (!psp_savemgr::read_save(S(dir), &info, false)) return "downloaded save has no valid PARAM.SFO";
        std::string local_disc = psp_savemgr::valid_disc_id(o.disc_id) ? o.disc_id : disc;
        std::string existing = o.existing ? o.existing(sfx) : "";
        if (existing.empty()) { auto it = local.find(sfx); if (it != local.end()) existing = it->second.name; }
        std::string d, x;
        if (!existing.empty() && psp_savemgr::split_name(existing, &d, &x)) local_disc = d;
        std::string name;
        if (std::string e = psp_savemgr::copy_save(info, o.saves_root, local_disc, true, &name, o.backup_root); !e.empty())
            return e;
        st.base[sfx] = m.hash;
        return "";
    }

    Item decide(const std::string& sfx) {
        Item it;
        it.suffix = sfx;
        auto l = local_hash.find(sfx);
        auto r = remote.find(sfx);
        if (l != local_hash.end()) it.local_mtime = local.at(sfx).mtime;
        if (r != remote.end()) it.remote = r->second;
        auto b = st.base.find(sfx);
        const std::string base = b == st.base.end() ? "" : b->second;

        if (l != local_hash.end() && r != remote.end() && l->second == r->second.hash) {
            st.base[sfx] = l->second;
            it.action = Action::Same;
            return it;
        }
        std::string err;
        if (r == remote.end()) {                       // only here
            err = push(sfx, false);
            it.action = err.empty() ? Action::Pushed : Action::Error;
            it.note = err.empty() ? "uploaded (new)" : err;
        } else if (l == local_hash.end()) {            // only there
            if (!o.allow_pull) { it.action = Action::Skipped; it.note = "in the cloud; downloaded at the next sync"; return it; }
            err = pull(sfx);
            it.action = err.empty() ? Action::Pulled : Action::Error;
            it.note = err.empty() ? "downloaded (new)" : err;
        } else if (!base.empty() && base == r->second.hash) {
            err = push(sfx, false);
            it.action = err.empty() ? Action::Pushed : Action::Error;
            it.note = err.empty() ? "uploaded" : err;
        } else if (!base.empty() && base == l->second) {
            if (!o.allow_pull) { it.action = Action::Skipped; it.note = "newer in the cloud; downloaded at the next sync"; return it; }
            err = pull(sfx);
            it.action = err.empty() ? Action::Pulled : Action::Error;
            it.note = err.empty() ? "downloaded" : err;
        } else {
            it.action = Action::Conflict;
            it.note = "changed on both sides";
            if (o.on_conflict) {
                const Choice c = o.on_conflict(it);
                it = apply(sfx, c, it);
            }
        }
        return it;
    }

    Item apply(const std::string& sfx, Choice c, Item it) {
        std::string err;
        if (c == Choice::KeepLocal) {
            err = push(sfx, true);
            it.action = err.empty() ? Action::Pushed : Action::Error;
            it.note = err.empty() ? "kept this device's save (the cloud copy is kept as " + sfx + ".bak)" : err;
        } else if (c == Choice::KeepRemote) {
            err = pull(sfx);
            it.action = err.empty() ? Action::Pulled : Action::Error;
            it.note = err.empty() ? "took the cloud save (yours is in the backup folder)" : err;
        } else {
            it.action = Action::Skipped;
            it.note = "left alone";
        }
        return it;
    }

    void persist() { save_state(o.state_path, st); }
};

}  // namespace

std::unique_ptr<Provider> make_folder_provider(const std::string& dir) { return std::make_unique<FolderProvider>(dir); }

std::string sha256_hex(const Bytes& data) {
    Sha256 s;
    s.update(data.data(), data.size());
    return s.hex();
}

std::string hash_save(const std::string& dir) {
    Sha256 s;
    bool any = false;
    for (const std::string& f : psp_savemgr::save_files(dir)) {
        if (f == "PARAM.SFO") continue;
        Bytes b;
        if (!read_file(P(dir) / P(f), &b)) return "";
        const uint64_t n = b.size();
        s.update(f.data(), f.size());
        s.update("\0", 1);
        s.update(&n, sizeof n);
        s.update(b.data(), b.size());
        any = true;
    }
    return any ? s.hex() : "";
}

std::string pack_save(const std::string& dir, Bytes* out) {
    out->assign(kMagic, kMagic + 8);
    const std::vector<std::string> files = psp_savemgr::save_files(dir);
    if (files.empty()) return "the save folder has no PARAM.SFO";
    put_u32(*out, uint32_t(files.size()));
    for (const std::string& f : files) {
        Bytes b;
        if (!read_file(P(dir) / P(f), &b)) return "cannot read " + f;
        put_u16(*out, uint16_t(f.size()));
        out->insert(out->end(), f.begin(), f.end());
        put_u64(*out, b.size());
        out->insert(out->end(), b.begin(), b.end());
    }
    return "";
}

std::string unpack_save(const Bytes& blob, const std::string& dir) {
    size_t pos = 0;
    auto need = [&](size_t n) { return blob.size() - pos >= n; };
    if (!need(12) || std::memcmp(blob.data(), kMagic, 8) != 0) return "not a save container";
    pos = 8;
    uint32_t count = 0;
    for (int i = 0; i < 4; i++) count |= uint32_t(blob[pos + i]) << (8 * i);
    pos += 4;
    if (count == 0 || count > 64) return "bad file count";
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    if (ec) return "cannot create " + dir;
    for (uint32_t i = 0; i < count; i++) {
        if (!need(2)) return "truncated";
        const size_t nl = blob[pos] | size_t(blob[pos + 1]) << 8;
        pos += 2;
        if (!need(nl + 8)) return "truncated";
        const std::string name(reinterpret_cast<const char*>(&blob[pos]), nl);
        pos += nl;
        uint64_t len = 0;
        for (int k = 0; k < 8; k++) len |= uint64_t(blob[pos + k]) << (8 * k);
        pos += 8;
        if (!plain_name(name)) return "unsafe file name in the container";
        if (len > blob.size() - pos) return "truncated";
        if (!write_file_atomic(P(dir) / P(name), blob.data() + pos, size_t(len))) return "cannot write " + name;
        pos += size_t(len);
    }
    return "";
}

std::string meta_to_text(const Meta& m) {
    return "hash=" + m.hash + "\nblob=" + m.blob + "\nmtime=" + std::to_string(static_cast<long long>(m.mtime)) + "\ndevice=" + m.device +
           "\ndisc=" + m.disc + "\n";
}

Meta meta_from_text(const std::string& text) {
    Meta m;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "hash" && v.size() == 64 && v.find_first_not_of("0123456789abcdef") == std::string::npos) m.hash = v;
        else if (k == "blob" && v.size() == 64 && v.find_first_not_of("0123456789abcdef") == std::string::npos) m.blob = v;
        else if (k == "mtime") m.mtime = time_t(std::strtoll(v.c_str(), nullptr, 10));
        else if (k == "device" && v.size() <= 32) m.device = v;
        else if (k == "disc" && v.size() <= 16) m.disc = v;
    }
    return m;
}

std::string device_id(const std::string& state_path) {
    State s = load_state(state_path);
    if (s.device.empty()) {
        s.device = new_device_id();
        save_state(state_path, s);
    }
    return s.device;
}

std::vector<Item> sync_all(Provider& p, const Options& o, std::string* error) {
    if (error) error->clear();
    std::vector<Item> out;
    Run r(p, o);
    if (r.st.device.empty()) r.st.device = new_device_id();
    r.scan_local();
    if (std::string e = r.scan_remote(); !e.empty()) {
        if (error) *error = e;
        return out;
    }
    std::set<std::string> all;
    for (auto& kv : r.local_hash) all.insert(kv.first);
    for (auto& kv : r.remote) all.insert(kv.first);
    for (const std::string& sfx : all) out.push_back(r.decide(sfx));
    r.persist();
    return out;
}

Item resolve_conflict(Provider& p, const Options& o, const std::string& suffix, Choice c) {
    Item it;
    it.suffix = suffix;
    Run r(p, o);
    if (r.st.device.empty()) r.st.device = new_device_id();
    r.scan_local();
    if (std::string e = r.scan_remote(); !e.empty()) { it.action = Action::Error; it.note = e; return it; }
    if (!r.local_hash.count(suffix) || !r.remote.count(suffix)) {
        it.action = Action::Error;
        it.note = "the save is no longer on both sides";
        return it;
    }
    it.local_mtime = r.local.at(suffix).mtime;
    it.remote = r.remote.at(suffix);
    it = r.apply(suffix, c, it);
    r.persist();
    return it;
}

}  // namespace psp_cloud
