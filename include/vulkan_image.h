#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

#include "vulkan_alloc.h"
#include "vulkan_buffer.h"

// 2D image for the Xbox texture path (HLE): fast VRAM preferred, slow GTT
// fallback. Tracks layout so transitions are explicit. Homebrew only.
struct VulkanImage {
  VkImage img = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  uint32_t w = 0;
  uint32_t h = 0;
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t mem_type = UINT32_MAX;
  bool fast = false;
  bool valid = false;
};

bool vulkan_image_create_2d(VulkanDev& d, uint32_t w, uint32_t h,
                            VkFormat format, bool fast, VulkanImage* out);
void vulkan_image_destroy(VulkanDev& d, VulkanImage& im);

// Record a layout transition into cmd (updates im.layout on success).
void vulkan_cmd_transition_image(VkCommandBuffer cb, VulkanImage& im,
                                 VkImageLayout new_layout);

// One-submit helpers (allocate cmd, record, fence-wait, free).
bool vulkan_image_clear_color(VulkanDev& d, VulkanImage& im,
                              VkClearColorValue color);
bool vulkan_image_copy_to_buffer(VulkanDev& d, VulkanImage& im,
                                 VulkanBuffer& dst);
