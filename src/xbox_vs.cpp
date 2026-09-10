#include "xbox_vs.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Extract `len` bits starting at `bit` from dword `dw`.
uint32_t field(const uint32_t* w, int dw, int bit, int len) {
  return (w[dw] >> bit) & ((1u << len) - 1u);
}

// Per-opcode source usage, matching the MAC unit table (A/B/C).
bool mac_uses_a(VsMacOp op) {
  switch (op) {
    case VsMacOp::Nop:
    case VsMacOp::Add:
      return false;
    default:
      return true;
  }
}

bool mac_uses_b(VsMacOp op) {
  switch (op) {
    case VsMacOp::Mov:
    case VsMacOp::Add:
    case VsMacOp::Arl:
      return false;
    default:
      return true;
  }
}

bool mac_uses_c(VsMacOp op) {
  switch (op) {
    case VsMacOp::Add:
    case VsMacOp::Mad:
      return true;
    default:
      return false;
  }
}

bool valid_output_index(uint32_t idx) {
  switch (idx) {
    case 0:   // oPos
    case 3:   // oD0
    case 4:   // oD1
    case 5:   // oFog
    case 6:   // oPts
    case 7:   // oB0
    case 8:   // oB1
    case 9:   // oT0
    case 10:  // oT1
    case 11:  // oT2
    case 12:  // oT3
      return true;
    default:
      return false;  // 1/2 and 13-15 are not documented outputs
  }
}

bool valid_r_index(VsSrcFile file, uint32_t reg) {
  return file != VsSrcFile::R || reg <= 12;
}

std::array<float, 4> swizzle4(const std::array<float, 4>& base,
                              const std::array<VsSwizzle, 4>& sw) {
  return {base[static_cast<int>(sw[0])], base[static_cast<int>(sw[1])],
          base[static_cast<int>(sw[2])], base[static_cast<int>(sw[3])]};
}

void apply_mask(float* dst, const std::array<float, 4>& v, uint32_t mask) {
  for (int i = 0; i < 4; ++i) {
    if (mask & (1u << i)) dst[i] = v[i];
  }
}

bool read_source(const VsSlot& slot, const VsSource& src,
                 const VsRegisters& regs, bool force_scalar,
                 std::array<float, 4>& out, std::string* error) {
  std::array<float, 4> base{};
  switch (src.file) {
    case VsSrcFile::R:
      base = (src.reg == 12) ? regs.o[0] : regs.r[src.reg];
      break;
    case VsSrcFile::V:
      base = regs.v[slot.input_index];
      break;
    case VsSrcFile::C: {
      int idx = static_cast<int>(slot.const_index);
      if (slot.a0_relative) idx += regs.a0;
      if (idx < 0 || idx > 191) {
        if (error) *error = "relative constant index out of range";
        return false;
      }
      base = regs.c[idx];
      break;
    }
    case VsSrcFile::None:
    default:
      if (error) *error = "source operand has no register file";
      return false;
  }

  if (force_scalar) {
    float c = base[static_cast<int>(src.swizzle[0])];
    out = {c, c, c, c};
  } else {
    out = swizzle4(base, src.swizzle);
  }
  if (src.negate) {
    for (float& x : out) x = -x;
  }
  return true;
}

std::array<float, 4> mac_compute(VsMacOp op, const std::array<float, 4>& a,
                                 const std::array<float, 4>& b,
                                 const std::array<float, 4>& c) {
  std::array<float, 4> r{};
  switch (op) {
    case VsMacOp::Mov:
      r = a;
      break;
    case VsMacOp::Mul:
      for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i];
      break;
    case VsMacOp::Add:
      for (int i = 0; i < 4; ++i) r[i] = a[i] + c[i];
      break;
    case VsMacOp::Mad:
      for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i] + c[i];
      break;
    case VsMacOp::Dp3: {
      float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
      r = {d, d, d, d};
      break;
    }
    case VsMacOp::Dph: {
      float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
      r = {d, d, d, d};
      break;
    }
    case VsMacOp::Dp4: {
      float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
      r = {d, d, d, d};
      break;
    }
    case VsMacOp::Dst:
      r = {1.0f, a[1] * b[1], a[2], b[3]};
      break;
    case VsMacOp::Min:
      for (int i = 0; i < 4; ++i) r[i] = std::min(a[i], b[i]);
      break;
    case VsMacOp::Max:
      for (int i = 0; i < 4; ++i) r[i] = std::max(a[i], b[i]);
      break;
    case VsMacOp::Slt:
      for (int i = 0; i < 4; ++i) r[i] = (a[i] < b[i]) ? 1.0f : 0.0f;
      break;
    case VsMacOp::Sge:
      for (int i = 0; i < 4; ++i) r[i] = (a[i] >= b[i]) ? 1.0f : 0.0f;
      break;
    case VsMacOp::Nop:
    case VsMacOp::Arl:
    default:
      break;
  }
  return r;
}

