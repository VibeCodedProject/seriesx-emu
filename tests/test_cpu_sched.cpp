#include <cstdint>
#include <cstdio>
#include <vector>

#include "cpu_sched.h"
#include "mem_map.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

// CHECK variant for use inside void lambdas (guest thread bodies): a
// failed check records the failure and leaves the thread.
static int g_fail = 0;
#define CHECK_T(cond)                                                 \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      g_fail = 1;                                                     \
      return;                                                         \
    }                                                                 \
  } while (0)

// Every assertion here is deterministic: scheduling uses virtual ticks,
// never wall-clock. Switch counts are exact because each dispatch into
// a thread counts once and each yield()/sleep()/join()/finish()
// performs exactly one real swapcontext.

static int test_ping_pong() {
  GuestScheduler sched;
  std::vector<int> order;
  uint64_t a = sched.spawn("A", [&](GuestThread& t) {
    for (int i = 0; i < 8; ++i) {
      order.push_back(0);
      if (i < 7) t.yield();  // no trailing yield: finishes on 8th dispatch
    }
  });
  uint64_t b = sched.spawn("B", [&](GuestThread& t) {
    for (int i = 0; i < 8; ++i) {
      order.push_back(1);
      if (i < 7) t.yield();
    }
  });
  CHECK(sched.run_until_complete());
  CHECK(order.size() == 16);
  for (size_t i = 0; i < order.size(); ++i) {
    // Strict A,B alternation proves control really bounced between two
    // independent stacks on every yield.
    CHECK(order[i] == static_cast<int>(i % 2));
  }
  CHECK(sched.switches_total() == 16);  // 8 dispatches per thread
  CHECK(sched.find(a)->switches() == 8);
  CHECK(sched.find(b)->switches() == 8);
  CHECK(sched.finished_count() == 2);
  CHECK(sched.find(a)->exit_code() == 0);
  CHECK(sched.find(b)->exit_code() == 0);
  std::puts("  ping_pong ok (16 real switches, strict alternation)");
  return 0;
}

// Deep recursion across a switch: 100 frames x 512 B of seeded stack
// data must survive the round trip intact.
static bool deep_ok = true;
static void deep_recurse(GuestThread& t, int depth) {
  uint8_t buf[512];
  for (size_t i = 0; i < sizeof(buf); ++i)
    buf[i] = static_cast<uint8_t>((depth * 7 + i) & 0xFF);
  if (depth < 100) {
    deep_recurse(t, depth + 1);
  } else {
    t.yield();  // ONE real context switch with ~50 KB live on the stack
  }
  for (size_t i = 0; i < sizeof(buf); ++i) {
    if (buf[i] != static_cast<uint8_t>((depth * 7 + i) & 0xFF)) {
      deep_ok = false;  // stack corruption -> context switch is fake
      return;
    }
  }
}

static int test_deep_stack() {
  GuestScheduler sched;
  sched.spawn("deep", [](GuestThread& t) { deep_recurse(t, 1); });
  CHECK(sched.run_until_complete());
  CHECK(deep_ok);
  CHECK(sched.switches_total() == 2);  // initial dispatch + the yield
  std::puts("  deep_stack ok (100 frames survive a real switch)");
  return 0;
}

static int test_sleep_order() {
  GuestScheduler sched;
  std::vector<int> order;
  uint64_t a = sched.spawn("A", [&](GuestThread& t) {
    t.sleep_ticks(5);
    order.push_back(0);
  });
  uint64_t b = sched.spawn("B", [&](GuestThread& t) {
    t.sleep_ticks(2);
    order.push_back(1);
  });
  uint64_t c = sched.spawn("C", [&](GuestThread&) { order.push_back(2); });
  (void)a;
  (void)b;
  (void)c;
  CHECK(sched.run_until_complete());
  // C never sleeps; B wakes after 2 virtual ticks; A after 5.
  CHECK(order.size() == 3);
  CHECK(order[0] == 2 && order[1] == 1 && order[2] == 0);
  CHECK(sched.current_tick() == 5);   // virtual clock advanced exactly
  CHECK(sched.switches_total() == 5);  // C:1, A:2, B:2 dispatches
  CHECK(sched.find(a)->state() == GuestThread::State::Finished);
  std::puts("  sleep_order ok (virtual ticks deterministic)");
  return 0;
}

