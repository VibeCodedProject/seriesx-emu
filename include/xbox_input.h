#pragma once
#include <cstdint>

// XInput HLE (homebrew only). Mirrors the real XINPUT_GAMEPAD layout so
// homebrew written against Xbox button bits just works: wButtons bitmask,
// bLeftTrigger/bRightTrigger 0..255, sThumbLX/LY/RX/RY int16 with Xbox
// deadzones applied on read.

enum XboxButton : uint16_t {
  kXboxBtnDpadUp = 0x0001,
  kXboxBtnDpadDown = 0x0002,
  kXboxBtnDpadLeft = 0x0004,
  kXboxBtnDpadRight = 0x0008,
  kXboxBtnStart = 0x0010,
  kXboxBtnBack = 0x0020,
  kXboxBtnLsb = 0x0040,  // left stick click
  kXboxBtnRsb = 0x0080,  // right stick click
  kXboxBtnLB = 0x0100,
  kXboxBtnRB = 0x0200,
  kXboxBtnA = 0x1000,
  kXboxBtnB = 0x2000,
  kXboxBtnX = 0x4000,
  kXboxBtnY = 0x8000,
};

// Official XInput deadzones.
constexpr int kXboxDeadzoneLeft = 7849;
constexpr int kXboxDeadzoneRight = 8689;
constexpr int kXboxTriggerThreshold = 30;

struct XboxGamepad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  uint32_t packet = 0;  // bumped on every state change
};

struct XboxRumble {
  uint16_t left_motor = 0;   // low-frequency 0..65535
  uint16_t right_motor = 0;  // high-frequency 0..65535
};

class XboxInput {
 public:
  static constexpr uint32_t kMaxUsers = 4;

  bool connected(uint32_t user) const;
  bool set_connected(uint32_t user, bool on);
  // Host injects raw hardware state; packet bumps iff state changed.
  bool set_state(uint32_t user, const XboxGamepad& raw);
  // Title reads processed state (deadzones applied to sticks).
  bool get_state(uint32_t user, XboxGamepad& out) const;
  bool set_rumble(uint32_t user, const XboxRumble& r);
  bool get_rumble(uint32_t user, XboxRumble& out) const;
  uint32_t connected_count() const;

  // Pure helpers (also used by tests).
  static int16_t apply_deadzone(int16_t v, int deadzone);
  static bool trigger_pressed(uint8_t v) { return v > kXboxTriggerThreshold; }
  // Buttons pressed in `cur` that were up in `prev` (rising edge).
  static uint16_t pressed_edge(uint16_t prev, uint16_t cur);

 private:
  bool connected_[kMaxUsers] = {true, false, false, false};
  XboxGamepad state_[kMaxUsers]{};
  XboxRumble rumble_[kMaxUsers]{};
};
