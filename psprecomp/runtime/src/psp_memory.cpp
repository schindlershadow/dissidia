#include "psp_memory.h"
#include "psp_platform.h"
#include <cstdio>

uint8_t* psp_memory_init() {
    // macOS uses MAP_ANON (not MAP_ANONYMOUS).
    // MAP_PRIVATE | MAP_ANON creates a sparse allocation — only touched
    // pages consume physical memory. This avoids committing the full 128MB.
    void* mem = psp_plat::alloc_zeroed(PSP_MEM_SIZE);

    if (!mem) {
        std::fprintf(stderr, "psp_memory_init: mmap(%zu bytes) failed\n",
                     PSP_MEM_SIZE);
        return nullptr;
    }

    return static_cast<uint8_t*>(mem);
}

void psp_memory_cleanup(uint8_t* rdram) {
    if (rdram) {
        psp_plat::free_pages(rdram, PSP_MEM_SIZE);
    }
}