bool ilu_forces_scalar(VsIluOp op) {
  switch (op) {
    case VsIluOp::Rcp:
    case VsIluOp::Rcc:
    case VsIluOp::Rsq:
    case VsIluOp::Exp:
    case VsIluOp::Log:
      return true;
    default:
      return false;
  }
}

std::array<float, 4> ilu_compute(VsIluOp op, const std::array<float, 4>& c) {
  constexpr float kInf = std::numeric_limits<float>::infinity();
  std::array<float, 4> r{};
  switch (op) {
    case VsIluOp::Mov:
      return c;
    case VsIluOp::Rcp: {
      float t = 1.0f / c[0];
      r = {t, t, t, t};
      break;
    }
    case VsIluOp::Rcc: {
      float t = 1.0f / c[0];
      if (t > 0.0f) {
        t = std::clamp(t, 5.42101e-20f, 1.884467e+19f);
      } else {
        t = std::clamp(t, -1.884467e+19f, -5.42101e-20f);
      }
      r = {t, t, t, t};
      break;
    }
    case VsIluOp::Rsq: {
      float s = c[0];
      float t;
      if (s == 0.0f) {
        t = kInf;
      } else if (std::isinf(s)) {
        t = 0.0f;
      } else {
        t = 1.0f / std::sqrt(std::fabs(s));
      }
      r = {t, t, t, t};
      break;
    }
    case VsIluOp::Exp: {
      float s = c[0];
      float fl = std::floor(s);
      r = {std::exp2(fl), s - fl, std::exp2(s), 1.0f};
      break;
    }
    case VsIluOp::Log: {
      float tmp = std::fabs(c[0]);
      if (tmp == 0.0f) {
        r = {-kInf, 1.0f, -kInf, 1.0f};
      } else {
        float fl = std::floor(std::log2(tmp));
        r = {fl, tmp / std::exp2(fl), std::log2(tmp), 1.0f};
      }
      break;
    }
    case VsIluOp::Lit: {
      constexpr float kEps = 1.0f / 256.0f;
      float sw = std::clamp(c[3], -(128.0f - kEps), 128.0f - kEps);
      float sx = std::max(c[0], 0.0f);
      float sy = std::max(c[1], 0.0f);
      float z = (sx > 0.0f) ? std::exp2(sw * std::log2(sy)) : 0.0f;
      r = {1.0f, sx, z, 1.0f};
      break;
    }
    case VsIluOp::Nop:
    default:
      break;
  }
  return r;
}

void write_temp(VsRegisters& regs, uint32_t reg,
                const std::array<float, 4>& v, uint32_t mask) {
  float* dst = (reg == 12) ? regs.o[0].data() : regs.r[reg].data();
  apply_mask(dst, v, mask);
}

bool write_output(const VsSlot& slot, const std::array<float, 4>& v,
                  VsRegisters& regs, std::string* error) {
  if (slot.out_file == VsOutFile::Constant) {
    if (slot.out_address > 191) {
      if (error) *error = "writeable constant index out of range";
      return false;
    }
    apply_mask(regs.c[slot.out_address].data(), v, slot.out_mask);
  } else {
    uint32_t idx = slot.out_address & 0xF;
    if (!valid_output_index(idx)) {
      if (error) *error = "output register reserved";
      return false;
    }
    apply_mask(regs.o[idx].data(), v, slot.out_mask);
  }
  return true;
}

