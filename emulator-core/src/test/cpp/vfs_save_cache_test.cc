// Native host test for VFS save renames, in-place overwrites, real host disk
// space reporting, per-title cache: utility drive remounting, and NullDevice
// Partition0 cache table + FATX ('XTAF') partition formatting (#125).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe::logging {
bool ShouldLog(LogLevel, uint32_t) { return false; }
namespace internal {
std::pair<char*, size_t> GetThreadBuffer() {
  static thread_local char buffer[1024];
  return {buffer, sizeof(buffer)};
}
void AppendLogLine(LogLevel, const char, size_t) {}
}  // namespace internal
}  // namespace xe::logging

#if XE_PLATFORM_ANDROID
namespace xe::filesystem {
int OpenAndroidContentFileDescriptor(const std::string_view, const char*) {
  return -1;
}
}  // namespace xe::filesystem
#endif

namespace {

using namespace xe;

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
  }
}

std::vector<uint8_t> read_entry_bytes(xe::vfs::Entry* entry) {
  xe::vfs::File* file = nullptr;
  expect(entry->Open(xe::vfs::FileAccess::kFileReadData, &file) ==
             X_STATUS_SUCCESS,
         "open entry for read");
  if (!file) {
    return {};
  }
  entry->update();
  std::vector<uint8_t> out(entry->size(), 0);
  size_t bytes_read = 0;
  expect(file->ReadSync(out, 0, &bytes_read) == X_STATUS_SUCCESS &&
             bytes_read == out.size(),
         "read all bytes");
  file->Destroy();
  return out;
}

void test_mass_effect_save_renames_and_overwrites(
    const std::filesystem::path& scratch) {
  using namespace xe;
  using namespace xe::vfs;

  const auto save_host_dir = scratch / "4D5307E8" / "00000001" / "Char_01";
  std::filesystem::create_directories(save_host_dir);

  VirtualFileSystem vfs;
  const std::string mount_path = "\\Device\\Content\\1\\";
  auto save_dev =
      std::make_unique<HostPathDevice>(mount_path, save_host_dir, false);
  expect(save_dev->Initialize(), "initialize save HostPathDevice");
  expect(save_dev->total_allocation_units() > 0 &&
             save_dev->available_allocation_units() > 0,
         "HostPathDevice reports real host disk capacity and free space");
  expect(vfs.RegisterDevice(std::move(save_dev)), "register save device");
  expect(vfs.RegisterSymbolicLink("save:", mount_path), "link save:");

  // 1) Atomic save pattern used by Mass Effect (4D5307E8): write .tmp then
  // rename via bare target name, symlink path, and multi-component mount path.
  FileAction action = FileAction::kDoesNotExist;
  File* tmp_file = nullptr;
  expect(vfs.OpenFile(nullptr, "save:\\Save_player_29264_00.tmp",
                      FileDisposition::kCreate, FileAccess::kFileWriteData,
                      false, false, &tmp_file, &action) == X_STATUS_SUCCESS,
         "create Save_player_29264_00.tmp");
  const std::vector<uint8_t> payload_v1 = {'M', 'E', '1', '_', 'V', '1'};
  size_t written = 0;
  expect(tmp_file->WriteSync(payload_v1, 0, &written) == X_STATUS_SUCCESS,
         "write Save_player_29264_00.tmp");
  Entry* tmp_entry = tmp_file->entry();
  tmp_file->Destroy();

  // Rename via multi-component device path (\Device\Content\1\...) — previously
  // broke in Entry::Rename because only a single path component was stripped!
  expect(vfs.RenameEntry(tmp_entry,
                         "\\Device\\Content\\1\\Char_01.MassEffectSave",
                         true) == X_STATUS_SUCCESS,
         "rename .tmp via multi-component \\Device\\Content\\1\\ path");
  expect(vfs.ResolvePath("save:\\Save_player_29264_00.tmp") == nullptr,
         ".tmp entry removed after rename");
  Entry* final_save = vfs.ResolvePath("save:\\Char_01.MassEffectSave");
  expect(final_save != nullptr && read_entry_bytes(final_save) == payload_v1,
         "renamed save readable through save: symlink");

  // 2) Replace existing save via save:\ symlink rename from a new .tmp.
  expect(vfs.OpenFile(nullptr, "save:\\Save_player_29265_00.tmp",
                      FileDisposition::kCreate, FileAccess::kFileWriteData,
                      false, false, &tmp_file, &action) == X_STATUS_SUCCESS,
         "create second .tmp save");
  const std::vector<uint8_t> payload_v2 = {'M', 'E', '1', '_', 'S',
                                           'A', 'V', 'E', '_', '2'};
  expect(tmp_file->WriteSync(payload_v2, 0, &written) == X_STATUS_SUCCESS,
         "write second .tmp save");
  tmp_entry = tmp_file->entry();
  tmp_file->Destroy();

  expect(vfs.RenameEntry(tmp_entry, "save:\\Char_01.MassEffectSave", true) ==
             X_STATUS_SUCCESS,
         "replace existing save via save:\\ symlink rename");
  final_save = vfs.ResolvePath("save:\\Char_01.MassEffectSave");
  expect(final_save != nullptr && read_entry_bytes(final_save) == payload_v2,
         "replaced save contains v2 bytes");

  // 3) Bare-filename rename in current directory.
  expect(vfs.RenameEntry(final_save, "AutoSave.MassEffectSave", false) ==
             X_STATUS_SUCCESS,
         "rename via bare filename");
  Entry* autosave = vfs.ResolvePath("save:\\AutoSave.MassEffectSave");
  expect(autosave != nullptr && read_entry_bytes(autosave) == payload_v2,
         "bare-renamed AutoSave readable");

  // 4) In-place HostPathEntry::Overwrite() with shorter content trims on final
  // handle close without breaking open handles.
  File* overwrite_file = nullptr;
  expect(vfs.OpenFile(nullptr, "save:\\AutoSave.MassEffectSave",
                      FileDisposition::kOverwriteIf, FileAccess::kFileWriteData,
                      false, false, &overwrite_file, &action) ==
             X_STATUS_SUCCESS &&
             action == FileAction::kOverwritten,
         "open existing save with kOverwriteIf");
  expect(overwrite_file->entry()->size() == 0,
         "Overwrite resets visible entry size to 0");
  const std::vector<uint8_t> short_payload = {'O', 'K'};
  expect(overwrite_file->WriteSync(short_payload, 0, &written) ==
             X_STATUS_SUCCESS,
         "write shorter payload into overwritten entry");
  overwrite_file->Destroy();

  expect(std::filesystem::file_size(save_host_dir /
                                    "AutoSave.MassEffectSave") == 2 &&
             read_entry_bytes(autosave) == short_payload,
         "host file truncated cleanly to new shorter length on close");
}

