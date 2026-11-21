#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Mirrors Shader::Translation synchronization primitives in gpu/shader.h (#123).
struct MockTranslation {
  std::atomic<bool> is_valid{false};
  std::atomic<bool> is_translated{false};
  std::atomic_flag claimed = ATOMIC_FLAG_INIT;

  bool TryClaimTranslation() {
    return !claimed.test_and_set(std::memory_order_acq_rel);
  }

  void WaitForTranslation() const {
    while (!is_translated.load(std::memory_order_acquire)) {
      is_translated.wait(false, std::memory_order_acquire);
    }
  }

  void PublishTranslation(bool valid) {
    is_valid.store(valid, std::memory_order_relaxed);
    is_translated.store(true, std::memory_order_release);
    is_translated.notify_all();
  }
};

void RunConcurrencyCase(bool early_bail_out) {
  MockTranslation tr;
  std::atomic<int> winners{0};
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> losers_observed_valid{0};
  std::atomic<int> losers_observed_invalid{0};

  auto worker = [&]() {
    ready.fetch_add(1, std::memory_order_relaxed);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    if (tr.TryClaimTranslation()) {
      winners.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      tr.PublishTranslation(!early_bail_out);
    } else {
      tr.WaitForTranslation();
      assert(tr.is_translated.load(std::memory_order_acquire));
      if (tr.is_valid.load(std::memory_order_relaxed)) {
        losers_observed_valid.fetch_add(1, std::memory_order_relaxed);
      } else {
        losers_observed_invalid.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(worker);
  }
  while (ready.load(std::memory_order_relaxed) < 4) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0);
  assert(elapsed.count() < 500);
  assert(winners.load() == 1);
  if (early_bail_out) {
    assert(losers_observed_valid.load() == 0);
    assert(losers_observed_invalid.load() == 3);
  } else {
    assert(losers_observed_valid.load() == 3);
    assert(losers_observed_invalid.load() == 0);
  }
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.is_open());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void VerifySources(const std::filesystem::path& xenia_src) {
  const std::string shader_h = ReadAll(xenia_src / "gpu" / "shader.h");
  assert(shader_h.find("void WaitForTranslation() const") != std::string::npos);
  assert(shader_h.find("is_translated_.wait(false,") != std::string::npos);
  assert(shader_h.find("void PublishTranslation(bool valid)") !=
         std::string::npos);
  assert(shader_h.find("is_translated_.notify_all();") != std::string::npos);

  const std::string translator_cc =
      ReadAll(xenia_src / "gpu" / "shader_translator.cc");
  assert(translator_cc.find("translation.PublishTranslation(false);") !=
         std::string::npos);
  assert(translator_cc.find("translation.is_translated_.notify_all();") !=
         std::string::npos);

  const std::string spirv_cache =
      ReadAll(xenia_src / "gpu" / "guest_spirv_shader_cache.cc");
  assert(spirv_cache.find("translation.WaitForTranslation();") !=
         std::string::npos);
  assert(spirv_cache.find("while (!translation.is_translated())") ==
         std::string::npos);

  const std::string pipe_cache =
      ReadAll(xenia_src / "gpu" / "vulkan" / "vulkan_pipeline_cache.cc");
  assert(pipe_cache.find("translation.WaitForTranslation();") !=
         std::string::npos);
  assert(pipe_cache.find("while (!translation.is_translated())") ==
         std::string::npos);

  const std::string audio_cc =
      ReadAll(xenia_src / "kernel" / "xboxkrnl" / "xboxkrnl_audio.cc");
  const size_t fn_pos =
      audio_cc.find("XAudioGetVoiceCategoryVolumeChangeMask_entry");
  assert(fn_pos != std::string::npos);
  const size_t next_export = audio_cc.find("DECLARE_XBOXKRNL_EXPORT", fn_pos);
  assert(next_export != std::string::npos);
  const std::string fn_body = audio_cc.substr(fn_pos, next_export - fn_pos);
  assert(fn_body.find("MaybeYield") == std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc >= 2);
  RunConcurrencyCase(/*early_bail_out=*/false);
  RunConcurrencyCase(/*early_bail_out=*/true);
  VerifySources(argv[1]);
  return 0;
}
