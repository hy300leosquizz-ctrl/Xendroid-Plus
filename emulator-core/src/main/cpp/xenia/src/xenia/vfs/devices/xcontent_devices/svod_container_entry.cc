/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_devices/svod_container_entry.h"

#include "xenia/base/logging.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_file.h"
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
#include "xenia/vfs/saf_tree_device.h"
#endif

namespace xe {
namespace vfs {

void MultiFileHandles::Add(std::filesystem::path path) {
  fragments_.push_back({std::move(path), {}, false, nullptr});
}

void MultiFileHandles::AddSafUri(std::string uri) {
  fragments_.push_back({{}, std::move(uri), true, nullptr});
}

xe::filesystem::FileHandle* MultiFileHandles::Get(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= fragments_.size()) {
    return nullptr;
  }
  auto& fragment = fragments_[index];
  if (!fragment.handle) {
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
    if (fragment.is_saf_uri) {
      fragment.handle = saf::OpenUriFileHandle(fragment.saf_uri);
      if (!fragment.handle) {
        XELOGE("Failed to open SAF SVOD fragment {}.", fragment.saf_uri);
      }
      return fragment.handle.get();
    }
#endif
    fragment.handle = xe::filesystem::FileHandle::OpenExisting(
        fragment.path, xe::filesystem::FileAccess::kGenericRead);
    if (!fragment.handle) {
      XELOGE("Failed to open SVOD file {}.", xe::path_to_utf8(fragment.path));
    }
  }
  return fragment.handle.get();
}

SvodContainerEntry::SvodContainerEntry(Device* device, Entry* parent,
                                       const std::string_view path,
                                       MultiFileHandles* files)
    : XContentContainerEntry(device, parent, path), files_(files) {}

SvodContainerEntry::~SvodContainerEntry() = default;

std::unique_ptr<SvodContainerEntry> SvodContainerEntry::Create(
    Device* device, Entry* parent, const std::string_view name,
    MultiFileHandles* files) {
  auto path = xe::utf8::join_guest_paths(parent->path(), name);
  return std::make_unique<SvodContainerEntry>(device, parent, path, files);
}

X_STATUS SvodContainerEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new SvodContainerFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

bool SvodContainerEntry::DeleteEntryInternal(Entry* /*entry*/) { return false; }

}  // namespace vfs
}  // namespace xe