bool vs_decode_slot(const uint32_t w[4], VsSlot& slot, std::string* error) {
  slot = VsSlot{};

  slot.ilu = static_cast<VsIluOp>(field(w, 1, 25, 3));
  uint32_t mac = field(w, 1, 21, 4);
  if (mac > 13) {
    if (error) *error = "MAC opcode is reserved";
    return false;
  }
  slot.mac = static_cast<VsMacOp>(mac);

  slot.const_index = static_cast<uint8_t>(field(w, 1, 13, 8));
  slot.input_index = static_cast<uint8_t>(field(w, 1, 9, 4));
  if (slot.const_index > 191) {
    if (error) *error = "constant index out of range";
    return false;
  }

  slot.a.file = static_cast<VsSrcFile>(field(w, 2, 26, 2));
  slot.a.reg = static_cast<uint8_t>(field(w, 2, 28, 4));
  slot.a.negate = field(w, 1, 8, 1) != 0;
  slot.a.swizzle = {static_cast<VsSwizzle>(field(w, 1, 6, 2)),
                    static_cast<VsSwizzle>(field(w, 1, 4, 2)),
                    static_cast<VsSwizzle>(field(w, 1, 2, 2)),
                    static_cast<VsSwizzle>(field(w, 1, 0, 2))};

  slot.b.file = static_cast<VsSrcFile>(field(w, 2, 11, 2));
  slot.b.reg = static_cast<uint8_t>(field(w, 2, 13, 4));
  slot.b.negate = field(w, 2, 25, 1) != 0;
  slot.b.swizzle = {static_cast<VsSwizzle>(field(w, 2, 23, 2)),
                    static_cast<VsSwizzle>(field(w, 2, 21, 2)),
                    static_cast<VsSwizzle>(field(w, 2, 19, 2)),
                    static_cast<VsSwizzle>(field(w, 2, 17, 2))};

  slot.c.file = static_cast<VsSrcFile>(field(w, 3, 28, 2));
  slot.c.reg = static_cast<uint8_t>((field(w, 2, 0, 2) << 2) |
                                    field(w, 3, 30, 2));
  slot.c.negate = field(w, 2, 10, 1) != 0;
  slot.c.swizzle = {static_cast<VsSwizzle>(field(w, 2, 8, 2)),
                    static_cast<VsSwizzle>(field(w, 2, 6, 2)),
                    static_cast<VsSwizzle>(field(w, 2, 4, 2)),
                    static_cast<VsSwizzle>(field(w, 2, 2, 2))};

  slot.mac_mask = static_cast<uint8_t>(field(w, 3, 24, 4));
  slot.temp_reg = static_cast<uint8_t>(field(w, 3, 20, 4));
  slot.ilu_mask = static_cast<uint8_t>(field(w, 3, 16, 4));
  slot.out_mask = static_cast<uint8_t>(field(w, 3, 12, 4));
  slot.out_file = static_cast<VsOutFile>(field(w, 3, 11, 1));
  slot.out_address = static_cast<uint8_t>(field(w, 3, 3, 8));
  slot.out_from_ilu = field(w, 3, 2, 1) != 0;
  slot.a0_relative = field(w, 3, 1, 1) != 0;
  slot.final = field(w, 3, 0, 1) != 0;

  auto file_ok = [&](const VsSource& s) {
    return s.file != VsSrcFile::None && valid_r_index(s.file, s.reg);
  };

  if (slot.ilu != VsIluOp::Nop) {
    if (!file_ok(slot.c)) {
      if (error) *error = "ILU source C is not a usable register";
      return false;
    }
  }
  if (slot.mac != VsMacOp::Nop) {
    if (mac_uses_a(slot.mac) && !file_ok(slot.a)) {
      if (error) *error = "MAC source A is not a usable register";
      return false;
    }
    if (mac_uses_b(slot.mac) && !file_ok(slot.b)) {
      if (error) *error = "MAC source B is not a usable register";
      return false;
    }
    if (mac_uses_c(slot.mac) && !file_ok(slot.c)) {
      if (error) *error = "MAC source C is not a usable register";
      return false;
    }
  }

  bool writes_temp = (slot.mac != VsMacOp::Nop && slot.mac_mask != 0) ||
                     (slot.ilu != VsIluOp::Nop && slot.ilu_mask != 0);
  if (writes_temp && slot.temp_reg > 12) {
    if (error) *error = "temporary register index out of range";
    return false;
  }

  if (slot.out_mask != 0) {
    if (slot.out_file == VsOutFile::Constant) {
      if (slot.out_address > 191) {
        if (error) *error = "writeable constant index out of range";
        return false;
      }
    } else if (!valid_output_index(slot.out_address & 0xF)) {
      if (error) *error = "output register reserved";
      return false;
    }
  }
  return true;
}

}  // namespace