static int test_join_exit_code() {
  GuestScheduler sched;
  std::vector<int> order;
  uint64_t worker = 0, joiner = 0, starter = 0;
  starter = sched.spawn("starter", [&](GuestThread&) {
    order.push_back(0);
  });
  worker = sched.spawn("worker", [&](GuestThread& t) {
    t.yield();
    t.exit(42);
  });
  joiner = sched.spawn("joiner", [&](GuestThread& t) {
    CHECK_T(t.handle() == joiner);
    // Block until worker is done: ordering is forced, not incidental.
    CHECK_T(t.join(worker));
    order.push_back(1);
  });
  CHECK(sched.run_until_complete());
  CHECK(g_fail == 0);
  CHECK(order.size() == 2);
  CHECK(order[0] == 0);  // starter ran before the joiner was released
  CHECK(order[1] == 1);
  CHECK(sched.find(starter)->exit_code() == 0);
  CHECK(sched.find(worker)->exit_code() == 42);
  CHECK(sched.find(joiner)->exit_code() == 0);
  // Host-side join is invalid (only guest threads can block).
  CHECK(!sched.join(worker));
  CHECK(!sched.join(0));
  CHECK(!sched.join(9999));
  std::puts("  join/exit_code ok");
  return 0;
}

static int test_deadlock_detected() {
  GuestScheduler sched;
  uint64_t a = 0, b = 0;
  a = sched.spawn("A", [&](GuestThread& t) { t.join(b); });
  b = sched.spawn("B", [&](GuestThread& t) { t.join(a); });
  CHECK(!sched.run_until_complete());
  CHECK(sched.deadlocked());
  std::puts("  deadlock ok (detected, run reports false)");
  return 0;
}

static int test_many_threads_mem_traps() {
  constexpr int kN = 25;
  MemMap mem;
  CHECK(mem.reserve());
  // One committed page per thread in the slow pool.
  for (int i = 0; i < kN; ++i) {
    CHECK(mem.commit_page(MemMap::kSlowBase +
                          static_cast<uint64_t>(i) * 4096));
  }
  std::vector<int> trap_log;
  uint64_t handles[kN];
  GuestScheduler sched([&](uint32_t id, uint64_t arg) {
    if (id == static_cast<uint32_t>(SysId::Log))
      trap_log.push_back(static_cast<int>(arg));
  });
  for (int i = 0; i < kN; ++i) {
    handles[i] = sched.spawn(
        "t" + std::to_string(i), [&](GuestThread& t) {
          t.trap(static_cast<uint32_t>(SysId::Log), t.handle());
          const int idx = static_cast<int>(t.handle() - 1);
          CHECK_T(mem.write32(MemMap::kSlowBase +
                                  static_cast<uint64_t>(idx) * 4096,
                              0xC0DE0000u + static_cast<uint32_t>(idx)));
          for (int y = 0; y <= idx % 5; ++y) t.yield();
        });
  }
  CHECK(sched.thread_count() == kN);
  CHECK(sched.run_until_complete());
  CHECK(g_fail == 0);
  CHECK(trap_log.size() == static_cast<size_t>(kN));
  CHECK(sched.finished_count() == kN);
  CHECK(sched.switches_total() >= kN);
  for (int i = 0; i < kN; ++i) {
    uint32_t v = 0;
    CHECK(mem.read32(MemMap::kSlowBase + static_cast<uint64_t>(i) * 4096, v));
    CHECK(v == 0xC0DE0000u + static_cast<uint32_t>(i));
    CHECK(sched.find(handles[i])->state() == GuestThread::State::Finished);
  }
  std::puts("  many_threads ok (25 fibers, traps + MemMap intact)");
  return 0;
}

static int test_nested_spawn() {
  GuestScheduler sched;
  std::vector<int> order;
  uint64_t child_handle = 0;
  uint64_t parent = sched.spawn("parent", [&](GuestThread& t) {
    order.push_back(0);
    t.yield();  // let any other ready thread go first
    // Spawn a child from inside a running fiber; the scheduler must
    // pick it up within the same run.
    child_handle = sched.spawn("child", [](GuestThread& c) {
      c.yield();
      c.exit(7);
    });
    order.push_back(2);
  });
  (void)parent;
  CHECK(sched.run_until_complete());
  CHECK(child_handle != 0);
  GuestThread* ch = sched.find(child_handle);
  CHECK(ch && ch->state() == GuestThread::State::Finished);
  CHECK(ch->exit_code() == 7);
  CHECK(order.size() == 2 && order[0] == 0 && order[1] == 2);
  std::puts("  nested_spawn ok");
  return 0;
}

int main() {
  CHECK(test_ping_pong() == 0);
  CHECK(test_deep_stack() == 0);
  CHECK(test_sleep_order() == 0);
  CHECK(test_join_exit_code() == 0);
  CHECK(test_deadlock_detected() == 0);
  CHECK(test_many_threads_mem_traps() == 0);
  CHECK(test_nested_spawn() == 0);
  std::puts("test_cpu_sched passed");
  return 0;
}
