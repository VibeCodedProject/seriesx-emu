#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// NV2A (original Xbox) vertex-shader microcode HLE, homebrew only.
//
// The Xbox GPU executes DX8-class NV_vertex_program 1.1 programs: up to 136
// instruction slots of 16 bytes (4 little-endian DWORDs) each. This module
// decodes those slots into named fields, validates them, and interprets the
// program against a guest register file. Nothing here is invented:
//
//  - Field bit positions and register files come from
//    xboxdevwiki.net/NV2A/Vertex_Shader (16 inputs v0-v15, 12 temporaries
//    R0-R11 + R12 mirroring oPos, 192 constants c0-c191, A0.x, 11 output
//    registers, the 136-slot limit, the ILU/MAC opcode tables).
//  - Instruction semantics come from the NV_vertex_program /
//    NV_vertex_program1_1 GL extensions, which the wiki states the hardware
//    implements, cross-checked against xqemu hw/xbox/nv2a/nv2a_vsh.c
//    (paired MAC/ILU destination rules, scalar-forcing ILU ops, ARL bias).
//
// Honest limits: ILU transcendentals (RCP/RCC/RSQ/EXP/LOG/LIT) are computed
// exactly, while the hardware approximates them; relative constant addressing
// out of c0-c191 is a hard error rather than a guessed wrap.

enum class VsIluOp : uint8_t { Nop = 0, Mov, Rcp, Rcc, Rsq, Exp, Log, Lit };

enum class VsMacOp : uint8_t {
  Nop = 0,
  Mov,
  Mul,
  Add,
  Mad,
  Dp3,
  Dph,
  Dp4,
  Dst,
  Min,
  Max,
  Slt,
  Sge,
  Arl,
};

enum class VsSwizzle : uint8_t { X = 0, Y, Z, W };

// 2-bit source register-file selector. Zero is not a usable file.
enum class VsSrcFile : uint8_t { None = 0, R = 1, V = 2, C = 3 };

// Destination file for the per-slot output write: a writeable constant or a
// vertex result (output) register.
enum class VsOutFile : uint8_t { Constant = 0, Register = 1 };

struct VsSource {
  VsSrcFile file = VsSrcFile::None;
  uint8_t reg = 0;  // R index, used when file == R (R0-R12)
  bool negate = false;
  std::array<VsSwizzle, 4> swizzle{
      {VsSwizzle::X, VsSwizzle::Y, VsSwizzle::Z, VsSwizzle::W}};
};

struct VsSlot {
  VsIluOp ilu = VsIluOp::Nop;
  VsMacOp mac = VsMacOp::Nop;
  uint8_t const_index = 0;  // shared C operand index, c0-c191
  uint8_t input_index = 0;  // shared V operand index, v0-v15
  VsSource a, b, c;

  // Destination temporary: one field shared by the MAC and ILU sub-ops.
  uint8_t mac_mask = 0;  // xyzw write mask for the MAC result
  uint8_t temp_reg = 0;  // R0-R12
  uint8_t ilu_mask = 0;  // xyzw write mask for the ILU result

  // Per-slot output write.
  uint8_t out_mask = 0;                 // xyzw write mask
  VsOutFile out_file = VsOutFile::Register;
  uint8_t out_address = 0;              // constant index or output register
  bool out_from_ilu = false;            // output source: false=MAC, true=ILU
  bool a0_relative = false;             // add A0.x to C operand indices
  bool final = false;                   // final instruction marker
};

struct VsProgram {
  std::vector<VsSlot> slots;  // includes the terminating slot
  bool valid = false;
  std::string error;
};

// Decode and validate up to `slot_count` 16-byte slots (words = 4 dwords per
// slot), stopping after the final marker. Invalid fields yield valid=false.
VsProgram vs_decode_program(const uint32_t* words, size_t slot_count);

// Guest register file for execution.
struct VsRegisters {
  std::array<std::array<float, 4>, 16> v{};   // vertex attributes v0-v15
  std::array<std::array<float, 4>, 192> c{};  // constants c0-c191
  std::array<std::array<float, 4>, 12> r{};   // temporaries R0-R11
  std::array<std::array<float, 4>, 16> o{};   // output regs; o[0] = oPos
  int a0 = 0;                                 // A0.x address register
};

// Initialize to the documented reset state: temporaries 0, outputs XYZ=0/W=1.
void vs_init_registers(VsRegisters& regs);

// Execute every slot of a decoded program. Returns false (and fills `error`)
// on a relative constant access outside c0-c191.
bool vs_execute(const VsProgram& program, VsRegisters& regs,
                std::string* error = nullptr);
