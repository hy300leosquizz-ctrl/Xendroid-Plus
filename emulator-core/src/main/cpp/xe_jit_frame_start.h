#ifndef XENDROID_XE_JIT_FRAME_START_H_
#define XENDROID_XE_JIT_FRAME_START_H_

#include <algorithm>
#include <cstdint>

namespace xe {
namespace hid {
namespace android {

// Computes the next input-poll hold duration (microseconds) for jit_frame_start.
// Mode: 0 = Off, 1 = Conservative (10 ms margin, 12 ms cap), 2 = Auto (6 ms margin, 20 ms cap).
//
// Invariants (fixes #144 regression):
// - Never freezes a previous delay_us when the presenter timestamp (swap_ns) is
//   behind or equal to prev_work_start_ns (pipelined frames where guest VdSwap
//   ran before the previous frame was presented): resets delay to 0 instead of
//   skipping backoff and sleeping up to 20 ms every frame.
// - Never holds on >= 56 FPS cadences (period_us < 18000) or stalls (> 100000 us).
// - Caps max_hold_us to at most (period_us - margin_us) so hold never exceeds
//   available frame budget.
inline int64_t ComputeJitFrameStartDelayUs(int32_t mode, uint64_t swap_ns,
                                           uint64_t prev_swap_ns,
                                           uint64_t prev_work_start_ns,
                                           int64_t current_delay_us,
                                           uint32_t& calm_frames) {
  if (mode <= 0) {
    calm_frames = 0;
    return 0;
  }
  if (prev_swap_ns == 0 || prev_work_start_ns == 0 || swap_ns <= prev_swap_ns ||
      swap_ns <= prev_work_start_ns) {
    calm_frames = 8;
    return 0;
  }

  const int64_t period_us =
      static_cast<int64_t>((swap_ns - prev_swap_ns) / 1000);
  const int64_t active_work_us =
      static_cast<int64_t>((swap_ns - prev_work_start_ns) / 1000);

  // Only pace steady ~10..55 Hz cadences (e.g. 30/40 FPS titles). High-rate
  // 60+ FPS games (<18 ms) have no safe hold budget, and >100 ms indicates loads.
  if (period_us < 18000 || period_us > 100000) {
    calm_frames = 8;
    return 0;
  }

  const int64_t margin_us = (mode == 1) ? 10000 : 6000;
  const int64_t mode_max_hold_us = (mode == 1) ? 12000 : 20000;
  const int64_t max_hold_us = std::min<int64_t>(
      mode_max_hold_us, std::max<int64_t>(0, period_us - margin_us));

  int64_t delay_us = std::clamp<int64_t>(current_delay_us, 0, max_hold_us);
  const int64_t remaining_slack_us = period_us - active_work_us - delay_us;
  if (remaining_slack_us < margin_us) {
    delay_us -= (margin_us - remaining_slack_us);
    calm_frames = 8;
  } else if (calm_frames > 0) {
    --calm_frames;
  } else {
    delay_us += std::min<int64_t>((remaining_slack_us - margin_us) / 8, 1000);
  }
  return std::clamp<int64_t>(delay_us, 0, max_hold_us);
}

}  // namespace android
}  // namespace hid
}  // namespace xe

#endif  // XENDROID_XE_JIT_FRAME_START_H_
