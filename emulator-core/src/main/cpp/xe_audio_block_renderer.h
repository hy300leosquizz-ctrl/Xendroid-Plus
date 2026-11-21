// The Android audio drivers' per-block processing, free of AAudio and OpenSL ES so the host
// tests drive it with a real signal: a guest block becomes host stereo (the 5.1 fold of the
// game's frames, or the media player's own stereo), its gain is applied and bounded, a block
// that did not arrive in time is concealed without a step, and the output is resampled at the
// rate control's rate. Callback thread only: no allocation, no lock, no wait.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "xenia/apu/conversion.h"

namespace xe {
namespace apu {

class AudioBlockRenderer {
 public:
  // The first frames of a block after a concealed gap ramp in from silence (~1.3 ms at 48 kHz).
  static constexpr uint32_t kFadeInFrames = 64;

  // frames: per guest block. guest_channels: 6 for the game's frames (sequential big endian
  // 5.1), 2 for the media player's (interleaved host endian stereo).
  AudioBlockRenderer(uint32_t frames, uint32_t guest_channels)
      : frames_(frames), guest_channels_(guest_channels), block_(size_t(frames) * 2, 0.0f),
        pos_(frames) {}

  struct Result {
    uint32_t blocks = 0;   // guest blocks taken in
    uint32_t gaps = 0;     // blocks concealed because none was queued
    uint32_t clipped = 0;  // samples bounded to full scale
  };

  // `count` stereo frames into `out`, consuming `rate` source frames per output frame (1 = as
  // they are). next_block() returns the next guest block, or nullptr when none is queued;
  // done(block) takes a block back once it is converted. `gain` scales the samples, which are
  // then bounded to [-1, 1].
  template <typename Next, typename Done>
  Result Render(float* out, int32_t count, float rate, float gain, Next&& next_block,
                Done&& done) {
    Result result;
    if (count <= 0) {
      return result;
    }

    // Unity-rate memcpy fast-path when phase is integer-aligned (frac_ == 0.0f on initial
    // call or 1.0f on steady-state unity callbacks). Preserves the exact 2-sample resampler
    // pipeline state (prev_l_/r_, cur_l_/r_, pos_, frac_) so switching into or out of
    // dynamic rate control is 100% seamless and bit-identical to the per-sample loop.
    if (rate == 1.0f && (frac_ == 0.0f || frac_ == 1.0f)) {
      int32_t i = 0;
      if (frac_ == 0.0f) {
        out[0] = prev_l_;
        out[1] = prev_r_;
        frac_ = 1.0f;
        i = 1;
      }
      while (i < count) {
        if (pos_ >= frames_) {
          Load(next_block, done, gain, result);
        }
        const uint32_t avail =
            std::min<uint32_t>(frames_ - pos_, static_cast<uint32_t>(count - i));
        out[size_t(i) * 2] = cur_l_;
        out[size_t(i) * 2 + 1] = cur_r_;
        if (avail == 1) {
          prev_l_ = cur_l_;
          prev_r_ = cur_r_;
          cur_l_ = block_[size_t(pos_) * 2];
          cur_r_ = block_[size_t(pos_) * 2 + 1];
        } else {
          std::memcpy(&out[size_t(i + 1) * 2], &block_[size_t(pos_) * 2],
                      size_t(avail - 1) * 2 * sizeof(float));
          const size_t last = size_t(pos_ + avail - 1) * 2;
          prev_l_ = block_[last - 2];
          prev_r_ = block_[last - 1];
          cur_l_ = block_[last];
          cur_r_ = block_[last + 1];
        }
        pos_ += avail;
        i += static_cast<int32_t>(avail);
      }
      return result;
    }

    for (int32_t i = 0; i < count; ++i) {
      while (frac_ >= 1.0f) {
        frac_ -= 1.0f;
        if (pos_ >= frames_) {
          Load(next_block, done, gain, result);
        }
        prev_l_ = cur_l_;
        prev_r_ = cur_r_;
        cur_l_ = block_[size_t(pos_) * 2];
        cur_r_ = block_[size_t(pos_) * 2 + 1];
        ++pos_;
      }
      out[size_t(i) * 2] = prev_l_ + frac_ * (cur_l_ - prev_l_);
      out[size_t(i) * 2 + 1] = prev_r_ + frac_ * (cur_r_ - prev_r_);
      frac_ += rate;
    }
    return result;
  }

 private:
  template <typename Next, typename Done>
  void Load(Next& next_block, Done& done, float gain, Result& result) {
    const float* block = next_block();
    pos_ = 0;
    if (!block) {
      ++result.gaps;
      Conceal();
      return;
    }
    if (guest_channels_ == 6) {
      conversion::sequential_6_BE_to_interleaved_2_LE(block_.data(), block, frames_);
    } else {
      std::memcpy(block_.data(), block, block_.size() * sizeof(float));
    }
    done(block);
    ++result.blocks;
    result.clipped += GainAndClamp(gain);
    if (fade_in_) {
      FadeIn();
      fade_in_ = false;
    }
    continues_ = true;
  }

