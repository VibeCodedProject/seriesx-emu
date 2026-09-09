#include "io_queue.h"

IoQueue::IoQueue(MemMap* mem, Vfs* vfs) : mem_(mem), vfs_(vfs) {
  // Persistent worker: created once, woken by submit()/auto-submit,
  // joined in the destructor. This is what makes the queue genuinely
  // asynchronous: completions progress while the caller keeps working.
  worker_ = std::thread(&IoQueue::worker_main, this);
}

IoQueue::~IoQueue() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  cv_work_.notify_all();
  if (worker_.joinable()) worker_.join();
}

// --- locked helpers (require mu_ held) ---

size_t IoQueue::pending_locked() const {
  size_t n = 0;
  for (const auto& r : reqs_) {
    if (r.status == Status::Pending) ++n;
  }
  return n;
}

// --- public API ---

bool IoQueue::enqueue_common(const std::string& vfs_path, uint64_t src_offset,
                             size_t size, uint64_t dest_gpa, uint64_t tag,
                             bool wait_for_slot) {
  if (size == 0 || !mem_ || !vfs_) return false;
  if (!Vfs::valid_path(vfs_path)) return false;
  if (!mem_->in_range(dest_gpa, size)) return false;
  // Destination must be committed already: the worker never commits on
  // behalf of the title (matches the fixed reservation model).
  if (!mem_->is_committed(dest_gpa, size)) return false;

  std::unique_lock<std::mutex> lock(mu_);
  for (const auto& r : reqs_) {
    if (r.tag == tag) return false;  // tags must be unique, ever
  }
  if (wait_for_slot) {
    // DirectStorage: "If Enqueue is called when there are no free
    // slots then it will block until a slot becomes available."
    cv_done_.wait(lock, [&] {
      return pending_locked() + in_flight_ < kCapacity;
    });
  } else if (pending_locked() + in_flight_ >= kCapacity) {
    return false;  // non-blocking variant refuses instead
  }

  reqs_.push_back(Req{.path = vfs_path,
                      .src_offset = src_offset,
                      .size = size,
                      .dest = dest_gpa,
                      .tag = tag,
                      .status = Status::Pending});

  // Auto-submit at half capacity (DirectStorage behaviour) so a full
  // backlog cannot sit unprocessed waiting for an explicit submit().
  if (auto_submit_ && pending_locked() >= kAutoSubmit && !submitted_) {
    submitted_ = true;
    cv_work_.notify_one();
  }
  return true;
}

bool IoQueue::enqueue(const std::string& vfs_path, uint64_t src_offset,
                      size_t size, uint64_t dest_gpa, uint64_t tag) {
  return enqueue_common(vfs_path, src_offset, size, dest_gpa, tag, false);
}

bool IoQueue::enqueue_wait(const std::string& vfs_path, uint64_t src_offset,
                           size_t size, uint64_t dest_gpa, uint64_t tag) {
  return enqueue_common(vfs_path, src_offset, size, dest_gpa, tag, true);
}

bool IoQueue::cancel(uint64_t tag) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& r : reqs_) {
    if (r.tag == tag && r.status == Status::Pending) {
      r.status = Status::Cancelled;
      cv_done_.notify_all();  // release waiters on this tag
      cv_work_.notify_all();  // let the worker re-arm an empty gate
      return true;
    }
  }
  return false;  // unknown, already executing, or terminal
}

bool IoQueue::submit() {
  std::lock_guard<std::mutex> lock(mu_);
  if (submitted_) return false;  // idempotent; worker already allowed
  if (pending_locked() == 0) return false;
  submitted_ = true;
  cv_work_.notify_one();  // "Submitting will wake up the worker thread"
  return true;
}

IoQueue::Status IoQueue::wait_tag(uint64_t tag) {
  std::unique_lock<std::mutex> lock(mu_);
  cv_done_.wait(lock, [&] {
    for (const auto& r : reqs_) {
      if (r.tag == tag) {
        return r.status == Status::Done || r.status == Status::Cancelled ||
               r.status == Status::Error;
      }
    }
    return true;  // unknown tag: report immediately
  });
  for (const auto& r : reqs_) {
    if (r.tag == tag) return r.status;
  }
  return Status::Error;
}

void IoQueue::wait_all() {
  // Waits for all SUBMITTED work to reach a terminal state. A request
  // that was enqueued but never covered by submit()/auto-submit is not
  // the worker's business (DirectStorage: "Requests do not start
  // processing until they are submitted"), so it does not block here.
  std::unique_lock<std::mutex> lock(mu_);
  cv_done_.wait(lock, [&] {
    if (in_flight_ > 0) return false;
    if (submitted_) return pending_locked() == 0;  // drain the open gate
    return true;  // gate closed: unsubmitted backlog is not waited on
  });
}

IoQueue::Status IoQueue::status(uint64_t tag) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& r : reqs_) {
    if (r.tag == tag) return r.status;
  }
  return Status::Error;
}

size_t IoQueue::pending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return pending_locked();
}

size_t IoQueue::in_flight() const {
  std::lock_guard<std::mutex> lock(mu_);
  return in_flight_;
}

size_t IoQueue::completed() const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t n = 0;
  for (const auto& r : reqs_) {
    if (r.status == Status::Done) ++n;
  }
  return n;
}

size_t IoQueue::slots_in_use() const {
  std::lock_guard<std::mutex> lock(mu_);
  return pending_locked() + in_flight_;
}

void IoQueue::worker_main() {
  std::unique_lock<std::mutex> lock(mu_);
  for (;;) {
    // Sleep until the submit gate opens or shutdown. Requests do not
    // start processing until they are submitted (DirectStorage rule).
    cv_work_.wait(lock, [&] { return stopping_ || submitted_; });
    if (stopping_) return;

    // In-order (FIFO) processing, one request at a time -- the Xbox
    // DirectStorage completion model. Drains the backlog that existed
    // for this submit; new requests wait for the next submit gate.
    Req r;
    bool found = false;
    for (auto& req : reqs_) {
      if (req.status == Status::Pending) {
        req.status = Status::InFlight;
        r = req;
        found = true;
        break;
      }
    }
    if (!found) {
      submitted_ = false;  // backlog drained: re-arm the submit gate
      continue;
    }
    ++in_flight_;

    // Perform the I/O WITHOUT holding the queue lock so enqueues,
    // cancels, status queries and waiters keep making progress (like a
    // real storage worker). Vfs is internally locked; the MemMap write
    // touches only this request's destination range.
    lock.unlock();
    std::vector<uint8_t> data;
    bool ok = vfs_->read_slice(r.path, r.src_offset, r.size, data);
    if (ok) ok = mem_->write_bytes(r.dest, data.data(), data.size());
    lock.lock();

    // Locate by unique tag (deque push_back while unlocked keeps
    // element addresses stable, but the tag lookup is belt-and-braces).
    for (auto& req : reqs_) {
      if (req.tag == r.tag && req.status == Status::InFlight) {
        req.status = ok ? Status::Done : Status::Error;
        break;
      }
    }
    --in_flight_;
    // Re-arm the submit gate when this submit's backlog is exhausted.
    // Done BEFORE notifying waiters, so a thread returning from
    // wait_tag() can rely on the gate state it observes.
    if (pending_locked() == 0) submitted_ = false;
    cv_done_.notify_all();  // wake wait_tag/wait_all/enqueue_wait
  }
}
