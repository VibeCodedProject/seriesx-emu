#include "xbox_rc.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;

// Working register set as it flows from stage to stage. Only the read/write
// registers live here; CONST0/CONST1/FOG are inputs.
struct Work {
  Vec4 primary{};
  Vec4 secondary{};
  Vec4 spare0{};
  Vec4 spare1{};
  std::array<Vec4, 4> tex{};
};

constexpr uint32_t kFinalConst = 8;

Vec4 load_value(const RcInputs& in, const Work& w, uint32_t cidx0,
                uint32_t cidx1, RcRegister r) {
  switch (r) {
    case RcRegister::Zero:
      return {0.0f, 0.0f, 0.0f, 0.0f};
    case RcRegister::Const0:
      return in.const0[cidx0];
    case RcRegister::Const1:
      return in.const1[cidx1];
    case RcRegister::Fog:
      return in.fog;
    case RcRegister::Primary:
      return w.primary;
    case RcRegister::Secondary:
      return w.secondary;
    case RcRegister::Tex0:
      return w.tex[0];
    case RcRegister::Tex1:
      return w.tex[1];
    case RcRegister::Tex2:
      return w.tex[2];
    case RcRegister::Tex3:
      return w.tex[3];
    case RcRegister::Spare0:
      return w.spare0;
    case RcRegister::Spare1:
      return w.spare1;
    case RcRegister::V1R0Sum:
    case RcRegister::EFProduct:
    default:
      return {0.0f, 0.0f, 0.0f, 0.0f};
  }
}

float rc_map(RcMapping m, float e) {
  switch (m) {
    case RcMapping::UnsignedIdentity:
      return std::max(0.0f, e);
    case RcMapping::UnsignedInvert:
      return 1.0f - std::min(std::max(e, 0.0f), 1.0f);
    case RcMapping::ExpandNormal:
      return 2.0f * std::max(0.0f, e) - 1.0f;
    case RcMapping::ExpandNegate:
      return -2.0f * std::max(0.0f, e) + 1.0f;
    case RcMapping::HalfBiasNormal:
      return std::max(0.0f, e) - 0.5f;
    case RcMapping::HalfBiasNegate:
      return -std::max(0.0f, e) + 0.5f;
    case RcMapping::SignedIdentity:
      return e;
    case RcMapping::SignedNegate:
      return -e;
  }
  return e;
}

Vec3 rgb_of(const RcInput& in, const Vec4& v) {
  if (in.channel == RcChannel::Rgb) {
    return {rc_map(in.mapping, v[0]), rc_map(in.mapping, v[1]),
            rc_map(in.mapping, v[2])};
  }
  float a = rc_map(in.mapping, v[3]);
  return {a, a, a};
}

float alpha_of(const RcInput& in, const Vec4& v) {
  float e = (in.channel == RcChannel::Alpha) ? v[3] : v[2];
  return rc_map(in.mapping, e);
}

float scale_scalar(float x, RcOutputScale s) {
  float y = x;
  switch (s) {
    case RcOutputScale::Identity:
      y = x;
      break;
    case RcOutputScale::Bias:
      y = x - 0.5f;
      break;
    case RcOutputScale::ShiftLeft1:
      y = x * 2.0f;
      break;
    case RcOutputScale::ShiftLeft1Bias:
      y = (x - 0.5f) * 2.0f;
      break;
    case RcOutputScale::ShiftLeft2:
      y = x * 4.0f;
      break;
    case RcOutputScale::ShiftRight1:
      y = x * 0.5f;
      break;
  }
  return std::clamp(y, -1.0f, 1.0f);
}

Vec3 scale_vec(const Vec3& v, RcOutputScale s) {
  return {scale_scalar(v[0], s), scale_scalar(v[1], s),
          scale_scalar(v[2], s)};
}

