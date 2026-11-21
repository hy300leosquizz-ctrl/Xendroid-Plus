// SPDX-License-Identifier: WTFPL
// Android Storage Access Framework (content://) multi-file tree & document VFS
// support for extracted .xex folders and multi-fragment SVOD/GOD containers on
// microSD / USB OTG, adapted cleanly from aenu1/ax360e document_file (WTFPL)
// using R8-safe Android framework DocumentsContract + ContentResolver JNI and
// 64-bit pread64 with bounded FD caching.

#ifndef XENIA_VFS_SAF_TREE_DEVICE_H_
#define XENIA_VFS_SAF_TREE_DEVICE_H_

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
#include <fcntl.h>
#include <jni.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern jobject g_context;
#endif

namespace xe {
namespace vfs {
namespace saf {

struct SafChildInfo {
  std::string uri;
  std::string document_id;
  std::string name;
  uint64_t size = 0;
  uint64_t last_modified_ms = 0;
  bool is_directory = false;
};

inline bool IsSafContentUri(std::string_view path_or_uri) {
  return path_or_uri.size() > 10 &&
         path_or_uri.substr(0, 10) == "content://";
}

#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid

class ScopedJniEnv {
 public:
  ScopedJniEnv() {
    if (!g_context) {
      return;
    }
    // Obtain JavaVM from g_context's object header isn't direct; cache JavaVM
    // lazily or query active VM via JNI_GetCreatedJavaVMs if linked, or from
    // StoredJavaVm().
    JavaVM* vm = StoredJavaVm();
    if (!vm) {
      return;
    }
    jint status = vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
      if (vm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
        attached_ = true;
      } else {
        env_ = nullptr;
      }
    }
  }

  ~ScopedJniEnv() {
    if (attached_) {
      if (JavaVM* vm = StoredJavaVm()) {
        vm->DetachCurrentThread();
      }
    }
  }

  JNIEnv* get() const { return env_; }
  explicit operator bool() const { return env_ != nullptr && g_context != nullptr; }

  static JavaVM*& StoredJavaVm() {
    static JavaVM* s_vm = nullptr;
    return s_vm;
  }

  static void SetJavaVmFromEnv(JNIEnv* env) {
    if (!env || StoredJavaVm()) {
      return;
    }
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) == JNI_OK && vm) {
      StoredJavaVm() = vm;
    }
  }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

inline bool ClearJniException(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return true;
  }
  return false;
}

inline std::string JStringToStdString(JNIEnv* env, jstring jstr) {
  if (!jstr) {
    return {};
  }
  const jchar* chars = env->GetStringChars(jstr, nullptr);
  const jsize len = env->GetStringLength(jstr);
  if (!chars) {
    ClearJniException(env);
    return {};
  }
  std::string out = xe::to_utf8(
      std::u16string_view(reinterpret_cast<const char16_t*>(chars), size_t(len)));
  env->ReleaseStringChars(jstr, chars);
  return out;
}

