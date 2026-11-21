#include "xe_jit_frame_start.h"

#include <cassert>
#include <cstdint>

namespace {

using xe::hid::android::ComputeJitFrameStartDelayUs;

constexpr uint64_t kMs = 1'000'000ULL;

void TestOffModeAlwaysZero() {
  uint32_t calm = 8;
  assert(ComputeJitFrameStartDelayUs(0, 100 * kMs, 66 * kMs, 80 * kMs, 15000,
                                     calm) == 0);
  assert(calm == 0);
}

void TestPipelinedPresentLagResetsStaleDelay() {
  // Regression #144: when guest VdSwap outruns host presentation, swap_ns can
  // stay behind prev_work_start_ns. A stale 20 ms hold must reset to 0 instead
  // of freezing at 20 ms every frame.
  uint32_t calm = 0;
  int64_t delay = ComputeJitFrameStartDelayUs(
      2, /*swap_ns=*/100 * kMs, /*prev_swap_ns=*/67 * kMs,
      /*prev_work_start_ns=*/105 * kMs, /*current_delay_us=*/20000, calm);
  assert(delay == 0);
  assert(calm == 8);

  calm = 0;
  delay = ComputeJitFrameStartDelayUs(
      2, /*swap_ns=*/100 * kMs, /*prev_swap_ns=*/100 * kMs,
      /*prev_work_start_ns=*/80 * kMs, /*current_delay_us=*/20000, calm);
  assert(delay == 0);
  assert(calm == 8);
}

void TestHighFramerateNeverHolds() {
  // 60 FPS cadence (16.667 ms) has no safe hold budget (e.g. COD MW2 #144).
  uint32_t calm = 0;
  const int64_t delay = ComputeJitFrameStartDelayUs(
      2, /*swap_ns=*/100'000'000ULL, /*prev_swap_ns=*/83'333'000ULL,
      /*prev_work_start_ns=*/95'000'000ULL, /*current_delay_us=*/10000, calm);
  assert(delay == 0);
  assert(calm == 8);
}

void Test30FpsRampAndImmediateBackoff() {
  // 30 FPS (33.333 ms) with 12 ms active work: after calm reaches 0, delay
  // slews upward while respecting the 20 ms mode cap, and backs off immediately
  // when work rises to 28 ms (slack < 6 ms).
  uint32_t calm = 0;
  int64_t delay = 0;
  for (int i = 0; i < 60; ++i) {
    delay = ComputeJitFrameStartDelayUs(
        2, /*swap_ns=*/100'000'000ULL, /*prev_swap_ns=*/66'667'000ULL,
        /*prev_work_start_ns=*/88'000'000ULL, delay, calm);
    assert(delay >= 0 && delay <= 20000);
  }
  assert(delay > 10000);
  assert(delay <= 20000);

  const int64_t backed_off = ComputeJitFrameStartDelayUs(
      2, /*swap_ns=*/100'000'000ULL, /*prev_swap_ns=*/66'667'000ULL,
      /*prev_work_start_ns=*/72'000'000ULL, delay, calm);
  assert(backed_off < delay);
  assert(calm == 8);
}

void TestLoadingStallResetsHold() {
  uint32_t calm = 0;
  const int64_t delay = ComputeJitFrameStartDelayUs(
      2, /*swap_ns=*/250 * kMs, /*prev_swap_ns=*/100 * kMs,
      /*prev_work_start_ns=*/230 * kMs, /*current_delay_us=*/15000, calm);
  assert(delay == 0);
  assert(calm == 8);
}

}  // namespace

int main() {
  TestOffModeAlwaysZero();
  TestPipelinedPresentLagResetsStaleDelay();
  TestHighFramerateNeverHolds();
  Test30FpsRampAndImmediateBackoff();
  TestLoadingStallResetsHold();
  return 0;
}
