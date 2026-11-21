/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_devices/svod_container_device.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_entry.h"
#include "xenia/vfs/gdfx_util.h"
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
#include "xenia/vfs/saf_tree_device.h"
#endif

namespace xe {
namespace vfs {

// Reads |length| bytes at |offset| of |file|, false unless all of them came.
static bool ReadAt(xe::filesystem::FileHandle* file, size_t offset,
                   void* buffer, size_t length) {
  size_t bytes_read = 0;
  return file && file->Read(offset, buffer, length, &bytes_read) &&
         bytes_read == length;
}

SvodContainerDevice::SvodContainerDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path,
    std::unique_ptr<XContentContainerHeader> host_header, size_t files_size)
    : XContentContainerDevice(mount_path, host_path, std::move(host_header),
                              files_size),
      svod_base_offset_(),
      svod_layout_() {
  SetName("FATX");
}

SvodContainerDevice::~SvodContainerDevice() { files_total_size_ = 0; }

SvodContainerDevice::Result SvodContainerDevice::LoadHostFiles() {
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
  const std::string host_utf8 = xe::path_to_utf8(host_path_);
  if (saf::IsSafContentUri(host_utf8)) {
    auto saf_fragments = saf::ListSiblingSvodFragments(host_utf8);
    if (saf_fragments.empty()) {
      XELOGE("SAF SVOD container has no sibling .data fragments for {}.",
             host_utf8);
      return Result::kFileMismatch;
    }
    if (saf_fragments.size() != header_->content_metadata.data_file_count) {
      XELOGE("SAF SVOD expecting {} data fragments, but {} are present.",
             header_->content_metadata.data_file_count.get(),
             saf_fragments.size());
      return Result::kFileMismatch;
    }
    for (auto& frag : saf_fragments) {
      uint64_t sz = frag.size ? frag.size : saf::QueryUriFileSize(frag.uri);
      files_total_size_ += static_cast<size_t>(sz);
      files_.AddSafUri(std::move(frag.uri));
    }
    XELOGI("SAF SVOD lazily registered {} data fragments.",
           saf_fragments.size());
    return Result::kSuccess;
  }
#endif

  std::filesystem::path data_fragment_path = host_path_;
  data_fragment_path += ".data";
  if (!std::filesystem::exists(data_fragment_path)) {
    XELOGE("STFS container is multi-file, but path {} does not exist.",
           xe::path_to_utf8(data_fragment_path));
    return Result::kFileMismatch;
  }

  // Ensure data fragment files are sorted deterministically on POSIX/Android.
  auto fragment_files = filesystem::ListFiles(data_fragment_path);
  std::sort(fragment_files.begin(), fragment_files.end(),
            [](const filesystem::FileInfo& left,
               const filesystem::FileInfo& right) {
              return left.name < right.name;
            });

  if (fragment_files.size() != header_->content_metadata.data_file_count) {
    XELOGE("SVOD expecting {} data fragments, but {} are present.",
           header_->content_metadata.data_file_count.get(),
           fragment_files.size());
    return Result::kFileMismatch;
  }

  for (size_t i = 0; i < fragment_files.size(); i++) {
    auto& fragment = fragment_files.at(i);
    if (fragment.type != xe::filesystem::FileInfo::Type::kFile) {
      XELOGE("SVOD data fragment {} is not a file.",
             xe::path_to_utf8(fragment.name));
      return Result::kFileMismatch;
    }
    auto path = fragment.path / fragment.name;

    uintmax_t fragment_size = fragment.total_size;
    if (!fragment_size) {
      std::error_code ec;
      fragment_size = std::filesystem::file_size(path, ec);
      if (ec) {
        XELOGI("Failed to size SVOD file {}.", xe::path_to_utf8(path));
        return Result::kReadError;
      }
    }

    files_total_size_ += static_cast<size_t>(fragment_size);
    files_.Add(std::move(path));
  }
  XELOGI("SVOD successfully registered {} lazy fragments.",
         fragment_files.size());
  return Result::kSuccess;
}

XContentContainerDevice::Result SvodContainerDevice::Read() {
  // SVOD Systems can have different layouts. The root block is
  // denoted by the magic "MICROSOFT*XBOX*MEDIA" and is always in
  // the first "actual" data fragment of the system.
  auto* svod_header = files_.Get(0);
  if (!svod_header) {
    XELOGE("SVOD has no readable first data fragment");
    return Result::kReadError;
  }

  size_t magic_offset = 0;
  if (const Result result = SetLayout(svod_header, magic_offset);
      result != Result::kSuccess) {
    return result;
  }

  // Parse the root directory
  struct {
    uint32_t block;
    uint32_t size;
    uint32_t creation_date;
    uint32_t creation_time;
  } root_data;
  static_assert_size(root_data, 0x10);

  if (!ReadAt(svod_header, magic_offset + 0x14, &root_data,
              sizeof(root_data))) {
    XELOGE("ReadSVOD failed to read root block data at 0x{:016X}",
           magic_offset + 0x14);
    return Result::kReadError;
  }

  const uint64_t root_creation_timestamp =
      decode_fat_timestamp(root_data.creation_date, root_data.creation_time);

  auto root_entry = new SvodContainerEntry(this, nullptr, "", &files_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry->access_timestamp_ = root_creation_timestamp;
  root_entry->create_timestamp_ = root_creation_timestamp;
  root_entry->write_timestamp_ = root_creation_timestamp;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  // Traverse all child entries
  const Result result = ReadEntry(root_data.block, 0, root_entry);
  directory_blocks_.clear();
  return result;
}

const uint8_t* SvodContainerDevice::ReadDirectoryBlock(size_t block) {
  auto [it, inserted] = directory_blocks_.try_emplace(block);
  if (!inserted) {
    return it->second.data();
  }

  size_t address = 0, file_index = 0;
  BlockToOffset(block, &address, &file_index);
  auto* file = files_.Get(file_index);
  if (!file || !ReadAt(file, address, it->second.data(), it->second.size())) {
    XELOGE("ReadEntrySVOD failed to read directory block at {:016X}", address);
    directory_blocks_.erase(it);
    return nullptr;
  }
  return it->second.data();
}

SvodContainerDevice::Result SvodContainerDevice::ReadEntry(
    uint32_t block, uint32_t ordinal, SvodContainerEntry* parent) {
  // For games with a large amount of files, the ordinal offset can overrun
  // the current block and potentially hit a hash block.
  const size_t ordinal_offset = size_t(ordinal) * 4;
  const uint8_t* directory_block =
      ReadDirectoryBlock(block + ordinal_offset / kGdfxSectorSize);
  if (!directory_block) {
    return Result::kReadError;
  }
  const size_t entry_offset = ordinal_offset % kGdfxSectorSize;
  const uint8_t* entry_data = directory_block + entry_offset;

#pragma pack(push, 1)
  struct {
    uint16_t node_l;
    uint16_t node_r;
    uint32_t data_block;
    uint32_t length;
    uint8_t attributes;
    uint8_t name_length;
  } dir_entry;
  static_assert_size(dir_entry, 0xE);
#pragma pack(pop)

  if (entry_offset + sizeof(uint16_t) > kGdfxSectorSize) {
    return Result::kReadError;
  }

  // An entry never spans blocks, so the end of one may only hold padding.
  if (xe::load<uint16_t>(entry_data) == kGdfxPaddingOffset) {
    const uint32_t padded_ordinal = GdfxPaddedEntryOrdinal(ordinal);
    return padded_ordinal ? ReadEntry(block, padded_ordinal, parent)
                          : Result::kSuccess;
  }

  if (entry_offset + sizeof(dir_entry) > kGdfxSectorSize) {
    XELOGE("ReadEntrySVOD directory entry {} of block {:08X} spans blocks",
           ordinal, block);
    return Result::kReadError;
  }
  std::memcpy(&dir_entry, entry_data, sizeof(dir_entry));
  if (entry_offset + sizeof(dir_entry) + dir_entry.name_length >
      kGdfxSectorSize) {
    XELOGE("ReadEntrySVOD directory entry {} of block {:08X} spans blocks",
           ordinal, block);
    return Result::kReadError;
  }

  // Filename is stored as Windows-1252, convert it to UTF-8.
  auto ansi_name =
      std::string(reinterpret_cast<const char*>(entry_data + sizeof(dir_entry)),
                  dir_entry.name_length);
  auto name = xe::win1252_to_utf8(ansi_name);

  // Fallback to normal name if for whatever reason conversion from 1252 code
  // page failed.
  if (name.empty()) {
    name = ansi_name;
  }

  // Read the left node
  if (dir_entry.node_l) {
    auto node_result = ReadEntry(block, dir_entry.node_l, parent);
    if (node_result != Result::kSuccess) {
      return node_result;
    }
  }

  // Read file & address of block's data
  size_t data_address = 0, data_file = 0;
  BlockToOffset(dir_entry.data_block, &data_address, &data_file);

  // Create the entry
  auto entry = SvodContainerEntry::Create(this, parent, name, &files_);
  if (dir_entry.attributes & kFileAttributeDirectory) {
    // Entry is a directory
    entry->attributes_ = kFileAttributeDirectory | kFileAttributeReadOnly;
    entry->data_offset_ = 0;
    entry->data_size_ = 0;
    entry->block_ = block;
    entry->access_timestamp_ = root_entry_->create_timestamp();
    entry->create_timestamp_ = root_entry_->create_timestamp();
    entry->write_timestamp_ = root_entry_->create_timestamp();

    if (dir_entry.length) {
      // If length is greater than 0, traverse the directory's children
      auto directory_result = ReadEntry(dir_entry.data_block, 0, entry.get());
      if (directory_result != Result::kSuccess) {
        return directory_result;
      }
    }
  } else {
    // Entry is a file
    entry->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
    entry->size_ = dir_entry.length;
    entry->allocation_size_ = xe::round_up(dir_entry.length, kBlockSize);
    entry->data_offset_ = data_address;
    entry->data_size_ = dir_entry.length;
    entry->block_ = dir_entry.data_block;
    entry->access_timestamp_ = root_entry_->create_timestamp();
    entry->create_timestamp_ = root_entry_->create_timestamp();
    entry->write_timestamp_ = root_entry_->create_timestamp();

    // Fill in all block records, sector by sector.
    if (entry->attributes() & X_FILE_ATTRIBUTE_NORMAL) {
      uint32_t block_index = dir_entry.data_block;
      size_t remaining_size = xe::round_up(dir_entry.length, 0x800);

      size_t last_record = ~size_t(0);
      size_t last_offset = ~size_t(0);
      size_t last_file = ~size_t(0);
      while (remaining_size) {
        constexpr size_t BLOCK_SIZE = 0x800;

        size_t offset = 0, file_index = 0;
        BlockToOffset(block_index, &offset, &file_index);

        block_index++;
        remaining_size -= BLOCK_SIZE;

        if (last_record != ~size_t(0) && file_index == last_file &&
            offset - last_offset == BLOCK_SIZE) {
          // Consecutive within the same fragment, so append to last entry.
          entry->block_list_[last_record].length += BLOCK_SIZE;
          last_offset = offset;
          continue;
        }

        entry->block_list_.push_back({file_index, offset, BLOCK_SIZE});
        last_record = entry->block_list_.size() - 1;
        last_offset = offset;
        last_file = file_index;
      }
    }
  }

  parent->children_.emplace_back(std::move(entry));

  // Read the right node.
  if (dir_entry.node_r) {
    auto node_result = ReadEntry(block, dir_entry.node_r, parent);
    if (node_result != Result::kSuccess) {
      return node_result;
    }
  }

  return Result::kSuccess;
}

XContentContainerDevice::Result SvodContainerDevice::SetLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  if (IsEDGFLayout()) {
    return SetEDGFLayout(header, magic_offset);
  }

  if (IsXSFLayout(header)) {
    return SetXSFLayout(header, magic_offset);
  }

  return SetNormalLayout(header, magic_offset);
}

XContentContainerDevice::Result SvodContainerDevice::SetEDGFLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  uint8_t magic_buf[20];
  if (!ReadAt(header, 0x2000, magic_buf, countof(magic_buf))) {
    XELOGE("ReadSVOD failed to read SVOD magic at 0x2000");
    return Result::kReadError;
  }