inline jobject ParseAndroidUri(JNIEnv* env, std::string_view uri_str) {
  jclass uri_cls = env->FindClass("android/net/Uri");
  if (!uri_cls || ClearJniException(env)) {
    return nullptr;
  }
  jmethodID parse_mid = env->GetStaticMethodID(
      uri_cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
  if (!parse_mid || ClearJniException(env)) {
    env->DeleteLocalRef(uri_cls);
    return nullptr;
  }
  std::u16string u16 = xe::to_utf16(uri_str);
  jstring j_uri_str = env->NewString(
      reinterpret_cast<const jchar*>(u16.data()), static_cast<jsize>(u16.size()));
  jobject uri_obj =
      j_uri_str ? env->CallStaticObjectMethod(uri_cls, parse_mid, j_uri_str)
                : nullptr;
  if (j_uri_str) {
    env->DeleteLocalRef(j_uri_str);
  }
  env->DeleteLocalRef(uri_cls);
  if (ClearJniException(env)) {
    return nullptr;
  }
  return uri_obj;
}

inline std::string UriToStdString(JNIEnv* env, jobject uri_obj) {
  if (!uri_obj) {
    return {};
  }
  jclass obj_cls = env->GetObjectClass(uri_obj);
  jmethodID to_str =
      env->GetMethodID(obj_cls, "toString", "()Ljava/lang/String;");
  env->DeleteLocalRef(obj_cls);
  if (!to_str || ClearJniException(env)) {
    return {};
  }
  auto jstr = static_cast<jstring>(env->CallObjectMethod(uri_obj, to_str));
  if (ClearJniException(env) || !jstr) {
    return {};
  }
  std::string out = JStringToStdString(env, jstr);
  env->DeleteLocalRef(jstr);
  return out;
}

// Opens a read-only detached POSIX file descriptor for any content:// URI.
inline int OpenUriReadFd(std::string_view uri_str) {
  ScopedJniEnv jni;
  if (!jni) {
    return -1;
  }
  JNIEnv* env = jni.get();
  jobject uri_obj = ParseAndroidUri(env, uri_str);
  if (!uri_obj) {
    return -1;
  }

  jclass ctx_cls = env->GetObjectClass(g_context);
  jmethodID get_cr = env->GetMethodID(
      ctx_cls, "getContentResolver", "()Landroid/content/ContentResolver;");
  env->DeleteLocalRef(ctx_cls);
  if (!get_cr || ClearJniException(env)) {
    env->DeleteLocalRef(uri_obj);
    return -1;
  }

  jobject cr = env->CallObjectMethod(g_context, get_cr);
  if (ClearJniException(env) || !cr) {
    env->DeleteLocalRef(uri_obj);
    return -1;
  }

  jclass cr_cls = env->GetObjectClass(cr);
  jmethodID open_pfd = env->GetMethodID(
      cr_cls, "openFileDescriptor",
      "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
  env->DeleteLocalRef(cr_cls);
  if (!open_pfd || ClearJniException(env)) {
    env->DeleteLocalRef(cr);
    env->DeleteLocalRef(uri_obj);
    return -1;
  }

  jstring mode_r = env->NewStringUTF("r");
  jobject pfd = env->CallObjectMethod(cr, open_pfd, uri_obj, mode_r);
  env->DeleteLocalRef(mode_r);
  env->DeleteLocalRef(cr);
  env->DeleteLocalRef(uri_obj);
  if (ClearJniException(env) || !pfd) {
    return -1;
  }

  jclass pfd_cls = env->GetObjectClass(pfd);
  jmethodID detach_fd = env->GetMethodID(pfd_cls, "detachFd", "()I");
  env->DeleteLocalRef(pfd_cls);
  int fd = -1;
  if (detach_fd && !ClearJniException(env)) {
    fd = env->CallIntMethod(pfd, detach_fd);
    if (ClearJniException(env)) {
      fd = -1;
    }
  }
  env->DeleteLocalRef(pfd);
  return fd;
}

inline uint64_t QueryUriFileSize(std::string_view uri_str) {
  int fd = OpenUriReadFd(uri_str);
  if (fd < 0) {
    return 0;
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
  ::close(fd);
  return size;
}

class PosixFdFileHandle final : public xe::filesystem::FileHandle {
 public:
  PosixFdFileHandle(const std::filesystem::path& path, int fd)
      : FileHandle(path), fd_(fd) {}

  ~PosixFdFileHandle() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    if (fd_ < 0) {
      return false;
    }
    auto* dst = static_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < buffer_length) {
#if defined(__ANDROID__) || defined(__linux__)
      const ssize_t got = ::pread64(
          fd_, dst + total, buffer_length - total,
          static_cast<off64_t>(static_cast<uint64_t>(file_offset) + total));
#else
      const ssize_t got = ::pread(
          fd_, dst + total, buffer_length - total,
          static_cast<off_t>(file_offset + total));
#endif
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (got == 0) {
        break;
      }
      total += static_cast<size_t>(got);
    }
    if (out_bytes_read) {
      *out_bytes_read = total;
    }
    return true;
  }

  bool Write(size_t /*file_offset*/, const void* /*buffer*/,
             size_t /*buffer_length*/, size_t* /*out_bytes_written*/) override {
    return false;
  }

  bool SetLength(size_t /*length*/) override { return false; }
  void Flush() override {}

 private:
  int fd_ = -1;
};

inline std::unique_ptr<xe::filesystem::FileHandle> OpenUriFileHandle(
    std::string_view uri_str) {
  const int fd = OpenUriReadFd(uri_str);
  if (fd < 0) {
    return nullptr;
  }
  return std::make_unique<PosixFdFileHandle>(xe::to_path(uri_str), fd);
}

// Extracts the document_id (or treeDocumentId when the URI is the root of a
// SAF tree) and builds child document listings in one ContentResolver query.
inline std::vector<SafChildInfo> ListTreeChildren(
    std::string_view tree_uri_str, std::string_view parent_doc_id_override = {}) {
  std::vector<SafChildInfo> children;
  ScopedJniEnv jni;
  if (!jni) {
    return children;
  }
  JNIEnv* env = jni.get();

  jobject tree_uri = ParseAndroidUri(env, tree_uri_str);
  if (!tree_uri) {
    return children;
  }

  jclass dc_cls = env->FindClass("android/provider/DocumentsContract");
  if (!dc_cls || ClearJniException(env)) {
    env->DeleteLocalRef(tree_uri);
    return children;
  }

  jmethodID is_doc_mid = env->GetStaticMethodID(
      dc_cls, "isDocumentUri", "(Landroid/content/Context;Landroid/net/Uri;)Z");
  jmethodID get_doc_id_mid = env->GetStaticMethodID(
      dc_cls, "getDocumentId", "(Landroid/net/Uri;)Ljava/lang/String;");
  jmethodID get_tree_id_mid = env->GetStaticMethodID(
      dc_cls, "getTreeDocumentId", "(Landroid/net/Uri;)Ljava/lang/String;");
  jmethodID build_children_mid = env->GetStaticMethodID(
      dc_cls, "buildChildDocumentsUriUsingTree",
      "(Landroid/net/Uri;Ljava/lang/String;)Landroid/net/Uri;");
  jmethodID build_doc_mid = env->GetStaticMethodID(
      dc_cls, "buildDocumentUriUsingTree",
      "(Landroid/net/Uri;Ljava/lang/String;)Landroid/net/Uri;");

  if (!is_doc_mid || !get_doc_id_mid || !get_tree_id_mid ||
      !build_children_mid || !build_doc_mid || ClearJniException(env)) {
    env->DeleteLocalRef(dc_cls);
    env->DeleteLocalRef(tree_uri);
    return children;
  }

  jstring parent_doc_id = nullptr;
  if (!parent_doc_id_override.empty()) {
    std::u16string u16 = xe::to_utf16(parent_doc_id_override);
    parent_doc_id = env->NewString(
        reinterpret_cast<const jchar*>(u16.data()), static_cast<jsize>(u16.size()));
  } else {
    const jboolean is_doc =
        env->CallStaticBooleanMethod(dc_cls, is_doc_mid, g_context, tree_uri);
    if (!ClearJniException(env) && is_doc) {
      parent_doc_id = static_cast<jstring>(
          env->CallStaticObjectMethod(dc_cls, get_doc_id_mid, tree_uri));
    }
    if (ClearJniException(env) || !parent_doc_id) {
      parent_doc_id = static_cast<jstring>(
          env->CallStaticObjectMethod(dc_cls, get_tree_id_mid, tree_uri));
    }
  }
  if (ClearJniException(env) || !parent_doc_id) {
    env->DeleteLocalRef(dc_cls);
    env->DeleteLocalRef(tree_uri);
    return children;
  }

  jobject children_uri = env->CallStaticObjectMethod(
      dc_cls, build_children_mid, tree_uri, parent_doc_id);
  env->DeleteLocalRef(parent_doc_id);
  if (ClearJniException(env) || !children_uri) {
    env->DeleteLocalRef(dc_cls);
    env->DeleteLocalRef(tree_uri);
    return children;
  }

  jclass ctx_cls = env->GetObjectClass(g_context);
  jmethodID get_cr = env->GetMethodID(
      ctx_cls, "getContentResolver", "()Landroid/content/ContentResolver;");
  env->DeleteLocalRef(ctx_cls);
  jobject cr = get_cr ? env->CallObjectMethod(g_context, get_cr) : nullptr;
  if (ClearJniException(env) || !cr) {
    env->DeleteLocalRef(children_uri);
    env->DeleteLocalRef(dc_cls);
    env->DeleteLocalRef(tree_uri);
    return children;
  }

  jclass str_cls = env->FindClass("java/lang/String");
  jobjectArray projection = env->NewObjectArray(5, str_cls, nullptr);
  const char* cols[5] = {"document_id", "_display_name", "mime_type", "_size",
                         "last_modified"};
  for (jsize i = 0; i < 5; ++i) {
    jstring col = env->NewStringUTF(cols[i]);
    env->SetObjectArrayElement(projection, i, col);
    env->DeleteLocalRef(col);
  }
  env->DeleteLocalRef(str_cls);

  jclass cr_cls = env->GetObjectClass(cr);
  jmethodID query_mid = env->GetMethodID(
      cr_cls, "query",
      "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;"
      "[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;");
  env->DeleteLocalRef(cr_cls);

  jobject cursor =
      query_mid ? env->CallObjectMethod(cr, query_mid, children_uri, projection,
                                        nullptr, nullptr, nullptr)
                : nullptr;
  env->DeleteLocalRef(projection);
  env->DeleteLocalRef(cr);
  env->DeleteLocalRef(children_uri);
  if (ClearJniException(env) || !cursor) {
    env->DeleteLocalRef(dc_cls);
    env->DeleteLocalRef(tree_uri);
    return children;
  }

  jclass cur_cls = env->GetObjectClass(cursor);
  jmethodID move_next = env->GetMethodID(cur_cls, "moveToNext", "()Z");
  jmethodID get_str =
      env->GetMethodID(cur_cls, "getString", "(I)Ljava/lang/String;");
  jmethodID get_long = env->GetMethodID(cur_cls, "getLong", "(I)J");
  jmethodID is_null = env->GetMethodID(cur_cls, "isNull", "(I)Z");
  jmethodID close_cur = env->GetMethodID(cur_cls, "close", "()V");
  env->DeleteLocalRef(cur_cls);

  constexpr const char* kDirMime = "vnd.android.document/directory";
  while (move_next && env->CallBooleanMethod(cursor, move_next) &&
         !ClearJniException(env)) {
    auto j_doc_id =
        static_cast<jstring>(env->CallObjectMethod(cursor, get_str, 0));
    auto j_name =
        static_cast<jstring>(env->CallObjectMethod(cursor, get_str, 1));
    auto j_mime =
        static_cast<jstring>(env->CallObjectMethod(cursor, get_str, 2));
    if (ClearJniException(env)) {
      if (j_doc_id) env->DeleteLocalRef(j_doc_id);
      if (j_name) env->DeleteLocalRef(j_name);
      if (j_mime) env->DeleteLocalRef(j_mime);
      break;
    }

    SafChildInfo item;
    item.document_id = JStringToStdString(env, j_doc_id);
    item.name = JStringToStdString(env, j_name);
    const std::string mime = JStringToStdString(env, j_mime);
    item.is_directory = (mime == kDirMime);

    if (is_null && !env->CallBooleanMethod(cursor, is_null, 3) &&
        !ClearJniException(env)) {
      const jlong sz = env->CallLongMethod(cursor, get_long, 3);
      if (!ClearJniException(env) && sz > 0) {
        item.size = static_cast<uint64_t>(sz);
      }
    }
    if (is_null && !env->CallBooleanMethod(cursor, is_null, 4) &&
        !ClearJniException(env)) {
      const jlong lm = env->CallLongMethod(cursor, get_long, 4);
      if (!ClearJniException(env) && lm > 0) {
        item.last_modified_ms = static_cast<uint64_t>(lm);
      }
    }

    if (j_doc_id && !item.name.empty()) {
      jobject child_uri = env->CallStaticObjectMethod(dc_cls, build_doc_mid,
                                                      tree_uri, j_doc_id);
      if (!ClearJniException(env) && child_uri) {
        item.uri = UriToStdString(env, child_uri);
        env->DeleteLocalRef(child_uri);
      }
      if (!item.uri.empty()) {
        children.push_back(std::move(item));
      }
    }

    if (j_doc_id) env->DeleteLocalRef(j_doc_id);
    if (j_name) env->DeleteLocalRef(j_name);
    if (j_mime) env->DeleteLocalRef(j_mime);
  }

  if (close_cur) {
    env->CallVoidMethod(cursor, close_cur);
    ClearJniException(env);
  }
  env->DeleteLocalRef(cursor);
  env->DeleteLocalRef(dc_cls);
  env->DeleteLocalRef(tree_uri);
  return children;
}

// Given a SAF document URI pointing at a file (e.g. default.xex or an SVOD
// header file inside a SAF tree), derives its parent document_id inside the
// same tree (when encoded as ".../tree/<treeId>/document/<parentId>%2F<leaf>").
inline bool DeriveSafParentDocumentId(std::string_view doc_uri_str,
                                      std::string& out_parent_doc_id,
                                      std::string& out_leaf_name) {
  ScopedJniEnv jni;
  if (!jni) {
    return false;
  }
  JNIEnv* env = jni.get();
  jobject uri_obj = ParseAndroidUri(env, doc_uri_str);
  if (!uri_obj) {
    return false;
  }
  jclass dc_cls = env->FindClass("android/provider/DocumentsContract");
  if (!dc_cls || ClearJniException(env)) {
    env->DeleteLocalRef(uri_obj);
    return false;
  }
  jmethodID is_doc_mid = env->GetStaticMethodID(
      dc_cls, "isDocumentUri", "(Landroid/content/Context;Landroid/net/Uri;)Z");
  jmethodID get_doc_id_mid = env->GetStaticMethodID(
      dc_cls, "getDocumentId", "(Landroid/net/Uri;)Ljava/lang/String;");
  jmethodID get_tree_id_mid = env->GetStaticMethodID(
      dc_cls, "getTreeDocumentId", "(Landroid/net/Uri;)Ljava/lang/String;");

  std::string doc_id;
  std::string tree_id;
  if (is_doc_mid && get_doc_id_mid &&
      env->CallStaticBooleanMethod(dc_cls, is_doc_mid, g_context, uri_obj) &&
      !ClearJniException(env)) {
    auto j_doc = static_cast<jstring>(
        env->CallStaticObjectMethod(dc_cls, get_doc_id_mid, uri_obj));
    if (!ClearJniException(env) && j_doc) {
      doc_id = JStringToStdString(env, j_doc);
      env->DeleteLocalRef(j_doc);
    }
  }
  if (get_tree_id_mid) {
    auto j_tree = static_cast<jstring>(
        env->CallStaticObjectMethod(dc_cls, get_tree_id_mid, uri_obj));
    if (!ClearJniException(env) && j_tree) {
      tree_id = JStringToStdString(env, j_tree);
      env->DeleteLocalRef(j_tree);
    }
  }
  env->DeleteLocalRef(dc_cls);
  env->DeleteLocalRef(uri_obj);

  if (doc_id.empty() || tree_id.empty()) {
    return false;
  }
  const size_t slash = doc_id.find_last_of('/');
  if (slash != std::string::npos) {
    out_parent_doc_id = doc_id.substr(0, slash);
    out_leaf_name = doc_id.substr(slash + 1);
    return true;
  }
  const size_t colon = doc_id.find(':');
  if (colon != std::string::npos && doc_id != tree_id) {
    out_parent_doc_id = tree_id;
    out_leaf_name = doc_id.substr(colon + 1);
    return true;
  }
  return false;
}

// Enumerates <header_name>.data/Data* fragments for a multi-file SVOD/GOD
// container stored on a SAF tree (microSD / USB OTG).
inline std::vector<SafChildInfo> ListSiblingSvodFragments(
    std::string_view header_uri_str) {
  std::string parent_doc_id;
  std::string header_name;
  if (!DeriveSafParentDocumentId(header_uri_str, parent_doc_id, header_name)) {
    return {};
  }
  const std::string target_dir = header_name + ".data";
  auto siblings = ListTreeChildren(header_uri_str, parent_doc_id);
  for (const auto& item : siblings) {
    if (item.is_directory &&
        utf8::equal_case(item.name, target_dir)) {
      auto fragments = ListTreeChildren(header_uri_str, item.document_id);
      fragments.erase(
          std::remove_if(fragments.begin(), fragments.end(),
                         [](const SafChildInfo& f) { return f.is_directory; }),
          fragments.end());
      std::sort(fragments.begin(), fragments.end(),
                [](const SafChildInfo& a, const SafChildInfo& b) {
                  return a.name < b.name;
                });
      return fragments;
    }
  }
  return {};
}

#else

inline int OpenUriReadFd(std::string_view) { return -1; }
inline uint64_t QueryUriFileSize(std::string_view) { return 0; }
inline std::unique_ptr<xe::filesystem::FileHandle> OpenUriFileHandle(
    std::string_view) {
  return nullptr;
}
inline std::vector<SafChildInfo> ListTreeChildren(std::string_view,
                                                  std::string_view = {}) {
  return {};
}
inline bool DeriveSafParentDocumentId(std::string_view, std::string&,
                                      std::string&) {
  return false;
}
inline std::vector<SafChildInfo> ListSiblingSvodFragments(std::string_view) {
  return {};
}

#endif  // XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid

}  // namespace saf