void test_cache_partitions_and_utility_drive_remount(
    const std::filesystem::path& scratch) {
  using namespace xe;
  using namespace xe::vfs;

  const auto cache_dir = scratch / "cache" / "4D5307E8";
  const auto header_path = scratch / "cache" / "Headers" / "4D5307E8.header";

  VirtualFileSystem vfs;
  auto cache_dev = std::make_unique<HostPathDevice>("\\CACHE", cache_dir, false);
  expect(cache_dev->Initialize(), "initialize per-title \\CACHE HostPathDevice");
  expect(vfs.RegisterDevice(std::move(cache_dev)), "register \\CACHE");
  expect(vfs.RegisterSymbolicLink("\\Device\\cache0", "\\CACHE"),
         "link \\Device\\cache0 -> \\CACHE");
  expect(vfs.RegisterSymbolicLink("\\Device\\cache1", "\\CACHE"),
         "link \\Device\\cache1 -> \\CACHE");

  const auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                           std::string("\\Cache1")};
  auto raw_dev =
      std::make_unique<NullDevice>("\\Device\\Harddisk0", null_paths);
  expect(raw_dev->Initialize(), "initialize Harddisk0 NullDevice");
  NullDevice* raw = raw_dev.get();
  raw->LoadCacheHeader(header_path);

  int format_calls = 0;
  raw->set_cache_format_handler([&]() {
    ++format_calls;
    Entry* root = vfs.ResolvePath("\\CACHE");
    if (!root) {
      return;
    }
    std::vector<Entry*> children;
    for (const auto& child : root->children()) {
      children.push_back(child.get());
    }
    for (Entry* child : children) {
      root->Delete(child);
    }
  });
  expect(vfs.RegisterDevice(std::move(raw_dev)), "register Harddisk0");

  // Simulate XMountUtilityDrive linking cache: -> \Device\cache0, writing a
  // cached asset, then unmounting and remounting cache: -> \Device\cache1.
  expect(vfs.RegisterSymbolicLink("cache:", "\\Device\\cache0"),
         "mount cache: -> \\Device\\cache0");
  FileAction action = FileAction::kDoesNotExist;
  File* cached_file = nullptr;
  expect(vfs.OpenFile(nullptr, "cache:\\shader_table.bin",
                      FileDisposition::kCreate, FileAccess::kFileWriteData,
                      false, false, &cached_file, &action) == X_STATUS_SUCCESS,
         "create file on cache:");
  const std::vector<uint8_t> cached_bytes = {1, 2, 3, 4};
  size_t io_bytes = 0;
  expect(cached_file->WriteSync(cached_bytes, 0, &io_bytes) == X_STATUS_SUCCESS,
         "write file on cache:");
  cached_file->Destroy();

  expect(vfs.UnregisterSymbolicLink("cache:") &&
             vfs.RegisterSymbolicLink("cache:", "\\Device\\cache1"),
         "remount cache: -> \\Device\\cache1");
  Entry* resolved_cached = vfs.ResolvePath("cache:\\shader_table.bin");
  expect(resolved_cached != nullptr &&
             read_entry_bytes(resolved_cached) == cached_bytes,
         "cache: keeps working across utility drive remount");

  // Partition0 cache table persistence (offset 0x800, size 0x400).
  Entry* part0 = vfs.ResolvePath("\\Device\\Harddisk0\\Partition0");
  expect(part0 != nullptr, "resolve Partition0");
  File* p0_file = nullptr;
  expect(part0->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                     &p0_file) == X_STATUS_SUCCESS,
         "open Partition0");
  std::vector<uint8_t> table(0x400);
  for (size_t i = 0; i < table.size(); ++i) {
    table[i] = static_cast<uint8_t>((i * 13) ^ 0x5A);
  }
  expect(p0_file->WriteSync(table, 0x800, &io_bytes) == X_STATUS_SUCCESS,
         "write Partition0 cache table");
  p0_file->Destroy();

  // Format Cache0 by writing a FATX ('XTAF') header with 8 sectors/cluster.
  Entry* cache0_part = vfs.ResolvePath("\\Device\\Harddisk0\\Cache0");
  expect(cache0_part != nullptr, "resolve Cache0 raw partition");
  expect(cache0_part->sectors_per_allocation_unit() == 0x80,
         "Cache0 starts with default 0x80 sectors/cluster");
  File* c0_file = nullptr;
  expect(cache0_part->Open(FileAccess::kFileWriteData, &c0_file) ==
             X_STATUS_SUCCESS,
         "open Cache0 raw partition");
  std::vector<uint8_t> fatx_hdr(0x1000, 0);
  xe::store_and_swap<uint32_t>(fatx_hdr.data(), 0x58544146);  // 'XTAF'
  xe::store_and_swap<uint32_t>(fatx_hdr.data() + 8, 8);
  expect(c0_file->WriteSync(fatx_hdr, 0, &io_bytes) == X_STATUS_SUCCESS,
         "write FATX header to Cache0");
  c0_file->Destroy();

  expect(format_calls == 1, "FATX format invoked cache_format_handler once");
  expect(cache0_part->sectors_per_allocation_unit() == 8,
         "Cache0 reports formatted 8 sectors/cluster");
  expect(vfs.ResolvePath("cache:\\shader_table.bin") == nullptr,
         "formatting Cache0 emptied title cache: folder");

  // Reload header into a fresh NullDevice (simulating RelaunchTitle / next
  // boot) and verify both Partition0 table and Cache0 cluster size persist.
  auto next_raw =
      std::make_unique<NullDevice>("\\Device\\Harddisk0", null_paths);
  expect(next_raw->Initialize(), "initialize reloaded NullDevice");
  next_raw->LoadCacheHeader(header_path);
  expect(next_raw->ResolvePath("\\Cache0")->sectors_per_allocation_unit() == 8,
         "reloaded NullDevice restores formatted Cache0 cluster size");

  File* next_p0 = nullptr;
  expect(next_raw->ResolvePath("\\Partition0")
                 ->Open(FileAccess::kFileReadData, &next_p0) ==
             X_STATUS_SUCCESS,
         "open reloaded Partition0");
  std::vector<uint8_t> read_back(0x400, 0);
  expect(next_p0->ReadSync(read_back, 0x800, &io_bytes) == X_STATUS_SUCCESS &&
             io_bytes == 0x400 && read_back == table,
         "reloaded NullDevice restores Partition0 cache table");
  next_p0->Destroy();
}

}  // namespace

int main(int argc, char** argv) {
  std::error_code tmp_ec;
  const auto base_tmp =
      argc > 1 ? std::filesystem::path(argv[1])
               : (std::filesystem::temp_directory_path(tmp_ec) /
                  "xendroid-vfs-save-cache-scratch");
  const auto scratch =
      (tmp_ec ? std::filesystem::path("/tmp") : base_tmp) /
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(scratch);

  test_mass_effect_save_renames_and_overwrites(scratch);
  test_cache_partitions_and_utility_drive_remount(scratch);

  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);

  if (failures == 0) {
    std::printf("vfs_save_cache_test: ok\n");
  }
  return failures == 0 ? 0 : 1;
}
