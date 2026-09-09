#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <ucontext.h>
#include <vector>

#include "cpu_native.h"  // SysId, TrapHandler

// Real user-space context switching for guest threads, implemented with
// POSIX ucontext (getcontext/makecontext/swapcontext), the stackful
// coroutine primitive behind many emulator schedulers. Every switch is
// a genuine machine-context save/restore performed by glibc:
// callee-saved registers, stack pointer, instruction pointer and the
// signal mask are stored into / restored from a ucontext_t. Nothing
// here simulates a switch -- tests/test_cpu_sched.cpp proves real
// interleaving, deep-stack integrity across switches and exact switch
// counts.
//
// Model (cooperative scheduler, as used by emulator guest threads):
//   - All guest threads run on ONE host thread; the scheduler picks the
//     next Ready thread round-robin. A thread keeps the CPU until it
//     yields, sleeps, blocks on join, or exits (no preemption timer --
//     cooperative is an HLE choice, documented honestly).
//   - Time is VIRTUAL and deterministic: a "tick" advances only when no
//     thread is Ready and at least one is Sleeping. No wall-clock reads
//     in the scheduling path, so tests are reproducible.
//   - Guest threads issue traps (SysId) through the shared handler,
//     same wire format as cpu_native.h.
//
// Honest limitations (do not remove):
//   1. C++ thread_local storage is per HOST thread, not per fiber: all
//      guest threads share the scheduler host thread's thread_local
//      storage. That is a property of ucontext switching. GUEST-visible
//      TLS (the Xbox TLS model) is solved separately and per-fiber in
//      fiber_tls.h, which reserves per-fiber storage on each fiber's
//      own stack -- do not confuse the two.
//   2. glibc swapcontext does not save/restore the FPU/SSE control
//      state (x87 control word, MXCSR). Threads must not change FP
//      rounding/exception modes.
//   3. Cooperative only: a thread that never yields monopolizes the
//      CPU (the standard trade-off of stackful schedulers).
//   4. Linux-only (matches the rest of this project: sys/mman.h ...).
class GuestScheduler;

using GuestThreadFn = std::function<void(class GuestThread&)>;

class GuestThread {
 public:
  enum class State : uint8_t {
    Ready = 0,
    Running = 1,
    Sleeping = 2,
    BlockedJoin = 3,
    Finished = 4,
    BlockedWait = 5,  // blocked on a scheduler signal token (see below)
  };

  // --- calls valid only from inside the thread's own body ---
  // Voluntary context switch: exactly one swapcontext to the scheduler.
  void yield();
  // Sleep `ticks` virtual ticks (deterministic; no wall clock), then
  // become Ready again.
  void sleep_ticks(uint64_t ticks);
  // Terminate this thread now (never returns). Code is readable via
  // exit_code() afterwards.
  [[noreturn]] void exit(int code);
  // Block the calling thread until `target` finishes. Returns false
  // without blocking on self-join or invalid target.
  bool join(uint64_t target);
  // Issue a trap to the host handler (SysId::Log, GpuFlush, ...).
  void trap(uint32_t syscall_id, uint64_t arg);

  // --- wait-channel state (used by GuestScheduler::block_current) ---
  uint64_t wait_token() const { return wait_token_; }
  // Result of the last blocked wait: true = woken by signal(), false =
  // woken by the virtual-time timeout. Only meaningful right after a
  // block_current() call returned.
  bool wait_timed_out() const { return wait_timed_out_; }

  // --- observability (safe from anywhere) ---
  uint64_t handle() const { return handle_; }   // 1-based; 0 invalid
  const std::string& name() const { return name_; }
  State state() const { return state_; }
  uint64_t switches() const { return switches_; }  // times switched INTO
  uint64_t wake_tick() const { return wake_tick_; }
  int exit_code() const { return exit_code_; }

 private:
  friend class GuestScheduler;
  GuestThread() = default;
  GuestThread(const GuestThread&) = delete;
  GuestThread& operator=(const GuestThread&) = delete;

  std::string name_;
  GuestThreadFn fn_;
  GuestScheduler* sched_ = nullptr;  // owner (set at spawn)
  ucontext_t ctx_{};                 // machine context (the real thing)
  bool started_ = false;             // has an entry context built
  uint64_t handle_ = 0;
  uint64_t switches_ = 0;
  State state_ = State::Ready;
  uint64_t wake_tick_ = 0;
  uint64_t join_target_ = 0;         // handle waited on when BlockedJoin
  uint64_t wait_token_ = 0;          // wait channel when BlockedWait
  bool wait_timed_out_ = false;      // wake reason of the last block
  int exit_code_ = 0;
  void* stack_base_ = nullptr;       // mmap region start (incl. guard)
  size_t stack_map_size_ = 0;
  size_t stack_top_reserve_ = 0;     // bytes at the top off-limits to this thread
};

class GuestScheduler {
 public:
  // trap: optional host handler invoked for guest traps.
  explicit GuestScheduler(TrapHandler trap = nullptr);
  ~GuestScheduler();

  GuestScheduler(const GuestScheduler&) = delete;
  GuestScheduler& operator=(const GuestScheduler&) = delete;

