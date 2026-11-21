/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef xendroid_XE_ANDROID_INPUT_DRIVER_H
#define xendroid_XE_ANDROID_INPUT_DRIVER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/hid/input_driver.h"
#include "xenia/ui/virtual_key.h"

DECLARE_int32(jit_frame_start);

namespace xe {
namespace hid {
namespace android {

class AndroidInputDriver final : public InputDriver {
 public:
  // Player slots P1..P4 (I01-I03). Slot 0 is always present: the
  // on-screen pad and the first physical controller feed it. The
  // frontend connects slots 1-3 as more controllers appear.
  static constexpr size_t kSlotCount = 4;

  struct KeyStatus {
    ui::VirtualKey id;
    bool pressed;
    int value;
    // Pressed as keystrokes see it: an analog trigger past XInput's
    // threshold (#97), the key's own state otherwise.
    bool stroke;
  };

  struct Slot {
    std::array<KeyStatus, 24> keys;
    std::array<KeyStatus, 24> prev_keys;
    uint32_t mask = 0;
    uint32_t packet_number = 1;
    bool connected = false;
    std::string name;
  };

  // Deliberately not the global critical region: analog axes push a
  // sample per motion event and nothing here needs its semantics.
  std::mutex key_status_mutex_;
  std::array<Slot, kSlotCount> slots_;
  // Guest rumble per slot (I04): left << 16 | right motor speed.
  std::array<std::atomic<uint32_t>, kSlotCount> rumble_{};

  explicit AndroidInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~AndroidInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  InputType GetInputType() const override;
  std::vector<InputDeviceInfo> EnumerateDevices() override;
  void OnKey(int key_index, bool pressed, short value) {
    OnKey(0, key_index, pressed, value);
  }
  void OnKey(size_t slot, int key_index, bool pressed, short value);
  void SetSlotConnected(size_t slot, bool connected, const std::string& name);
  uint32_t Rumble(size_t slot) const;

 private:
  bool SlotConnected(uint32_t slot);
  // Just-in-Time Frame Start (ported cleanly from XDroid360 9b41ad8d2,
  // BSD-3-Clause): holds the first controller read of each guest frame so
  // input is sampled right before the guest CPU needs to start building the
  // next frame (-15 to -20 ms input latency at 30 FPS without FPS loss).
  // Automatically disabled whenever Frame Generation paces presentation.
  void HoldForJitFrameStart();

  std::atomic<uint64_t> jit_held_swap_{~uint64_t(0)};
  std::atomic<uint64_t> jit_prev_swap_ns_{0};
  std::atomic<uint64_t> jit_last_work_start_ns_{0};
  std::atomic<uint32_t> jit_delay_us_{0};
  std::atomic<uint32_t> jit_calm_frames_{0};
};

}  // namespace android
}  // namespace hid
}  // namespace xe

#endif  // xendroid_XE_ANDROID_INPUT_DRIVER_H