VsProgram vs_decode_program(const uint32_t* words, size_t slot_count) {
  VsProgram program;
  if (!words) {
    program.error = "null program";
    return program;
  }
  const size_t limit = std::min<size_t>(slot_count, 136);
  for (size_t i = 0; i < limit; ++i) {
    VsSlot slot;
    std::string err;
    if (!vs_decode_slot(words + i * 4, slot, &err)) {
      program.error = "slot " + std::to_string(i) + ": " + err;
      return program;
    }
    program.slots.push_back(slot);
    if (slot.final) {
      program.valid = true;
      return program;
    }
  }
  program.error = "final instruction marker missing";
  return program;
}

void vs_init_registers(VsRegisters& regs) {
  for (auto& x : regs.v) x = {0.0f, 0.0f, 0.0f, 0.0f};
  for (auto& x : regs.c) x = {0.0f, 0.0f, 0.0f, 0.0f};
  for (auto& x : regs.r) x = {0.0f, 0.0f, 0.0f, 0.0f};
  for (auto& x : regs.o) x = {0.0f, 0.0f, 0.0f, 1.0f};
  regs.a0 = 0;
}

bool vs_execute(const VsProgram& program, VsRegisters& regs,
                std::string* error) {
  if (!program.valid) {
    if (error) *error = program.error.empty() ? "invalid program"
                                              : program.error;
    return false;
  }
  std::string err;
  for (const VsSlot& slot : program.slots) {
    bool has_mac = slot.mac != VsMacOp::Nop;
    bool has_ilu = slot.ilu != VsIluOp::Nop;
    std::array<float, 4> mac_res{};
    std::array<float, 4> ilu_res{};

    if (has_mac) {
      std::array<float, 4> a{}, b{}, c{};
      if (mac_uses_a(slot.mac) &&
          !read_source(slot, slot.a, regs, false, a, &err)) {
        if (error) *error = err;
        return false;
      }
      if (mac_uses_b(slot.mac) &&
          !read_source(slot, slot.b, regs, false, b, &err)) {
        if (error) *error = err;
        return false;
      }
      if (mac_uses_c(slot.mac) &&
          !read_source(slot, slot.c, regs, false, c, &err)) {
        if (error) *error = err;
        return false;
      }
      if (slot.mac == VsMacOp::Arl) {
        // Address register load: a0 = floor(src.x + bias). The hardware rounds
        // toward the intended value for byte-normalized attributes (xqemu).
        regs.a0 = static_cast<int>(std::floor(a[0] + 0.001f));
        mac_res = a;
      } else {
        mac_res = mac_compute(slot.mac, a, b, c);
      }
    }

    if (has_ilu) {
      std::array<float, 4> c{};
      if (!read_source(slot, slot.c, regs, ilu_forces_scalar(slot.ilu), c,
                       &err)) {
        if (error) *error = err;
        return false;
      }
      ilu_res = ilu_compute(slot.ilu, c);
    }

    // Temporary writes. A paired ILU always owns R1, so a MAC write to R1 in
    // the same slot is suppressed (xqemu nv2a_vsh.c decode_opcode).
    if (has_mac) {
      bool suppressed = has_ilu && slot.temp_reg == 1;
      if (!suppressed && slot.mac_mask != 0) {
        write_temp(regs, slot.temp_reg, mac_res, slot.mac_mask);
      }
    }
    if (has_ilu && slot.ilu_mask != 0) {
      uint32_t dest = has_mac ? 1 : slot.temp_reg;
      write_temp(regs, dest, ilu_res, slot.ilu_mask);
    }

    if (slot.out_mask != 0) {
      if (slot.out_from_ilu && has_ilu) {
        if (!write_output(slot, ilu_res, regs, &err)) {
          if (error) *error = err;
          return false;
        }
      } else if (!slot.out_from_ilu && has_mac) {
        if (!write_output(slot, mac_res, regs, &err)) {
          if (error) *error = err;
          return false;
        }
      }
    }
  }
  return true;
}
