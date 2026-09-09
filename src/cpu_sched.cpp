#include "cpu_sched.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <utility>

// --- GuestThread: callable from inside the fiber body only. ---

void GuestThread::yield() { sched_->thread_yield(this); }

void GuestThread::sleep_ticks(uint64_t ticks) {
  sched_->thread_sleep(this, ticks);
}

void GuestThread::exit(int code) { sched_->thread_finish(this, code); }

bool GuestThread::join(uint64_t target) { return sched_->join(target); }

void GuestThread::trap(uint32_t syscall_id, uint64_t arg) {
  if (sched_->trap_) sched_->trap_(syscall_id, arg);
}

// --- GuestScheduler ---

GuestScheduler::GuestScheduler(TrapHandler trap) : trap_(std::move(trap)) {}

GuestScheduler::~GuestScheduler() {
  for (auto& t : threads_) {
    if (t->stack_base_) {
      munmap(t->stack_base_, t->stack_map_size_);
      t->stack_base_ = nullptr;
    }
  }
}

GuestThread* GuestScheduler::find(uint64_t handle) {
  if (handle == 0 || handle >= next_handle_) return nullptr;
  // Handles are 1-based indices; vector only grows, objects are
  // heap-stable behind unique_ptr.
  return threads_[static_cast<size_t>(handle - 1)].get();
}

uint64_t GuestScheduler::finished_count() const {
  uint64_t n = 0;
  for (const auto& t : threads_)
    if (t->state_ == GuestThread::State::Finished) ++n;
  return n;
}

uint64_t GuestScheduler::spawn(const std::string& name, GuestThreadFn fn) {
  return spawn_reserved(name, std::move(fn), 0);
}