  if (std::memcmp(magic_buf, MEDIA_MAGIC, countof(magic_buf)) != 0) {
    XELOGE("SVOD uses an EGDF layout, but the magic block was not found.");
    return Result::kFileMismatch;
  }

  svod_base_offset_ = 0x0000;
  magic_offset = 0x2000;
  svod_layout_ = SvodLayoutType::kEnhancedGDF;
  XELOGI("SVOD uses an EGDF layout. Magic block present at 0x2000.");
  return Result::kSuccess;
}

bool SvodContainerDevice::IsXSFLayout(
    xe::filesystem::FileHandle* header) const {
  uint8_t magic_buf[20];
  if (!ReadAt(header, 0x12000, magic_buf, countof(magic_buf))) {
    XELOGE("ReadSVOD failed to read SVOD magic at 0x12000");
    return false;
  }

  return std::memcmp(magic_buf, MEDIA_MAGIC, countof(magic_buf)) == 0;
}

XContentContainerDevice::Result SvodContainerDevice::SetXSFLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  uint8_t magic_buf[20];
  const char* XSF_MAGIC = "XSF";

  if (!ReadAt(header, 0x2000, magic_buf, 3)) {
    XELOGE("ReadSVOD failed to read SVOD XSF magic at 0x2000");
    return Result::kReadError;
  }

  svod_base_offset_ = 0x10000;
  magic_offset = 0x12000;

  if (std::memcmp(magic_buf, XSF_MAGIC, 3) != 0) {
    svod_layout_ = SvodLayoutType::kUnknown;
    XELOGI("SVOD appears to use an XSF layout, but no header is present.");
    XELOGI("SVOD magic block found at 0x12000");
    return Result::kSuccess;
  }

  svod_layout_ = SvodLayoutType::kXSF;
  XELOGI("SVOD uses an XSF layout. Magic block present at 0x12000.");
  XELOGI("Game was likely converted using a third-party tool.");
  return Result::kSuccess;
}