Vec4* target_ptr(Work& w, RcRegister r) {
  switch (r) {
    case RcRegister::Primary:
      return &w.primary;
    case RcRegister::Secondary:
      return &w.secondary;
    case RcRegister::Spare0:
      return &w.spare0;
    case RcRegister::Spare1:
      return &w.spare1;
    case RcRegister::Tex0:
      return &w.tex[0];
    case RcRegister::Tex1:
      return &w.tex[1];
    case RcRegister::Tex2:
      return &w.tex[2];
    case RcRegister::Tex3:
      return &w.tex[3];
    default:
      return nullptr;  // Zero = discard
  }
}

void write_rgb(Work& w, RcRegister r, const Vec3& v) {
  Vec4* p = target_ptr(w, r);
  if (!p) return;
  (*p)[0] = v[0];
  (*p)[1] = v[1];
  (*p)[2] = v[2];
}

void write_alpha(Work& w, RcRegister r, float a) {
  Vec4* p = target_ptr(w, r);
  if (!p) return;
  (*p)[3] = a;
}

void write_blue_to_alpha(Work& w, RcRegister r, const Vec3& v) {
  Vec4* p = target_ptr(w, r);
  if (!p) return;
  (*p)[3] = v[2];
}

bool is_writable_target(RcRegister r) {
  switch (r) {
    case RcRegister::Zero:
    case RcRegister::Primary:
    case RcRegister::Secondary:
    case RcRegister::Spare0:
    case RcRegister::Spare1:
    case RcRegister::Tex0:
    case RcRegister::Tex1:
    case RcRegister::Tex2:
    case RcRegister::Tex3:
      return true;
    default:
      return false;
  }
}

bool is_pseudo(RcRegister r) {
  return r == RcRegister::V1R0Sum || r == RcRegister::EFProduct;
}

bool targets_unique(const RcOutput& o, std::string& err) {
  RcRegister t[3] = {o.ab, o.cd, o.sum};
  for (int i = 0; i < 3; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      if (t[i] != RcRegister::Zero && t[i] == t[j]) {
        err = "combiner outputs must target unique registers";
        return false;
      }
    }
  }
  return true;
}

bool validate(const RcConfig& c, std::string& err) {
  if (c.num_stages > 8) {
    err = "too many combiner stages";
    return false;
  }
  if (c.mux != RcMuxMode::Msb) {
    err = "NV2A MUX LSB mode is not reverse-engineered";
    return false;
  }
  for (uint32_t s = 0; s < c.num_stages; ++s) {
    const RcStage& st = c.stages[s];
    for (const RcInput& in : st.rgb) {
      if (is_pseudo(in.reg)) {
        err = "pseudo register is only valid in the final combiner";
        return false;
      }
    }
    for (const RcInput& in : st.alpha) {
      if (is_pseudo(in.reg)) {
        err = "pseudo register is only valid in the final combiner";
        return false;
      }
    }
    if (!is_writable_target(st.rgb_out.ab) ||
        !is_writable_target(st.rgb_out.cd) ||
        !is_writable_target(st.rgb_out.sum)) {
      err = "combiner output targets a non-writable register";
      return false;
    }
    if (!targets_unique(st.rgb_out, err)) return false;
    if (!is_writable_target(st.alpha_out.ab) ||
        !is_writable_target(st.alpha_out.cd) ||
        !is_writable_target(st.alpha_out.sum)) {
      err = "combiner output targets a non-writable register";
      return false;
    }
    if (!targets_unique(st.alpha_out, err)) return false;
    // Dot products are RGB-only (they collapse a vector to a scalar).
    if (st.alpha_out.ab_dot || st.alpha_out.cd_dot) {
      err = "dot product is valid only for the RGB combiner portion";
      return false;
    }
  }

  auto final_ok = [&](RcMapping m) {
    return m == RcMapping::UnsignedIdentity || m == RcMapping::UnsignedInvert;
  };
  for (const RcInput& in : c.final.in) {
    if (!final_ok(in.mapping)) {
      err = "final combiner allows only unsigned identity/invert mappings";
      return false;
    }
  }
  if (c.final.in[4].reg == RcRegister::V1R0Sum ||
      c.final.in[4].reg == RcRegister::EFProduct ||
      c.final.in[5].reg == RcRegister::V1R0Sum ||
      c.final.in[5].reg == RcRegister::EFProduct ||
      c.final.in[6].reg == RcRegister::V1R0Sum ||
      c.final.in[6].reg == RcRegister::EFProduct) {
    err = "final combiner E/F/G cannot be a pseudo register";
    return false;
  }
  if (c.final.in[0].reg == RcRegister::V1R0Sum) {
    err = "final combiner A cannot be the V1+Spare0 sum";
    return false;
  }
  return true;
}

}  // namespace

