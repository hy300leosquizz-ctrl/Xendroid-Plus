/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"

#include <cstring>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_device.h"
#include "xenia/vfs/file.h"
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
#include "xenia/vfs/saf_tree_device.h"
#endif

namespace xe {
namespace vfs {

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path) {
  std::error_code ec;
  std::unique_ptr<xe::filesystem::FileHandle> file;
  uintmax_t file_size = 0;

#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
  const std::string path_utf8 = xe::path_to_utf8(host_path);
  if (saf::IsSafContentUri(path_utf8)) {
    file = saf::OpenUriFileHandle(path_utf8);
    if (!file) {
      XELOGE("Failed to open SAF XContent URI: {}", path_utf8);
      return nullptr;
    }
    file_size = saf::QueryUriFileSize(path_utf8);
  }
#endif

  if (!file) {
    if (std::filesystem::is_directory(host_path, ec)) {
      return nullptr;
    }
    file = xe::filesystem::FileHandle::OpenExisting(
        host_path, xe::filesystem::FileAccess::kFileReadData);
    if (!file) {
      if (!std::filesystem::exists(host_path, ec)) {
        XELOGE("Path to XContent container does not exist: {}",
               xe::path_to_utf8(host_path));
      }
      return nullptr;
    }
    file_size = std::filesystem::file_size(host_path, ec);
    if (ec) {
      return nullptr;
    }
  }

  if (file_size < sizeof(XContentContainerHeader)) {
    return nullptr;
  }

  auto header = std::make_unique<XContentContainerHeader>();
  size_t bytes_read = 0;
  if (!file->Read(0, header.get(), sizeof(XContentContainerHeader),
                  &bytes_read) ||
      bytes_read != sizeof(XContentContainerHeader) ||
      !header->content_header.is_magic_valid()) {
    return nullptr;
  }

  switch (header->content_metadata.volume_type) {
    case XContentVolumeType::kStfs:
      return std::make_unique<StfsContainerDevice>(
          mount_path, host_path, std::move(header),
          static_cast<size_t>(file_size), std::move(file));
    case XContentVolumeType::kSvod:
      return std::make_unique<SvodContainerDevice>(
          mount_path, host_path, std::move(header),
          static_cast<size_t>(file_size));
    default:
      return nullptr;
  }
}

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(const std::string_view mount_path,
                                             Entry* entry) {
  if (!entry || (entry->attributes() & kFileAttributeDirectory)) {
    return nullptr;
  }

  File* file = nullptr;
  if (entry->Open(FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS ||
      !file) {
    XELOGE("Error opening XContent file {}", entry->absolute_path());
    return nullptr;
  }

  const auto header = ReadContainerHeader(file);
  if (!header || !header->content_header.is_magic_valid() ||
      header->content_metadata.volume_type != XContentVolumeType::kStfs) {
    file->Destroy();
    return nullptr;
  }

  return std::make_unique<StfsContainerDevice>(mount_path, file);
}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path,
    std::unique_ptr<XContentContainerHeader> host_header, size_t files_size)
    : Device(mount_path),
      name_("XContent"),
      host_path_(host_path),
      files_total_size_(files_size),
      header_(std::make_unique<XContentContainerHeader>()),
      host_header_(std::move(host_header)) {}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path)
    : XContentContainerDevice(mount_path, host_path, nullptr, 0) {}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, File* package_file)
    : XContentContainerDevice(mount_path, std::filesystem::path(), nullptr, 0) {
  package_file_ = package_file;
}

XContentContainerDevice::~XContentContainerDevice() {
  if (package_file_) {
    package_file_->Destroy();
    package_file_ = nullptr;
  }
}

bool XContentContainerDevice::Initialize() {
  std::unique_ptr<XContentContainerHeader> header;
  if (package_file_) {
    files_total_size_ = package_file_->entry()->size();
    header = ReadContainerHeader(package_file_);
  } else if (host_header_) {
    XELOGI("Loading XContent header file: {}", xe::path_to_utf8(host_path_));
    header = std::move(host_header_);
  } else {
    header = ReadContainerHeader(host_path_);
    if (header) {
      std::error_code ec;
      const uintmax_t sz = std::filesystem::file_size(host_path_, ec);
      if (!ec) {
        files_total_size_ = static_cast<size_t>(sz);
      }
    }
  }

  auto header_loading_result = VerifyHeader(std::move(header));
  if (header_loading_result != Result::kSuccess) {
    XELOGE("Error reading XContent header: {}",
           static_cast<int32_t>(header_loading_result));
    return false;
  }

  SetupContainer();

  if (LoadHostFiles() != Result::kSuccess) {
    XELOGE("Error loading XContent host files.");
    return false;
  }

  return Read() == Result::kSuccess;
}