XContentContainerDevice::Result SvodContainerDevice::SetNormalLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  uint8_t magic_buf[20];

  // A single-file system usually has its magic block at 0xD000: the STFS
  // header is 0xB000 bytes and the remaining 0x2000 is from hash tables. In
  // most cases, these will be STFS, not SVOD. Some single-file packages are
  // laid out like multiple-file ones instead, with it at 0x2000.
  if (header_->content_metadata.data_file_count == 1 &&
      ReadAt(header, 0xD000, magic_buf, countof(magic_buf)) &&
      std::memcmp(magic_buf, MEDIA_MAGIC, countof(magic_buf)) == 0) {
    svod_base_offset_ = 0xB000;
    svod_layout_ = SvodLayoutType::kSingleFile;
    XELOGI("SVOD is a single file. Magic block present at 0xD000.");
    magic_offset = 0xD000;
    return Result::kSuccess;
  }

  if (!ReadAt(header, 0x2000, magic_buf, countof(magic_buf))) {
    XELOGE("ReadSVOD failed to read SVOD magic at 0x2000");
    return Result::kReadError;
  }
  if (std::memcmp(magic_buf, MEDIA_MAGIC, countof(magic_buf)) != 0) {
    XELOGE("Could not locate SVOD magic block.");
    return Result::kReadError;
  }

  svod_base_offset_ = 0x0000;
  svod_layout_ = SvodLayoutType::kMultipleFiles;
  XELOGI("SVOD uses a multiple-file layout. Magic block present at 0x2000.");
  magic_offset = 0x2000;
  return Result::kSuccess;
}

