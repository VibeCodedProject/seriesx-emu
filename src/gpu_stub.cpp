#include "gpu_stub.h"

#include <sstream>

#include "cpu_native.h"
#include "mem_map.h"

void GpuStub::submit(const GpuPacket& pkt) {
  ++submitted_;
  std::ostringstream os;
  os << "gpu pkt#" << submitted_ << " type=" << pkt.type << " addr=0x" << std::hex
     << pkt.addr << " size=" << std::dec << pkt.size;
  last_log_ = os.str();
}

bool GpuStub::heap_ok(const XboxGpuAddr& a) const {
  if (!mem_) return false;
  if (a.size == 0 || !mem_->in_range(a.gpa, a.size)) return false;
  const bool fast = mem_->is_fast(a.gpa) &&
                    mem_->is_fast(a.gpa + a.size - 1);
  if (a.heap == XboxHeapClass::GpuOptimal) return fast;
  return !fast;  // Standard must sit fully in the slow pool
}

bool GpuStub::commit_ok(const XboxGpuAddr& a) const {
  if (!mem_) return false;
  return mem_->is_writable(a.gpa, a.size);
}

void GpuStub::gpu_complete(uint64_t value) {
  if (value > fence_completed_ && value <= fence_signalled_)
    fence_completed_ = value;
}

bool GpuStub::submit_xbox(const XboxPacket& pkt) {
  ++submitted_;
  std::ostringstream os;
  os << "xgpu pkt#" << submitted_ << " op=" << static_cast<uint32_t>(pkt.type);
  bool ok = false;

  switch (pkt.type) {
    case XboxPktType::Nop:
      ok = true;
      break;
    case XboxPktType::Flip:
      ++frame_id_;
      last_flip_qpc_ = cpu_qpc_now();
      ok = true;
      break;
    case XboxPktType::Timestamp:
      timestamps_.push_back(cpu_qpc_now());
      ok = true;
      break;
    case XboxPktType::FenceSignal:
      if (pkt.fence_value > fence_signalled_) {
        fence_signalled_ = pkt.fence_value;
        fence_completed_ = pkt.fence_value;  // HLE: immediate retire
        ok = true;
      } else {
        ++errors_;  // fence regression: Xbox requires monotonic values
      }
      break;
    case XboxPktType::FillMemory: {
      if (!heap_ok(pkt.dst) || !commit_ok(pkt.dst) || pkt.dst.size % 4 != 0) {
        ++errors_;
        break;
      }
      // Word-fill via 32-bit writes so per-page protection is enforced.
      bool good = true;
      for (uint64_t off = 0; off < pkt.dst.size; off += 4) {
        if (!mem_->write32(pkt.dst.gpa + off, pkt.pattern)) {
          good = false;
          break;
        }
      }
      if (good) {
        bytes_filled_ += pkt.dst.size;
        ok = true;
      } else {
        ++errors_;
      }
      break;
    }
    case XboxPktType::CopyMemory: {
      if (pkt.dst.size == 0 || pkt.dst.size != pkt.src.size ||
          !heap_ok(pkt.dst) || !heap_ok(pkt.src) || !commit_ok(pkt.dst) ||
          !mem_->is_readable(pkt.src.gpa, pkt.src.size)) {
        ++errors_;
        break;
      }
      // Overlap between src and dst is undefined on the real CP: reject.
      const uint64_t d_end = pkt.dst.gpa + pkt.dst.size;
      const uint64_t s_end = pkt.src.gpa + pkt.src.size;
      if (pkt.dst.gpa < s_end && pkt.src.gpa < d_end) {
        ++errors_;
        break;
      }
      std::vector<uint8_t> tmp(pkt.src.size);
      if (!mem_->read_bytes(pkt.src.gpa, tmp.data(), tmp.size()) ||
          !mem_->write_bytes(pkt.dst.gpa, tmp.data(), tmp.size())) {
        ++errors_;
        break;
      }
      bytes_copied_ += pkt.dst.size;
      ok = true;
      break;
    }
    case XboxPktType::ClearRenderTarget: {
      const uint64_t need = static_cast<uint64_t>(pkt.rt_width) *
                            static_cast<uint64_t>(pkt.rt_height) * 4ULL;
      if (pkt.rt_width == 0 || pkt.rt_height == 0 || need != pkt.dst.size ||
          !heap_ok(pkt.dst) || !commit_ok(pkt.dst)) {
        ++errors_;
        break;
      }
      const uint32_t px = static_cast<uint32_t>(pkt.color[0]) |
                          (static_cast<uint32_t>(pkt.color[1]) << 8) |
                          (static_cast<uint32_t>(pkt.color[2]) << 16) |
                          (static_cast<uint32_t>(pkt.color[3]) << 24);
      bool good = true;
      for (uint64_t off = 0; off < need; off += 4) {
        if (!mem_->write32(pkt.dst.gpa + off, px)) {
          good = false;
          break;
        }
      }
      if (good) {
        ++clears_;
        bytes_filled_ += need;
        ok = true;
      } else {
        ++errors_;
      }
      break;
    }
  }

  os << (ok ? " ok" : " ERR");
  if (pkt.type == XboxPktType::FenceSignal)
    os << " fence=" << pkt.fence_value;
  if (pkt.type == XboxPktType::Flip) os << " frame=" << frame_id_;
  last_log_ = os.str();
  return ok;
}
