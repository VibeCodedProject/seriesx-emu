#include "xbox_vs.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "xbox_rc.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

namespace {

bool near(float a, float b, float eps = 1e-4f) {
  return std::fabs(a - b) <= eps;
}

bool near4(const std::array<float, 4>& a, const std::array<float, 4>& b) {
  for (int i = 0; i < 4; ++i) {
    if (!near(a[i], b[i])) return false;
  }
  return true;
}

// Builds a 16-byte vertex-shader slot field by field (little-endian dwords).
struct SB {
  uint32_t w[4] = {0, 0, 0, 0};

  void set(int dw, int bit, int len, uint32_t v) {
    uint32_t mask = ((1u << len) - 1u) << bit;
    w[dw] = (w[dw] & ~mask) | ((v << bit) & mask);
  }
  void ilu(VsIluOp o) { set(1, 25, 3, static_cast<uint32_t>(o)); }
  void mac(VsMacOp o) { set(1, 21, 4, static_cast<uint32_t>(o)); }
  void cst(uint32_t i) { set(1, 13, 8, i); }
  void vin(uint32_t i) { set(1, 9, 4, i); }

  void a(VsSrcFile f, uint32_t r, bool neg, VsSwizzle x = VsSwizzle::X,
         VsSwizzle y = VsSwizzle::Y, VsSwizzle z = VsSwizzle::Z,
         VsSwizzle ww = VsSwizzle::W) {
    set(2, 26, 2, static_cast<uint32_t>(f));
    set(2, 28, 4, r);
    set(1, 8, 1, neg ? 1 : 0);
    set(1, 6, 2, static_cast<uint32_t>(x));
    set(1, 4, 2, static_cast<uint32_t>(y));
    set(1, 2, 2, static_cast<uint32_t>(z));
    set(1, 0, 2, static_cast<uint32_t>(ww));
  }
  void b(VsSrcFile f, uint32_t r, bool neg, VsSwizzle x = VsSwizzle::X,
         VsSwizzle y = VsSwizzle::Y, VsSwizzle z = VsSwizzle::Z,
         VsSwizzle ww = VsSwizzle::W) {
    set(2, 11, 2, static_cast<uint32_t>(f));
    set(2, 13, 4, r);
    set(2, 25, 1, neg ? 1 : 0);
    set(2, 23, 2, static_cast<uint32_t>(x));
    set(2, 21, 2, static_cast<uint32_t>(y));
    set(2, 19, 2, static_cast<uint32_t>(z));
    set(2, 17, 2, static_cast<uint32_t>(ww));
  }
  void c(VsSrcFile f, uint32_t r, bool neg, VsSwizzle x = VsSwizzle::X,
         VsSwizzle y = VsSwizzle::Y, VsSwizzle z = VsSwizzle::Z,
         VsSwizzle ww = VsSwizzle::W) {
    set(3, 28, 2, static_cast<uint32_t>(f));
    set(2, 0, 2, (r >> 2) & 3);
    set(3, 30, 2, r & 3);
    set(2, 10, 1, neg ? 1 : 0);
    set(2, 8, 2, static_cast<uint32_t>(x));
    set(2, 6, 2, static_cast<uint32_t>(y));
    set(2, 4, 2, static_cast<uint32_t>(z));
    set(2, 2, 2, static_cast<uint32_t>(ww));
  }
  void mac_mask(uint32_t m) { set(3, 24, 4, m); }
  void temp(uint32_t r) { set(3, 20, 4, r); }
  void ilu_mask(uint32_t m) { set(3, 16, 4, m); }
  void out(uint32_t mask, VsOutFile f, uint32_t addr, bool from_ilu) {
    set(3, 12, 4, mask);
    set(3, 11, 1, static_cast<uint32_t>(f));
    set(3, 3, 8, addr);
    set(3, 2, 1, from_ilu ? 1 : 0);
  }
  void a0(bool v) { set(3, 1, 1, v ? 1 : 0); }
  void final(bool v) { set(3, 0, 1, v ? 1 : 0); }
};

VsProgram decode(const std::vector<SB>& slots) {
  std::vector<uint32_t> words;
  for (const SB& s : slots) {
    words.insert(words.end(), {s.w[0], s.w[1], s.w[2], s.w[3]});
  }
  return vs_decode_program(words.data(), slots.size());
}

constexpr uint32_t kAll = 0xF;

}  // namespace