uint64_t GuestScheduler::spawn_reserved(const std::string& name,
                                        GuestThreadFn fn,
                                        size_t stack_top_reserve) {
  // The usable stack must stay large enough to run a thread body; keep
  // at least 16 KiB usable no matter what a caller asks to reserve.
  constexpr size_t kMinUsable = 16u << 10;
  if (stack_top_reserve > kStackSize - kMinUsable) return 0;

  std::unique_ptr<GuestThread> t(new GuestThread());
  t->name_ = name;
  t->fn_ = std::move(fn);
  t->sched_ = this;
  t->handle_ = next_handle_++;
  t->stack_top_reserve_ = stack_top_reserve;

  // Stack layout: [guard 4 KiB PROT_NONE][256 KiB RW]. Stacks grow down
  // on x86_64, so overflow faults on the guard page instead of silently
  // corrupting neighbouring allocations. MAP_NORESERVE: commit pages on
  // touch, like a real thread stack.
  t->stack_map_size_ = kGuardSize + kStackSize;
  void* region = mmap(nullptr, t->stack_map_size_, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (region == MAP_FAILED) return 0;
  if (mprotect(region, kGuardSize, PROT_NONE) != 0) {
    munmap(region, t->stack_map_size_);
    return 0;
  }
  t->stack_base_ = region;
  threads_.push_back(std::move(t));
  return threads_.back()->handle_;
}

// Portable pointer pass-through for makecontext: its varargs are int
// (32-bit), so a 64-bit pointer is split into two halves -- the
// canonical technique; makecontext(3) documents int args only.
void GuestScheduler::trampoline(int lo, int hi) {
  const uintptr_t uptr =
      (static_cast<uintptr_t>(static_cast<uint32_t>(hi)) << 32) |
      static_cast<uint32_t>(lo);
  GuestScheduler* self = reinterpret_cast<GuestScheduler*>(uptr);
  GuestThread* t = self->current_;  // set right before switching in
  t->started_ = true;
  t->state_ = GuestThread::State::Running;
  if (t->fn_) t->fn_(*t);
  // Body returned without exit(): finalize like exit(0). Never returns.
  self->thread_finish(t, 0);
}

// (Re)build a fresh thread's entry context. Started threads resume from
// their own saved context instead -- no rearm.
void GuestScheduler::rearm(GuestThread* t) {
  auto* uc = &t->ctx_;
  getcontext(uc);
  uc->uc_stack.ss_sp = static_cast<char*>(t->stack_base_) + kGuardSize;
  // Reserve the top region: makecontext starts the fiber with its stack
  // pointer at ss_sp + ss_size, so shrinking ss_size makes the initial
  // (and all later) stack usage stop below the reserved bytes.
  uc->uc_stack.ss_size = kStackSize - t->stack_top_reserve_;
  uc->uc_link = &sched_ctx_;  // stray returns land back in scheduler
  const uintptr_t sptr = reinterpret_cast<uintptr_t>(this);
  makecontext(uc, reinterpret_cast<void (*)(void)>(&trampoline), 2,
              static_cast<int>(sptr & 0xffffffffu),
              static_cast<int>(sptr >> 32));
}

// One REAL machine-context switch: saves callee-saved registers, stack
// pointer, instruction pointer and signal mask of the current execution
// into t->ctx_, then activates the scheduler context. When the scheduler
// later re-dispatches this thread, execution RESUMES right here, so the
// call returns normally into the thread helper and the fiber body
// continues after its yield()/sleep_ticks()/join() call.
void GuestScheduler::switch_to_scheduler(GuestThread* t) {
  swapcontext(&t->ctx_, &sched_ctx_);
}

void GuestScheduler::thread_yield(GuestThread* t) {
  t->state_ = GuestThread::State::Ready;
  switch_to_scheduler(t);
}

void GuestScheduler::thread_sleep(GuestThread* t, uint64_t ticks) {
  if (ticks == 0) {
    thread_yield(t);
    return;
  }
  t->state_ = GuestThread::State::Sleeping;
  // Virtual time: wakes when tick_ reaches wake_tick_. Scheduler
  // advances tick_ by 1 per idle pass, so this is deterministic.
  t->wake_tick_ = tick_ + ticks;
  switch_to_scheduler(t);
}

bool GuestScheduler::block_current(uint64_t token, uint64_t timeout_ticks) {
  GuestThread* self = current_;
  if (!self) return false;  // host context: cannot block a host caller
  thread_block(self, token, timeout_ticks);
  // Resumed: the scheduler decided WHY we woke before switching back.
  return !self->wait_timed_out_;
}

void GuestScheduler::thread_block(GuestThread* t, uint64_t token,
                                  uint64_t timeout) {
  t->state_ = GuestThread::State::BlockedWait;
  t->wait_token_ = token;
  t->wait_timed_out_ = false;
  // kNoTimeout == 0 means "no deadline"; wake_tick_ is reused as the
  // absolute virtual tick of the deadline (UINT64_MAX when none).
  t->wake_tick_ = (timeout == kNoTimeout) ? UINT64_MAX : tick_ + timeout;
  switch_to_scheduler(t);
}

uint64_t GuestScheduler::signal(uint64_t token, bool all) {
  uint64_t woken = 0;
  for (auto& t : threads_) {
    if (t->state_ != GuestThread::State::BlockedWait) continue;
    if (t->wait_token_ != token) continue;
    t->state_ = GuestThread::State::Ready;
    t->wait_timed_out_ = false;
    ++woken;
    if (!all) break;  // FIFO: first spawned waiter wakes first
  }
  return woken;
}

void* GuestScheduler::stack_top_reserve(uint64_t handle) {
  GuestThread* t = find(handle);
  if (!t || t->stack_top_reserve_ == 0) return nullptr;
  // The reserved region sits at the very top of the stack mmap: above
  // the guard + usable stack, ending at stack_base_ + stack_map_size_.
  return static_cast<char*>(t->stack_base_) + t->stack_map_size_ -
         t->stack_top_reserve_;
}

[[noreturn]] void GuestScheduler::thread_finish(GuestThread* t, int code) {
  t->exit_code_ = code;
  t->state_ = GuestThread::State::Finished;
  // Run the finish hook while this thread is still switched in, so
  // observers see a consistent current_handle() (fiber_tls records the
  // TLS detach event here). Exceptions would unwind into the fiber
  // trampoline; hooks are expected not to throw.
  if (!finish_hooks_.empty()) {
    for (auto& hook : finish_hooks_) hook(*t);
  }
  // A Finished thread is never re-dispatched, so this loop never runs a
  // second iteration; it exists so control cannot fall off the end of a
  // [[noreturn]] path if that invariant were ever violated.
  for (;;) switch_to_scheduler(t);
}

GuestThread* GuestScheduler::pick_next() {
  // Unblock joins whose target finished.
  for (auto& t : threads_) {
    if (t->state_ != GuestThread::State::BlockedJoin) continue;
    GuestThread* tgt = find(t->join_target_);
    if (!tgt || tgt->state_ == GuestThread::State::Finished)
      t->state_ = GuestThread::State::Ready;
  }
  // Wake sleepers whose virtual deadline passed.
  for (auto& t : threads_) {
    if (t->state_ == GuestThread::State::Sleeping &&
        t->wake_tick_ <= tick_)
      t->state_ = GuestThread::State::Ready;
  }
  // Time out blocked waits whose virtual deadline passed. The scheduler
  // marks the wake reason BEFORE the thread runs again, so block_current
  // can report it.
  for (auto& t : threads_) {
    if (t->state_ == GuestThread::State::BlockedWait &&
        t->wake_tick_ <= tick_) {
      t->state_ = GuestThread::State::Ready;
      t->wait_timed_out_ = true;
    }
  }
  // Round-robin over Ready threads starting at rr_cursor_.
  const size_t n = threads_.size();
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (rr_cursor_ + i) % n;
    GuestThread* t = threads_[idx].get();
    if (t->state_ != GuestThread::State::Ready) continue;
    rr_cursor_ = (idx + 1) % n;
    return t;
  }
  return nullptr;
}