#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid

class SafTreeDevice;
class SafTreeEntry;

class SafTreeFile final : public File {
 public:
  SafTreeFile(uint32_t file_access, SafTreeEntry* entry)
      : File(file_access, reinterpret_cast<Entry*>(entry)), saf_entry_(entry) {}
  ~SafTreeFile() override = default;

  void Destroy() override { delete this; }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> /*buffer*/, size_t /*byte_offset*/,
                     size_t* /*out_bytes_written*/) override {
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t /*length*/) override {
    return X_STATUS_ACCESS_DENIED;
  }

 private:
  SafTreeEntry* saf_entry_;
};

class SafTreeEntry final : public Entry {
 public:
  SafTreeEntry(Device* device, Entry* parent, std::string_view path,
               std::string uri, std::string document_id)
      : Entry(device, parent, path),
        uri_(std::move(uri)),
        document_id_(std::move(document_id)) {}
  ~SafTreeEntry() override = default;

  const std::string& uri() const { return uri_; }
  const std::string& document_id() const { return document_id_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override {
    *out_file = new SafTreeFile(desired_access, this);
    return X_STATUS_SUCCESS;
  }

  bool can_map() const override {
    return (attributes_ & kFileAttributeDirectory) == 0;
  }

  std::unique_ptr<MappedMemory> OpenMapped(MappedMemory::Mode mode,
                                           size_t offset,
                                           size_t length) override {
    if (mode != MappedMemory::Mode::kRead ||
        (attributes_ & kFileAttributeDirectory) != 0) {
      return nullptr;
    }
    const int fd = saf::OpenUriReadFd(uri_);
    if (fd < 0) {
      return nullptr;
    }
#if XE_PLATFORM_xendroid
    auto whole = MappedMemory::OpenForUnixFd(fd);
    if (!whole) {
      return nullptr;
    }
#else
    struct stat64 st = {};
    if (::fstat64(fd, &st) != 0 || st.st_size <= 0) {
      ::close(fd);
      return nullptr;
    }
    const size_t file_len = static_cast<size_t>(st.st_size);
    void* mapped = ::mmap64(nullptr, file_len, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
      return nullptr;
    }
    class DirectPosixMapped final : public MappedMemory {
     public:
      DirectPosixMapped(void* p, size_t len)
          : MappedMemory(p, len), raw_(p), raw_len_(len) {}
      ~DirectPosixMapped() override { Close(0); }
      void Close(uint64_t /*truncate_size*/) override {
        if (raw_) {
          ::munmap(raw_, raw_len_);
          raw_ = nullptr;
          raw_len_ = 0;
          data_ = nullptr;
          size_ = 0;
        }
      }

     private:
      void* raw_;
      size_t raw_len_;
    };
    std::unique_ptr<MappedMemory> whole =
        std::make_unique<DirectPosixMapped>(mapped, file_len);
#endif
    if (offset == 0 && (length == 0 || length >= whole->size())) {
      return whole;
    }
    if (offset >= whole->size()) {
      return nullptr;
    }
    const size_t slice_len = std::min(length ? length : whole->size() - offset,
                                      whole->size() - offset);
    class OwningMappedSlice final : public MappedMemory {
     public:
      OwningMappedSlice(std::unique_ptr<MappedMemory> owner, size_t off,
                        size_t len)
          : MappedMemory(owner->data() + off, len), owner_(std::move(owner)) {}
      void Close(uint64_t t) override {
        if (owner_) {
          owner_->Close(t);
          owner_.reset();
          data_ = nullptr;
          size_ = 0;
        }
      }

     private:
      std::unique_ptr<MappedMemory> owner_;
    };
    return std::make_unique<OwningMappedSlice>(std::move(whole), offset,
                                               slice_len);
  }

 private:
  friend class SafTreeDevice;
  bool DeleteEntryInternal(Entry* /*entry*/) override { return false; }

  std::string uri_;
  std::string document_id_;
};

// Bounded FD pool + tree-backed VFS device for extracted .xex directories on
// Android SAF (microSD / USB OTG). Keeps at most kMaxCachedFds open descriptors
// so games with thousands of loose files never trigger EMFILE.
class SafTreeDevice final : public Device {
 public:
  static constexpr size_t kMaxCachedFds = 64;

