#include "gpu_batch.h"

#include "mem_map.h"

GpuBatch::~GpuBatch() { shutdown(); }

bool GpuBatch::init(VulkanDev& dev, const std::vector<RootParam>& params) {
  shutdown();
  if (!dev.valid || params.empty() || params.size() > 16) return false;

  uint32_t push_count = 0;
  uint32_t bindings = 0;
  for (const auto& p : params) {
    if (p.type == RootParamType::Constants32) {
      if (p.count == 0 || p.count > 32) return false;
      push_count += p.count;
    } else {
      if (p.count == 0 || p.count > 16) return false;
      bindings += p.count;
    }
  }
  if (push_count > 32 || bindings > 16) return false;

  dev_ = &dev;
  params_ = params;

  // One descriptor set: one UNIFORM_BUFFER binding per Cbv/Table descriptor.
  std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
  layout_bindings.reserve(bindings);
  uint32_t binding = 0;
  for (const auto& p : params) {
    if (p.type == RootParamType::Constants32) continue;
    for (uint32_t i = 0; i < p.count; ++i) {
      VkDescriptorSetLayoutBinding b{};
      b.binding = binding++;
      b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      b.descriptorCount = 1;
      b.stageFlags = VK_SHADER_STAGE_ALL;
      layout_bindings.push_back(b);
    }
  }
  if (!layout_bindings.empty()) {
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(layout_bindings.size());
    li.pBindings = layout_bindings.data();
    if (vkCreateDescriptorSetLayout(dev_->dev, &li, nullptr, &set_layout_) !=
        VK_SUCCESS) {
      shutdown();
      return false;
    }
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_size.descriptorCount = bindings;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(dev_->dev, &pi, nullptr, &pool_) !=
        VK_SUCCESS) {
      shutdown();
      return false;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(dev_->dev, &ai, &set_) != VK_SUCCESS) {
      shutdown();
      return false;
    }
  }

  // Pipeline layout: set (if any) + push-constant range for Constants32.
  VkPipelineLayoutCreateInfo pli{};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPushConstantRange range{};
  if (push_count > 0) {
    range.stageFlags = VK_SHADER_STAGE_ALL;
    range.size = push_count * 4;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
  }
  if (set_layout_ != VK_NULL_HANDLE) {
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
  }
  if (vkCreatePipelineLayout(dev_->dev, &pli, nullptr, &pipeline_layout_) !=
      VK_SUCCESS) {
    shutdown();
    return false;
  }

  push_data_.assign(push_count, 0);
  push_size_ = push_count;
  uniform_bindings_ = bindings;
  valid_ = true;
  return true;
}