bool GuestScheduler::run_until_complete() {
  for (;;) {
    bool all_done = true;
    for (auto& t : threads_) {
      if (t->state_ != GuestThread::State::Finished) {
        all_done = false;
        break;
      }
    }
    if (all_done) return true;  // also covers zero threads

    GuestThread* t = pick_next();
    if (t) {
      if (!t->started_) rearm(t);
      current_ = t;
      ++t->switches_;
      ++switches_total_;
      // Real switch INTO the thread. When the thread yields, sleeps,
      // joins or finishes it swaps back to sched_ctx_ and control
      // resumes at the line right below.
      if (swapcontext(&sched_ctx_, &t->ctx_) != 0) {
        failed_ = true;  // per swapcontext(3): ENOMEM-class failure
        return false;
      }
      current_ = nullptr;
      continue;
    }

    // Nothing runnable: advance virtual time if anyone sleeps or has a
    // finite wait deadline (the deadline needs ticks to reach it).
    bool any_timed = false, any_blocked = false;
    for (auto& th : threads_) {
      if (th->state_ == GuestThread::State::Sleeping) any_timed = true;
      if (th->state_ == GuestThread::State::BlockedWait &&
          th->wake_tick_ != UINT64_MAX)
        any_timed = true;
      if (th->state_ == GuestThread::State::BlockedJoin ||
          (th->state_ == GuestThread::State::BlockedWait &&
           th->wake_tick_ == UINT64_MAX))
        any_blocked = true;
    }
    if (any_timed) {
      ++tick_;  // deterministic virtual clock, no wall clock
      continue;
    }
    if (any_blocked) {
      deadlocked_ = true;  // joins / infinite waits that can never fire
      return false;
    }
    return true;  // no ready, no sleeping, no blocked -> all finished
  }
}

bool GuestScheduler::join(uint64_t target) {
  GuestThread* self = current_;
  if (!self) return false;  // called from host, not inside a thread
  if (target == self->handle_) return false;  // self-join: deadlock
  GuestThread* tgt = find(target);
  if (!tgt) return false;
  if (tgt->state_ == GuestThread::State::Finished) return true;
  self->join_target_ = target;
  self->state_ = GuestThread::State::BlockedJoin;
  switch_to_scheduler(self);  // resumes after target finished
  return true;
}