bool XContentContainerDevice::IsContainerFile(
    const std::filesystem::path& host_path) {
  auto header = ReadContainerHeader(host_path);
  return header && header->content_header.is_magic_valid();
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(
    const std::filesystem::path& file_path) {
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
  const std::string path_utf8 = xe::path_to_utf8(file_path);
  if (saf::IsSafContentUri(path_utf8)) {
    auto handle = saf::OpenUriFileHandle(path_utf8);
    if (!handle) {
      return {};
    }
    auto header = std::make_unique<XContentContainerHeader>();
    size_t got = 0;
    if (!handle->Read(0, header.get(), sizeof(XContentContainerHeader), &got) ||
        got != sizeof(XContentContainerHeader)) {
      return {};
    }
    return header;
  }
#endif

  std::error_code ec;
  if (!std::filesystem::exists(file_path, ec) ||
      std::filesystem::is_directory(file_path, ec)) {
    return {};
  }
  if (std::filesystem::file_size(file_path, ec) <
          sizeof(XContentContainerHeader) ||
      ec) {
    return {};
  }

  auto header_file = xe::filesystem::OpenFile(file_path, "rb");
  if (!header_file) {
    return {};
  }

  auto header = ReadContainerHeader(header_file);
  fclose(header_file);
  return header;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(FILE* host_file) {
  auto header = std::make_unique<XContentContainerHeader>();
  if (fread(header.get(), sizeof(XContentContainerHeader), 1, host_file) != 1) {
    return nullptr;
  }
  return header;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(File* file) {
  if (!file) {
    return nullptr;
  }
  auto header = std::make_unique<XContentContainerHeader>();
  size_t bytes_read = 0;
  if (file->ReadSync(
          {reinterpret_cast<uint8_t*>(header.get()), sizeof(*header)}, 0,
          &bytes_read) != X_STATUS_SUCCESS ||
      bytes_read != sizeof(XContentContainerHeader)) {
    return nullptr;
  }
  return header;
}

Entry* XContentContainerDevice::ResolvePath(const std::string_view path) {
  XELOGFS("StfsContainerDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

void XContentContainerDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

kernel::xam::XCONTENT_AGGREGATE_DATA
XContentContainerDevice::ContentDataFromHeader(
    const XContentContainerHeader& header) {
  kernel::xam::XCONTENT_AGGREGATE_DATA data;
  std::memset(&data, 0, sizeof(kernel::xam::XCONTENT_AGGREGATE_DATA));

  data.device_id = 1;
  data.title_id = header.content_metadata.execution_info.title_id;
  data.content_type = header.content_metadata.content_type;

  auto name = header.content_metadata.display_name(XLanguage::kEnglish);
  if (name.empty()) {
    for (uint32_t i = 1; i <= header.content_metadata.kNumLanguagesV2; i++) {
      name = header.content_metadata.display_name((XLanguage)i);
      if (!name.empty()) {
        break;
      }
    }
  }

  data.set_display_name(name);
  return data;
}

uint32_t XContentContainerDevice::LicenseMaskFromHeader(
    const XContentContainerHeader& header) {
  uint32_t final_license = 0;
  for (uint8_t i = 0; i < license_count; i++) {
    final_license |= header.content_header.licenses[i].license_bits;
  }
  return final_license;
}

kernel::xam::XCONTENT_AGGREGATE_DATA XContentContainerDevice::content_header()
    const {
  return ContentDataFromHeader(*header_);
}

XContentContainerDevice::Result XContentContainerDevice::VerifyHeader(
    std::unique_ptr<XContentContainerHeader> header) {
  if (files_total_size_ < sizeof(XContentContainerHeader)) {
    return Result::kTooSmall;
  }
  if (header == nullptr) {
    return Result::kReadError;
  }
  if (!header->content_header.is_magic_valid()) {
    return Result::kFileMismatch;
  }
  header_ = std::move(header);
  return Result::kSuccess;
}

}  // namespace vfs
}  // namespace xe
