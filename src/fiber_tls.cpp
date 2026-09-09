#include "fiber_tls.h"

#include <cstring>
#include <memory>
#include <utility>

FiberTls::FiberTls(GuestScheduler& sched) : sched_(sched) {}

void FiberTls::set_init(Init init) {
  if (init.index >= kSlots) return;  // reject invalid index, keep old init
  init_ = std::make_unique<Init>(std::move(init));
}

uint64_t FiberTls::spawn_with_tls(const std::string& name, GuestThreadFn fn) {
  if (!init_) return 0;  // no TLS payload configured

  // Allocation layout carved from the fiber stack top:
  //   [void* slots[kSlots]] [template copy][zero fill]
  // rounded to 16 bytes so both the array and the data stay aligned.
  const size_t array_bytes = kSlots * sizeof(void*);
  const size_t data_bytes = init_->raw.size() + init_->zero_fill;
  const size_t reserve = (array_bytes + data_bytes + 15) & ~size_t(15);

  // Wire the scheduler finish hook once: TLS detach events must be
  // recorded while the finishing fiber is still current.
  if (!hook_installed_) {
    FiberTls* self = this;
    sched_.set_on_finish([self](GuestThread& t) {
      self->record(t.handle(), 0, kReasonThreadDetach);
    });
    hook_installed_ = true;
  }

  const uint64_t fiber =
      sched_.spawn_reserved(name, std::move(fn), reserve);
  if (fiber == 0) return 0;

  const uint32_t reason =
      main_seen_ ? kReasonThreadAttach : kReasonProcessAttach;
  install(fiber);
  main_seen_ = true;
  // Attach record: for each callback VA, one notification, in table
  // order -- the order a CPU would dispatch them. When the XBE has no
  // callback table, a single zero-callback record still documents the
  // notification the runtime would raise.
  if (init_->callbacks.empty()) {
    record(fiber, 0, reason);
  } else {
    for (uint32_t cb : init_->callbacks) record(fiber, cb, reason);
  }
  return fiber;
}

bool FiberTls::install(uint64_t fiber) {
  void* top = sched_.stack_top_reserve(fiber);
  if (!top) return false;

  const size_t array_bytes = kSlots * sizeof(void*);
  Block b;
  b.array = top;
  b.data = static_cast<uint8_t*>(top) + array_bytes;
  b.size = data_size();

  // Array: all slots null, then slot[index] -> data area. This makes
  // step 3 of the guest access sequence work: array[index] = block.
  void** slots = static_cast<void**>(b.array);
  for (size_t i = 0; i < kSlots; ++i) slots[i] = nullptr;
  slots[init_->index] = b.data;

  // Template copy + zero fill (per-fiber block content).
  uint8_t* d = static_cast<uint8_t*>(b.data);
  if (!init_->raw.empty()) std::memcpy(d, init_->raw.data(), init_->raw.size());
  if (init_->zero_fill)
    std::memset(d + init_->raw.size(), 0, init_->zero_fill);

  blocks_.emplace_back(fiber, b);
  return true;
}

void FiberTls::record(uint64_t fiber, uint32_t callback, uint32_t reason) {
  events_.push_back(Event{fiber, callback, reason});
}

void* FiberTls::data_of(uint64_t fiber, size_t offset, size_t size) {
  for (auto& [h, b] : blocks_) {
    if (h != fiber) continue;
    if (offset + size > b.size) return nullptr;
    return static_cast<uint8_t*>(b.data) + offset;
  }
  return nullptr;
}

const void* FiberTls::data_of(uint64_t fiber, size_t offset, size_t size) const {
  return const_cast<FiberTls*>(this)->data_of(fiber, offset, size);
}

void* FiberTls::current_data(size_t offset, size_t size) {
  return data_of(sched_.current_handle(), offset, size);
}

const void* FiberTls::current_data(size_t offset, size_t size) const {
  return data_of(sched_.current_handle(), offset, size);
}

void* FiberTls::array_of(uint64_t fiber) {
  for (auto& [h, b] : blocks_)
    if (h == fiber) return b.array;
  return nullptr;
}

void* FiberTls::current_array() { return array_of(sched_.current_handle()); }

bool FiberTls::has_tls(uint64_t fiber) const {
  for (auto& [h, b] : blocks_)
    if (h == fiber) return true;
  return false;
}
