#include "vulkan_present.h"

#ifdef DISPLAY_GLFW_ENABLED

#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>

struct VulkanPresenter::Impl {
  GLFWwindow* win = nullptr;
  VkInstance inst = VK_NULL_HANDLE;
  VkSurfaceKHR surf = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t qfam = 0;
  VkSwapchainKHR swap = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool swizzle_rb = false;
  VkExtent2D extent{};
  std::vector<VkImage> images;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  VkDeviceSize staging_size = 0;
  VkFence frame_fence = VK_NULL_HANDLE;
  VkSemaphore acquire_sem = VK_NULL_HANDLE;
  bool glfw_inited = false;
};

namespace {

bool has_dev_ext(VkPhysicalDevice p, const char* name) {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateDeviceExtensionProperties(p, nullptr, &n, exts.data());
  for (const auto& e : exts)
    if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}

bool pick_physical(VkInstance inst, VkSurfaceKHR surf,
                   VkPhysicalDevice* out_phys, uint32_t* out_qfam) {
  uint32_t n = 0;
  if (vkEnumeratePhysicalDevices(inst, &n, nullptr) != VK_SUCCESS || n == 0)
    return false;
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(inst, &n, devs.data());
  // Prefer discrete AMD, like the rest of the project.
  std::sort(devs.begin(), devs.end(), [](VkPhysicalDevice a, VkPhysicalDevice b) {
    VkPhysicalDeviceProperties pa{}, pb{};
    vkGetPhysicalDeviceProperties(a, &pa);
    vkGetPhysicalDeviceProperties(b, &pb);
    const int sa = (pa.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                       ? (pa.vendorID == 0x1002 ? 2 : 1)
                       : 0;
    const int sb = (pb.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                       ? (pb.vendorID == 0x1002 ? 2 : 1)
                       : 0;
    return sa > sb;
  });
  for (auto p : devs) {
    if (!has_dev_ext(p, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
    for (uint32_t i = 0; i < qn; ++i) {
      if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      VkBool32 sup = VK_FALSE;
      if (vkGetPhysicalDeviceSurfaceSupportKHR(p, i, surf, &sup) != VK_SUCCESS)
        continue;
      if (sup) {
        *out_phys = p;
        *out_qfam = i;
        return true;
      }
    }
  }
  return false;
}

bool make_swapchain(VulkanPresenter::Impl* im) {
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(im->phys, im->surf, &caps);
  VkExtent2D ex = im->extent;
  ex.width = std::clamp(ex.width, caps.minImageExtent.width,
                        caps.maxImageExtent.width);
  ex.height = std::clamp(ex.height, caps.minImageExtent.height,
                         caps.maxImageExtent.height);
  im->extent = ex;

  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(im->phys, im->surf, &nf, nullptr);
  if (nf == 0) return false;
  std::vector<VkSurfaceFormatKHR> fmts(nf);
  vkGetPhysicalDeviceSurfaceFormatsKHR(im->phys, im->surf, &nf, fmts.data());
  // Byte-identical 32-bit RGBA layouts only; UNORM preferred, SRGB
  // siblings accepted (same bytes on the wire, presentation only).
  // R and B are swizzled on upload for the B8G8R8A8 variants.
  static constexpr VkFormat kWant[] = {
      VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
  VkSurfaceFormatKHR chosen = fmts[0];
  bool found = false;
  for (VkFormat want : kWant) {
    for (const auto& f : fmts) {
      if (f.format == want) {
        chosen = f;
        found = true;
        break;
      }
    }
    if (found) break;
  }
  if (!found) return false;
  im->format = chosen.format;
  im->swizzle_rb = (chosen.format == VK_FORMAT_B8G8R8A8_UNORM ||
                    chosen.format == VK_FORMAT_B8G8R8A8_SRGB);

  uint32_t nm = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(im->phys, im->surf, &nm, nullptr);
  std::vector<VkPresentModeKHR> modes(nm);
  vkGetPhysicalDeviceSurfacePresentModesKHR(im->phys, im->surf, &nm,
                                           modes.data());
  VkPresentModeKHR pmode = VK_PRESENT_MODE_FIFO_KHR;  // vsync, guaranteed
  bool have_fifo = false;
  for (auto m : modes)
    if (m == VK_PRESENT_MODE_FIFO_KHR) have_fifo = true;
  if (!have_fifo) pmode = modes[0];

  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && count > caps.maxImageCount)
    count = caps.maxImageCount;

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = im->surf;
  sci.minImageCount = count;
  sci.imageFormat = im->format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = ex;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = pmode;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = im->swap;
  VkSwapchainKHR fresh = VK_NULL_HANDLE;
  if (vkCreateSwapchainKHR(im->dev, &sci, nullptr, &fresh) != VK_SUCCESS)
    return false;
  if (im->swap) vkDestroySwapchainKHR(im->dev, im->swap, nullptr);
  im->swap = fresh;

  uint32_t ni = 0;
  vkGetSwapchainImagesKHR(im->dev, im->swap, &ni, nullptr);
  im->images.resize(ni);
  vkGetSwapchainImagesKHR(im->dev, im->swap, &ni, im->images.data());
  return !im->images.empty();
}

bool make_staging(VulkanPresenter::Impl* im, VkDeviceSize size) {
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(im->dev, &bci, nullptr, &im->staging) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(im->dev, im->staging, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(im->phys, &mp);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (!(req.memoryTypeBits & (1u << i))) continue;
    const auto f = mp.memoryTypes[i].propertyFlags;
    if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      type = i;
      break;
    }
  }
  if (type == UINT32_MAX) return false;
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (vkAllocateMemory(im->dev, &ai, nullptr, &im->staging_mem) != VK_SUCCESS)
    return false;
  if (vkBindBufferMemory(im->dev, im->staging, im->staging_mem, 0) !=
      VK_SUCCESS)
    return false;
  im->staging_size = size;
  return true;
}

void transition(VkCommandBuffer cb, VkImage img, VkImageLayout old_l,
                VkImageLayout new_l, VkAccessFlags src, VkAccessFlags dst,
                VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = old_l;
  b.newLayout = new_l;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.layerCount = 1;
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// GLFW is process-wide: refcount so two presenters (or a retry after a
// failed init) cannot terminate each other's window.
int g_glfw_refs = 0;
bool glfw_acquire() {
  if (g_glfw_refs == 0 && !glfwInit()) return false;
  ++g_glfw_refs;
  return true;
}
void glfw_release() {
  if (g_glfw_refs > 0 && --g_glfw_refs == 0) glfwTerminate();
}

// Tear down a fully- or partially-constructed Impl: every handle is
// null-checked, so all init() failure paths funnel through here.
void teardown(VulkanPresenter::Impl* im) {
  if (!im) return;
  if (im->dev) vkDeviceWaitIdle(im->dev);
  if (im->dev && im->acquire_sem)
    vkDestroySemaphore(im->dev, im->acquire_sem, nullptr);
  if (im->dev && im->frame_fence)
    vkDestroyFence(im->dev, im->frame_fence, nullptr);
  if (im->dev && im->staging) vkDestroyBuffer(im->dev, im->staging, nullptr);
  if (im->dev && im->staging_mem)
    vkFreeMemory(im->dev, im->staging_mem, nullptr);
  if (im->dev && im->pool) vkDestroyCommandPool(im->dev, im->pool, nullptr);
  if (im->dev && im->swap) vkDestroySwapchainKHR(im->dev, im->swap, nullptr);
  if (im->dev) vkDestroyDevice(im->dev, nullptr);
  if (im->inst && im->surf) vkDestroySurfaceKHR(im->inst, im->surf, nullptr);
  if (im->inst) vkDestroyInstance(im->inst, nullptr);
  if (im->win) glfwDestroyWindow(im->win);
  if (im->glfw_inited) glfw_release();
  delete im;
}

}  // namespace

bool VulkanPresenter::glfw_enabled() const { return true; }

bool VulkanPresenter::init(uint32_t w, uint32_t h, const std::string& title,
                           bool visible) {
  shutdown();
  if (w == 0 || h == 0 || w > 1920 || h > 1080) return false;
  Impl* im = new Impl();
  im->extent = VkExtent2D{w, h};

  if (!glfw_acquire()) {
    delete im;
    return false;
  }
  im->glfw_inited = true;
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
  im->win = glfwCreateWindow(static_cast<int>(w), static_cast<int>(h),
                             title.c_str(), nullptr, nullptr);
  if (!im->win) {
    teardown(im);
    return false;
  }

  uint32_t next = 0;
  const char** req = glfwGetRequiredInstanceExtensions(&next);
  if (!req || next == 0) {
    teardown(im);
    return false;
  }
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = next;
  ici.ppEnabledExtensionNames = req;
  if (vkCreateInstance(&ici, nullptr, &im->inst) != VK_SUCCESS) {
    teardown(im);
    return false;
  }
  if (glfwCreateWindowSurface(im->inst, im->win, nullptr, &im->surf) !=
      VK_SUCCESS) {
    teardown(im);
    return false;
  }
  if (!pick_physical(im->inst, im->surf, &im->phys, &im->qfam)) {
    teardown(im);
    return false;
  }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = im->qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = &dext;
  if (vkCreateDevice(im->phys, &dci, nullptr, &im->dev) != VK_SUCCESS) {
    teardown(im);
    return false;
  }
  vkGetDeviceQueue(im->dev, im->qfam, 0, &im->queue);

  if (!make_swapchain(im)) {
    teardown(im);
    return false;
  }

  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = im->qfam;
  if (vkCreateCommandPool(im->dev, &pci, nullptr, &im->pool) != VK_SUCCESS) {
    teardown(im);
    return false;
  }
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = im->pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(im->dev, &cai, &im->cmd) != VK_SUCCESS) {
    teardown(im);
    return false;
  }
  const VkDeviceSize stage = static_cast<VkDeviceSize>(w) * h * 4;
  if (!make_staging(im, stage)) {
    teardown(im);
    return false;
  }
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(im->dev, &fi, nullptr, &im->frame_fence) != VK_SUCCESS) {
    teardown(im);
    return false;
  }
  VkSemaphoreCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (vkCreateSemaphore(im->dev, &si, nullptr, &im->acquire_sem) !=
      VK_SUCCESS) {
    teardown(im);
    return false;
  }

  impl_ = im;
  width_ = im->extent.width;
  height_ = im->extent.height;
  valid_ = true;
  return true;
}

void VulkanPresenter::shutdown() {
  Impl* im = impl_;
  impl_ = nullptr;
  valid_ = false;
  width_ = height_ = 0;
  teardown(im);
}

bool VulkanPresenter::poll() {
  if (!valid_ || !impl_ || !impl_->win) return false;
  glfwPollEvents();
  return !glfwWindowShouldClose(impl_->win);
}

bool VulkanPresenter::should_close() const {
  if (!valid_ || !impl_ || !impl_->win) return true;
  return glfwWindowShouldClose(impl_->win) != 0;
}

bool VulkanPresenter::upload_and_present(const uint8_t* pixels, uint32_t w,
                                         uint32_t h) {
  if (!valid_ || !impl_ || !pixels) {
    ++errors_;
    return false;
  }
  Impl* im = impl_;
  if (w != width_ || h != height_) {
    ++errors_;
    return false;
  }
  const size_t need = static_cast<size_t>(w) * h * 4;
  if (static_cast<VkDeviceSize>(need) > im->staging_size) {
    ++errors_;
    return false;
  }
  void* map = nullptr;
  if (vkMapMemory(im->dev, im->staging_mem, 0, need, 0, &map) != VK_SUCCESS) {
    ++errors_;
    return false;
  }
  if (!im->swizzle_rb) {
    std::memcpy(map, pixels, need);
  } else {
    const uint8_t* s = pixels;
    uint8_t* d = static_cast<uint8_t*>(map);
    for (size_t i = 0; i < need; i += 4) {
      d[i] = s[i + 2];
      d[i + 1] = s[i + 1];
      d[i + 2] = s[i];
      d[i + 3] = s[i + 3];
    }
  }
  vkUnmapMemory(im->dev, im->staging_mem);

  uint32_t idx = 0;
  VkResult acq = vkAcquireNextImageKHR(im->dev, im->swap, 1'000'000'000ULL,
                                       im->acquire_sem, VK_NULL_HANDLE, &idx);
  if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
    vkDeviceWaitIdle(im->dev);
    if (!make_swapchain(im)) {
      ++errors_;
      return false;
    }
    return false;  // benign resize: recreated, caller retries next frame
  }
  if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
    ++errors_;
    return false;
  }
  vkResetCommandBuffer(im->cmd, 0);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(im->cmd, &bi) != VK_SUCCESS) {
    ++errors_;
    return false;
  }
  transition(im->cmd, im->images[idx], VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {width_, height_, 1};
  vkCmdCopyBufferToImage(im->cmd, im->staging, im->images[idx],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  transition(im->cmd, im->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
             VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  if (vkEndCommandBuffer(im->cmd) != VK_SUCCESS) {
    ++errors_;
    return false;
  }
  vkResetFences(im->dev, 1, &im->frame_fence);
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &im->acquire_sem;
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &im->cmd;
  if (vkQueueSubmit(im->queue, 1, &si, im->frame_fence) != VK_SUCCESS) {
    ++errors_;
    return false;
  }
  if (vkWaitForFences(im->dev, 1, &im->frame_fence, VK_TRUE, 5'000'000'000ULL) !=
      VK_SUCCESS) {
    ++errors_;
    return false;
  }
  VkPresentInfoKHR pi{};
  pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  pi.swapchainCount = 1;
  pi.pSwapchains = &im->swap;
  pi.pImageIndices = &idx;
  VkResult pr = vkQueuePresentKHR(im->queue, &pi);
  if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
    vkDeviceWaitIdle(im->dev);
    make_swapchain(im);  // best effort; frame was still queued
    ++presents_;
    return true;
  }
  if (pr != VK_SUCCESS) {
    ++errors_;
    return false;
  }
  ++presents_;
  return true;
}

#else  // !DISPLAY_GLFW_ENABLED: dependency-free stub

bool VulkanPresenter::glfw_enabled() const { return false; }
bool VulkanPresenter::init(uint32_t, uint32_t, const std::string&, bool) {
  return false;
}
void VulkanPresenter::shutdown() {}
bool VulkanPresenter::poll() { return false; }
bool VulkanPresenter::should_close() const { return true; }
bool VulkanPresenter::upload_and_present(const uint8_t*, uint32_t, uint32_t) {
  ++errors_;
  return false;
}

#endif
