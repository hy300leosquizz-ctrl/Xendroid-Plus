#include <bit>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {

// Reference implementation of the previous 19-op software RoundTowardZero
// (including XenDroid's pure-float 1 - 2^-24 ULP step).
float OldRoundTowardZero(float rounded, float error, float a, float b) {
  volatile float r = rounded;
  volatile float e = error;
  // SPIR-V uses OpFOrdNotEqual (ordered: false if either operand is NaN),
  // whereas C++ operator!= is unordered (true if NaN).
  bool e_ord_ne_zero = !std::isnan(e) && (e != 0.0f);
  bool r_ord_ne_zero = !std::isnan(r) && (r != 0.0f);
  bool step = e_ord_ne_zero && ((e < 0.0f) != (r < 0.0f)) && r_ord_ne_zero;
  volatile float factor = step ? (1.0f - 0x1.0p-24f) : 1.0f;
  volatile float result = r * factor;
  bool a_ord_ne_inf = !std::isnan(a) && (std::fabs(a) != INFINITY);
  bool b_ord_ne_inf = !std::isnan(b) && (std::fabs(b) != INFINITY);
  bool overflowed = (std::fabs(r) == INFINITY) && a_ord_ne_inf && b_ord_ne_inf;
  if (overflowed) {
    return std::fmin(std::fmax(float(r), -FLT_MAX), FLT_MAX);
  }
  return result;
}

// Streamlined RoundTowardZero matching spirv_shader_translator_alu.cc.
float NewRoundTowardZero(float rounded, float error, float a, float b,
                         bool same_operand = false) {
  volatile float r = rounded;
  volatile float e = error;
  bool rounded_neg = r < 0.0f;
  volatile float neg_e = -e;
  float signed_error = rounded_neg ? float(neg_e) : float(e);
  bool step = signed_error < 0.0f;
  volatile float factor = step ? (1.0f - 0x1.0p-24f) : 1.0f;
  volatile float result = r * factor;
  float abs_max = same_operand ? std::fabs(a)
                               : std::fmax(std::fabs(a), std::fabs(b));
  bool overflowed = (std::fabs(r) == INFINITY) && (abs_max < INFINITY);
  float clamped_overflow = rounded_neg ? -FLT_MAX : FLT_MAX;
  return overflowed ? clamped_overflow : float(result);
}

// Infinite-precision reference for float32 RTZ addition (handles exponent gaps
// >= 53 bits where 53-bit IEEE-754 double would round away the smaller addend).
float ExactRefAddRtz(float a, float b) {
  volatile float sum = a + b;
  if (std::isnan(sum)) {
    return sum;
  }
  if (std::isinf(sum) && std::isfinite(a) && std::isfinite(b)) {
    return std::copysign(FLT_MAX, sum);
  }
  if (!std::isfinite(sum) || sum == 0.0f) {
    return sum;
  }
  double exact = double(a) + double(b);
  bool rounded_away = false;
  if (exact == double(a) && b != 0.0f) {
    // |b| < 0.5 ulp_double(a), so sum == a and the exact real sum is strictly
    // closer to zero iff a and b have opposite signs.
    rounded_away = (a > 0.0f && b < 0.0f) || (a < 0.0f && b > 0.0f);
  } else if (exact == double(b) && a != 0.0f) {
    rounded_away = (b > 0.0f && a < 0.0f) || (b < 0.0f && a > 0.0f);
  } else {
    rounded_away = std::fabs(double(sum)) > std::fabs(exact);
  }
  if (rounded_away) {
    volatile float factor = 1.0f - 0x1.0p-24f;
    volatile float stepped = sum * factor;
    return stepped;
  }
  return sum;
}

// Exact double-precision reference for float32 RTZ multiplication (the product
// of any two finite float32 significands has 48 bits <= 53 double bits).
float ExactRefMulRtz(float a, float b) {
  volatile float prod = a * b;
  if (std::isnan(prod)) {
    return prod;
  }
  if (std::isinf(prod) && std::isfinite(a) && std::isfinite(b)) {
    return std::copysign(FLT_MAX, prod);
  }
  if (!std::isfinite(prod) || prod == 0.0f) {
    return prod;
  }
  double exact = double(a) * double(b);
  if (std::fabs(double(prod)) > std::fabs(exact)) {
    volatile float factor = 1.0f - 0x1.0p-24f;
    volatile float stepped = prod * factor;
    return stepped;
  }
  return prod;
}