  SafTreeDevice(std::string_view mount_path, std::string tree_uri,
                std::string root_doc_id = {}, std::string fs_name = "GDFX")
      : Device(mount_path),
        name_(std::move(fs_name)),
        tree_uri_(std::move(tree_uri)),
        root_doc_id_(std::move(root_doc_id)) {}

  ~SafTreeDevice() override {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    for (auto& slot : fd_cache_) {
      if (slot.fd >= 0) {
        ::close(slot.fd);
        slot.fd = -1;
      }
    }
  }

  bool Initialize() override {
    auto root = std::make_unique<SafTreeEntry>(this, nullptr, "", tree_uri_,
                                               root_doc_id_);
    root->attributes_ = kFileAttributeDirectory | kFileAttributeReadOnly;
    SafTreeEntry* root_ptr = root.get();
    root_entry_ = std::move(root);
    PopulateDirectory(root_ptr, 0);
    return !root_ptr->children_.empty();
  }

  void Dump(StringBuffer* string_buffer) override {
    auto global_lock = global_critical_region_.Acquire();
    if (root_entry_) {
      root_entry_->Dump(string_buffer, 0);
    }
  }

  Entry* ResolvePath(const std::string_view path) override {
    return root_entry_ ? root_entry_->ResolvePath(path) : nullptr;
  }

