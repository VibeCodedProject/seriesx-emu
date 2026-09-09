#include <cstdio>
#include <vector>

#include "io_queue.h"
#include "mem_map.h"
#include "xbox_vfs.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

int main() {
  // --- MemMap protect ---
  MemMap m;
  CHECK(m.reserve());
  CHECK(m.commit_range(0x10000, 8192));
  CHECK(m.write32(0x10000, 0xABCDu));
  CHECK(m.protect(0x10000, 4096, MemProt::ReadOnly));
  CHECK(!m.is_writable(0x10000));
  CHECK(m.is_readable(0x10000));
  CHECK(!m.write32(0x10000, 1u));  // RO write fails, no fault
  uint32_t v = 0;
  CHECK(m.read32(0x10000, v) && v == 0xABCDu);
  CHECK(m.protect(0x10000, 4096, MemProt::ReadWrite));
  CHECK(m.write32(0x10000, 2u));
  CHECK(m.protect(0x10000, 4096, MemProt::NoAccess));
  CHECK(!m.is_readable(0x10000) && !m.is_writable(0x10000));
  CHECK(!m.read32(0x10000, v));
  CHECK(m.translate(0x10000) == nullptr);
  CHECK(!m.protect(0x40000, 4096, MemProt::ReadOnly));  // uncommitted
  CHECK(m.protect(0x10000, 4096, MemProt::ReadWrite));  // restore
  std::puts("protect ok");

  // --- MemMap alias ---
  CHECK(m.commit_range(0x20000, 4096));
  CHECK(m.commit_range(0x30000, 4096));
  CHECK(m.write32(0x20000, 0x1234u));
  CHECK(m.alias_map(0x30000, 0x20000, 4096));
  CHECK(m.alias_count() == 1);
  CHECK(m.read32(0x30000, v) && v == 0x1234u);  // dst mirrors src
  CHECK(m.write32(0x30000, 0x5678u));
  CHECK(m.read32(0x20000, v) && v == 0x5678u);  // write via alias
  CHECK(!m.alias_map(0x30000, 0x20000, 4096));  // dst overlap rejected
  CHECK(!m.alias_map(0x20000, 0x20000, 4096));  // self overlap rejected
  CHECK(m.alias_unmap(0x30000));
  CHECK(m.alias_count() == 0);
  CHECK(!m.alias_unmap(0x30000));
  std::puts("alias ok");

  // --- VFS ---
  Vfs fs;
  CHECK(fs.title().id == 0xFFFF0001u);
  CHECK(Vfs::valid_path("S:\\hello.bin"));
  CHECK(!Vfs::valid_path("C:\\evil.bin"));
  CHECK(!Vfs::valid_path("S:\\..\\evil"));
  CHECK(!Vfs::valid_path("S:"));
  std::vector<uint8_t> hello = {'h', 'i', 0, static_cast<uint8_t>(0xFF)};
  CHECK(fs.create_file("S:\\hello.bin", hello));
  CHECK(!fs.create_file("S:\\hello.bin", hello));  // duplicate
  CHECK(fs.exists("S:\\hello.bin"));
  std::vector<uint8_t> back;
  CHECK(fs.read_file("S:\\hello.bin", back) && back == hello);
  CHECK(!fs.read_file("S:\\missing.bin", back));
  CHECK(fs.create_file("T:\\empty.bin", std::vector<uint8_t>{}));
  size_t sz = 99;
  CHECK(fs.file_size("T:\\empty.bin", sz) && sz == 0);
  CHECK(!fs.create_file("X:\\bad.bin", hello));
  // read_slice: bounded in-file reads, EOF is a hard failure.
  std::vector<uint8_t> slice;
  CHECK((fs.read_slice("S:\\hello.bin", 1, 2, slice) &&
         slice == std::vector<uint8_t>{'i', 0}));
  CHECK(!fs.read_slice("S:\\hello.bin", 3, 2, slice));  // past EOF
  CHECK(!fs.read_slice("S:\\hello.bin", 0, 0, slice));  // empty
  CHECK(!fs.read_slice("S:\\missing.bin", 0, 1, slice));
  std::puts("vfs ok");

  // --- IoQueue: real async worker (DirectStorage semantics) ---
  const uint64_t kDest = MemMap::kSlowBase + 0x50000;
  std::vector<uint8_t> payload(4096);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)(i & 0xFF);
  CHECK(fs.write_file("S:\\payload.bin", payload));
  CHECK(m.commit_range(kDest, 4096));
  {
    IoQueue io(&m, &fs);
    io.set_auto_submit_enabled(false);  // strict manual mode for this run
    CHECK(io.enqueue("S:\\payload.bin", 0, 1024, kDest, 1));
    CHECK(io.enqueue("S:\\payload.bin", 1024, 1024, kDest + 1024, 2));
    CHECK(io.enqueue("S:\\payload.bin", 2048, 1024, kDest + 2048, 3));
    CHECK(!io.enqueue("S:\\payload.bin", 0, 16, kDest, 1));  // dup tag
    CHECK(!io.enqueue("S:\\payload.bin", 0, 16, MemMap::kTotalSize, 9));  // OOB
    CHECK(!io.enqueue("S:\\payload.bin", 0, 4097, kDest, 10));  // uncommitted
    CHECK(io.cancel(2));  // pre-start cancel: best-effort, works
    CHECK(io.status(2) == IoQueue::Status::Cancelled);
    CHECK(io.submit());
    // Post-submit enqueue is legal (real DirectStorage keeps accepting
    // requests); it must still complete once the worker reaches it.
    CHECK(io.enqueue("S:\\payload.bin", 0, 16, kDest + 4080, 5));
    io.submit();  // flush the new request too (return value is timing
                  // dependent: gate may still be open -- do not assert)
    io.wait_all();
    CHECK(io.status(1) == IoQueue::Status::Done);
    CHECK(io.status(2) == IoQueue::Status::Cancelled);
    CHECK(io.status(3) == IoQueue::Status::Done);
    CHECK(io.status(5) == IoQueue::Status::Done);
    CHECK(io.completed() == 3);
    CHECK(io.slots_in_use() == 0);  // terminal requests free their slots
    // Tag 1/3/5 landed, tag 2 (cancelled) left its slice untouched.
    std::vector<uint8_t> got(4096, 0);
    CHECK(m.read_bytes(kDest, got.data(), 1024));
    for (size_t i = 0; i < 1024; ++i) CHECK(got[i] == payload[i]);
    CHECK(m.read_bytes(kDest + 2048, got.data(), 1024));
    for (size_t i = 0; i < 1024; ++i) CHECK(got[i] == payload[2048 + i]);
    CHECK(m.read_bytes(kDest + 4080, got.data(), 16));
    for (size_t i = 0; i < 16; ++i) CHECK(got[i] == payload[i]);
    std::vector<uint8_t> hole(1024, 0);
    CHECK(m.read_bytes(kDest + 1024, hole.data(), 1024));
    for (size_t i = 0; i < 1024; ++i) CHECK(hole[i] == 0);
    // Submit with an empty backlog is a no-op (false), deterministic
    // here because wait_all() drained everything above.
    CHECK(!io.submit());
  }
  {
    // Errors surface per request; wait_tag returns the terminal state.
    IoQueue io2(&m, &fs);
    io2.set_auto_submit_enabled(false);
    CHECK(io2.enqueue("S:\\missing.bin", 0, 16, kDest, 7));
    CHECK(io2.submit());
    CHECK(io2.wait_tag(7) == IoQueue::Status::Error);
    // The submit gate has re-armed (wait_tag returned), so this submit
    // is a fresh flush: deterministic true.
    CHECK(io2.enqueue("S:\\payload.bin", 4000, 200, kDest, 8));  // past EOF
    CHECK(io2.submit());
    CHECK(io2.wait_tag(8) == IoQueue::Status::Error);
    // Unknown tag: immediate terminal report.
    CHECK(io2.wait_tag(9999) == IoQueue::Status::Error);
    // Cancel after completion must fail (best-effort only).
    CHECK(!io2.cancel(7));
  }
  {
    // Auto-submit: at half capacity (4 of 8 slots) the backlog starts
    // processing with NO explicit submit() call.
    IoQueue ioa(&m, &fs);
    for (uint64_t t = 0; t < IoQueue::kAutoSubmit; ++t) {
      CHECK(ioa.enqueue("S:\\payload.bin", 0, 64, kDest, 300 + t));
    }
    ioa.wait_all();  // must not hang: auto-submit already started work
    CHECK(ioa.completed() == IoQueue::kAutoSubmit);
  }
  {
    // Slot capacity: 8 slots; non-blocking enqueue refuses #9 and
    // blocking enqueue_wait unblocks as the worker drains a slot.
    IoQueue io3(&m, &fs);
    io3.set_auto_submit_enabled(false);
    for (uint64_t t = 0; t < IoQueue::kCapacity; ++t) {
      CHECK(io3.enqueue("S:\\payload.bin", 0, 64, kDest, 100 + t));
    }
    CHECK(io3.slots_in_use() == IoQueue::kCapacity);  // full, not draining
    CHECK(!io3.enqueue("S:\\payload.bin", 0, 64, kDest, 200));  // full
    CHECK(io3.submit());
    // 9th request: blocks until a slot frees, then must succeed.
    CHECK(io3.enqueue_wait("S:\\payload.bin", 0, 64, kDest, 200));
    // The backlog that submit() covered may already be drained, so the
    // new request needs its own flush (DirectStorage: requests do not
    // start until submitted). Gate may still be open -- do not assert.
    io3.submit();
    io3.wait_all();
    CHECK(io3.completed() == IoQueue::kCapacity + 1);
  }
  std::puts("io_queue ok");

  std::puts("test_vfs_io passed");
  return 0;
}
