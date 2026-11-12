// Run-time loader for the Vulkan entry points. The runtime never links libvulkan: the release is cross-built by the bundled
// toolchain (no Vulkan SDK, no import library), and a machine without a Vulkan driver must still start and fall back to OpenGL.
// SDL opens the loader (libvulkan.so.1 / vulkan-1.dll / MoltenVK) and hands over vkGetInstanceProcAddr; every function the
// backend uses is a global function pointer with the usual name (VK_NO_PROTOTYPES), resolved here.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR   // vkCreateAndroidSurfaceKHR: a custom driver cannot create its surface through SDL's own loader
#endif
#include <vulkan/vulkan.h>
#include <string>

// Every Vulkan function the backend calls, except the four that exist before an instance does.
#define PSP_VK_FUNCS(X) \
    X(vkAcquireNextImageKHR) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBlitImage) \
    X(vkCmdBuildAccelerationStructuresKHR) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdDispatch) \
    X(vkCmdDraw) \
    X(vkCmdDrawMultiEXT) \
    X(vkCmdEndRenderPass) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPushConstants) \
    X(vkCmdResetQueryPool) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetViewport) \
    X(vkCmdWriteTimestamp) \
    X(vkCreateAccelerationStructureKHR) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateComputePipelines) \
    X(vkCreateDebugUtilsMessengerEXT) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateDevice) \
    X(vkCreateFence) \
    X(vkCreateFramebuffer) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImageView) \
    X(vkCreatePipelineCache) \
    X(vkCreatePipelineLayout) \
    X(vkCreateQueryPool) \
    X(vkCreateRenderPass) \
    X(vkCreateSampler) \
    X(vkCreateSemaphore) \
    X(vkCreateShaderModule) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroyAccelerationStructureKHR) \
    X(vkDestroyBuffer) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDebugUtilsMessengerEXT) \
    X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDevice) \
    X(vkDestroyFence) \
    X(vkDestroyFramebuffer) \
    X(vkDestroyImageView) \
    X(vkDestroyInstance) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineCache) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroyQueryPool) \
    X(vkDestroyRenderPass) \
    X(vkDestroySampler) \
    X(vkDestroySemaphore) \
    X(vkDestroyShaderModule) \
    X(vkDestroySurfaceKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkEnumeratePhysicalDevices) \
    X(vkFreeCommandBuffers) \
    X(vkFreeMemory) \
    X(vkGetAccelerationStructureBuildSizesKHR) \
    X(vkGetAccelerationStructureDeviceAddressKHR) \
    X(vkGetBufferDeviceAddress) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceBufferMemoryRequirements) \
    X(vkGetDeviceProcAddr) \
    X(vkGetDeviceQueue) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetQueryPoolResults) \
    X(vkGetSwapchainImagesKHR) \
    X(vkMapMemory) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkResetCommandPool) \
    X(vkResetDescriptorPool) \
    X(vkResetFences) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences) \

#define PSP_VK_DECLARE(name) extern PFN_##name name;
PSP_VK_DECLARE(vkGetInstanceProcAddr)
PSP_VK_DECLARE(vkCreateInstance)
PSP_VK_DECLARE(vkEnumerateInstanceExtensionProperties)
PSP_VK_DECLARE(vkEnumerateInstanceLayerProperties)
PSP_VK_FUNCS(PSP_VK_DECLARE)
#ifdef __ANDROID__
extern PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR;   // resolved by load_instance (Android only)
#endif

namespace pspvk {
/// True when Vulkan was opened through a custom driver (psp_gpu_driver.h) instead of the phone's own: SDL's surface helper cannot be
/// used then (it talks to the system loader), so Context creates the surface with vkCreateAndroidSurfaceKHR itself.
bool using_custom_driver();
/// Opens the Vulkan loader and resolves the global entry points. False (with *err) when there is no Vulkan on this machine.
bool load_global(std::string* err);
/// Resolves everything else through the instance (null for functions of extensions the driver lacks).
void load_instance(VkInstance instance);
/// Re-resolves device-level functions straight from the device (skips the loader trampoline).
void load_device(VkDevice device);
}  // namespace pspvk