float SimAluAdd(float a, float b, bool use_new) {
  volatile float va = a, vb = b;
  volatile float sum = va + vb;
  volatile float b_rounded = sum - va;
  volatile float sum_minus_br = sum - b_rounded;
  volatile float err_a = va - sum_minus_br;
  volatile float err_b = vb - b_rounded;
  volatile float error = err_a + err_b;
  return use_new ? NewRoundTowardZero(sum, error, a, b, false)
                 : OldRoundTowardZero(sum, error, a, b);
}

void VeltkampSplit(float value, float& high, float& low) {
  volatile float v = value;
  volatile float c = 4097.0f;
  volatile float scaled = v * c;
  volatile float diff = scaled - v;
  volatile float h = scaled - diff;
  volatile float l = v - h;
  high = h;
  low = l;
}

float SimAluMultiply(float a, float b, bool use_new, bool same_operand) {
  volatile float va = a, vb = b;
  volatile float product = va * vb;
  float a_high, a_low, b_high, b_low;
  VeltkampSplit(a, a_high, a_low);
  if (use_new && same_operand) {
    b_high = a_high;
    b_low = a_low;
  } else {
    VeltkampSplit(b, b_high, b_low);
  }
  volatile float hh = a_high * b_high;
  volatile float e0 = hh - product;
  volatile float hl = a_high * b_low;
  volatile float e1 = e0 + hl;
  volatile float lh = a_low * b_high;
  volatile float e2 = e1 + lh;
  volatile float ll = a_low * b_low;
  volatile float error = e2 + ll;
  return use_new ? NewRoundTowardZero(product, error, a, b, same_operand)
                 : OldRoundTowardZero(product, error, a, b);
}

bool SameFloatBitsOrBothNan(float x, float y) {
  if (std::isnan(x) && std::isnan(y)) {
    return true;
  }
  return std::bit_cast<uint32_t>(x) == std::bit_cast<uint32_t>(y);
}

