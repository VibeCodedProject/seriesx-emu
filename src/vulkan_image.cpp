#include "vulkan_image.h"

namespace {

bool pick_mem_type(VulkanDev& d, uint32_t type_bits, bool fast,
                   uint32_t* out) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(d.phys, &mp);
  const uint32_t preferred = fast ? d.fast_type : d.slow_type;
  if (preferred != UINT32_MAX && (type_bits & (1u << preferred))) {
    *out = preferred;
    return true;
  }
  // Any compatible type: prefer DEVICE_LOCAL for fast, any for slow.
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (!(type_bits & (1u << i))) continue;
    const auto f = mp.memoryTypes[i].propertyFlags;
    if (fast && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      *out = i;
      return true;
    }
    if (!fast) {
      *out = i;
      return true;
    }
  }
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (type_bits & (1u << i)) {
      *out = i;
      return true;
    }
  }
  return false;
}

void barrier_stage(VkImageLayout old_l, VkImageLayout new_l,
                   VkAccessFlags& src_access, VkAccessFlags& dst_access,
                   VkPipelineStageFlags& src_stage,
                   VkPipelineStageFlags& dst_stage) {
  src_access = 0;
  dst_access = 0;
  src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (old_l == VK_IMAGE_LAYOUT_UNDEFINED &&
      new_l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
  } else if (old_l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_access = VK_ACCESS_TRANSFER_READ_BIT;
  } else if (old_l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
             new_l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    src_access = VK_ACCESS_TRANSFER_READ_BIT;
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dst_access = VK_ACCESS_SHADER_READ_BIT;
  } else if (old_l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    // Clear-then-sample without an intermediate copy (the clear path).
    src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dst_access = VK_ACCESS_SHADER_READ_BIT;
  } else if (old_l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             new_l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    // Re-copy after a clear left the image sampled (the copy path).
    src_access = VK_ACCESS_SHADER_READ_BIT;
    src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_access = VK_ACCESS_TRANSFER_READ_BIT;
  } else if (old_l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
             new_l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    // Re-clear after a copy left the image as transfer source.
    src_access = VK_ACCESS_TRANSFER_READ_BIT;
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
  } else if (new_l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dst_access = VK_ACCESS_SHADER_READ_BIT;
  } else if (new_l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
  } else if (new_l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    dst_access = VK_ACCESS_TRANSFER_READ_BIT;
  }
}

bool submit_one_time(VulkanDev& d, VkCommandBuffer cb) {
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(d.dev, &fi, nullptr, &fence) != VK_SUCCESS) return false;
  bool ok = false;
  if (vkQueueSubmit(d.queue, 1, &si, fence) == VK_SUCCESS) {
    ok = vkWaitForFences(d.dev, 1, &fence, VK_TRUE, 5'000'000'000ull) ==
         VK_SUCCESS;
  }
  vkDestroyFence(d.dev, fence, nullptr);
  return ok;
}

}  // namespace

bool vulkan_image_create_2d(VulkanDev& d, uint32_t w, uint32_t h,
                            VkFormat format, bool fast, VulkanImage* out) {
  if (!d.valid || !out || w == 0 || h == 0) return false;
  VulkanImage im{};
  im.w = w;
  im.h = h;
  im.format = format;
  im.fast = fast;

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(d.dev, &ici, nullptr, &im.img) != VK_SUCCESS) return false;

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(d.dev, im.img, &req);
  if (!pick_mem_type(d, req.memoryTypeBits, fast, &im.mem_type)) {
    vkDestroyImage(d.dev, im.img, nullptr);
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = im.mem_type;
  if (vkAllocateMemory(d.dev, &ai, nullptr, &im.mem) != VK_SUCCESS) {
    vkDestroyImage(d.dev, im.img, nullptr);
    return false;
  }
  if (vkBindImageMemory(d.dev, im.img, im.mem, 0) != VK_SUCCESS) {
    vkFreeMemory(d.dev, im.mem, nullptr);
    vkDestroyImage(d.dev, im.img, nullptr);
    return false;
  }
  im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  im.valid = true;
  *out = im;
  return true;
}

void vulkan_image_destroy(VulkanDev& d, VulkanImage& im) {
  if (im.img) vkDestroyImage(d.dev, im.img, nullptr);
  if (im.mem) vkFreeMemory(d.dev, im.mem, nullptr);
  im = VulkanImage{};
}

void vulkan_cmd_transition_image(VkCommandBuffer cb, VulkanImage& im,
                                 VkImageLayout new_layout) {
  VkAccessFlags src_access, dst_access;
  VkPipelineStageFlags src_stage, dst_stage;
  barrier_stage(im.layout, new_layout, src_access, dst_access, src_stage,
                dst_stage);
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = im.layout;
  b.newLayout = new_layout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.img;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.baseMipLevel = 0;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.baseArrayLayer = 0;
  b.subresourceRange.layerCount = 1;
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                       &b);
  im.layout = new_layout;
}

bool vulkan_image_clear_color(VulkanDev& d, VulkanImage& im,
                              VkClearColorValue color) {
  if (!d.valid || !im.valid) return false;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = d.cmd_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(d.dev, &ai, &cb) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool ok = false;
  if (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
    vulkan_cmd_transition_image(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);
    vulkan_cmd_transition_image(cb, im,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vkEndCommandBuffer(cb) == VK_SUCCESS) ok = submit_one_time(d, cb);
  }
  vkFreeCommandBuffers(d.dev, d.cmd_pool, 1, &cb);
  return ok;
}

bool vulkan_image_copy_to_buffer(VulkanDev& d, VulkanImage& im,
                                 VulkanBuffer& dst) {
  if (!d.valid || !im.valid || !dst.valid) return false;
  const VkDeviceSize need =
      static_cast<VkDeviceSize>(im.w) * im.h * 4;  // R8G8B8A8
  if (dst.size < need) return false;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = d.cmd_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(d.dev, &ai, &cb) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool ok = false;
  if (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
    vulkan_cmd_transition_image(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {im.w, im.h, 1};
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst.buf, 1, &region);
    if (vkEndCommandBuffer(cb) == VK_SUCCESS) ok = submit_one_time(d, cb);
  }
  vkFreeCommandBuffers(d.dev, d.cmd_pool, 1, &cb);
  return ok;
}
