/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_GDFX_UTIL_H_
#define XENIA_VFS_GDFX_UTIL_H_

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include "xenia/base/memory.h"

namespace xe {
namespace vfs {

constexpr size_t kGdfxSectorSize = 2048;
constexpr char kGdfxMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kGdfxMagicSize = 20;
constexpr uint8_t kGdfxFileAttributeDirectory = 0x10;
// The left offset read from directory padding. An entry never spans sectors,
// so the rest of a sector the next entry doesn't fit in is filled with 0xFF, as
// is the one sector of an empty directory.
constexpr uint16_t kGdfxPaddingOffset = 0xFFFF;

// The ordinal of the entry that padding at |ordinal| stands in for, which
// starts the next sector, or 0 when the padding starts a sector and there is
// no entry.
inline uint32_t GdfxPaddedEntryOrdinal(uint32_t ordinal) {
  const size_t offset = size_t(ordinal) * 4;
  if (offset % kGdfxSectorSize == 0) {
    return 0;
  }
  return uint32_t((offset / kGdfxSectorSize + 1) * kGdfxSectorSize / 4);
}

// Known offsets where the game partition might start.
constexpr uint64_t kGdfxLikelyOffsets[] = {
    0x00000000ULL, 0x0000FB20ULL, 0x00020600ULL, 0x02080000ULL, 0x0FD90000ULL,
};

struct GdfxPartitionInfo {
  uint64_t game_offset;
  uint32_t root_sector;
  uint32_t root_size;
};

inline bool GdfxVerifyMagic(const uint8_t* data, size_t size, size_t offset) {
  if (offset > size || kGdfxMagicSize > size - offset) {
    return false;
  }
  return std::memcmp(data + offset, kGdfxMagic, kGdfxMagicSize) == 0;
}

inline std::optional<GdfxPartitionInfo> GdfxFindPartition(const uint8_t* data,
                                                          size_t size) {
  for (uint64_t offset : kGdfxLikelyOffsets) {
    uint64_t sector32_offset = offset + (32ULL * kGdfxSectorSize);
    if (sector32_offset > size || 28 > size - sector32_offset) {
      continue;
    }
    if (GdfxVerifyMagic(data, size, static_cast<size_t>(sector32_offset))) {
      const uint8_t* fs_ptr = data + sector32_offset;
      uint32_t root_sector = xe::load<uint32_t>(fs_ptr + 20);
      uint32_t root_size = xe::load<uint32_t>(fs_ptr + 24);

      if (root_size < 13 || root_size > 32 * 1024 * 1024) {
        continue;
      }

      return GdfxPartitionInfo{offset, root_sector, root_size};
    }
  }
  return std::nullopt;
}

// ReaderFn(offset, dst, length) -> true when all |length| bytes were read.
using GdfxReadFn = std::function<bool(uint64_t, void*, size_t)>;

inline std::optional<GdfxPartitionInfo> GdfxFindPartitionFromReader(
    uint64_t image_size, const GdfxReadFn& reader) {
  uint8_t header_buf[28];
  for (uint64_t offset : kGdfxLikelyOffsets) {
    const uint64_t sector32_offset = offset + (32ULL * kGdfxSectorSize);
    if (sector32_offset > image_size ||
        sizeof(header_buf) > image_size - sector32_offset) {
      continue;
    }
    if (!reader(sector32_offset, header_buf, sizeof(header_buf))) {
      continue;
    }
    if (std::memcmp(header_buf, kGdfxMagic, kGdfxMagicSize) != 0) {
      continue;
    }
    uint32_t root_sector = xe::load<uint32_t>(header_buf + 20);
    uint32_t root_size = xe::load<uint32_t>(header_buf + 24);
    if (root_size < 13 || root_size > 32 * 1024 * 1024) {
      continue;
    }
    return GdfxPartitionInfo{offset, root_sector, root_size};
  }
  return std::nullopt;
}

struct GdfxFileLocation {
  uint64_t offset;
  uint32_t length;
};

inline bool GdfxEqualsIgnoreCase(const char* a, size_t a_len, const char* b,
                                 size_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  for (size_t i = 0; i < a_len; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca += 32;
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb += 32;
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

namespace detail {

inline std::optional<GdfxFileLocation> GdfxFindFileInDirectoryBuffer(
    uint64_t game_offset, const uint8_t* dir_buffer, size_t dir_size,
    uint32_t entry_ordinal, const char* target_name, size_t target_len,
    int depth) {
  if (depth > 100) {
    return std::nullopt;
  }

  size_t entry_offset = static_cast<size_t>(entry_ordinal) * 4;
  if (entry_offset + 2 > dir_size) {
    return std::nullopt;
  }
  if (xe::load<uint16_t>(dir_buffer + entry_offset) == kGdfxPaddingOffset) {
    const uint32_t padded = GdfxPaddedEntryOrdinal(entry_ordinal);
    return padded ? GdfxFindFileInDirectoryBuffer(
                        game_offset, dir_buffer, dir_size, padded, target_name,
                        target_len, depth + 1)
                  : std::nullopt;
  }
  if (entry_offset + 14 > dir_size) {
    return std::nullopt;
  }

  const uint8_t* p = dir_buffer + entry_offset;
  uint16_t node_l = xe::load<uint16_t>(p + 0);
  uint16_t node_r = xe::load<uint16_t>(p + 2);
  uint32_t sector = xe::load<uint32_t>(p + 4);
  uint32_t length = xe::load<uint32_t>(p + 8);
  uint8_t attributes = p[12];
  uint8_t name_length = p[13];

  if (entry_offset + 14 + name_length > dir_size) {
    return std::nullopt;
  }

  const char* name = reinterpret_cast<const char*>(p + 14);

  if (node_l != 0) {
    auto result = GdfxFindFileInDirectoryBuffer(
        game_offset, dir_buffer, dir_size, node_l, target_name, target_len,
        depth + 1);
    if (result) {
      return result;
    }
  }

  bool is_directory = (attributes & kGdfxFileAttributeDirectory) != 0;
  if (!is_directory &&
      GdfxEqualsIgnoreCase(name, name_length, target_name, target_len)) {
    uint64_t file_offset =
        game_offset + (static_cast<uint64_t>(sector) * kGdfxSectorSize);
    return GdfxFileLocation{file_offset, length};
  }

  if (node_r != 0) {
    auto result = GdfxFindFileInDirectoryBuffer(
        game_offset, dir_buffer, dir_size, node_r, target_name, target_len,
        depth + 1);
    if (result) {
      return result;
    }
  }

  return std::nullopt;
}

}  // namespace detail

inline std::optional<GdfxFileLocation> GdfxFindFile(
    const uint8_t* data, size_t size, const GdfxPartitionInfo& partition,
    const char* filename) {
  uint64_t root_offset =
      partition.game_offset +
      (static_cast<uint64_t>(partition.root_sector) * kGdfxSectorSize);

  if (root_offset > size || partition.root_size > size - root_offset) {
    return std::nullopt;
  }

  const uint8_t* root_buffer = data + root_offset;
  size_t filename_len = std::strlen(filename);

  return detail::GdfxFindFileInDirectoryBuffer(
      partition.game_offset, root_buffer, partition.root_size, 0, filename,
      filename_len, 0);
}

inline std::optional<GdfxFileLocation> GdfxFindFileFromReader(
    uint64_t image_size, const GdfxPartitionInfo& partition,
    const char* filename, const GdfxReadFn& reader) {
  const uint64_t root_offset =
      partition.game_offset +
      (static_cast<uint64_t>(partition.root_sector) * kGdfxSectorSize);
  if (root_offset > image_size ||
      partition.root_size > image_size - root_offset) {
    return std::nullopt;
  }
  std::vector<uint8_t> root_buffer(partition.root_size);
  if (!reader(root_offset, root_buffer.data(), root_buffer.size())) {
    return std::nullopt;
  }
  return detail::GdfxFindFileInDirectoryBuffer(
      partition.game_offset, root_buffer.data(), root_buffer.size(), 0,
      filename, std::strlen(filename), 0);
}

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_GDFX_UTIL_H_