  // The 5.1 fold peaks at ~2.9x a channel, so loud content can pass full scale. Bounded here
  // so the result is the same on every device, and counted: the fix for persistent clipping is
  // less gain, not a harder limit. A NaN from the guest passes both bounds: it is silenced (and
  // counted) instead of reaching the device and the next gap's concealment.
  uint32_t GainAndClamp(float gain) {
    uint32_t clipped = 0;
    float* const data = block_.data();
    const size_t n = block_.size();
    size_t i = 0;

#if defined(__ARM_NEON) || defined(__aarch64__)
    const float32x4_t gain_v = vdupq_n_f32(gain);
    const float32x4_t one_v = vdupq_n_f32(1.0f);
    const float32x4_t neg_one_v = vdupq_n_f32(-1.0f);
    const float32x4_t zero_v = vdupq_n_f32(0.0f);
    uint32x4_t clip_acc = vdupq_n_u32(0);

    for (; i + 4 <= n; i += 4) {
      const float32x4_t s = vmulq_f32(vld1q_f32(data + i), gain_v);
      const uint32x4_t not_nan = vceqq_f32(s, s);
      const uint32x4_t in_range =
          vandq_u32(vcgeq_f32(s, neg_one_v), vcleq_f32(s, one_v));
      clip_acc = vaddq_u32(clip_acc, vshrq_n_u32(vmvnq_u32(in_range), 31));
      const float32x4_t finite_or_zero = vbslq_f32(not_nan, s, zero_v);
      const float32x4_t bounded =
          vminq_f32(vmaxq_f32(finite_or_zero, neg_one_v), one_v);
      vst1q_f32(data + i, bounded);
    }
#if defined(__aarch64__)
    clipped += vaddvq_u32(clip_acc);
#else
    const uint32x2_t pair_sum =
        vadd_u32(vget_low_u32(clip_acc), vget_high_u32(clip_acc));
    clipped += vget_lane_u32(vpadd_u32(pair_sum, pair_sum), 0);
#endif
#endif

    for (; i < n; ++i) {
      float s = data[i] * gain;
      if (s > 1.0f) {
        s = 1.0f;
        ++clipped;
      } else if (s < -1.0f) {
        s = -1.0f;
        ++clipped;
      } else if (std::isnan(s)) {
        s = 0.0f;
        ++clipped;
      }
      data[i] = s;
    }
    return clipped;
  }

  // A block that did not arrive. The first one after real audio continues the last block
  // mirrored in time - from its last frame, so there is no step where it starts - and fades
  // it to silence across the block (a raised cosine); the next missing ones are silence, and
  // the next real block ramps in from silence.
  void Conceal() {
    fade_in_ = true;
    if (!continues_) {
      std::fill(block_.begin(), block_.end(), 0.0f);
      return;
    }
    continues_ = false;
    for (uint32_t f = 0, g = frames_ - 1; f < g; ++f, --g) {
      std::swap(block_[size_t(f) * 2], block_[size_t(g) * 2]);
      std::swap(block_[size_t(f) * 2 + 1], block_[size_t(g) * 2 + 1]);
    }
    const float step = frames_ > 1 ? 3.14159265f / float(frames_ - 1) : 0.0f;
    for (uint32_t f = 0; f < frames_; ++f) {
      const float fade = 0.5f + 0.5f * std::cos(step * float(f));
      block_[size_t(f) * 2] *= fade;
      block_[size_t(f) * 2 + 1] *= fade;
    }
  }

  void FadeIn() {
    const uint32_t ramp = std::min(kFadeInFrames, frames_);
    for (uint32_t f = 0; f < ramp; ++f) {
      const float g = float(f) / float(ramp);
      block_[size_t(f) * 2] *= g;
      block_[size_t(f) * 2 + 1] *= g;
    }
  }

  const uint32_t frames_;
  const uint32_t guest_channels_;
  // The block being played, host stereo after gain; pos_ = its frames already consumed.
  std::vector<float> block_;
  uint32_t pos_;
  // The last block was real audio, which a gap can continue (mirrored) from.
  bool continues_ = false;
  // A gap was concealed: the next real block ramps in.
  bool fade_in_ = false;
  // Linear resampler state.
  float frac_ = 0.0f;
  float prev_l_ = 0.0f, prev_r_ = 0.0f;
  float cur_l_ = 0.0f, cur_r_ = 0.0f;
};

}  // namespace apu
}  // namespace xe
