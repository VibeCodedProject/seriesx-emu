#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mem_map.h"
#include "xbox_vfs.h"

// Asynchronous file-to-memory queue modelled on DirectStorage
// (microsoft/DirectStorage DeveloperGuidance.md; Xbox-flavoured:
// in-order processing). Semantics implemented here, all real:
//   - Fixed slot capacity (kCapacity). Every enqueued request occupies
//     one slot until it completes; enqueue() refuses when full and
//     enqueue_wait() blocks for a free slot ("Enqueue ... will block
//     until a slot becomes available").
//   - Requests do NOT start until submit() ("Requests do not start
//     processing until they are submitted").
//   - Auto-submit when the pending backlog reaches half capacity
//     ("Requests are also automatically submitted when the queue is
//     half full").
//   - submit() wakes the persistent worker thread (condvar, no
//     polling). The worker is created in the constructor and joined in
//     the destructor.
//   - In-order processing/completion (Xbox DirectStorage model; the PC
//     API completes out of order -- documented difference).
//   - wait_tag()/wait_all() block on a condvar until requests reach a
//     terminal state -- real blocking, no spin/poll.
//   - cancel() is an emulator-side extra: real DirectStorage has no
//     request cancellation. It is best-effort and only affects
//     requests that have not started executing yet.
//
// Reads go through Vfs::read_slice (bounded in-file slices), landing in
// committed MemMap pages via write_bytes. Vfs is internally locked, so
// files may be created/updated while the worker runs.
class IoQueue {
 public:
  enum class Status : uint8_t { Pending, InFlight, Done, Cancelled, Error };

  // Slots in the request queue (DirectStorage queues are created with
  // a fixed capacity; 8 mirrors the small GPU-adjacent queues).
  static constexpr size_t kCapacity = 8;
  // Auto-submit threshold: pending >= capacity/2.
  static constexpr size_t kAutoSubmit = kCapacity / 2;

  IoQueue(MemMap* mem, Vfs* vfs);
  ~IoQueue();

  IoQueue(const IoQueue&) = delete;
  IoQueue& operator=(const IoQueue&) = delete;

  // Queue a read of [offset, offset+size) from vfs path into guest
  // dest. Non-blocking: returns false if all slots are in use, the tag
  // is duplicate, the path/range is invalid, or the destination is
  // out of range/uncommitted. Auto-submits at half capacity.
  bool enqueue(const std::string& vfs_path, uint64_t src_offset, size_t size,
               uint64_t dest_gpa, uint64_t tag);
  // Blocking variant matching DirectStorage enqueue semantics: waits
  // until a slot frees, then enqueues. Returns false on invalid args
  // (never blocks for those).
  bool enqueue_wait(const std::string& vfs_path, uint64_t src_offset,
                    size_t size, uint64_t dest_gpa, uint64_t tag);

  // Best-effort cancel: only requests that have not started executing
  // can be cancelled (Done/Error/InFlight cannot).
  bool cancel(uint64_t tag);

  // Hand the pending backlog to the worker ("Submitting will wake up
  // the worker thread"). Idempotent: false when already submitted and
  // nothing new is pending.
  bool submit();

  // Auto-submit is a DirectStorage convenience ("Requests are also
  // automatically submitted when the queue is half full"). Disabling it
  // gives strict manual mode: only explicit submit() starts work (the
  // primary DirectStorage flow). Default: enabled.
  void set_auto_submit_enabled(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    auto_submit_ = on;
  }

  // Block until `tag` reaches a terminal state (Done/Cancelled/Error).
  // Returns that status. Unknown tags return Error immediately. Note:
  // the request must eventually be submitted (submit() or auto-submit)
  // or cancelled, otherwise this blocks forever -- the same contract as
  // waiting on a DirectStorage status entry for a batch never flushed.
  Status wait_tag(uint64_t tag);
  // Block until every SUBMITTED request reaches a terminal state (and
  // the worker is idle). An enqueued-but-never-submitted backlog does
  // not block wait_all -- call submit() first if you want it drained.
  void wait_all();

  Status status(uint64_t tag) const;  // non-blocking snapshot
  size_t pending() const;             // not yet started
  size_t in_flight() const;           // executing on the worker
  size_t completed() const;           // Done
  size_t slots_in_use() const;        // pending + in_flight

 private:
  struct Req {
    std::string path;
    uint64_t src_offset = 0;
    size_t size = 0;
    uint64_t dest = 0;
    uint64_t tag = 0;
    Status status = Status::Pending;
  };

  bool enqueue_common(const std::string& vfs_path, uint64_t src_offset,
                      size_t size, uint64_t dest_gpa, uint64_t tag,
                      bool wait_for_slot);
  void worker_main();
  size_t pending_locked() const;  // requires mu_ held

  MemMap* mem_ = nullptr;
  Vfs* vfs_ = nullptr;

  mutable std::mutex mu_;
  std::condition_variable cv_work_;  // wakes the worker
  std::condition_variable cv_done_;  // wakes wait_tag/wait_all/enqueue_wait
  std::deque<Req> reqs_;             // FIFO; records kept for status()
  size_t in_flight_ = 0;
  bool submitted_ = false;
  bool auto_submit_ = true;
  bool stopping_ = false;
  std::thread worker_;
};