RcResult rc_evaluate(const RcConfig& config, const RcInputs& inputs) {
  RcResult result;
  std::string err;
  if (!validate(config, err)) {
    result.error = err;
    return result;
  }
  if (config.mux != RcMuxMode::Msb) {
    result.error = "NV2A MUX LSB mode is not reverse-engineered";
    return result;
  }

  Work w;
  w.primary = inputs.primary;
  w.secondary = inputs.secondary;
  w.spare0 = inputs.spare0;
  w.spare1 = inputs.spare1;
  w.tex = inputs.tex;

  for (uint32_t s = 0; s < config.num_stages; ++s) {
    const RcStage& st = config.stages[s];
    uint32_t cidx0 = config.unique_const0 ? s : 0;
    uint32_t cidx1 = config.unique_const1 ? s : 0;
    const float spare0_alpha = w.spare0[3];

    // RGB portion.
    Vec4 va = load_value(inputs, w, cidx0, cidx1, st.rgb[0].reg);
    Vec4 vb = load_value(inputs, w, cidx0, cidx1, st.rgb[1].reg);
    Vec4 vc = load_value(inputs, w, cidx0, cidx1, st.rgb[2].reg);
    Vec4 vd = load_value(inputs, w, cidx0, cidx1, st.rgb[3].reg);
    Vec3 a = rgb_of(st.rgb[0], va);
    Vec3 b = rgb_of(st.rgb[1], vb);
    Vec3 c = rgb_of(st.rgb[2], vc);
    Vec3 d = rgb_of(st.rgb[3], vd);

    auto dot3 = [](const Vec3& x, const Vec3& y) {
      float v = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
      return Vec3{v, v, v};
    };
    auto mul3 = [](const Vec3& x, const Vec3& y) {
      return Vec3{x[0] * y[0], x[1] * y[1], x[2] * y[2]};
    };
    auto add3 = [](const Vec3& x, const Vec3& y) {
      return Vec3{x[0] + y[0], x[1] + y[1], x[2] + y[2]};
    };

    Vec3 ab = st.rgb_out.ab_dot ? dot3(a, b) : mul3(a, b);
    Vec3 cd = st.rgb_out.cd_dot ? dot3(c, d) : mul3(c, d);
    Vec3 sum = st.rgb_out.mux_sum
                   ? (spare0_alpha >= 0.5f ? cd : ab)
                   : add3(ab, cd);

    Vec3 ab_s = scale_vec(ab, st.rgb_out.scale);
    Vec3 cd_s = scale_vec(cd, st.rgb_out.scale);
    Vec3 sum_s = scale_vec(sum, st.rgb_out.scale);

    write_rgb(w, st.rgb_out.ab, ab_s);
    write_rgb(w, st.rgb_out.cd, cd_s);
    write_rgb(w, st.rgb_out.sum, sum_s);
    if (st.rgb_out.ab_blue_to_alpha) {
      write_blue_to_alpha(w, st.rgb_out.ab, ab);
    }
    if (st.rgb_out.cd_blue_to_alpha) {
      write_blue_to_alpha(w, st.rgb_out.cd, cd);
    }

    // Alpha portion.
    float aa = alpha_of(st.alpha[0], va);
    float ba = alpha_of(st.alpha[1], vb);
    float ca = alpha_of(st.alpha[2], vc);
    float da = alpha_of(st.alpha[3], vd);
    float ab_a = aa * ba;
    float cd_a = ca * da;
    float sum_a =
        st.alpha_out.mux_sum ? (spare0_alpha >= 0.5f ? cd_a : ab_a)
                             : ab_a + cd_a;
    write_alpha(w, st.alpha_out.ab, scale_scalar(ab_a, st.alpha_out.scale));
    write_alpha(w, st.alpha_out.cd, scale_scalar(cd_a, st.alpha_out.scale));
    write_alpha(w, st.alpha_out.sum,
                scale_scalar(sum_a, st.alpha_out.scale));
  }

  // Final combiner.
  auto val = [&](RcRegister r) -> Vec4 {
    return load_value(inputs, w, kFinalConst, kFinalConst, r);
  };
  const RcFinal& fin = config.final;
  Vec3 e = rgb_of(fin.in[4], val(fin.in[4].reg));
  Vec3 f = rgb_of(fin.in[5], val(fin.in[5].reg));
  Vec3 ef{e[0] * f[0], e[1] * f[1], e[2] * f[2]};

  Vec3 v1{rc_map(RcMapping::UnsignedIdentity, w.secondary[0]),
          rc_map(RcMapping::UnsignedIdentity, w.secondary[1]),
          rc_map(RcMapping::UnsignedIdentity, w.secondary[2])};
  Vec3 r0{rc_map(RcMapping::UnsignedIdentity, w.spare0[0]),
          rc_map(RcMapping::UnsignedIdentity, w.spare0[1]),
          rc_map(RcMapping::UnsignedIdentity, w.spare0[2])};
  if (fin.complement_v1) {
    for (float& x : v1) x = rc_map(RcMapping::UnsignedInvert, x);
  }
  if (fin.complement_r0) {
    for (float& x : r0) x = rc_map(RcMapping::UnsignedInvert, x);
  }
  Vec3 v1r0{v1[0] + r0[0], v1[1] + r0[1], v1[2] + r0[2]};
  if (fin.clamp_sum) {
    for (float& x : v1r0) x = std::clamp(x, 0.0f, 1.0f);
  }

  auto final_val = [&](RcRegister r) -> Vec4 {
    if (r == RcRegister::EFProduct) {
      return {ef[0], ef[1], ef[2], 0.0f};
    }
    if (r == RcRegister::V1R0Sum) {
      return {v1r0[0], v1r0[1], v1r0[2], 0.0f};
    }
    return val(r);
  };

  Vec3 A = rgb_of(fin.in[0], final_val(fin.in[0].reg));
  Vec3 B = rgb_of(fin.in[1], final_val(fin.in[1].reg));
  Vec3 C = rgb_of(fin.in[2], final_val(fin.in[2].reg));
  Vec3 D = rgb_of(fin.in[3], final_val(fin.in[3].reg));
  float G = alpha_of(fin.in[6], final_val(fin.in[6].reg));

  for (int i = 0; i < 3; ++i) {
    // fcoc = D + (1 - A)*C + A*B, clamped to the framebuffer range.
    float v = D[i] + (1.0f - A[i]) * C[i] + A[i] * B[i];
    result.rgba[i] = std::clamp(std::min(v, 1.0f), 0.0f, 1.0f);
  }
  result.rgba[3] = std::clamp(G, 0.0f, 1.0f);
  result.ok = true;
  return result;
}

std::array<uint8_t, 4> rc_to_rgba8(const std::array<float, 4>& c) {
  std::array<uint8_t, 4> out{};
  for (int i = 0; i < 4; ++i) {
    float v = std::clamp(c[i], 0.0f, 1.0f);
    out[i] = static_cast<uint8_t>(std::lround(v * 255.0f));
  }
  return out;
}