int main() {
  // ---- xbox_vs: field decode ----
  {
    SB s;
    s.ilu(VsIluOp::Mov);
    s.mac(VsMacOp::Mad);
    s.cst(5);
    s.vin(3);
    s.a(VsSrcFile::R, 2, true, VsSwizzle::W, VsSwizzle::Z, VsSwizzle::Y,
        VsSwizzle::X);
    s.b(VsSrcFile::C, 0, false);
    s.c(VsSrcFile::V, 1, false, VsSwizzle::Y, VsSwizzle::Z, VsSwizzle::W,
        VsSwizzle::X);
    s.mac_mask(0xA);
    s.temp(6);
    s.ilu_mask(0x5);
    s.out(kAll, VsOutFile::Register, 3, true);
    s.a0(true);
    s.final(true);

    VsProgram p = decode({s});
    CHECK(p.valid && p.slots.size() == 1);
    const VsSlot& d = p.slots[0];
    CHECK(d.ilu == VsIluOp::Mov && d.mac == VsMacOp::Mad);
    CHECK(d.const_index == 5 && d.input_index == 3);
    CHECK(d.a.file == VsSrcFile::R && d.a.reg == 2 && d.a.negate);
    CHECK(d.a.swizzle[0] == VsSwizzle::W && d.a.swizzle[3] == VsSwizzle::X);
    CHECK(d.b.file == VsSrcFile::C && !d.b.negate);
    CHECK(d.c.file == VsSrcFile::V);
    CHECK(d.mac_mask == 0xA && d.temp_reg == 6 && d.ilu_mask == 0x5);
    CHECK(d.out_mask == kAll && d.out_file == VsOutFile::Register &&
          d.out_address == 3 && d.out_from_ilu);
    CHECK(d.a0_relative && d.final);
    std::puts("vs field decode ok");
  }

  // ---- xbox_vs: validation rejects bad encodings ----
  {
    SB bad_mac;
    bad_mac.mac(static_cast<VsMacOp>(14));
    bad_mac.final(true);
    CHECK(!decode({bad_mac}).valid);

    SB bad_src;
    bad_src.mac(VsMacOp::Mov);  // MAC MOV uses A
    bad_src.a(VsSrcFile::None, 0, false);
    bad_src.final(true);
    CHECK(!decode({bad_src}).valid);

    SB bad_out;
    bad_out.out(kAll, VsOutFile::Register, 1, false);  // reserved output
    bad_out.final(true);
    CHECK(!decode({bad_out}).valid);

    SB bad_temp;
    bad_temp.mac(VsMacOp::Mov);
    bad_temp.a(VsSrcFile::R, 0, false);
    bad_temp.mac_mask(kAll);
    bad_temp.temp(15);
    bad_temp.final(true);
    CHECK(!decode({bad_temp}).valid);

    SB bad_const;
    bad_const.mac(VsMacOp::Mov);
    bad_const.a(VsSrcFile::C, 0, false);
    bad_const.cst(200);
    bad_const.final(true);
    CHECK(!decode({bad_const}).valid);

    SB no_final;
    no_final.mac(VsMacOp::Mov);
    no_final.a(VsSrcFile::R, 0, false);
    CHECK(!decode({no_final}).valid);
    std::puts("vs validation ok");
  }

  // ---- xbox_vs: init state + MOV oPos from a constant ----
  {
    SB s;
    s.mac(VsMacOp::Mov);
    s.a(VsSrcFile::C, 0, false);
    s.cst(0);
    s.out(kAll, VsOutFile::Register, 0, false);
    s.final(true);
    VsProgram p = decode({s});
    CHECK(p.valid);

    VsRegisters r;
    vs_init_registers(r);
    CHECK(near4(r.o[0], {0, 0, 0, 1}));
    CHECK(near4(r.r[0], {0, 0, 0, 0}));
    r.c[0] = {1, 2, 3, 4};
    std::string err;
    CHECK(vs_execute(p, r, &err));
    CHECK(near4(r.o[0], {1, 2, 3, 4}));
    std::puts("vs mov oPos ok");
  }

  // ---- xbox_vs: DP4 transform with per-component output masks ----
  {
    std::vector<SB> slots;
    const float rows[4][4] = {{2, 0, 0, 0}, {0, 3, 0, 0},
                              {0, 0, 4, 0}, {0, 0, 0, 5}};
    for (int i = 0; i < 4; ++i) {
      SB s;
      s.mac(VsMacOp::Dp4);
      s.a(VsSrcFile::V, 0, false);
      s.b(VsSrcFile::C, 0, false);
      s.vin(0);
      s.cst(static_cast<uint32_t>(i));
      s.out(1u << i, VsOutFile::Register, 0, false);  // oPos component i
      s.final(i == 3);
      slots.push_back(s);
    }

    VsProgram p = decode(slots);
    CHECK(p.valid && p.slots.size() == 4);
    VsRegisters r;
    vs_init_registers(r);
    r.v[0] = {1, 1, 1, 1};
    for (int i = 0; i < 4; ++i) {
      r.c[i] = {rows[i][0], rows[i][1], rows[i][2], rows[i][3]};
    }
    CHECK(vs_execute(p, r));
    CHECK(near4(r.o[0], {2, 3, 4, 5}));
    std::puts("vs dp4 transform ok");
  }

  // ---- xbox_vs: paired MAC + ILU (ILU owns R1, MAC write to R1 suppressed) --
  {
    SB s;
    s.mac(VsMacOp::Mul);
    s.a(VsSrcFile::V, 0, false);
    s.b(VsSrcFile::C, 0, false);
    s.vin(0);
    s.cst(0);
    s.mac_mask(kAll);
    s.temp(1);  // MAC wants R1 but ILU is present -> suppressed
    s.ilu(VsIluOp::Rcp);
    s.c(VsSrcFile::C, 0, false);
    s.ilu_mask(kAll);
    s.out(kAll, VsOutFile::Register, 3, false);  // MAC result feeds oD0
    s.final(true);

    VsProgram p = decode({s});
    CHECK(p.valid);
    VsRegisters r;
    vs_init_registers(r);
    r.v[0] = {2, 4, 6, 8};
    r.c[0] = {1, 2, 3, 4};
    CHECK(vs_execute(p, r));
    CHECK(near4(r.o[3], {2, 8, 18, 32}));  // MAC * c
    CHECK(near(r.r[1][0], 1.0f));          // ILU rcp(c.x) owns R1
    std::puts("vs paired mac/ilu ok");
  }

  // ---- xbox_vs: ILU exact values ----
  {
    struct Case {
      VsIluOp op;
      float in0;
      float expected[4];
    };
    const Case cases[] = {
        {VsIluOp::Rcp, 4.0f, {0.25f, 0.25f, 0.25f, 0.25f}},
        {VsIluOp::Rsq, 4.0f, {0.5f, 0.5f, 0.5f, 0.5f}},
        {VsIluOp::Exp, 2.5f, {4.0f, 0.5f, 5.656854f, 1.0f}},
        {VsIluOp::Log, 8.0f, {3.0f, 1.0f, 3.0f, 1.0f}},
    };
    for (const Case& tc : cases) {
      SB s;
      s.ilu(tc.op);
      s.c(VsSrcFile::C, 0, false);
      s.cst(0);
      s.out(kAll, VsOutFile::Register, 3, true);
      s.final(true);
      VsProgram p = decode({s});
      CHECK(p.valid);
      VsRegisters r;
      vs_init_registers(r);
      r.c[0] = {tc.in0, 0, 0, 0};
      CHECK(vs_execute(p, r));
      CHECK(near4(r.o[3], {tc.expected[0], tc.expected[1], tc.expected[2],
                           tc.expected[3]}));
    }

    // LIT reads the swizzled C as a vector.
    SB lit;
    lit.ilu(VsIluOp::Lit);
    lit.c(VsSrcFile::C, 0, false);
    lit.cst(0);
    lit.out(kAll, VsOutFile::Register, 3, true);
    lit.final(true);
    VsProgram lp = decode({lit});
    VsRegisters lr;
    vs_init_registers(lr);
    lr.c[0] = {1, 4, 0, 1};
    CHECK(vs_execute(lp, lr));
    CHECK(near4(lr.o[3], {1, 1, 4, 1}));
    std::puts("vs ilu values ok");
  }

  // ---- xbox_vs: swizzle + negate ----
  {
    SB s;
    s.mac(VsMacOp::Mov);
    s.a(VsSrcFile::V, 0, true, VsSwizzle::W, VsSwizzle::Z, VsSwizzle::Y,
        VsSwizzle::X);
    s.vin(0);
    s.out(kAll, VsOutFile::Register, 3, false);
    s.final(true);
    VsProgram p = decode({s});
    VsRegisters r;
    vs_init_registers(r);
    r.v[0] = {1, 2, 3, 4};
    CHECK(vs_execute(p, r));
    CHECK(near4(r.o[3], {-4, -3, -2, -1}));
    std::puts("vs swizzle/negate ok");
  }

  // ---- xbox_vs: ARL + relative constant addressing ----
  {
    std::vector<SB> slots;
    SB arl;
    arl.mac(VsMacOp::Arl);
    arl.a(VsSrcFile::C, 0, false);
    arl.cst(0);
    slots.push_back(arl);

    SB mov;
    mov.mac(VsMacOp::Mov);
    mov.a(VsSrcFile::C, 0, false);
    mov.cst(0);
    mov.a0(true);
    mov.out(kAll, VsOutFile::Register, 3, false);
    mov.final(true);
    slots.push_back(mov);

    VsProgram p = decode(slots);
    CHECK(p.valid);
    VsRegisters r;
    vs_init_registers(r);
    r.c[0] = {2, 0, 0, 0};
    r.c[2] = {7, 8, 9, 10};  // c[0 + A0(2)]
    CHECK(vs_execute(p, r));
    CHECK(r.a0 == 2);
    CHECK(near4(r.o[3], {7, 8, 9, 10}));

    // Out-of-range relative access is a hard error.
    r.c[0] = {200, 0, 0, 0};
    std::string err;
    CHECK(!vs_execute(p, r, &err));
    CHECK(!err.empty());
    std::puts("vs arl/relative ok");
  }

  // ---- xbox_vs: writeable constant output + R12 mirrors oPos ----
  {
    SB s;
    s.mac(VsMacOp::Mov);
    s.a(VsSrcFile::V, 0, false);
    s.vin(0);
    s.out(kAll, VsOutFile::Constant, 5, false);
    s.final(true);
    VsProgram p = decode({s});
    CHECK(p.valid);
    VsRegisters r;
    vs_init_registers(r);
    r.v[0] = {11, 12, 13, 14};
    CHECK(vs_execute(p, r));
    CHECK(near4(r.c[5], {11, 12, 13, 14}));

    SB w;
    w.mac(VsMacOp::Mov);
    w.a(VsSrcFile::V, 0, false);
    w.vin(0);
    w.out(kAll, VsOutFile::Register, 0, false);
    w.final(true);
    VsProgram wp = decode({w});
    VsRegisters wr;
    vs_init_registers(wr);
    wr.v[0] = {5, 6, 7, 8};
    CHECK(vs_execute(wp, wr));
    CHECK(near4(wr.o[0], {5, 6, 7, 8}));
    std::puts("vs writeable const + oPos ok");
  }

  // ================= xbox_rc: register combiners =================
  auto rcin = [](RcRegister reg, RcChannel ch = RcChannel::Rgb,
                 RcMapping m = RcMapping::UnsignedIdentity) {
    return RcInput{reg, ch, m};
  };

  // ---- rc_to_rgba8 ----
  {
    auto px = rc_to_rgba8({0.0f, 0.5f, 1.0f, 0.25f});
    CHECK(px[0] == 0 && px[1] == 128 && px[2] == 255 && px[3] == 64);
    std::puts("rc rgba8 ok");
  }

  // ---- final combiner multiply: D=0, A=primary, B=tex0 ----
  {
    RcConfig cfg;
    cfg.final.in[0] = rcin(RcRegister::Primary);
    cfg.final.in[1] = rcin(RcRegister::Tex0);
    cfg.final.in[2] = rcin(RcRegister::Zero);
    cfg.final.in[3] = rcin(RcRegister::Zero);
    cfg.final.in[4] = rcin(RcRegister::Zero);
    cfg.final.in[5] = rcin(RcRegister::Zero);
    cfg.final.in[6] = rcin(RcRegister::Zero);
    RcInputs in;
    in.primary = {0.5f, 0.25f, 1.0f, 1.0f};
    in.tex[0] = {0.4f, 0.8f, 0.5f, 1.0f};
    RcResult r = rc_evaluate(cfg, in);
    CHECK(r.ok);
    CHECK(near4(r.rgba, {0.2f, 0.2f, 0.5f, 0.0f}));
    std::puts("rc final multiply ok");
  }

  // ---- general stage modulate into spare0, read by the final D ----
  {
    RcConfig cfg;
    cfg.num_stages = 1;
    cfg.stages[0].rgb[0] = rcin(RcRegister::Primary);
    cfg.stages[0].rgb[1] = rcin(RcRegister::Tex0);
    cfg.stages[0].rgb[2] = rcin(RcRegister::Zero);
    cfg.stages[0].rgb[3] = rcin(RcRegister::Zero);
    cfg.stages[0].rgb_out.sum = RcRegister::Spare0;
    cfg.final.in[3] = rcin(RcRegister::Spare0);  // D = spare0
    RcInputs in;
    in.primary = {0.5f, 0.25f, 1.0f, 1.0f};
    in.tex[0] = {0.4f, 0.8f, 0.5f, 1.0f};
    RcResult r = rc_evaluate(cfg, in);
    CHECK(r.ok);
    CHECK(near4(r.rgba, {0.2f, 0.2f, 0.5f, 0.0f}));
    std::puts("rc stage modulate ok");
  }

  // ---- MUX sum selects AB or CD on spare0.alpha ----
  {
    RcConfig cfg;
    cfg.num_stages = 1;
    cfg.stages[0].rgb[0] = rcin(RcRegister::Primary);  // A
    cfg.stages[0].rgb[1] = rcin(RcRegister::Tex0);     // B -> ab = 0.5
    cfg.stages[0].rgb[2] = rcin(RcRegister::Const0);   // C
    cfg.stages[0].rgb[3] = rcin(RcRegister::Const1);   // D -> cd = 0.2
    cfg.stages[0].rgb_out.sum = RcRegister::Spare0;
    cfg.stages[0].rgb_out.mux_sum = true;
    cfg.final.in[3] = rcin(RcRegister::Spare0);
    RcInputs in;
    in.primary = {1, 1, 1, 1};
    in.tex[0] = {0.5f, 0.5f, 0.5f, 1.0f};
    in.const0[0] = {0.25f, 0.25f, 0.25f, 1.0f};
    in.const1[0] = {0.8f, 0.8f, 0.8f, 1.0f};
    in.spare0 = {0, 0, 0, 0.8f};  // >= 0.5 -> pick CD
    RcResult hi = rc_evaluate(cfg, in);
    CHECK(hi.ok && near(hi.rgba[0], 0.2f));
    in.spare0 = {0, 0, 0, 0.2f};  // < 0.5 -> pick AB
    RcResult lo = rc_evaluate(cfg, in);
    CHECK(lo.ok && near(lo.rgba[0], 0.5f));
    std::puts("rc mux ok");
  }

  // ---- per-stage constants ----
  {
    RcConfig cfg;
    cfg.num_stages = 2;
    cfg.unique_const0 = true;
    cfg.unique_const1 = true;
    for (int s = 0; s < 2; ++s) {
      cfg.stages[s].rgb[0] = rcin(RcRegister::Const0);
      cfg.stages[s].rgb[1] = rcin(RcRegister::Const1);
      cfg.stages[s].rgb_out.sum = RcRegister::Spare0;
    }
    cfg.final.in[3] = rcin(RcRegister::Spare0);
    RcInputs in;
    in.const0[0] = {1, 1, 1, 1};
    in.const1[0] = {1, 1, 1, 1};
    in.const0[1] = {0.25f, 0.25f, 0.25f, 1.0f};
    in.const1[1] = {1, 1, 1, 1};
    RcResult r = rc_evaluate(cfg, in);
    CHECK(r.ok && near(r.rgba[0], 0.25f));
    std::puts("rc per-stage constants ok");
  }

  // ---- blue-to-alpha, dot product, output bias ----
  {
    RcConfig cfg;
    cfg.num_stages = 1;
    cfg.stages[0].rgb[0] = rcin(RcRegister::Primary);
    cfg.stages[0].rgb[1] = rcin(RcRegister::Tex0);
    cfg.stages[0].rgb[2] = rcin(RcRegister::Zero);
    cfg.stages[0].rgb[3] = rcin(RcRegister::Zero);
    cfg.stages[0].rgb_out.ab = RcRegister::Spare0;
    cfg.stages[0].rgb_out.ab_blue_to_alpha = true;
    cfg.final.in[6] = rcin(RcRegister::Spare0, RcChannel::Alpha);  // G
    RcInputs in;
    in.primary = {1, 1, 0.8f, 1};
    in.tex[0] = {1, 1, 0.5f, 1};
    RcResult r = rc_evaluate(cfg, in);
    CHECK(r.ok && near(r.rgba[3], 0.4f));  // ab.b = 0.8 * 0.5

    RcConfig dot;
    dot.num_stages = 1;
    dot.stages[0].rgb[0] = rcin(RcRegister::Primary);
    dot.stages[0].rgb[1] = rcin(RcRegister::Tex0);
    dot.stages[0].rgb[2] = rcin(RcRegister::Zero);
    dot.stages[0].rgb[3] = rcin(RcRegister::Zero);
    dot.stages[0].rgb_out.ab = RcRegister::Spare0;
    dot.stages[0].rgb_out.ab_dot = true;
    dot.final.in[3] = rcin(RcRegister::Spare0);
    RcInputs di;
    di.primary = {0.2f, 0.4f, 0.4f, 1};
    di.tex[0] = {1, 1, 0.5f, 1};
    RcResult dr = rc_evaluate(dot, di);
    CHECK(dr.ok && near(dr.rgba[0], 0.8f) && near(dr.rgba[1], 0.8f));

    RcConfig bias;
    bias.num_stages = 1;
    bias.stages[0].rgb[0] = rcin(RcRegister::Primary);
    bias.stages[0].rgb[1] = rcin(RcRegister::Const0);
    bias.stages[0].rgb[2] = rcin(RcRegister::Zero);
    bias.stages[0].rgb[3] = rcin(RcRegister::Zero);
    bias.stages[0].rgb_out.sum = RcRegister::Spare0;
    bias.stages[0].rgb_out.scale = RcOutputScale::Bias;
    bias.final.in[3] = rcin(RcRegister::Spare0);
    RcInputs bi;
    bi.primary = {1, 1, 1, 1};
    bi.const0[0] = {1, 1, 1, 1};
    RcResult br = rc_evaluate(bias, bi);
    CHECK(br.ok && near(br.rgba[0], 0.5f));
    std::puts("rc blue/dot/bias ok");
  }

  // ---- final combiner sum: complement + clamp ----
  {
    RcConfig cfg;
    cfg.final.in[3] = rcin(RcRegister::V1R0Sum);  // D = V1 + R0
    cfg.final.complement_r0 = true;
    RcInputs in;
    in.secondary = {0.2f, 0.2f, 0.2f, 1};
    in.spare0 = {0.3f, 0.3f, 0.3f, 1};
    RcResult r = rc_evaluate(cfg, in);
    CHECK(r.ok && near(r.rgba[0], 0.9f));  // 0.2 + (1 - 0.3)
    cfg.final.clamp_sum = true;
    in.secondary = {0.6f, 0.6f, 0.6f, 1};
    in.spare0 = {0.0f, 0.0f, 0.0f, 1};
    RcResult c = rc_evaluate(cfg, in);
    CHECK(c.ok && near(c.rgba[0], 1.0f));
    std::puts("rc final sum ok");
  }

  // ---- validation failures ----
  {
    RcConfig lsb;
    lsb.mux = RcMuxMode::Lsb;
    CHECK(!rc_evaluate(lsb, RcInputs{}).ok);

    RcConfig many;
    many.num_stages = 9;
    CHECK(!rc_evaluate(many, RcInputs{}).ok);

    RcConfig dot_alpha;
    dot_alpha.num_stages = 1;
    dot_alpha.stages[0].alpha_out.ab_dot = true;
    CHECK(!rc_evaluate(dot_alpha, RcInputs{}).ok);

    RcConfig bad_target;
    bad_target.num_stages = 1;
    bad_target.stages[0].rgb_out.sum = RcRegister::Fog;
    CHECK(!rc_evaluate(bad_target, RcInputs{}).ok);

    RcConfig dup;
    dup.num_stages = 1;
    dup.stages[0].rgb_out.ab = RcRegister::Spare0;
    dup.stages[0].rgb_out.cd = RcRegister::Spare0;
    CHECK(!rc_evaluate(dup, RcInputs{}).ok);

    RcConfig bad_map;
    bad_map.final.in[0] = rcin(RcRegister::Primary, RcChannel::Rgb,
                               RcMapping::ExpandNormal);
    CHECK(!rc_evaluate(bad_map, RcInputs{}).ok);

    RcConfig bad_a;
    bad_a.final.in[0] = rcin(RcRegister::V1R0Sum);
    CHECK(!rc_evaluate(bad_a, RcInputs{}).ok);

    RcConfig bad_e;
    bad_e.final.in[4] = rcin(RcRegister::EFProduct);
    CHECK(!rc_evaluate(bad_e, RcInputs{}).ok);
    std::puts("rc validation ok");
  }

  std::puts("test_xbox_shaders passed");
  return 0;
}