void GpuBatch::shutdown() {
  if (dev_ && dev_->dev) {
    if (cmd_ != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(dev_->dev, dev_->cmd_pool, 1, &cmd_);
      cmd_ = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(dev_->dev, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
    }
    if (set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(dev_->dev, set_layout_, nullptr);
      set_layout_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(dev_->dev, pipeline_layout_, nullptr);
      pipeline_layout_ = VK_NULL_HANDLE;
    }
  } else {
    cmd_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
  }
  set_ = VK_NULL_HANDLE;
  dev_ = nullptr;
  mem_ = nullptr;
  bindings_.clear();
  params_.clear();
  push_data_.clear();
  push_size_ = 0;
  uniform_bindings_ = 0;
  fence_signalled_ = 0;
  valid_ = false;
}

GpuBatch::Binding* GpuBatch::find_binding(uint64_t gpa) {
  for (auto& b : bindings_) {
    if (b.gpa == gpa) return &b;
  }
  return nullptr;
}

bool GpuBatch::register_buffer(uint64_t gpa, VulkanBuffer& buf) {
  if (!buf.valid || buf.size == 0) return false;
  if (buf.size > UINT32_MAX) return false;
  if (mem_) {
    const auto sz = static_cast<size_t>(buf.size);
    if (!mem_->in_range(gpa, sz) || !mem_->is_committed(gpa, sz))
      return false;
    // Xbox rule: GPU-optimal (fast VRAM) bindings must live in fast pool.
    if (buf.fast && !mem_->is_fast(gpa)) return false;
  }
  if (find_binding(gpa)) return false;
  bindings_.push_back(Binding{.gpa = gpa, .buf = &buf});
  return true;
}

void GpuBatch::unregister_buffer(uint64_t gpa) {
  for (size_t i = 0; i < bindings_.size(); ++i) {
    if (bindings_[i].gpa == gpa) {
      bindings_.erase(bindings_.begin() + static_cast<ptrdiff_t>(i));
      return;
    }
  }
}

bool GpuBatch::bind_gpa(uint32_t root_index, uint64_t gpa) {
  Binding* b = find_binding(gpa);
  if (!b || !b->buf) return false;
  return bind(root_index, *b->buf);
}

bool GpuBatch::copy_gpa(uint64_t dst_gpa, uint64_t src_gpa, uint32_t size) {
  if (size == 0) return false;
  Binding* d = find_binding(dst_gpa);
  Binding* s = find_binding(src_gpa);
  if (!d || !s || !d->buf || !s->buf) return false;
  if (d->buf->size != s->buf->size) return false;
  if (static_cast<uint64_t>(size) != d->buf->size) return false;
  return copy(*d->buf, *s->buf);
}

bool GpuBatch::begin() {
  if (!valid_ || cmd_ != VK_NULL_HANDLE) return false;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = dev_->cmd_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(dev_->dev, &ai, &cmd_) != VK_SUCCESS) {
    cmd_ = VK_NULL_HANDLE;
    return false;
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev_->dev, dev_->cmd_pool, 1, &cmd_);
    cmd_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool GpuBatch::bind(uint32_t root_index, VulkanBuffer& buf) {
  if (!valid_ || root_index >= params_.size() || !buf.valid) return false;
  const auto& p = params_[root_index];
  if (p.type == RootParamType::Constants32) return false;
  if (set_ == VK_NULL_HANDLE) return false;
  // Root -> binding: count bindings of preceding non-constant params.
  uint32_t binding = 0;
  for (uint32_t i = 0; i < root_index; ++i) {
    if (params_[i].type != RootParamType::Constants32)
      binding += params_[i].count;
  }
  if (binding >= uniform_bindings_) return false;
  VkDescriptorBufferInfo info{};
  info.buffer = buf.buf;
  info.offset = 0;
  info.range = buf.size;
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = set_;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w.pBufferInfo = &info;
  vkUpdateDescriptorSets(dev_->dev, 1, &w, 0, nullptr);
  return true;
}

bool GpuBatch::push(uint32_t root_index, const uint32_t* vals,
                    uint32_t count) {
  if (!valid_ || root_index >= params_.size() || !vals) return false;
  const auto& p = params_[root_index];
  if (p.type != RootParamType::Constants32 || count != p.count) return false;
  // Offset = preceding Constants32 counts.
  uint32_t offset = 0;
  for (uint32_t i = 0; i < root_index; ++i) {
    if (params_[i].type == RootParamType::Constants32)
      offset += params_[i].count;
  }
  if (offset + count > push_data_.size()) return false;
  for (uint32_t i = 0; i < count; ++i) push_data_[offset + i] = vals[i];
  return true;
}

bool GpuBatch::copy(VulkanBuffer& dst, VulkanBuffer& src) {
  if (!valid_ || cmd_ == VK_NULL_HANDLE) return false;
  if (!dst.valid || !src.valid || dst.size != src.size) return false;
  VkBufferCopy region{};
  region.size = src.size;
  vkCmdCopyBuffer(cmd_, src.buf, dst.buf, 1, &region);
  return true;
}

bool GpuBatch::submit_fence(GpuStub* log, uint64_t fence_value) {
  if (fence_value <= fence_signalled_) return false;  // Xbox: monotonic
  if (!valid_ || cmd_ == VK_NULL_HANDLE) return false;
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  bool ok = false;
  if (vkCreateFence(dev_->dev, &fi, nullptr, &fence) == VK_SUCCESS) {
    if (vkQueueSubmit(dev_->queue, 1, &si, fence) == VK_SUCCESS) {
      ok = vkWaitForFences(dev_->dev, 1, &fence, VK_TRUE, 5'000'000'000ull) ==
           VK_SUCCESS;
    }
    vkDestroyFence(dev_->dev, fence, nullptr);
  }
  vkFreeCommandBuffers(dev_->dev, dev_->cmd_pool, 1, &cmd_);
  cmd_ = VK_NULL_HANDLE;
  if (ok) {
    ++submits_;
    fence_signalled_ = fence_value;
    if (log)
      log->submit(GpuPacket{.type = 10,
                            .addr = push_size_,
                            .size = static_cast<uint32_t>(submits_)});
  }
  return ok;
}

bool GpuBatch::submit(GpuStub* log) {
  return submit_fence(log, fence_signalled_ + 1);
}
