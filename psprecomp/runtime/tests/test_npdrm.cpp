// Host test for psp_npdrm (EDAT / PGD decryption): round trips through encrypt_edat, tamper checks, plain files.
#include "../src/hle/psp_npdrm.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    uint8_t seed[16];
    for (int i = 0; i < 16; i++) seed[i] = uint8_t(0x11 * i + 3);
    const size_t sizes[] = {1, 15, 16, 17, 100, 0x2000, 0x2001, 70000};
    const uint32_t blocks[] = {16, 1024, 0x2000};
    for (int drm : {1, 3}) {
        for (size_t n : sizes) {
            for (uint32_t bs : blocks) {
                std::vector<uint8_t> plain(n);
                for (size_t i = 0; i < n; i++) plain[i] = uint8_t((i * 131 + (i >> 8) * 7 + 5) ^ (i >> 3));
                std::vector<uint8_t> edat, back;
                CHECK(psp_npdrm::encrypt_edat(plain.data(), n, bs, seed, edat, drm) == 0);
                CHECK(psp_npdrm::is_edat(edat.data(), edat.size()));
                if (n >= 32) CHECK(std::memcmp(edat.data() + 0x120, plain.data(), 32) != 0);   // really encrypted
                CHECK(psp_npdrm::decrypt_edat(edat.data(), edat.size(), back) == 0);
                CHECK(back == plain);
                // a flipped bit in the header's MAC area is refused
                std::vector<uint8_t> bad = edat;
                bad[0x90 + 0x10] ^= 1;
                CHECK(psp_npdrm::decrypt_edat(bad.data(), bad.size(), back) == -2 && back.empty());
            }
        }
    }
    // not an EDAT
    std::vector<uint8_t> plainfile(200, 0x42), out;
    CHECK(!psp_npdrm::is_edat(plainfile.data(), plainfile.size()));
    CHECK(psp_npdrm::decrypt_edat(plainfile.data(), plainfile.size(), out) == -1);
    // a truncated EDAT
    std::vector<uint8_t> e;
    std::vector<uint8_t> data(1000, 9);
    CHECK(psp_npdrm::encrypt_edat(data.data(), data.size(), 1024, seed, e) == 0);
    CHECK(psp_npdrm::decrypt_edat(e.data(), 0x60, out) == -2);
    if (g_fail == 0) std::printf("test_npdrm: ok\n");
    return g_fail ? 1 : 0;
}
