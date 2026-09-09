#pragma once
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

#include "gpu_stub.h"
#include "vulkan_alloc.h"
#include "vulkan_buffer.h"

// Xbox D3D12-root-signature HLE batch over Vulkan (homebrew only).
// Root params map to: Constants32 -> push-constant range,
// Cbv/Table -> UNIFORM_BUFFER descriptor bindings in one set.
// Xbox layer: buffers are registered by GPU VA (= guest GPA) and bound
// by address, with fast/slow heap-class + MemMap commit validation, so a
// title cannot bind memory outside the 10GB GPU-optimal / 6GB standard
// split. Each submit signals a monotonic Xbox fence value and logs one
// packet to GpuStub. No shaders/pipelines yet: batch records real copy
// commands (the copy path titles use for uploads) and fence-submits.
enum class RootParamType : uint8_t { Constants32 = 0, Cbv = 1, Table = 2 };

struct RootParam {
  RootParamType type = RootParamType::Constants32;
  uint32_t count = 0;  // Constants32: #u32 values; Cbv: 1; Table: #descriptors
};

class GpuBatch {
 public:
  GpuBatch() = default;
  ~GpuBatch();

  GpuBatch(const GpuBatch&) = delete;
  GpuBatch& operator=(const GpuBatch&) = delete;

  bool init(VulkanDev& dev, const std::vector<RootParam>& params);
  void shutdown();

  bool valid() const { return valid_; }
  bool recording() const { return cmd_ != VK_NULL_HANDLE; }
  uint64_t submits() const { return submits_; }
  uint32_t push_size() const { return push_size_; }

  bool begin();
  // Bind a buffer to a Cbv/Table root index (descriptor validated).
  bool bind(uint32_t root_index, VulkanBuffer& buf);
  // Push u32 constants to a Constants32 root index.
  bool push(uint32_t root_index, const uint32_t* vals, uint32_t count);
  // Record a buffer copy inside the open batch (sizes must match).
  bool copy(VulkanBuffer& dst, VulkanBuffer& src);
  // End, fence-submit to the device queue, log one packet. Returns false
  // if not recording.
  bool submit(GpuStub* log = nullptr);

  // --- Xbox VA layer ---
  // Attach guest memory for address validation (not owned).
  void attach_mem(class MemMap* mem) { mem_ = mem; }
  // Register a Vulkan backing buffer at Xbox GPU VA `gpa`.
  // GPA range must be committed; GPU-optimal bindings must be fast-pool.
  bool register_buffer(uint64_t gpa, VulkanBuffer& buf);
  void unregister_buffer(uint64_t gpa);
  // Bind/copy by GPU VA (looks up the registration, then delegates to
  // bind()/copy() so the Vulkan path stays real).
  bool bind_gpa(uint32_t root_index, uint64_t gpa);
  bool copy_gpa(uint64_t dst_gpa, uint64_t src_gpa, uint32_t size);
  // Submit with an explicit monotonic Xbox fence value.
  bool submit_fence(GpuStub* log, uint64_t fence_value);
  uint64_t fence_signalled() const { return fence_signalled_; }

 private:
  VulkanDev* dev_ = nullptr;
  class MemMap* mem_ = nullptr;
  struct Binding {
    uint64_t gpa = 0;
    VulkanBuffer* buf = nullptr;
  };
  std::vector<Binding> bindings_;
  Binding* find_binding(uint64_t gpa);
  std::vector<RootParam> params_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  std::vector<uint32_t> push_data_;
  uint32_t push_size_ = 0;
  uint32_t uniform_bindings_ = 0;
  uint64_t submits_ = 0;
  uint64_t fence_signalled_ = 0;
  bool valid_ = false;
};
