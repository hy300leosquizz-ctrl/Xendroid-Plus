/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/iso_metadata.h"

#include <cerrno>
#include <vector>

#if !XE_PLATFORM_WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/vfs/gdfx_util.h"
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
#include "xenia/vfs/saf_tree_device.h"
#endif

namespace xe {
namespace vfs {

namespace {

#if !XE_PLATFORM_WIN32
bool PreadExact64(int fd, uint64_t offset, void* dst, size_t length) {
  auto* out = static_cast<uint8_t*>(dst);
  size_t total = 0;
  while (total < length) {
#if defined(__ANDROID__) || defined(__linux__)
    const ssize_t got = ::pread64(fd, out + total, length - total,
                                  static_cast<off64_t>(offset + total));
#else
    const ssize_t got = ::pread(fd, out + total, length - total,
                                static_cast<off_t>(offset + total));
#endif
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (got == 0) {
      return false;
    }
    total += static_cast<size_t>(got);
  }
  return true;
}
#endif

}  // namespace

std::optional<XexMetadata> ExtractIsoMetadata(const uint8_t* data,
                                              size_t size) {
  auto partition = GdfxFindPartition(data, size);
  if (!partition) {
    return std::nullopt;
  }

  auto location = GdfxFindFile(data, size, *partition, "default.xex");
  if (!location) {
    return std::nullopt;
  }

  if (location->offset > size || location->length > size - location->offset) {
    return std::nullopt;
  }

  return ExtractXexMetadata(data + location->offset, location->length);
}

std::optional<XexMetadata> ExtractIsoMetadata(
    const std::filesystem::path& path) {
  // Probe GDFX headers and read only default.xex via 64-bit pread64 instead of
  // memory-mapping the full 7.3-8.5 GB ISO during library scans.
#if !XE_PLATFORM_WIN32
  int fd = -1;
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
  const std::string path_utf8 = xe::path_to_utf8(path);
  if (saf::IsSafContentUri(path_utf8)) {
    fd = saf::OpenUriReadFd(path_utf8);
  }
#endif
  if (fd < 0) {
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_LARGEFILE
    flags |= O_LARGEFILE;
#endif
    fd = ::open(path.c_str(), flags);
  }
  if (fd >= 0) {
    struct FdGuard {
      int fd;
      ~FdGuard() {
        if (fd >= 0) ::close(fd);
      }
    } guard{fd};

    struct stat st {};
    uint64_t image_size = 0;
    if (::fstat(fd, &st) == 0 && st.st_size > 0) {
      image_size = static_cast<uint64_t>(st.st_size);
    } else {
      const off_t end = ::lseek(fd, 0, SEEK_END);
      if (end > 0) {
        image_size = static_cast<uint64_t>(end);
      }
    }
    if (image_size == 0) {
      return std::nullopt;
    }

    auto reader = [fd](uint64_t off, void* dst, size_t len) {
      return PreadExact64(fd, off, dst, len);
    };
    auto partition = GdfxFindPartitionFromReader(image_size, reader);
    if (!partition) {
      return std::nullopt;
    }
    auto location =
        GdfxFindFileFromReader(image_size, *partition, "default.xex", reader);
    if (!location || location->length == 0 || location->offset > image_size ||
        location->length > image_size - location->offset ||
        location->length > 128 * 1024 * 1024) {
      return std::nullopt;
    }
    std::vector<uint8_t> xex_bytes(location->length);
    if (!PreadExact64(fd, location->offset, xex_bytes.data(),
                      xex_bytes.size())) {
      return std::nullopt;
    }
    return ExtractXexMetadata(xex_bytes.data(), xex_bytes.size());
  }
#endif

  auto mmap = xe::MappedMemory::Open(path, xe::MappedMemory::Mode::kRead);
  if (!mmap) {
    return std::nullopt;
  }
  return ExtractIsoMetadata(mmap->data(), mmap->size());
}

}  // namespace vfs
}  // namespace xe
