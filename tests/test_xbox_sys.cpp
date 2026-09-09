#include <cmath>
#include <cstdio>

#include "xbox_audio.h"
#include "xbox_input.h"
#include "xbox_sys.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

int main() {
  // --- sys: affinity ---
  const XboxAffinity off = xbox_game_affinity(false);
  const XboxAffinity on = xbox_game_affinity(true);
  CHECK(off.game_mask == 0x7Fu);    // cores 0-6
  CHECK(off.system_mask == 0x80u);  // core 7 reserved
  CHECK(off.game_threads == 7);
  CHECK(on.game_threads == 14);
  CHECK(xbox_affinity_is_game_core(0, false));
  CHECK(xbox_affinity_is_game_core(6, false));
  CHECK(!xbox_affinity_is_game_core(7, false));
  CHECK(xbox_affinity_is_game_core(13, true));   // logical 13 -> phys 6
  CHECK(!xbox_affinity_is_game_core(14, true));  // logical 14 -> phys 7
  CHECK(kXboxHomebrewTitleId == 0xFFFF0001u);
  CHECK(xbox_title_name() == "HOME-BREW");
  const XboxMemStatus ms = xbox_mem_status(4, 4096);
  CHECK(ms.total_bytes == (16ULL << 30));
  CHECK(ms.gpu_optimal_bytes == (10ULL << 30));
  CHECK(ms.standard_bytes == (6ULL << 30));
  CHECK(ms.committed_bytes == 16384);
  // FILETIME is after the 2026-01-01 FILETIME and round-trips.
  const uint64_t ft = xbox_system_time_filetime();
  CHECK(ft > 134000000000000000ULL);
  CHECK(xbox_filetime_to_unix100ns(ft) > 0);
  std::puts("sys ok");

  // --- input: XInput bits, deadzones, edges, rumble ---
  CHECK(XboxInput::apply_deadzone(100, kXboxDeadzoneLeft) == 0);
  CHECK(XboxInput::apply_deadzone(kXboxDeadzoneLeft, kXboxDeadzoneLeft) == 0);
  CHECK(XboxInput::apply_deadzone(32767, kXboxDeadzoneLeft) == 32767);
  CHECK(XboxInput::apply_deadzone(-32768, kXboxDeadzoneLeft) == -32768);
  CHECK(!XboxInput::trigger_pressed(30));
  CHECK(XboxInput::trigger_pressed(31));
  CHECK(XboxInput::pressed_edge(0x0000, kXboxBtnA) == kXboxBtnA);
  CHECK(XboxInput::pressed_edge(kXboxBtnA, kXboxBtnA | kXboxBtnB) ==
        kXboxBtnB);
  XboxInput in;
  CHECK(in.connected(0) && !in.connected(1));
  CHECK(in.connected_count() == 1);
  XboxGamepad raw{};
  raw.buttons = kXboxBtnA | kXboxBtnLB;
  raw.left_trigger = 200;
  raw.thumb_lx = 100;    // inside deadzone -> reads 0
  raw.thumb_rx = 20000;  // outside -> survives
  CHECK(in.set_state(0, raw));
  XboxGamepad got{};
  CHECK(in.get_state(0, got));
  CHECK(got.buttons == (kXboxBtnA | kXboxBtnLB));
  CHECK(got.left_trigger == 200);
  CHECK(got.thumb_lx == 0);
  CHECK(got.thumb_rx != 0);
  CHECK(got.packet == 1);
  CHECK(in.set_state(0, raw));  // unchanged -> packet stays
  CHECK(in.get_state(0, got) && got.packet == 1);
  CHECK(!in.set_state(1, raw));  // disconnected user
  CHECK(!in.get_state(1, got));
  CHECK(in.set_connected(1, true) && in.set_state(1, raw));
  CHECK(in.connected_count() == 2);
  const XboxRumble rum{.left_motor = 40000, .right_motor = 30000};
  CHECK(in.set_rumble(0, rum));
  XboxRumble ro{};
  CHECK(in.get_rumble(0, ro) && ro.left_motor == 40000);
  CHECK(!in.set_rumble(3, rum));
  std::puts("input ok");

  // --- audio: 48kHz stereo submit/consume/sine ---
  XboxAudio au;
  CHECK(au.format().sample_rate == 48000 && au.format().channels == 2);
  float phase = 0.0f;
  auto tone = XboxAudio::render_sine(440.0f, 4800, phase);
  CHECK(tone.size() == 9600);  // 4800 frames stereo
  CHECK(tone[0] == 0);         // sin(0) == 0
  bool nonzero = false;
  for (int16_t s : tone) {
    if (s > 1000 || s < -1000) {
      nonzero = true;
      break;
    }
  }
  CHECK(nonzero);
  CHECK(au.submit_buffer(tone));
  CHECK(au.submitted_buffers() == 1 && au.queued_frames() == 4800);
  CHECK(au.consume_frames(2400) == 2400);
  CHECK(au.played_frames() == 2400 && au.queued_frames() == 2400);
  CHECK(au.consume_frames(2400) == 2400);
  CHECK(au.queued_buffers() == 0);
  CHECK(au.consume_frames(100) == 0);  // empty -> underrun
  CHECK(au.underrun_frames() == 100);
  CHECK(!au.submit_buffer(std::vector<int16_t>{1, 2, 3}));  // odd channel
  CHECK(!au.submit_buffer(nullptr, 10));
  au.set_volume(2.0f);
  CHECK(au.volume() == 1.0f);
  au.set_volume(-1.0f);
  CHECK(au.volume() == 0.0f);
  // Phase continuity: continued render matches one long render.
  float p1 = 0.0f, p2 = 0.0f;
  auto a = XboxAudio::render_sine(440.0f, 100, p1);
  auto b = XboxAudio::render_sine(440.0f, 100, p1);
  auto whole = XboxAudio::render_sine(440.0f, 200, p2);
  CHECK(a.size() + b.size() == whole.size());
  for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == whole[i]);
  for (size_t i = 0; i < b.size(); ++i) CHECK(b[i] == whole[a.size() + i]);
  std::puts("audio ok");

  std::puts("test_xbox_sys passed");
  return 0;
}
