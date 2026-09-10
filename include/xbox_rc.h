#pragma once
#include <array>
#include <cstdint>
#include <string>

// NV2A (original Xbox) pixel pipeline HLE: register combiners, homebrew only.
//
// The Xbox has no DX8 pixel shaders. Its fragment stage is register combiners
// plus texture shaders. This module models the combiner machine and evaluates
// it on the CPU. Grounding:
//
//  - xboxdevwiki.net/NV2A/Pixel_Combiner documents the NV2A deviations from
//    the GL extensions: ZERO and DISCARD share register index 0, a single
//    ALPHA flag selects component usage, per-stage constant colors, and the
//    MUX MSB/LSB select.
//  - The NV_register_combiners and NV_register_combiners2 GL specs define the
//    machine the wiki says the hardware implements: the A/B/C/D inputs and
//    mappings, the ten general-combiner expressions, output scale/bias, the
//    special "mux" sum, and the final combiner A..G.
//  - Cross-checked against xqemu hw/xbox/nv2a/nv2a_psh.c (register indices,
//    mapping functions, output flags, final-combiner settings).
//
// The general-combiner math is float, as the GL spec is written. The wiki
// notes it is unknown whether the NV2A really quantizes to 9-bit signed fixed
// point, so this model does not pretend to. Texture shaders (the samplers that
// produce the T0-T3 register values) are out of scope here: the caller passes
// the sampled texture registers as inputs.

// Combiner register set. Index 0 is both ZERO (as an input) and DISCARD (as an
// output), which is the NV2A quirk; the read/write entries map to the D3D
// names R0/R1.
enum class RcRegister : uint8_t {
  Zero = 0x0,       // read: 0, write: discard
  Const0 = 0x1,
  Const1 = 0x2,
  Fog = 0x3,
  Primary = 0x4,    // V0 / diffuse
  Secondary = 0x5,  // V1 / specular
  // 0x6, 0x7 unused
  Tex0 = 0x8,
  Tex1 = 0x9,
  Tex2 = 0xa,
  Tex3 = 0xb,
  Spare0 = 0xc,     // R0
  Spare1 = 0xd,     // R1
  V1R0Sum = 0xe,    // final-combiner pseudo register
  EFProduct = 0xf,  // final-combiner pseudo register
};

// Input range mappings (NV_register_combiners table 4).
enum class RcMapping : uint8_t {
  UnsignedIdentity = 0x00,
  UnsignedInvert = 0x20,
  ExpandNormal = 0x40,
  ExpandNegate = 0x60,
  HalfBiasNormal = 0x80,
  HalfBiasNegate = 0xa0,
  SignedIdentity = 0xc0,
  SignedNegate = 0xe0,
};

// Component usage. For the RGB portion: Rgb -> .rgb, Alpha -> .aaa.
// For the alpha portion: Rgb is the hardware BLUE option -> .b, Alpha -> .a.
enum class RcChannel : uint8_t { Rgb = 0x00, Alpha = 0x10 };

// Output scale/bias (xqemu PS_COMBINEROUTPUT_*).
enum class RcOutputScale : uint8_t {
  Identity = 0x00,
  Bias = 0x08,             // x - 0.5
  ShiftLeft1 = 0x10,       // x * 2
  ShiftLeft1Bias = 0x18,   // (x - 0.5) * 2
  ShiftLeft2 = 0x20,       // x * 4
  ShiftRight1 = 0x30,      // x / 2
};

struct RcInput {
  RcRegister reg = RcRegister::Zero;
  RcChannel channel = RcChannel::Rgb;
  RcMapping mapping = RcMapping::UnsignedIdentity;
};

struct RcOutput {
  RcRegister ab = RcRegister::Zero;   // Zero means DISCARD
  RcRegister cd = RcRegister::Zero;
  RcRegister sum = RcRegister::Zero;
  RcOutputScale scale = RcOutputScale::Identity;
  bool ab_dot = false;   // dot product instead of componentwise product
  bool cd_dot = false;
  bool mux_sum = false;  // sum = MUX(ab, cd) instead of ab + cd
  bool ab_blue_to_alpha = false;
  bool cd_blue_to_alpha = false;
};

struct RcStage {
  std::array<RcInput, 4> rgb{};    // A, B, C, D
  std::array<RcInput, 4> alpha{};  // A, B, C, D
  RcOutput rgb_out{};
  RcOutput alpha_out{};
};

struct RcFinal {
  std::array<RcInput, 7> in{};  // A, B, C, D, E, F, G
  bool clamp_sum = false;       // clamp the V1+Spare0 sum to [0,1]
  bool complement_v1 = false;   // unsigned-invert V1 in the sum
  bool complement_r0 = false;   // unsigned-invert Spare0 in the sum
};

// NV2A's special "or" (MUX) select. The wiki documents that the flag exists
// but leaves the LSB interpretation as FIXME, so only MSB is evaluable.
enum class RcMuxMode : uint8_t { Msb = 0, Lsb = 1 };

struct RcConfig {
  uint32_t num_stages = 0;     // active general combiners, 0-8
  RcMuxMode mux = RcMuxMode::Msb;
  bool unique_const0 = false;  // per-stage Const0 instead of shared [0]
  bool unique_const1 = false;
  std::array<RcStage, 8> stages{};
  RcFinal final{};
};

// Inputs: the sampled texture registers and the interpolated vertex colors.
// const0/const1 are indexed by stage when unique_const* is set, else [0].
// Index 8 is the final combiner's own constant, which NV2A always keeps
// distinct from the general stages.
struct RcInputs {
  std::array<float, 4> primary{};
  std::array<float, 4> secondary{};
  std::array<float, 4> fog{};  // rgb = fog color, a = fog factor
  std::array<std::array<float, 4>, 4> tex{};
  std::array<std::array<float, 4>, 9> const0{};
  std::array<std::array<float, 4>, 9> const1{};
  std::array<float, 4> spare0{};  // stage-0 register set seed
  std::array<float, 4> spare1{};
};

struct RcResult {
  std::array<float, 4> rgba{{0.0f, 0.0f, 0.0f, 0.0f}};
  bool ok = false;
  std::string error;
};

// Evaluate the combiner chain and final combiner. The result color is clamped
// to [0,1] for RGBA8 output.
RcResult rc_evaluate(const RcConfig& config, const RcInputs& inputs);

// Quantize a [0,1] float color the way the framebuffer would.
std::array<uint8_t, 4> rc_to_rgba8(const std::array<float, 4>& c);
