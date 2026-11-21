/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_file.h"

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_entry.h"

namespace xe {
namespace vfs {

DiscImageFile::DiscImageFile(uint32_t file_access, DiscImageEntry* entry)
    : File(file_access, entry), entry_(entry) {}

DiscImageFile::~DiscImageFile() = default;

void DiscImageFile::Destroy() { delete this; }

X_STATUS DiscImageFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_read) {
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }

  auto* disc_device = static_cast<DiscImageDevice*>(entry_->device());
  const uint64_t image_size = disc_device->image_size();
  if (static_cast<uint64_t>(entry_->data_offset()) >= image_size) {
    xe::FatalError("This ISO image is corrupted and cannot be played.");
    return X_STATUS_END_OF_FILE;
  }

  const uint64_t real_offset =
      static_cast<uint64_t>(entry_->data_offset()) + byte_offset;
  if (real_offset >= image_size) {
    // Truncated image, the file starts inside it but ends past its end.
    return X_STATUS_END_OF_FILE;
  }

  const size_t max_entry_len = entry_->data_size() - byte_offset;
  const size_t real_length = static_cast<size_t>(std::min<uint64_t>(
      {static_cast<uint64_t>(buffer.size()),
       static_cast<uint64_t>(max_entry_len), image_size - real_offset}));
  const size_t got =
      disc_device->ReadImageAt(real_offset, buffer.data(), real_length);
  if (got == 0 && real_length > 0) {
    return X_STATUS_END_OF_FILE;
  }
  *out_bytes_read = got;
  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe
