// Handle table, waitable objects and the virtual file system.
#include "objects.h"

#include <chrono>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>
#include <unordered_map>

namespace rt {

// ---- Handles -------------------------------------------------------------------------------

static std::mutex g_handles_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
static uint32_t g_next_handle = 0x100;

uint32_t handle_new(std::shared_ptr<KObject> obj) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  uint32_t h = g_next_handle;
  g_next_handle += 4;
  g_handles[h] = std::move(obj);
  return h;
}

std::shared_ptr<KObject> handle_get(uint32_t h) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}

bool handle_close(uint32_t h) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  return g_handles.erase(h) == 1;
}

// ---- Waitables -----------------------------------------------------------------------------

template <typename Pred>
static bool wait_cv(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, uint32_t timeout_ms, Pred pred) {
  if (timeout_ms == 0xffffffffu) {
    cv.wait(lk, pred);
    return true;
  }
  return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
}

bool MutexObject::wait(uint32_t timeout_ms) {
  uint32_t me = current_thread() ? current_thread()->id : 0;
  std::unique_lock<std::mutex> lk(m);
  if (owner == me) {
    count++;
    return true;
  }
  if (!wait_cv(cv, lk, timeout_ms, [&] { return owner == 0; })) return false;
  owner = me;
  count = 1;
  return true;
}

bool MutexObject::release() {
  uint32_t me = current_thread() ? current_thread()->id : 0;
  std::lock_guard<std::mutex> lk(m);
  if (owner != me) return false;
  if (--count == 0) {
    owner = 0;
    cv.notify_one();
  }
  return true;
}

bool ThreadObject::wait(uint32_t timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!thread || !thread->finished.load()) {
    if (timeout_ms != 0xffffffffu && std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// ---- VFS -----------------------------------------------------------------------------------

const char* const kInstallDir = "C:\\Games\\Shrek 2 Team Action\\";
const char* const kGameDir = "C:\\Games\\Shrek 2 Team Action\\Game\\";
static std::string g_host_root;  // host directory corresponding to kInstallDir (with trailing '/')

void vfs_init(const std::string& host_install_root) {
  g_host_root = host_install_root;
  if (!g_host_root.empty() && g_host_root.back() != '/') g_host_root += '/';
}

std::string vfs_host_root() { return g_host_root; }

static std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string vfs_full_path(const std::string& in) {
  std::string p = in;
  for (auto& c : p)
    if (c == '/') c = '\\';
  bool absolute = p.size() >= 2 && p[1] == ':';
  if (!absolute) p = (p.size() && p[0] == '\\' ? std::string("C:") : std::string(kGameDir)) + p;
  // Normalise components.
  std::vector<std::string> parts;
  std::string drive = p.substr(0, 2);
  size_t i = 2;
  while (i < p.size()) {
    size_t j = p.find('\\', i);
    if (j == std::string::npos) j = p.size();
    std::string comp = p.substr(i, j - i);
    if (comp == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!comp.empty() && comp != ".") {
      parts.push_back(comp);
    }
    i = j + 1;
  }
  std::string out = drive;
  for (auto& c : parts) out += "\\" + c;
  if (parts.empty()) out += "\\";
  return out;
}

bool vfs_is_dir(const std::string& host_path) {
  struct stat st;
  return stat(host_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Case-insensitive lookup of one path component inside a host directory.
static std::string find_entry(const std::string& dir, const std::string& name) {
  std::string want = lower(name);
  DIR* d = opendir(dir.c_str());
  if (!d) return "";
  std::string found;
  while (dirent* e = readdir(d)) {
    if (lower(e->d_name) == want) {
      found = e->d_name;
      break;
    }
  }
  closedir(d);
  return found;
}

std::string vfs_host_path(const std::string& guest, bool for_create) {
  std::string full = vfs_full_path(guest);
  std::string root = std::string(kInstallDir);
  root.pop_back();  // without trailing backslash
  if (lower(full).rfind(lower(root), 0) != 0) return "";
  std::string rel = full.substr(root.size());
  std::string host = g_host_root;
  host.pop_back();
  size_t i = rel.size() && rel[0] == '\\' ? 1 : 0;
  while (i < rel.size()) {
    size_t j = rel.find('\\', i);
    if (j == std::string::npos) j = rel.size();
    std::string comp = rel.substr(i, j - i);
    std::string match = find_entry(host, comp);
    bool last = j == rel.size();
    if (match.empty()) {
      if (last && for_create) match = comp;
      else return "";
    }
    host += "/" + match;
    i = j + 1;
  }
  return host;
}

}  // namespace rt