  bool is_read_only() const override { return true; }
  bool supports_concurrent_io() const override { return true; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 16; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  X_STATUS ReadEntryAt(const SafTreeEntry* entry, std::span<uint8_t> buffer,
                       size_t byte_offset, size_t* out_bytes_read) {
    if (byte_offset >= entry->size()) {
      return X_STATUS_END_OF_FILE;
    }
    const size_t want = std::min(buffer.size(), entry->size() - byte_offset);
    if (want == 0) {
      *out_bytes_read = 0;
      return X_STATUS_SUCCESS;
    }

    const int dup_fd = AcquireDupFd(entry->uri());
    if (dup_fd < 0) {
      return X_STATUS_ACCESS_DENIED;
    }
    size_t total = 0;
    while (total < want) {
      const ssize_t got = ::pread64(
          dup_fd, buffer.data() + total, want - total,
          static_cast<off64_t>(static_cast<uint64_t>(byte_offset) + total));
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        ::close(dup_fd);
        return X_STATUS_UNSUCCESSFUL;
      }
      if (got == 0) {
        break;
      }
      total += static_cast<size_t>(got);
    }
    ::close(dup_fd);
    *out_bytes_read = total;
    return total > 0 ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
  }

 private:
  struct CachedFd {
    std::string uri;
    int fd = -1;
    uint64_t tick = 0;
  };

