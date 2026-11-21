/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_device.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if !XE_PLATFORM_WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/vfs/devices/disc_image_entry.h"
#include "xenia/vfs/gdfx_util.h"
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
#include "xenia/vfs/saf_tree_device.h"
#endif

DEFINE_bool(
    iso_stream_pread, true,
    "Stream .iso disc images on demand via thread-safe pread64 with a 16 MiB "
    "striped user-space LRU cache instead of mapping the entire 8.5 GB image "
    "into virtual memory (prevents multi-GB VmRSS page-cache growth and "
    "Android lmkd kills).",
    "Storage");

namespace xe {
namespace vfs {

using namespace xe::literals;

namespace {

constexpr int kMaxDirectoryTreeDepth = 256;

size_t QueryHostPageSize() {
#if !XE_PLATFORM_WIN32
  const long ps = ::sysconf(_SC_PAGESIZE);
  if (ps > 0) {
    return static_cast<size_t>(ps);
  }
#endif
  return 4096;
}

}  // namespace

DiscImageDevice::DiscImageDevice(const std::string_view mount_path,
                                 const std::filesystem::path& host_path)
    : Device(mount_path),
      name_("GDFX"),
      host_path_(host_path),
      host_page_size_(QueryHostPageSize()) {}

DiscImageDevice::~DiscImageDevice() {
#if !XE_PLATFORM_WIN32
  if (stream_fd_ >= 0) {
    ::close(stream_fd_);
    stream_fd_ = -1;
  }
#endif
}

bool DiscImageDevice::OpenStreamingHandle() {
#if !XE_PLATFORM_WIN32
  int fd = -1;
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
  const std::string path_utf8 = xe::path_to_utf8(host_path_);
  if (saf::IsSafContentUri(path_utf8)) {
    fd = saf::OpenUriReadFd(path_utf8);
  }
#endif
  if (fd < 0) {
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_LARGEFILE
    flags |= O_LARGEFILE;
#endif
    fd = ::open(host_path_.c_str(), flags);
  }
  if (fd < 0) {
    return false;
  }

  struct stat st {};
  uint64_t size = 0;
  if (::fstat(fd, &st) == 0 && st.st_size > 0) {
    size = static_cast<uint64_t>(st.st_size);
  } else {
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end > 0) {
      size = static_cast<uint64_t>(end);
    }
  }
  if (size == 0) {
    ::close(fd);
    return false;
  }

