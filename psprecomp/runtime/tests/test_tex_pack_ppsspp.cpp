// PPSSPP-compatible texture pack: hash functions and key/lookup against a throw-away pack folder.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "psp_tex_pack.h"

std::string psp_io_ms0_host_path() { return "/nonexistent/ms0"; }   // the real one lives in the IO layer

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static std::vector<uint8_t> tiny_png() {   // 1x1 RGBA PNG (red)
    static const uint8_t png[] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,0x49,0x48,0x44,0x52,0,0,0,1,0,0,0,1,8,6,0,0,0,0x1F,0x15,0xC4,0x89,
        0,0,0,0x0D,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0xF8,0xCF,0xC0,0xF0,0x1F,0,0x05,0x00,0x01,0xFF,0x56,0xC7,0x2F,0x0D,0,0,0,0,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};
    return std::vector<uint8_t>(png, png + sizeof png);
}

int main() {
    // published XXH32 / XXH64 test vectors (seed 0)
    CHECK(psp_texpack_xxh32((const uint8_t*)"", 0, 0) == 0x02CC5D05u);
    CHECK(psp_texpack_xxh32((const uint8_t*)"abc", 3, 0) == 0x32D153FFu);
    const char* s = "Nobody inspects the spammish repetition";   // 39 bytes: covers the 16-byte main loop + tails
    CHECK(psp_texpack_xxh32((const uint8_t*)s, std::strlen(s), 0) == 0xE2293B2Fu);

    // contiguous (bufw <= width): the hash is over bpp*bufw*h/8 bytes
    std::vector<uint8_t> mem(4096);
    for (size_t i = 0; i < mem.size(); i++) mem[i] = (uint8_t)(i * 7 + 3);
    uint32_t a = psp_texpack_ppsspp_content_hash(mem.data(), mem.size(), 3 /*8888*/, 16, 8, 16, 2);
    uint32_t b = psp_texpack_ppsspp_content_hash(mem.data(), mem.size(), 3, 16, 8, 16, 2);
    CHECK(a == b && a != 0);
    mem[100] ^= 1;   // inside the 16*8*4 = 512 hashed bytes
    CHECK(psp_texpack_ppsspp_content_hash(mem.data(), mem.size(), 3, 16, 8, 16, 2) != a);
    mem[100] ^= 1;
    mem[600] ^= 1;   // beyond them
    CHECK(psp_texpack_ppsspp_content_hash(mem.data(), mem.size(), 3, 16, 8, 16, 2) == a);
    // with gaps (bufw > width) every row is hashed separately and combined (r*11) ^ row
    uint32_t g = psp_texpack_ppsspp_content_hash(mem.data(), mem.size(), 3, 16, 4, 32, 2);
    CHECK(g != 0);
    mem[16 * 4] ^= 1;   // inside the gap of row 0 (bytes 64..127): not hashed
    CHECK(psp_texpack_ppsspp_content_hash(mem.data(), mem.size(), 3, 16, 4, 32, 2) == g);
    mem[16 * 4] ^= 1;

    // a pack folder with textures.ini: key computation + lookup
    std::string dir = "/tmp/psp_texpack_ppsspp_test";
    std::system(("rm -rf " + dir + " && mkdir -p " + dir + "/ui").c_str());
    setenv("PSPRECOMP_TEXPACK", dir.c_str(), 1);
    setenv("PSPRECOMP_TEXPACK_LOG", "1", 1);
    PspPpssppTex t{};
    t.addr = 0x09123400; t.size_reg = 0x0404; t.fmt = 3; t.width = 16; t.height = 16; t.bufw = 16;
    t.mem = mem.data(); t.mem_len = mem.size();
    const uint32_t ch = psp_texpack_ppsspp_content_hash(mem.data(), mem.size(), 3, 16, 16, 16, 2);
    char key_noaddr[64], key_addr[64];
    std::snprintf(key_noaddr, sizeof key_noaddr, "%016llx%08x", (unsigned long long)0x0404, ch);
    std::snprintf(key_addr, sizeof key_addr, "%016llx%08x", (unsigned long long)(((uint64_t)(0x09123400 & 0x3FFFFFFF) << 32) | 0x0404), ch);
    {
        std::ofstream ini(dir + "/textures.ini");
        ini << "[options]\nversion = 1\nhash = xxh64\nignoreAddress = true\n\n[hashes]\n" << key_noaddr << " = ui/red.png\n"
            << "00000000deadbeef00000000 = ui/other.png\n";
    }
    { auto p = tiny_png(); std::ofstream f(dir + "/ui/red.png", std::ios::binary); f.write((const char*)p.data(), (std::streamsize)p.size()); }
    CHECK(psp_texpack_ppsspp_active());
    std::vector<uint8_t> px; int w = 0, h = 0;
    CHECK(psp_texpack_ppsspp_lookup(t, px, w, h));          // found through the address-less key (ignoreAddress)
    CHECK(w == 1 && h == 1 && px.size() == 4 && px[0] == 255 && px[1] == 0 && px[2] == 0);
    t.size_reg = 0x0505;                                      // another size: other key: miss
    CHECK(!psp_texpack_ppsspp_lookup(t, px, w, h));
    (void)key_addr;
    std::system(("rm -rf " + dir).c_str());
    std::printf(failures ? "%d FAILED\n" : "all PPSSPP texture pack tests passed\n", failures);
    return failures ? 1 : 0;
}
