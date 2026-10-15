// Kernel objects (files, mutexes, threads, mappings) behind Win32 HANDLE values, and the VFS.
#pragma once

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "rt.h"

namespace rt {

constexpr uint32_t kInvalidHandle = 0xffffffffu;
constexpr uint32_t kCurrentProcess = 0xffffffffu;
constexpr uint32_t kCurrentThread = 0xfffffffeu;

// Win32 error codes used by the HLE.
enum : uint32_t {
  ERROR_SUCCESS = 0,
  ERROR_FILE_NOT_FOUND = 2,
  ERROR_PATH_NOT_FOUND = 3,
  ERROR_ACCESS_DENIED = 5,
  ERROR_INVALID_HANDLE = 6,
  ERROR_NOT_ENOUGH_MEMORY = 8,
  ERROR_HANDLE_EOF = 38,
  ERROR_INVALID_PARAMETER = 87,
  ERROR_INSUFFICIENT_BUFFER = 122,
  ERROR_PROC_NOT_FOUND = 127,
  ERROR_MOD_NOT_FOUND = 126,
  ERROR_ALREADY_EXISTS = 183,
  ERROR_NO_MORE_ITEMS = 259,
  ERROR_IO_PENDING = 997,
};

struct KObject {
  virtual ~KObject() = default;
  virtual const char* kind() const = 0;
  // Waitable objects: return true when signalled within timeout_ms (0xffffffff = infinite).
  virtual bool wait(uint32_t timeout_ms) {
    (void)timeout_ms;
    return true;
  }
};

uint32_t handle_new(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> handle_get(uint32_t h);
bool handle_close(uint32_t h);
template <typename T>
std::shared_ptr<T> handle_as(uint32_t h) {
  return std::dynamic_pointer_cast<T>(handle_get(h));
}

struct FileObject : KObject {
  FILE* fp = nullptr;
  std::string guest_path, host_path;
  bool writable = false;
  bool is_console = false;
  ~FileObject() override {
    if (fp && !is_console) fclose(fp);
  }
  const char* kind() const override { return "file"; }
};

struct MutexObject : KObject {
  std::mutex m;
  std::condition_variable cv;
  uint32_t owner = 0;
  uint32_t count = 0;
  std::string name;
  const char* kind() const override { return "mutex"; }
  bool wait(uint32_t timeout_ms) override;
  bool release();
};

struct ThreadObject : KObject {
  GuestThread* thread = nullptr;
  uint32_t start = 0, param = 0;
  bool suspended = false;
  const char* kind() const override { return "thread"; }
  bool wait(uint32_t timeout_ms) override;
};

struct MappingObject : KObject {
  std::shared_ptr<FileObject> file;
  uint32_t size = 0;
  const char* kind() const override { return "mapping"; }
};

// ---- Virtual file system -------------------------------------------------------------------
// The game sees itself installed at kInstallDir; that directory maps to the host install root.
extern const char* const kInstallDir;  // e.g. "C:\\Games\\Shrek 2 Team Action\\"
extern const char* const kGameDir;     // kInstallDir + "Game\\"
void vfs_init(const std::string& host_install_root);
std::string vfs_full_path(const std::string& guest_path);  // absolute, backslashes, '.' and '..' resolved
// Map a guest path to a host path. For existing files the match is case-insensitive; for new
// files the parent directory is matched and the leaf name kept. Returns "" when unmapped.
std::string vfs_host_path(const std::string& guest_path, bool for_create);
bool vfs_is_dir(const std::string& host_path);
std::string vfs_host_root();

}  // namespace rt
