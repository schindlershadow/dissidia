// HDR output hooks the Vulkan backend fills in; read by the settings page and the final pass (psp_post.cpp).
#pragma once

struct PspHdrHooks {
    bool (*available)() = nullptr;               // the surface offers an HDR format (scRGB or HDR10)
    int (*encoding)() = nullptr;                 // 0 SDR, 1 scRGB linear (80 nits = 1.0), 2 HDR10 PQ; what the swapchain is now
    void (*set_wanted)(bool) = nullptr;          // ask the backend to switch swapchain (a restart-free swapchain rebuild)
    const char* (*reason)() = nullptr;           // why it is unavailable, for the settings page
};
extern PspHdrHooks g_psp_hdr;
