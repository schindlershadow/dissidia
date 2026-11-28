// PSP NPDRM EDAT (PGD) decryption: see psp_npdrm.h. A direct port of PPSSPP's ext/libkirk/amctrl.c, restricted to what an EDAT
// needs (pgd_open with flag 2 and no given vkey, pgd_decrypt_block), on top of psp_savedata_crypto's KIRK subset.
#include "psp_npdrm.h"

#include "psp_savedata_crypto.h"

#include <algorithm>
#include <cstring>

namespace psp_npdrm {
namespace {

using psp_sdcrypto::kirk_aes_cmd;
using psp_sdcrypto::kirk_fuse_cmd;

constexpr uint8_t loc_1CD4[16] = {0xE3, 0x50, 0xED, 0x1D, 0x91, 0x0A, 0x1F, 0xD0, 0x29, 0xBB, 0x1C, 0x3E, 0xF3, 0x40, 0x77, 0xFB};
constexpr uint8_t loc_1CE4[16] = {0x13, 0x5F, 0xA4, 0x7C, 0xAB, 0x39, 0x5B, 0xA4, 0x76, 0xB8, 0xCC, 0xA9, 0x8F, 0x3A, 0x04, 0x45};
constexpr uint8_t loc_1CF4[16] = {0x67, 0x8D, 0x7F, 0xA3, 0x2A, 0x9C, 0xA0, 0xD1, 0x50, 0x8A, 0xD8, 0x38, 0x5E, 0x4B, 0x01, 0x7E};
constexpr uint8_t dnas_key1A90[16] = {0xED, 0xE2, 0x5D, 0x2D, 0xBB, 0xF8, 0x12, 0xE5, 0x3C, 0x5C, 0x59, 0x32, 0xFA, 0xE3, 0xE2, 0x43};

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

// amctrl's working buffer: a 0x14 byte KIRK header and up to 0x800 bytes of data (+ slack)
struct Kirk { uint8_t buf[0x14 + 0x800 + 0x20]; };

int do_kirk4(uint8_t* buf, int size, int type) { return kirk_aes_cmd(buf, size, type, true) ? 0x80510311 : 0; }
int do_kirk7(uint8_t* buf, int size, int type) { return kirk_aes_cmd(buf, size, type, false) ? 0x80510311 : 0; }
int kirk5(uint8_t* buf, int size) { return kirk_fuse_cmd(buf, size, true) ? 0x80510312 : 0; }
int kirk8(uint8_t* buf, int size) { return kirk_fuse_cmd(buf, size, false) ? 0x80510312 : 0; }

struct MacKey { int type = 0; int pad_size = 0; uint8_t key[16] = {}; uint8_t pad[16] = {}; };
struct CipherKey { int type = 0; uint32_t seed = 0; uint8_t key[16] = {}; };

int sub_158(Kirk& k, int size, uint8_t* key, int key_type) {
    for (int i = 0; i < 16; i++) k.buf[0x14 + i] ^= key[i];
    const int r = do_kirk4(k.buf, size, key_type);
    if (r) return r;
    std::memcpy(key, k.buf + size + 4, 16);   // the last 16 bytes of the output chain into the next block
    return 0;
}

void mac_init(MacKey& m, int type) {
    m = MacKey();
    m.type = type;
}

int mac_update(Kirk& k, MacKey& m, const uint8_t* buf, int size) {
    if (m.pad_size < 0 || m.pad_size > 16) return 0x80510302;
    if (m.pad_size + size <= 16) {
        std::memcpy(m.pad + m.pad_size, buf, size_t(size));
        m.pad_size += size;
        return 0;
    }
    uint8_t* kbuf = k.buf + 0x14;
    std::memcpy(kbuf, m.pad, size_t(m.pad_size));
    int p = m.pad_size;
    m.pad_size += size;
    m.pad_size &= 0x0f;
    if (m.pad_size == 0) m.pad_size = 16;
    size -= m.pad_size;
    std::memcpy(m.pad, buf + size, size_t(m.pad_size));   // the tail waits in pad for the next update / final
    const int type = m.type == 2 ? 0x3A : 0x38;
    while (size) {
        const int ksize = size + p >= 0x0800 ? 0x0800 : size + p;
        std::memcpy(kbuf + p, buf, size_t(ksize - p));
        const int r = sub_158(k, ksize, m.key, type);
        if (r) return r;
        size -= ksize - p;
        buf += ksize - p;
        p = 0;
    }
    return 0;
}

void double_gf(uint8_t t[16]) {   // CMAC subkey: shift left one bit, xor 0x87 when a bit fell out
    const uint8_t carry = (t[0] & 0x80) ? 0x87 : 0;
    for (int i = 0; i < 15; i++) t[i] = uint8_t((t[i] << 1) | (t[i + 1] >> 7));
    t[15] = uint8_t((t[15] << 1) ^ carry);
}

int mac_final(Kirk& k, MacKey& m, uint8_t* buf, const uint8_t* vkey) {
    if (m.pad_size < 0 || m.pad_size > 16) return 0x80510302;
    const int code = m.type == 2 ? 0x3A : 0x38;
    uint8_t* kbuf = k.buf + 0x14;
    uint8_t tmp[16], tmp1[16];
    std::memset(kbuf, 0, 16);
    int r = do_kirk4(k.buf, 16, code);
    if (r) return r;
    std::memcpy(tmp, kbuf, 16);
    double_gf(tmp);
    if (m.pad_size < 16) {
        double_gf(tmp);
        m.pad[m.pad_size] = 0x80;
        if (m.pad_size + 1 < 16) std::memset(m.pad + m.pad_size + 1, 0, size_t(16 - m.pad_size - 1));
    }
    for (int i = 0; i < 16; i++) m.pad[i] ^= tmp[i];
    std::memcpy(kbuf, m.pad, 16);
    std::memcpy(tmp1, m.key, 16);
    r = sub_158(k, 0x10, tmp1, code);
    if (r) return r;
    for (int i = 0; i < 16; i++) tmp1[i] ^= loc_1CD4[i];
    if (m.type == 2) {
        std::memcpy(kbuf, tmp1, 16);
        r = kirk5(k.buf, 0x10);
        if (r) return r;
        r = do_kirk4(k.buf, 0x10, code);
        if (r) return r;
        std::memcpy(tmp1, kbuf, 16);
    }
    if (vkey) {
        for (int i = 0; i < 16; i++) tmp1[i] ^= vkey[i];
        std::memcpy(kbuf, tmp1, 16);
        r = do_kirk4(k.buf, 0x10, code);
        if (r) return r;
        std::memcpy(tmp1, kbuf, 16);
    }
    std::memcpy(buf, tmp1, 16);
    m = MacKey();
    return 0;
}

// the MAC of `m` against the 16 bytes at `out` (type 3: they are first decrypted); with `want` it only returns the MAC
int mac_final2(Kirk& k, MacKey& m, const uint8_t* out, const uint8_t* vkey, uint8_t* want = nullptr) {
    const int type = m.type;
    uint8_t tmp[16];
    int r = mac_final(k, m, tmp, vkey);
    if (r) return r;
    if (want) { std::memcpy(want, tmp, 16); return 0; }
    if (type == 3) {
        std::memcpy(k.buf + 0x14, out, 16);
        do_kirk7(k.buf, 0x10, 0x63);
    } else {
        std::memcpy(k.buf, out, 16);
    }
    for (int i = 0; i < 16; i++)
        if (k.buf[i] != tmp[i]) return 0x80510300;
    return 0;
}

// the version key hidden in a header's MAC_70 (no vkey given)
int bbmac_getkey(Kirk& k, MacKey& m, const uint8_t* bbmac, uint8_t* vkey, uint8_t* mac_out = nullptr) {
    const int type = m.type;
    uint8_t tmp[16], tmp1[16];
    int r = mac_final(k, m, tmp, nullptr);
    if (r) return r;
    if (mac_out) { std::memcpy(mac_out, tmp, 16); return 0; }
    if (type == 3) {
        std::memcpy(k.buf + 0x14, bbmac, 16);
        do_kirk7(k.buf, 0x10, 0x63);
    } else {
        std::memcpy(k.buf, bbmac, 16);
    }
    std::memcpy(tmp1, k.buf, 16);
    std::memcpy(k.buf + 0x14, tmp1, 16);
    do_kirk7(k.buf, 0x10, type == 2 ? 0x3A : 0x38);
    for (int i = 0; i < 16; i++) vkey[i] = tmp[i] ^ k.buf[i];
    return 0;
}

int sub_1F8(uint8_t* buf, int size, uint8_t* key, int key_type) {
    uint8_t tmp[16];
    std::memcpy(tmp, buf + size + 0x14 - 16, 16);
    const int r = do_kirk7(buf, size, key_type);
    if (r) return r;
    for (int i = 0; i < 16; i++) buf[i] ^= key[i];
    std::memcpy(key, tmp, 16);
    return 0;
}

// one keystream chunk (<= 0x800 bytes) XORed into dbuf; the same call encrypts and decrypts
int sub_428(Kirk& k, uint8_t* dbuf, int size, CipherKey& c) {
    uint8_t* kbuf = k.buf;
    uint8_t tmp1[16], tmp2[16];
    std::memcpy(kbuf + 0x14, c.key, 16);
    for (int i = 0; i < 16; i++) kbuf[0x14 + i] ^= loc_1CF4[i];
    const int r0 = c.type == 2 ? kirk8(kbuf, 16) : do_kirk7(kbuf, 16, 0x39);
    if (r0) return r0;
    for (int i = 0; i < 16; i++) kbuf[i] ^= loc_1CE4[i];
    std::memcpy(tmp2, kbuf, 16);
    if (c.seed == 1) {
        std::memset(tmp1, 0, 16);
    } else {
        std::memcpy(tmp1, tmp2, 16);
        wr32(tmp1 + 0x0c, c.seed - 1);
    }
    for (int i = 0; i < size; i += 16) {
        std::memcpy(kbuf + 0x14 + i, tmp2, 12);
        wr32(kbuf + 0x14 + i + 12, c.seed);
        c.seed += 1;
    }
    const int r = sub_1F8(kbuf, size, tmp1, 0x63);
    if (r) return r;
    for (int i = 0; i < size; i++) dbuf[i] ^= kbuf[i];
    return 0;
}

void cipher_init(CipherKey& c, int type, const uint8_t* header_key, const uint8_t* version_key, uint32_t seed) {
    c.type = type;
    c.seed = seed + 1;
    for (int i = 0; i < 16; i++) c.key[i] = uint8_t(header_key[i] ^ (version_key ? version_key[i] : 0));
}

int cipher_update(Kirk& k, CipherKey& c, uint8_t* data, int size) {
    int p = 0;
    while (size > 0) {
        const int dsize = size >= 0x0800 ? 0x0800 : size;
        const int r = sub_428(k, data + p, dsize, c);
        if (r) return r;
        size -= dsize;
        p += dsize;
    }
    return 0;
}

struct Pgd {
    uint32_t key_index = 0, drm_type = 0, mac_type = 0, cipher_type = 0;
    uint32_t data_size = 0, block_size = 0, data_offset = 0;
    uint8_t vkey[16] = {}, dkey[16] = {};
};

bool valid_block_size(uint32_t b) { return b >= 16 && b <= 0x100000 && (b & (b - 1)) == 0; }

// pgd_open(kirk, hdr, 2, nullptr): the 0x90 byte header is decrypted in place (0x30..0x60)
bool pgd_open(Kirk& k, uint8_t* hdr, Pgd& g) {
    g.key_index = rd32(hdr + 4);
    g.drm_type = rd32(hdr + 8);
    if (g.drm_type == 1) {
        g.mac_type = g.key_index > 1 ? 3 : 1;
        g.cipher_type = 1;
    } else {
        g.mac_type = 2;
        g.cipher_type = 2;
    }
    MacKey m;
    mac_init(m, int(g.mac_type));
    mac_update(k, m, hdr + 0x00, 0x80);
    if (mac_final2(k, m, hdr + 0x80, dnas_key1A90)) return false;   // MAC_0x80 check
    mac_init(m, int(g.mac_type));
    mac_update(k, m, hdr + 0x00, 0x70);
    bbmac_getkey(k, m, hdr + 0x70, g.vkey);                          // the version key comes out of MAC_0x70
    CipherKey c;
    cipher_init(c, int(g.cipher_type), hdr + 0x10, g.vkey, 0);
    if (cipher_update(k, c, hdr + 0x30, 0x30)) return false;
    g.data_size = rd32(hdr + 0x44);
    g.block_size = rd32(hdr + 0x48);
    g.data_offset = rd32(hdr + 0x4c);
    std::memcpy(g.dkey, hdr + 0x30, 16);
    return valid_block_size(g.block_size);
}

const uint8_t kMagic[8] = {0x00, 'P', 'S', 'P', 'E', 'D', 'A', 'T'};

}  // namespace

bool is_edat(const uint8_t* p, size_t n) { return n >= 8 && std::memcmp(p, kMagic, 8) == 0; }

int decrypt_edat(const uint8_t* file, size_t n, std::vector<uint8_t>& plain) {
    plain.clear();
    if (!is_edat(file, n)) return -1;
    constexpr size_t kPgd = 0x90;
    if (n < kPgd + 0x90) return -2;
    Kirk k;
    uint8_t hdr[0x90];
    std::memcpy(hdr, file + kPgd, 0x90);
    Pgd g;
    if (!pgd_open(k, hdr, g)) return -2;
    const uint64_t start = uint64_t(kPgd) + g.data_offset;
    if (start > n || g.data_size > (1u << 30)) return -2;
    plain.assign(g.data_size, 0);
    std::vector<uint8_t> blk(g.block_size);
    uint64_t blkno = 0;
    for (uint64_t off = 0; off < g.data_size; off += g.block_size, ++blkno) {
        std::fill(blk.begin(), blk.end(), uint8_t(0));
        const uint64_t at = start + off;
        if (at < n) std::memcpy(blk.data(), file + at, size_t(std::min<uint64_t>(g.block_size, n - at)));
        CipherKey c;
        cipher_init(c, int(g.cipher_type), g.dkey, g.vkey, uint32_t((blkno * g.block_size) >> 4));
        if (cipher_update(k, c, blk.data(), int(g.block_size))) { plain.clear(); return -2; }
        std::memcpy(plain.data() + off, blk.data(), size_t(std::min<uint64_t>(g.block_size, g.data_size - off)));
    }
    return 0;
}

int encrypt_edat(const uint8_t* plain, size_t n, uint32_t block_size, const uint8_t seed16[16], std::vector<uint8_t>& out, int drm_type) {
    out.clear();
    if (!valid_block_size(block_size) || n > (1u << 30)) return -1;
    Kirk k;
    const int mac_t = drm_type == 1 ? 1 : 2, cipher_t = mac_t, code = mac_t == 2 ? 0x3A : 0x38;
    const size_t aligned = (n + 15) & ~size_t(15);
    const size_t blocks = (aligned + block_size - 1) / block_size;
    // the version key and the descriptor we want (dkey[16], pad[4], data_size, block_size, data_offset = 0x90)
    uint8_t vkey[16], desc[0x30] = {};
    for (int i = 0; i < 16; i++) {
        vkey[i] = uint8_t(seed16[i] ^ (0x3C + 11 * i));
        desc[i] = uint8_t(seed16[(i + 5) & 15] ^ (0xA5 + i));
    }
    wr32(desc + 0x14, uint32_t(n));
    wr32(desc + 0x18, block_size);
    wr32(desc + 0x1c, 0x90);
    uint8_t hdr[0x90] = {};
    wr32(hdr + 4, 0);   // key index
    wr32(hdr + 8, uint32_t(drm_type));   // 1: MAC / cipher type 1 (fixed keys); else type 2 (the per-console fuse key)
    std::memcpy(hdr + 0x10, seed16, 16);   // the header key
    std::memcpy(hdr + 0x30, desc, 0x30);
    CipherKey c;
    cipher_init(c, cipher_t, hdr + 0x10, vkey, 0);
    if (cipher_update(k, c, hdr + 0x30, 0x30)) return -2;   // the keystream is its own inverse
    // MAC_0x70 is chosen so that bbmac_getkey gives vkey: bbmac = AES(vault[0x38], vkey ^ MAC of the first 0x70 bytes)
    MacKey m;
    uint8_t tmp[16];
    mac_init(m, mac_t);
    mac_update(k, m, hdr, 0x70);
    if (bbmac_getkey(k, m, nullptr, nullptr, tmp)) return -2;
    for (int i = 0; i < 16; i++) k.buf[0x14 + i] = uint8_t(vkey[i] ^ tmp[i]);
    if (do_kirk4(k.buf, 16, code)) return -2;
    std::memcpy(hdr + 0x70, k.buf + 0x14, 16);
    // MAC_0x80 over the first 0x80 bytes, stored as it is (type 1)
    mac_init(m, mac_t);
    mac_update(k, m, hdr, 0x80);
    if (mac_final2(k, m, nullptr, dnas_key1A90, tmp)) return -2;
    std::memcpy(hdr + 0x80, tmp, 16);
    out.assign(0x120 + blocks * block_size, 0);
    std::memcpy(out.data(), kMagic, 8);
    std::memcpy(out.data() + 0x90, hdr, 0x90);
    std::vector<uint8_t> blk(block_size);
    for (size_t b = 0; b < blocks; ++b) {
        std::fill(blk.begin(), blk.end(), uint8_t(0));
        const size_t off = b * block_size;
        if (off < n) std::memcpy(blk.data(), plain + off, std::min<size_t>(block_size, n - off));
        CipherKey ck;
        cipher_init(ck, cipher_t, desc, vkey, uint32_t((uint64_t(b) * block_size) >> 4));
        if (cipher_update(k, ck, blk.data(), int(block_size))) { out.clear(); return -2; }
        std::memcpy(out.data() + 0x120 + off, blk.data(), block_size);
    }
    return 0;
}

}  // namespace psp_npdrm
