#include "xbox_input.h"

#include <cmath>
#include <cstdint>

int16_t XboxInput::apply_deadzone(int16_t v, int deadzone) {
  const int mag = v < 0 ? -static_cast<int>(v) : static_cast<int>(v);
  if (mag < deadzone) return 0;
  // Rescale [deadzone, 32767] -> [0, 32767] preserving sign (XInput style).
  const int scaled = (mag - deadzone) * 32767 / (32767 - deadzone);
  const int out = v < 0 ? -scaled : scaled;
  if (out > 32767) return 32767;
  if (out < -32768) return -32768;
  return static_cast<int16_t>(out);
}

uint16_t XboxInput::pressed_edge(uint16_t prev, uint16_t cur) {
  return static_cast<uint16_t>(cur & ~prev);
}

bool XboxInput::connected(uint32_t user) const {
  if (user >= kMaxUsers) return false;
  return connected_[user];
}

bool XboxInput::set_connected(uint32_t user, bool on) {
  if (user >= kMaxUsers) return false;
  connected_[user] = on;
  if (!on) {
    state_[user] = XboxGamepad{};
    rumble_[user] = XboxRumble{};
  }
  return true;
}

bool XboxInput::set_state(uint32_t user, const XboxGamepad& raw) {
  if (user >= kMaxUsers || !connected_[user]) return false;
  XboxGamepad& cur = state_[user];
  const bool changed =
      cur.buttons != raw.buttons || cur.left_trigger != raw.left_trigger ||
      cur.right_trigger != raw.right_trigger || cur.thumb_lx != raw.thumb_lx ||
      cur.thumb_ly != raw.thumb_ly || cur.thumb_rx != raw.thumb_rx ||
      cur.thumb_ry != raw.thumb_ry;
  const uint32_t keep = cur.packet;
  cur = raw;
  cur.packet = changed ? keep + 1 : keep;
  return true;
}

bool XboxInput::get_state(uint32_t user, XboxGamepad& out) const {
  if (user >= kMaxUsers || !connected_[user]) return false;
  out = state_[user];
  out.thumb_lx = apply_deadzone(out.thumb_lx, kXboxDeadzoneLeft);
  out.thumb_ly = apply_deadzone(out.thumb_ly, kXboxDeadzoneLeft);
  out.thumb_rx = apply_deadzone(out.thumb_rx, kXboxDeadzoneRight);
  out.thumb_ry = apply_deadzone(out.thumb_ry, kXboxDeadzoneRight);
  return true;
}

bool XboxInput::set_rumble(uint32_t user, const XboxRumble& r) {
  if (user >= kMaxUsers || !connected_[user]) return false;
  rumble_[user] = r;
  return true;
}

bool XboxInput::get_rumble(uint32_t user, XboxRumble& out) const {
  if (user >= kMaxUsers || !connected_[user]) return false;
  out = rumble_[user];
  return true;
}

uint32_t XboxInput::connected_count() const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < kMaxUsers; ++i)
    if (connected_[i]) ++n;
  return n;
}