void TestStreamlinedRtzMatchesOldAndExactReference() {
  std::vector<float> interesting = {
      0.0f,
      -0.0f,
      1.0f,
      -1.0f,
      0.5f,
      -0.5f,
      2.0f,
      -2.0f,
      1.0f - 0x1.0p-24f,
      -(1.0f - 0x1.0p-24f),
      1.0f + 0x1.0p-23f,
      -(1.0f + 0x1.0p-23f),
      0x1.fffffep+0f,
      -0x1.fffffep+0f,
      0x1.0p-24f,
      -0x1.0p-24f,
      0x1.0p-25f,
      -0x1.0p-25f,
      0x1.0p-126f,
      -0x1.0p-126f,
      0x1.fffffep-126f,
      -0x1.fffffep-126f,
      0x1.0p+60f,
      -0x1.0p+60f,
      0x1.fffffep+126f,
      -0x1.fffffep+126f,
      FLT_MAX,
      -FLT_MAX,
      INFINITY,
      -INFINITY,
      std::numeric_limits<float>::quiet_NaN(),
  };

  for (int exp = -120; exp <= 120; exp += 15) {
    float base = std::ldexp(1.0f, exp);
    float ulp = std::ldexp(1.0f, exp - 23);
    interesting.push_back(base + ulp);
    interesting.push_back(-(base + ulp));
    interesting.push_back(base + 3.0f * ulp);
    interesting.push_back(-(base + 3.0f * ulp));
    interesting.push_back(std::ldexp(1.0f, exp - 24));
    interesting.push_back(-std::ldexp(1.0f, exp - 24));
  }

  for (float a : interesting) {
    // Squaring fast-path (same_operand == true).
    float old_sq = SimAluMultiply(a, a, false, false);
    float new_sq = SimAluMultiply(a, a, true, true);
    assert(SameFloatBitsOrBothNan(old_sq, new_sq));

    // Exact swizzle-constant fast-paths (0.0 and +-1.0).
    volatile float va = a;
    volatile float zero_pos = 0.0f;
    volatile float zero_neg = -0.0f;
    volatile float one_pos = 1.0f;
    volatile float one_neg = -1.0f;
    assert(SameFloatBitsOrBothNan(SimAluAdd(a, 0.0f, true), va + zero_pos));
    assert(SameFloatBitsOrBothNan(SimAluAdd(a, -0.0f, true), va + zero_neg));
    assert(SameFloatBitsOrBothNan(SimAluMultiply(a, 0.0f, true, false),
                                  va * zero_pos));
    assert(SameFloatBitsOrBothNan(SimAluMultiply(a, -0.0f, true, false),
                                  va * zero_neg));
    assert(SameFloatBitsOrBothNan(SimAluMultiply(a, 1.0f, true, false),
                                  va * one_pos));
    assert(SameFloatBitsOrBothNan(SimAluMultiply(a, -1.0f, true, false),
                                  va * one_neg));

    for (float b : interesting) {
      float old_add = SimAluAdd(a, b, false);
      float new_add = SimAluAdd(a, b, true);
      assert(SameFloatBitsOrBothNan(old_add, new_add));
      assert(SameFloatBitsOrBothNan(new_add, ExactRefAddRtz(a, b)));

      float old_mul = SimAluMultiply(a, b, false, false);
      float new_mul = SimAluMultiply(a, b, true, false);
      assert(SameFloatBitsOrBothNan(old_mul, new_mul));
      if (std::fabs(a) < 0x1.0p+114f && std::fabs(b) < 0x1.0p+114f &&
          std::fabs(double(a) * double(b)) >= 0x1.0p-102) {
        assert(SameFloatBitsOrBothNan(new_mul, ExactRefMulRtz(a, b)));
      }
    }
  }

  // Deterministic pseudo-random sweep of 50,000 normal float pairs.
  uint64_t rng = 0x9E3779B97F4A7C15ULL;
  auto next_normal_float = [&]() -> float {
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    uint64_t r = rng * 0x2545F4914F6CDD1DULL;
    uint32_t sign = uint32_t((r >> 31) & 1u) << 31;
    uint32_t biased_exp = 20u + uint32_t(r % 201u);
    uint32_t mantissa = uint32_t((r >> 8) & 0x7FFFFFu);
    return std::bit_cast<float>(sign | (biased_exp << 23) | mantissa);
  };
  for (int i = 0; i < 50000; ++i) {
    float a = next_normal_float();
    float b = next_normal_float();
    float old_add = SimAluAdd(a, b, false);
    float new_add = SimAluAdd(a, b, true);
    assert(SameFloatBitsOrBothNan(old_add, new_add));
    assert(SameFloatBitsOrBothNan(new_add, ExactRefAddRtz(a, b)));

    float old_mul = SimAluMultiply(a, b, false, false);
    float new_mul = SimAluMultiply(a, b, true, false);
    assert(SameFloatBitsOrBothNan(old_mul, new_mul));
    if (std::fabs(double(a) * double(b)) >= 0x1.0p-102) {
      assert(SameFloatBitsOrBothNan(new_mul, ExactRefMulRtz(a, b)));
    }
  }
}

// Mirrors RenderTargetCache::ColorOverlapsDepthStencil (9da693480).
bool ColorOverlapsDepthStencilModel(
    xe::gpu::xenos::ColorRenderTargetFormat color_format,
    uint32_t color_keep_mask_low, uint32_t color_keep_mask_high,
    xe::gpu::reg::RB_DEPTHCONTROL depth_control) {
  uint32_t depth_stencil_used_bits = 0;
  if (depth_control.z_enable) {
    depth_stencil_used_bits |= 0xFFFFFF00u;
  }
  if (depth_control.stencil_enable) {
    depth_stencil_used_bits |= 0x000000FFu;
  }
  uint32_t color_written_bits = ~color_keep_mask_low;
  if (xe::gpu::xenos::IsColorRenderTargetFormat64bpp(color_format)) {
    color_written_bits |= ~color_keep_mask_high;
  }
  return (color_written_bits & depth_stencil_used_bits) != 0;
}