  int AcquireDupFd(const std::string& uri) {
    {
      std::lock_guard<std::mutex> lock(fd_mutex_);
      for (auto& slot : fd_cache_) {
        if (slot.fd >= 0 && slot.uri == uri) {
          slot.tick = ++fd_clock_;
          return ::dup(slot.fd);
        }
      }
    }

    const int opened = saf::OpenUriReadFd(uri);
    if (opened < 0) {
      return -1;
    }
    const int caller_fd = ::dup(opened);

    std::lock_guard<std::mutex> lock(fd_mutex_);
    size_t victim = 0;
    uint64_t oldest = ~uint64_t(0);
    for (size_t i = 0; i < fd_cache_.size(); ++i) {
      if (fd_cache_[i].fd < 0) {
        victim = i;
        oldest = 0;
        break;
      }
      if (fd_cache_[i].tick < oldest) {
        oldest = fd_cache_[i].tick;
        victim = i;
      }
    }
    if (fd_cache_[victim].fd >= 0) {
      ::close(fd_cache_[victim].fd);
    }
    fd_cache_[victim].uri = uri;
    fd_cache_[victim].fd = opened;
    fd_cache_[victim].tick = ++fd_clock_;
    return caller_fd;
  }

  void PopulateDirectory(SafTreeEntry* dir_entry, int depth) {
    if (depth > 32) {
      return;
    }
    auto children =
        saf::ListTreeChildren(tree_uri_, dir_entry->document_id());
    for (auto& item : children) {
      auto child_path = xe::utf8::join_guest_paths(dir_entry->path(), item.name);
      auto child = std::make_unique<SafTreeEntry>(
          this, dir_entry, child_path, std::move(item.uri),
          std::move(item.document_id));
      const uint64_t nt_time =
          item.last_modified_ms
              ? (item.last_modified_ms * 10000ULL) + 116444736000000000ULL
              : (10000ULL * 11644473600000ULL);
      child->create_timestamp_ = nt_time;
      child->access_timestamp_ = nt_time;
      child->write_timestamp_ = nt_time;

      if (item.is_directory) {
        child->attributes_ = kFileAttributeDirectory | kFileAttributeReadOnly;
        child->size_ = 0;
        child->allocation_size_ = 0;
        SafTreeEntry* child_ptr = child.get();
        dir_entry->children_.push_back(std::move(child));
        PopulateDirectory(child_ptr, depth + 1);
      } else {
        child->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
        child->size_ = static_cast<size_t>(item.size);
        child->allocation_size_ = xe::round_up(child->size_, bytes_per_sector());
        dir_entry->children_.push_back(std::move(child));
      }
    }
  }

  std::string name_;
  std::string tree_uri_;
  std::string root_doc_id_;
  std::unique_ptr<Entry> root_entry_;
  std::mutex fd_mutex_;
  uint64_t fd_clock_ = 0;
  std::array<CachedFd, kMaxCachedFds> fd_cache_{};
};

inline X_STATUS SafTreeFile::ReadSync(std::span<uint8_t> buffer,
                                      size_t byte_offset,
                                      size_t* out_bytes_read) {
  auto* dev = static_cast<SafTreeDevice*>(saf_entry_->device());
  return dev->ReadEntryAt(saf_entry_, buffer, byte_offset, out_bytes_read);
}

#endif  // XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_SAF_TREE_DEVICE_H_
