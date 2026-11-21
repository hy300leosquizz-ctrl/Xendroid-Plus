/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_DISC_IMAGE_DEVICE_H_
#define XENIA_VFS_DEVICES_DISC_IMAGE_DEVICE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/vfs/device.h"

DECLARE_bool(iso_stream_pread);

namespace xe {
namespace vfs {

class DiscImageEntry;

constexpr fourcc_t kXSFSignature = make_fourcc(0x58, 0x53, 0x46, 0x1A);

class DiscImageDevice : public Device {
 public:
  // 16 MiB user-space block cache (16 stripes x 16 blocks x 64 KiB) for
  // on-demand pread64 ISO streaming: zero cross-stripe contention across the
  // 4 GuestScheduler I/O worker threads and -2 to -5 GiB VmRSS vs 8.5 GB mmap.
  static constexpr size_t kCacheStripeCount = 16;
  static constexpr size_t kCacheBlocksPerStripe = 16;
  static constexpr size_t kCacheBlockSize = 64 * 1024;
  static constexpr size_t kCacheBypassThreshold = 256 * 1024;

  DiscImageDevice(const std::string_view mount_path,
                  const std::filesystem::path& host_path);
  ~DiscImageDevice() override;

  bool Initialize() override;
  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool supports_concurrent_io() const override { return true; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override {
    return uint32_t(image_size_ / sectors_per_allocation_unit() /
                    bytes_per_sector());
  }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  const std::filesystem::path& host_path() const { return host_path_; }
  uint64_t image_size() const { return image_size_; }
  MappedMemory* mmap() const { return mmap_.get(); }
  int stream_fd() const { return stream_fd_; }

  // Thread-safe 64-bit random/streaming read from the disc image (serves from
  // mmap_ when mapped, or pread64 + 16 MiB striped LRU cache when streaming).
  // Returns the number of bytes actually read (0 on EOF or I/O error).
  size_t ReadImageAt(uint64_t offset, uint8_t* buffer, size_t length);

 private:
  enum class Error {
    kSuccess = 0,
    kErrorOutOfMemory = -1,
    kErrorReadError = -10,
    kErrorFileMismatch = -30,
    kErrorDamagedFile = -31,
  };

  struct CacheLine {
    uint64_t block_index = ~uint64_t(0);
    uint64_t last_used = 0;
    uint32_t valid_bytes = 0;
    std::unique_ptr<uint8_t[]> data;
  };

  struct alignas(64) CacheStripe {
    std::mutex mutex;
    uint64_t clock = 0;
    std::array<CacheLine, kCacheBlocksPerStripe> lines;
  };

  struct ParseState {
    const uint8_t* ptr = nullptr;
    uint64_t size = 0;         // Size (bytes) of total image (64-bit for XGD3).
    uint64_t game_offset = 0;  // Offset (bytes) of game partition.
    uint64_t root_sector = 0;  // Offset (sector) of root.
    uint64_t root_offset = 0;  // Offset (bytes) of root.
    uint64_t root_size = 0;    // Size (bytes) of root.
  };

  bool OpenStreamingHandle();
  size_t RawPread64(uint64_t offset, uint8_t* dst, size_t length,
                    bool drop_page_cache) const;
  bool ReadExactForParse(uint64_t offset, void* dst, size_t length);

  Error Verify(ParseState* state);
  Error ReadAllEntries(ParseState* state, const uint8_t* root_buffer,
                       size_t root_buffer_size);
  bool ReadEntry(ParseState* state, const uint8_t* buffer, size_t buffer_size,
                 uint32_t entry_ordinal, DiscImageEntry* parent, int depth);

  std::string name_;
  std::filesystem::path host_path_;
  std::unique_ptr<Entry> root_entry_;
  std::unique_ptr<MappedMemory> mmap_;
  uint64_t image_size_ = 0;
  int stream_fd_ = -1;
  size_t host_page_size_ = 4096;
  std::unique_ptr<xe::filesystem::FileHandle> fallback_file_;
  std::unique_ptr<std::array<CacheStripe, kCacheStripeCount>> cache_stripes_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_DISC_IMAGE_DEVICE_H_
