// Vulkan foundation test: headless device + allocator, clear an image, read it back; with --window also opens an SDL
// Vulkan window and presents a few cleared frames. Skips (exit 0, "SKIP") when no Vulkan device exists.
#include "../src/vk/psp_vk_context.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../third_party/vk_mem_alloc.h"

using namespace pspvk;

static bool clear_readback(Context& c, uint8_t want[4]) {
    VmaAllocator va = (VmaAllocator)c.allocator();
    const uint32_t W = 64, H = 32;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = {W, H, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkImage img; VmaAllocation ia;
    if (vmaCreateImage(va, &ii, &ai, &img, &ia, nullptr) != VK_SUCCESS) return false;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = W * H * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo bai{}; bai.usage = VMA_MEMORY_USAGE_GPU_TO_CPU; bai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf; VmaAllocation ba; VmaAllocationInfo binfo;
    if (vmaCreateBuffer(va, &bi, &bai, &buf, &ba, &binfo) != VK_SUCCESS) return false;

    VkCommandBuffer cb = c.begin_once();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue cv = {{want[0] / 255.f, want[1] / 255.f, want[2] / 255.f, want[3] / 255.f}};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &b.subresourceRange);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy rg{}; rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
    bool ok = c.end_once(cb);
    if (ok) {
        vmaInvalidateAllocation(va, ba, 0, VK_WHOLE_SIZE);
        const uint8_t* p = (const uint8_t*)binfo.pMappedData;
        for (uint32_t i = 0; i < W * H && ok; i++)
            for (int k = 0; k < 4; k++) if (std::abs((int)p[i * 4 + k] - (int)want[k]) > 1) { std::printf("pixel %u ch %d: got %d want %d\n", i, k, p[i * 4 + k], want[k]); ok = false; break; }
    }
    vmaDestroyBuffer(va, buf, ba); vmaDestroyImage(va, img, ia);
    return ok;
}

int main(int argc, char** argv) {
    bool window = false;
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--window")) window = true;
    ContextOptions o; o.validation = std::getenv("PSPRECOMP_VK_VALIDATION") != nullptr;
    if (const char* d = std::getenv("PSPRECOMP_VK_DEVICE")) { if (*d >= '0' && *d <= '9') o.device_index = std::atoi(d); else o.device_name = d; }
    SDL_Window* win = nullptr;
    if (window) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::printf("SKIP: SDL_Init: %s\n", SDL_GetError()); return 0; }
        win = SDL_CreateWindow("vk test", 0, 0, 480, 272, SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
        if (!win) { std::printf("SKIP: no Vulkan window: %s\n", SDL_GetError()); return 0; }
        o.window = win;
    }
    Context c; std::string err;
    if (!c.init(o, &err)) { std::printf("SKIP: %s\n", err.c_str()); return 0; }
    uint8_t colors[3][4] = {{255, 0, 0, 255}, {16, 128, 240, 77}, {0, 0, 0, 0}};
    for (auto& col : colors) if (!clear_readback(c, col)) { std::printf("FAIL: clear/readback mismatch on %s\n", c.device_name().c_str()); return 1; }
    std::printf("PASS: headless clear+readback on %s\n", c.device_name().c_str());
    if (window) {
        if (!c.create_swapchain(480, 272, true, &err)) { std::printf("FAIL: swapchain: %s\n", err.c_str()); return 1; }
        VkDevice dev = c.device();
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore acq, rel; vkCreateSemaphore(dev, &sci, nullptr, &acq); vkCreateSemaphore(dev, &sci, nullptr, &rel);
        for (int f = 0; f < 5; f++) {
            uint32_t idx;
            if (vkAcquireNextImageKHR(dev, c.swapchain().handle, UINT64_MAX, acq, VK_NULL_HANDLE, &idx) != VK_SUCCESS) { std::printf("FAIL: acquire\n"); return 1; }
            VkCommandBuffer cb = c.begin_once();
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.image = c.swapchain().images[idx]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkClearColorValue cv = {{0.f, f / 5.f, 0.5f, 1.f}};
            vkCmdClearColorImage(cb, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &b.subresourceRange);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            vkEndCommandBuffer(cb);
            VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acq; si.pWaitDstStageMask = &ws;
            si.commandBufferCount = 1; si.pCommandBuffers = &cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &rel;
            vkQueueSubmit(c.queue(), 1, &si, VK_NULL_HANDLE);
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &rel;
            VkSwapchainKHR sc = c.swapchain().handle; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
            VkResult pr = vkQueuePresentKHR(c.queue(), &pi);
            if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { std::printf("FAIL: present %d\n", pr); return 1; }
            vkQueueWaitIdle(c.queue());
            vkFreeCommandBuffers(dev, c.command_pool(), 1, &cb);
        }
        vkDestroySemaphore(dev, acq, nullptr); vkDestroySemaphore(dev, rel, nullptr);
        std::printf("PASS: swapchain present x5 (%ux%u)\n", c.swapchain().extent.width, c.swapchain().extent.height);
    }
    c.destroy();
    if (win) SDL_DestroyWindow(win);
    return 0;
}