void SvodContainerDevice::BlockToOffset(size_t block, size_t* out_address,
                                        size_t* out_file_index) const {
  constexpr size_t BLOCK_SIZE = 0x800;
  constexpr size_t HASH_BLOCK_SIZE = 0x1000;
  constexpr size_t BLOCKS_PER_L0_HASH = 0x198;
  constexpr size_t HASHES_PER_L1_HASH = 0xA1C4;
  constexpr size_t BLOCKS_PER_FILE = 0x14388;
  constexpr size_t MAX_FILE_SIZE = 0xA290000;
  const size_t BLOCK_OFFSET =
      header_->content_metadata.volume_descriptor.svod.start_data_block();

  // Resolve the true block address and file index
  size_t true_block = block - (BLOCK_OFFSET * 2);
  if (svod_layout_ == SvodLayoutType::kEnhancedGDF) {
    // EGDF has an 0x1000 byte offset, which is two blocks
    true_block += 0x2;
  }

  size_t file_block = true_block % BLOCKS_PER_FILE;
  size_t file_index = true_block / BLOCKS_PER_FILE;
  size_t offset = 0;

  // Calculate offset caused by Level0 Hash Tables
  size_t level0_table_count = (file_block / BLOCKS_PER_L0_HASH) + 1;
  offset += level0_table_count * HASH_BLOCK_SIZE;

  // Calculate offset caused by Level1 Hash Tables
  size_t level1_table_count = (level0_table_count / HASHES_PER_L1_HASH) + 1;
  offset += level1_table_count * HASH_BLOCK_SIZE;

  // For single-file SVOD layouts, include the size of the header in the offset.
  if (svod_layout_ == SvodLayoutType::kSingleFile) {
    offset += svod_base_offset_;
  }

  size_t block_address = (file_block * BLOCK_SIZE) + offset;

  // If the offset causes the block address to overrun the file, round it.
  if (block_address >= MAX_FILE_SIZE) {
    file_index += 1;
    block_address %= MAX_FILE_SIZE;
    block_address += 0x2000;
  }

  *out_address = block_address;
  *out_file_index = file_index;
}

}  // namespace vfs
}  // namespace xe