  // Create a guest thread with its own mmap'd stack + guard page.
  // Returns the thread handle, or 0 on allocation failure. Callable
  // from the host and from inside running guest threads.
  uint64_t spawn(const std::string& name, GuestThreadFn fn);

  // Same, but reserves `stack_top_reserve` bytes at the TOP of the
  // thread's stack region that the thread can never touch (its entry
  // stack pointer starts below the reserve). Used by fiber_tls to place
  // per-fiber TLS storage on the fiber's own stack, the way Xapi's
  // thread startup carves TLS data out of the thread stack on real
  // hardware. Returns 0 if the reserve is too large.
  uint64_t spawn_reserved(const std::string& name, GuestThreadFn fn,
                          size_t stack_top_reserve);

  // Block the CALLING guest thread until `target` finishes. Returns
  // false without blocking when called from the host, on self-join or
  // on an invalid target. Deadlocks are reported by run_until_complete.
  bool join(uint64_t target);

  // Real blocking wait on an opaque token (a wait channel). The calling
  // thread stops being scheduled until ANOTHER thread (or the host)
  // calls signal(token), or until `timeout_ticks` virtual ticks pass
  // (timeout_ticks == kNoTimeout waits forever). This is the primitive
  // Xbox kernel waits (KeWaitForSingleObject on events, critical
  // section contention, ...) are built on in xbox_krnl. Returns true
  // when woken by signal(), false when the timeout expired. Must be
  // called from inside a running fiber; returns false immediately
  // (without switching) when called from the host.
  bool block_current(uint64_t token, uint64_t timeout_ticks = kNoTimeout);

  // Wake waiters on `token` (FIFO by spawn order): one, or all with
  // all=true. Returns the number of threads woken. Safe from host and
  // from inside fibers. Threads whose timeout already expired are NOT
  // woken (they wake via the timeout path instead).
  uint64_t signal(uint64_t token, bool all = false);

  // block_current timeout sentinel: wait forever.
  static constexpr uint64_t kNoTimeout = 0;

  // Run until every thread has finished. Returns false on deadlock
  // (threads blocked on joins that can never be satisfied) or on a
  // swapcontext failure (failed() == true).
  bool run_until_complete();

  // --- observability ---
  uint64_t switches_total() const { return switches_total_; }
  uint64_t thread_count() const { return threads_.size(); }
  uint64_t finished_count() const;
  bool deadlocked() const { return deadlocked_; }
  bool failed() const { return failed_; }
  uint64_t current_tick() const { return tick_; }
  GuestThread* find(uint64_t handle);  // nullptr if invalid

  // Handle of the thread currently switched in, or 0 when called from
  // the host outside any thread. This is the anchor for per-fiber
  // (guest) TLS: it identifies which fiber's storage is "current".
  uint64_t current_handle() const { return current_ ? current_->handle_ : 0; }

  // Host pointer to the reserved stack-top region created by
  // spawn_reserved(), or nullptr for plain spawns / invalid handles.
  // The region stays untouched by the thread for its whole lifetime.
  void* stack_top_reserve(uint64_t handle);

  // Called from thread_finish() while the finishing thread is still
  // switched in, BEFORE it swaps away for the last time. Lets observers
  // (fiber_tls detach events, kernel mutant abandonment) run with
  // current_handle() still valid. set_on_finish() keeps the classic
  // single-observer semantics (clears prior hooks); add_finish_hook()
  // chains an additional observer without disturbing existing ones.
  // Hooks must be installed before run_until_complete(); they must not
  // throw, and they must not re-enter the scheduler.
  void set_on_finish(std::function<void(GuestThread&)> hook) {
    finish_hooks_.clear();
    if (hook) finish_hooks_.push_back(std::move(hook));
  }
  void add_finish_hook(std::function<void(GuestThread&)> hook) {
    if (hook) finish_hooks_.push_back(std::move(hook));
  }

  // Stack config (per thread): 4 KiB PROT_NONE guard below the usable
  // 256 KiB region, so stack overflow faults instead of corrupting.
  static constexpr size_t kStackSize = 256u << 10;
  static constexpr size_t kGuardSize = 4096;

 private:
  friend class GuestThread;
  // Thread-side helpers: set state, then one real switch to scheduler.
  void thread_yield(GuestThread* t);
  void thread_sleep(GuestThread* t, uint64_t ticks);
  [[noreturn]] void thread_finish(GuestThread* t, int code);
  void switch_to_scheduler(GuestThread* t);
  void thread_block(GuestThread* t, uint64_t token, uint64_t timeout);

  // Scheduler-side: unblock joins, wake sleepers, pick next Ready.
  GuestThread* pick_next();
  void rearm(GuestThread* t);  // build fresh entry context
  static void trampoline(int lo, int hi);

  TrapHandler trap_;
  std::vector<std::function<void(GuestThread&)>> finish_hooks_;
  std::vector<std::unique_ptr<GuestThread>> threads_;
  ucontext_t sched_ctx_{};   // scheduler/host context
  GuestThread* current_ = nullptr;  // thread being switched into
  uint64_t next_handle_ = 1;
  uint64_t tick_ = 0;        // virtual ticks
  uint64_t rr_cursor_ = 0;   // round-robin fairness cursor
  uint64_t switches_total_ = 0;
  bool deadlocked_ = false;
  bool failed_ = false;
};