  stream_fd_ = fd;
  image_size_ = size;
  cache_stripes_ =
      std::make_unique<std::array<CacheStripe, kCacheStripeCount>>();
  return true;
#else
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(host_path_, ec);
  if (ec || size == 0) {
    return false;
  }
  fallback_file_ = xe::filesystem::FileHandle::OpenExisting(
      host_path_, xe::filesystem::FileAccess::kGenericRead);
  if (!fallback_file_) {
    return false;
  }
  image_size_ = static_cast<uint64_t>(size);
  cache_stripes_ =
      std::make_unique<std::array<CacheStripe, kCacheStripeCount>>();
  return true;
#endif
}

size_t DiscImageDevice::RawPread64(uint64_t offset, uint8_t* dst, size_t length,
                                   bool drop_page_cache) const {
  if (offset >= image_size_ || length == 0) {
    return 0;
  }
  const size_t clamped =
      static_cast<size_t>(std::min<uint64_t>(length, image_size_ - offset));
#if !XE_PLATFORM_WIN32
  if (stream_fd_ < 0) {
    return 0;
  }
  size_t total = 0;
  while (total < clamped) {
    const uint64_t cur_off = offset + total;
#if defined(__ANDROID__) || defined(__linux__)
    const ssize_t got = ::pread64(
        stream_fd_, dst + total, clamped - total, static_cast<off64_t>(cur_off));
#else
    const ssize_t got = ::pread(
        stream_fd_, dst + total, clamped - total, static_cast<off_t>(cur_off));
#endif
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (got == 0) {
      break;
    }
    total += static_cast<size_t>(got);
  }
#if defined(POSIX_FADV_DONTNEED) && (defined(__ANDROID__) || defined(__linux__))
  if (drop_page_cache && total >= host_page_size_) {
    const uint64_t aligned_start =
        (offset + host_page_size_ - 1) & ~uint64_t(host_page_size_ - 1);
    const uint64_t aligned_end =
        (offset + total) & ~uint64_t(host_page_size_ - 1);
    if (aligned_end > aligned_start) {
      ::posix_fadvise(stream_fd_, static_cast<off_t>(aligned_start),
                      static_cast<off_t>(aligned_end - aligned_start),
                      POSIX_FADV_DONTNEED);
    }
  }
#else
  (void)drop_page_cache;
#endif
  return total;
#else
  (void)drop_page_cache;
  if (!fallback_file_) {
    return 0;
  }
  size_t bytes_read = 0;
  if (!fallback_file_->Read(static_cast<size_t>(offset), dst, clamped,
                            &bytes_read)) {
    return 0;
  }
  return bytes_read;
#endif
}

size_t DiscImageDevice::ReadImageAt(uint64_t offset, uint8_t* buffer,
                                    size_t length) {
  if (offset >= image_size_ || length == 0) {
    return 0;
  }
  const size_t clamped =
      static_cast<size_t>(std::min<uint64_t>(length, image_size_ - offset));

  if (mmap_) {
    std::memcpy(buffer, mmap_->data() + offset, clamped);
    return clamped;
  }

  // Large sequential streams bypass the 16 MiB user-space block cache and
  // immediately release clean kernel page-cache pages via POSIX_FADV_DONTNEED.
  if (clamped > kCacheBypassThreshold || !cache_stripes_) {
    return RawPread64(offset, buffer, clamped, /*drop_page_cache=*/true);
  }

  size_t copied = 0;
  while (copied < clamped) {
    const uint64_t cur_offset = offset + copied;
    const uint64_t block_index = cur_offset / kCacheBlockSize;
    const size_t in_block_offset =
        static_cast<size_t>(cur_offset % kCacheBlockSize);
    const size_t want =
        std::min(clamped - copied, kCacheBlockSize - in_block_offset);

    const size_t stripe_idx =
        static_cast<size_t>((block_index ^ (block_index >> 4)) &
                            (kCacheStripeCount - 1));
    CacheStripe& stripe = (*cache_stripes_)[stripe_idx];

    std::unique_lock<std::mutex> lock(stripe.mutex);
    CacheLine* hit = nullptr;
    size_t lru_slot = 0;
    uint64_t oldest_tick = ~uint64_t(0);
    for (size_t i = 0; i < kCacheBlocksPerStripe; ++i) {
      CacheLine& line = stripe.lines[i];
      if (line.block_index == block_index && line.data) {
        hit = &line;
        break;
      }
      if (line.block_index == ~uint64_t(0) || line.last_used < oldest_tick) {
        oldest_tick = line.last_used;
        lru_slot = i;
        if (line.block_index == ~uint64_t(0)) {
          break;
        }
      }
    }

    if (hit) {
      hit->last_used = ++stripe.clock;
      if (in_block_offset >= hit->valid_bytes) {
        break;
      }
      const size_t avail =
          std::min(want, static_cast<size_t>(hit->valid_bytes) - in_block_offset);
      std::memcpy(buffer + copied, hit->data.get() + in_block_offset, avail);
      copied += avail;
      if (avail < want) {
        break;
      }
      continue;
    }

    // Populate into a stack/heap scratch buffer outside any other stripe lock;
    // release stripe mutex during the pread64 syscall so other blocks in the
    // same stripe never wait on storage I/O.
    std::unique_ptr<uint8_t[]> fresh_buf = std::move(stripe.lines[lru_slot].data);
    stripe.lines[lru_slot].block_index = ~uint64_t(0);
    stripe.lines[lru_slot].valid_bytes = 0;
    lock.unlock();

    if (!fresh_buf) {
      fresh_buf = std::make_unique<uint8_t[]>(kCacheBlockSize);
    }
    const uint64_t block_byte_offset = block_index * kCacheBlockSize;
    const size_t got = RawPread64(block_byte_offset, fresh_buf.get(),
                                  kCacheBlockSize, /*drop_page_cache=*/true);
    if (got <= in_block_offset) {
      lock.lock();
      if (!stripe.lines[lru_slot].data) {
        stripe.lines[lru_slot].data = std::move(fresh_buf);
      }
      break;
    }

    const size_t avail = std::min(want, got - in_block_offset);
    std::memcpy(buffer + copied, fresh_buf.get() + in_block_offset, avail);
    copied += avail;

    lock.lock();
    // Re-check in case another thread populated the same block concurrently.
    bool already_present = false;
    size_t target_slot = lru_slot;
    uint64_t target_oldest = ~uint64_t(0);
    for (size_t i = 0; i < kCacheBlocksPerStripe; ++i) {
      if (stripe.lines[i].block_index == block_index && stripe.lines[i].data) {
        stripe.lines[i].last_used = ++stripe.clock;
        already_present = true;
        break;
      }
      if (stripe.lines[i].block_index == ~uint64_t(0) ||
          stripe.lines[i].last_used < target_oldest) {
        target_oldest = stripe.lines[i].last_used;
        target_slot = i;
        if (stripe.lines[i].block_index == ~uint64_t(0)) {
          break;
        }
      }
    }
    if (!already_present) {
      CacheLine& dst_line = stripe.lines[target_slot];
      dst_line.data = std::move(fresh_buf);
      dst_line.block_index = block_index;
      dst_line.valid_bytes = static_cast<uint32_t>(got);
      dst_line.last_used = ++stripe.clock;
    }
    if (avail < want) {
      break;
    }
  }

  return copied;
}

bool DiscImageDevice::ReadExactForParse(uint64_t offset, void* dst,
                                        size_t length) {
  if (offset > image_size_ || length > image_size_ - offset) {
    return false;
  }
  return ReadImageAt(offset, static_cast<uint8_t*>(dst), length) == length;
}

bool DiscImageDevice::Initialize() {
  if (!cvars::iso_stream_pread) {
    mmap_ = MappedMemory::Open(host_path_, MappedMemory::Mode::kRead);
    if (mmap_) {
      image_size_ = mmap_->size();
    } else {
      XELOGW(
          "DiscImageDevice: full mmap failed for {}, falling back to pread64 "
          "streaming",
          xe::path_to_utf8(host_path_));
    }
  }

  if (!mmap_ && !OpenStreamingHandle()) {
    XELOGE("Disc image could not be opened: {}", xe::path_to_utf8(host_path_));
    return false;
  }
  XELOGFS("DiscImageDevice::Initialize ({})",
          mmap_ ? "mmap" : "pread64-stream");

  ParseState state{};
  state.ptr = mmap_ ? mmap_->data() : nullptr;
  state.size = image_size_;
  auto result = Verify(&state);
  if (result != Error::kSuccess) {
    XELOGE("Failed to verify disc image header: {}",
           static_cast<int32_t>(result));
    return false;
  }

  if (state.root_offset > state.size ||
      state.root_size > state.size - state.root_offset) {
    XELOGE("Disc image root directory table out of bounds");
    return false;
  }

  if (state.ptr) {
    result = ReadAllEntries(&state, state.ptr + state.root_offset,
                            static_cast<size_t>(state.root_size));
  } else {
    std::vector<uint8_t> root_buffer(static_cast<size_t>(state.root_size));
    if (!ReadExactForParse(state.root_offset, root_buffer.data(),
                           root_buffer.size())) {
      XELOGE("Failed to read GDFX root directory table");
      return false;
    }
    result = ReadAllEntries(&state, root_buffer.data(), root_buffer.size());
  }

  if (result != Error::kSuccess) {
    XELOGE("Failed to read all GDFX entries: {}", static_cast<int32_t>(result));
    return false;
  }

  return true;
}

void DiscImageDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* DiscImageDevice::ResolvePath(const std::string_view path) {
  XELOGFS("DiscImageDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

DiscImageDevice::Error DiscImageDevice::Verify(ParseState* state) {
  std::optional<GdfxPartitionInfo> partition;
  if (state->ptr) {
    partition =
        GdfxFindPartition(state->ptr, static_cast<size_t>(state->size));
  } else {
    partition = GdfxFindPartitionFromReader(
        state->size, [this](uint64_t off, void* dst, size_t len) {
          return ReadExactForParse(off, dst, len);
        });
  }
  if (!partition) {
    return Error::kErrorFileMismatch;
  }

  state->game_offset = partition->game_offset;
  state->root_sector = partition->root_sector;
  state->root_size = partition->root_size;
  state->root_offset =
      state->game_offset + (state->root_sector * kGdfxSectorSize);

  return Error::kSuccess;
}

DiscImageDevice::Error DiscImageDevice::ReadAllEntries(
    ParseState* state, const uint8_t* root_buffer, size_t root_buffer_size) {
  auto root_entry = new DiscImageEntry(this, nullptr, "", mmap_.get());
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  if (!ReadEntry(state, root_buffer, root_buffer_size, 0, root_entry, 0)) {
    return Error::kErrorOutOfMemory;
  }

  return Error::kSuccess;
}

bool DiscImageDevice::ReadEntry(ParseState* state, const uint8_t* buffer,
                                size_t buffer_size, uint32_t entry_ordinal,
                                DiscImageEntry* parent, int depth) {
  if (depth > kMaxDirectoryTreeDepth) {
    return false;
  }

  const size_t entry_offset = static_cast<size_t>(entry_ordinal) * 4;
  if (entry_offset + 2 > buffer_size) {
    return false;
  }
  if (xe::load<uint16_t>(buffer + entry_offset) == kGdfxPaddingOffset) {
    const uint32_t padded = GdfxPaddedEntryOrdinal(entry_ordinal);
    return padded ? ReadEntry(state, buffer, buffer_size, padded, parent,
                              depth + 1)
                  : true;
  }
  if (entry_offset + 14 > buffer_size) {
    return false;
  }

  const uint8_t* p = buffer + entry_offset;

  uint16_t node_l = xe::load<uint16_t>(p + 0);
  uint16_t node_r = xe::load<uint16_t>(p + 2);
  uint64_t sector = xe::load<uint32_t>(p + 4);
  size_t length = xe::load<uint32_t>(p + 8);
  uint8_t attributes = xe::load<uint8_t>(p + 12);
  uint8_t name_length = xe::load<uint8_t>(p + 13);
  if (entry_offset + 14 + name_length > buffer_size) {
    return false;
  }
  auto name_buffer = reinterpret_cast<const char*>(p + 14);

  if (node_l &&
      !ReadEntry(state, buffer, buffer_size, node_l, parent, depth + 1)) {
    return false;
  }

  // Filename is stored as Windows-1252, convert it to UTF-8.
  auto ansi_name = std::string(name_buffer, name_length);
  auto name = xe::win1252_to_utf8(ansi_name);
  if (name.empty()) {
    name = ansi_name;
  }

  auto entry = DiscImageEntry::Create(this, parent, name, mmap_.get());
  entry->attributes_ = attributes | kFileAttributeReadOnly;
  entry->size_ = length;
  entry->allocation_size_ = xe::round_up(length, bytes_per_sector());

  // Set to January 1, 1970 (UTC) in 100-nanosecond intervals
  entry->create_timestamp_ = 10000 * 11644473600000LL;
  entry->access_timestamp_ = 10000 * 11644473600000LL;
  entry->write_timestamp_ = 10000 * 11644473600000LL;

  if (attributes & kFileAttributeDirectory) {
    entry->data_offset_ = 0;
    entry->data_size_ = 0;
    if (length) {
      const uint64_t folder_offset =
          state->game_offset + (sector * kGdfxSectorSize);
      const size_t folder_table_size = xe::round_up(length, kGdfxSectorSize);
      if (folder_offset >= state->size ||
          folder_table_size > 64 * 1024 * 1024) {
        return false;
      }
      const size_t clamped_table_size = static_cast<size_t>(std::min<uint64_t>(
          folder_table_size, state->size - folder_offset));
      if (state->ptr) {
        const uint8_t* folder_ptr = state->ptr + folder_offset;
        if (!ReadEntry(state, folder_ptr, clamped_table_size, 0, entry.get(),
                       depth + 1)) {
          return false;
        }
      } else {
        std::vector<uint8_t> folder_buf(clamped_table_size);
        if (!ReadExactForParse(folder_offset, folder_buf.data(),
                               folder_buf.size())) {
          return false;
        }
        if (!ReadEntry(state, folder_buf.data(), folder_buf.size(), 0,
                       entry.get(), depth + 1)) {
          return false;
        }
      }
    }
  } else {
    entry->data_offset_ =
        static_cast<size_t>(state->game_offset + (sector * kGdfxSectorSize));
    entry->data_size_ = length;
  }

  parent->children_.emplace_back(std::move(entry));

  if (node_r &&
      !ReadEntry(state, buffer, buffer_size, node_r, parent, depth + 1)) {
    return false;
  }

  return true;
}

}  // namespace vfs
}  // namespace xe
