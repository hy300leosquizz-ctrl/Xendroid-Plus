/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_entry.h"

#include <algorithm>
#if !XE_PLATFORM_WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "xenia/base/math.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_file.h"

namespace xe {
namespace vfs {

namespace {

#if !XE_PLATFORM_WIN32
// Owns a page-aligned per-slice mmap so DiscImageEntry::OpenMapped works on
// both 4 KiB and 16 KiB Android kernels even when real_offset is only 2 KiB
// GDFX-sector aligned (or 0xFB20 / 0x20600 partition-shifted), without mapping
// the entire 8.5 GB ISO.
class OwningSliceMappedMemory final : public MappedMemory {
 public:
  OwningSliceMappedMemory(uint8_t* slice_ptr, size_t slice_len, void* map_base,
                          size_t map_len)
      : MappedMemory(slice_ptr, slice_len),
        map_base_(map_base),
        map_len_(map_len) {}

  ~OwningSliceMappedMemory() override { Close(0); }

  void Close(uint64_t /*truncate_size*/) override {
    if (map_base_) {
      ::munmap(map_base_, map_len_);
      map_base_ = nullptr;
      map_len_ = 0;
      data_ = nullptr;
      size_ = 0;
    }
  }

 private:
  void* map_base_ = nullptr;
  size_t map_len_ = 0;
};
#else
class OwningBackingSliceMappedMemory final : public MappedMemory {
 public:
  OwningBackingSliceMappedMemory(std::unique_ptr<MappedMemory> backing,
                                 size_t delta, size_t slice_len)
      : MappedMemory(backing->data() + delta, slice_len),
        backing_(std::move(backing)) {}

  void Close(uint64_t truncate_size) override {
    if (backing_) {
      backing_->Close(truncate_size);
      backing_.reset();
      data_ = nullptr;
      size_ = 0;
    }
  }

 private:
  std::unique_ptr<MappedMemory> backing_;
};
#endif

}  // namespace

DiscImageEntry::DiscImageEntry(Device* device, Entry* parent,
                               const std::string_view path, MappedMemory* mmap)
    : Entry(device, parent, path),
      mmap_(mmap),
      data_offset_(0),
      data_size_(0) {}

DiscImageEntry::~DiscImageEntry() = default;

std::unique_ptr<DiscImageEntry> DiscImageEntry::Create(
    Device* device, Entry* parent, const std::string_view name,
    MappedMemory* mmap) {
  auto path = xe::utf8::join_guest_paths(parent->path(), name);
  auto entry = std::make_unique<DiscImageEntry>(device, parent, path, mmap);
  return std::move(entry);
}

X_STATUS DiscImageEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new DiscImageFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

std::unique_ptr<MappedMemory> DiscImageEntry::OpenMapped(
    MappedMemory::Mode mode, size_t offset, size_t length) {
  if (mode != MappedMemory::Mode::kRead) {
    return nullptr;
  }

  auto* disc_device = static_cast<DiscImageDevice*>(device_);
  const uint64_t total_image_size =
      mmap_ ? static_cast<uint64_t>(mmap_->size()) : disc_device->image_size();
  const uint64_t real_offset =
      static_cast<uint64_t>(data_offset_) + static_cast<uint64_t>(offset);
  if (real_offset >= total_image_size || offset >= data_size_) {
    return nullptr;
  }

  size_t real_length =
      length ? std::min(length, data_size_ - offset) : (data_size_ - offset);
  real_length = static_cast<size_t>(
      std::min<uint64_t>(real_length, total_image_size - real_offset));
  if (real_length == 0) {
    return nullptr;
  }

  if (mmap_) {
    return mmap_->Slice(static_cast<size_t>(real_offset), real_length);
  }

#if !XE_PLATFORM_WIN32
  const int fd = disc_device->stream_fd();
  if (fd < 0) {
    return nullptr;
  }
  const long ps = ::sysconf(_SC_PAGESIZE);
  const uint64_t page_size = ps > 0 ? static_cast<uint64_t>(ps) : 4096ULL;
  const uint64_t aligned_offset = real_offset & ~(page_size - 1ULL);
  const size_t delta = static_cast<size_t>(real_offset - aligned_offset);
  const size_t map_len = real_length + delta;
#if defined(__ANDROID__) || defined(__linux__)
  void* map_base = ::mmap64(nullptr, map_len, PROT_READ, MAP_SHARED, fd,
                            static_cast<off64_t>(aligned_offset));
#else
  void* map_base = ::mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd,
                          static_cast<off_t>(aligned_offset));
#endif
  if (map_base == MAP_FAILED) {
    return nullptr;
  }
  return std::make_unique<OwningSliceMappedMemory>(
      static_cast<uint8_t*>(map_base) + delta, real_length, map_base, map_len);
#else
  const uint64_t page_granularity = 65536ULL;
  const uint64_t aligned_offset = real_offset & ~(page_granularity - 1ULL);
  const size_t delta = static_cast<size_t>(real_offset - aligned_offset);
  auto backing = MappedMemory::Open(disc_device->host_path(), Mode::kRead,
                                    static_cast<size_t>(aligned_offset),
                                    real_length + delta);
  if (!backing) {
    return nullptr;
  }
  return std::make_unique<OwningBackingSliceMappedMemory>(std::move(backing),
                                                          delta, real_length);
#endif
}

bool DiscImageEntry::DeleteEntryInternal(Entry* entry) { return false; }

}  // namespace vfs
}  // namespace xe