void TestEdramColorDepthAliasOverlap() {
  using xe::gpu::reg::RB_DEPTHCONTROL;
  using xe::gpu::xenos::ColorRenderTargetFormat;

  RB_DEPTHCONTROL depth_only = {};
  depth_only.z_enable = 1;
  depth_only.z_write_enable = 0;
  depth_only.stencil_enable = 0;

  RB_DEPTHCONTROL stencil_only = {};
  stencil_only.z_enable = 0;
  stencil_only.stencil_enable = 1;

  RB_DEPTHCONTROL both_enabled = {};
  both_enabled.z_enable = 1;
  both_enabled.stencil_enable = 1;

  // Forza Horizon interior/clouds case: 8_8_8_8 writing only channel R [7:0]
  // (keep_mask_low = 0xFFFFFF00) while reading depth [31:8].
  assert(!ColorOverlapsDepthStencilModel(ColorRenderTargetFormat::k_8_8_8_8,
                                         0xFFFFFF00u, ~0u, depth_only));
  assert(ColorOverlapsDepthStencilModel(ColorRenderTargetFormat::k_8_8_8_8,
                                        0xFFFFFF00u, ~0u, stencil_only));
  assert(ColorOverlapsDepthStencilModel(ColorRenderTargetFormat::k_8_8_8_8,
                                        0xFFFFFF00u, ~0u, both_enabled));

  // Writing GBA [31:8] (keep_mask_low = 0x000000FF) overlaps depth, not stencil.
  assert(ColorOverlapsDepthStencilModel(ColorRenderTargetFormat::k_8_8_8_8,
                                        0x000000FFu, ~0u, depth_only));
  assert(!ColorOverlapsDepthStencilModel(ColorRenderTargetFormat::k_8_8_8_8,
                                         0x000000FFu, ~0u, stencil_only));

  // 2_10_10_10 channel R writes [9:0], touching depth bits [9:8].
  assert(ColorOverlapsDepthStencilModel(ColorRenderTargetFormat::k_2_10_10_10,
                                        ~0x3FFu, ~0u, depth_only));

  // 64bpp format writing only high dword BA [63:32] conservatively overlaps
  // 32bpp depth samples.
  assert(ColorOverlapsDepthStencilModel(
      ColorRenderTargetFormat::k_16_16_16_16, ~0u, 0xFFFF0000u, depth_only));
  // Disabled color write (all keep bits set) never overlaps.
  assert(!ColorOverlapsDepthStencilModel(
      ColorRenderTargetFormat::k_16_16_16_16, ~0u, ~0u, both_enabled));
}

void TestCentroidBarycentricClamping() {
  auto clamp_barycentrics = [](float bx, float by, float bz, float& out_y,
                               float& out_z) {
    float cx = std::fmax(bx, 0.0f);
    float cy = std::fmax(by, 0.0f);
    float cz = std::fmax(bz, 0.0f);
    bool outside = (bx < 0.0f) || (by < 0.0f) || (bz < 0.0f);
    float sum = (cx + cy) + cz;
    out_y = outside ? (cy / sum) : by;
    out_z = outside ? (cz / sum) : bz;
  };

  // Inside triangle: weights stay bit-exact.
  float cy = 0.0f, cz = 0.0f;
  clamp_barycentrics(0.25f, 0.375f, 0.375f, cy, cz);
  assert(cy == 0.375f && cz == 0.375f);

  // MSAA pixel center outside edge: clamped weights stay inside [0, 1] and sum
  // to <= 1, preventing extrapolation past triangle bounds.
  clamp_barycentrics(-0.25f, 0.75f, 0.50f, cy, cz);
  assert(cy >= 0.0f && cz >= 0.0f && (cy + cz) <= 1.000001f);
  assert(std::fabs(cy - 0.60f) < 1e-6f && std::fabs(cz - 0.40f) < 1e-6f);

  // Constant vertex attribute v0 == v1 == v2 == V stays exact even on edge.
  float v = 0.1234567f;
  float interp = v + (v - v) * cy + (v - v) * cz;
  assert(interp == v);
}

}  // namespace

int main() {
  TestStreamlinedRtzMatchesOldAndExactReference();
  TestEdramColorDepthAliasOverlap();
  TestCentroidBarycentricClamping();
  std::puts("gpu_compat_perf_test: ok");
  return 0;
}
