// Handle table, waitable objects and the virtual file system.
#include "objects.h"

#include <algorithm>
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
  if (owner == 0 && waiters.empty()) {
    owner = me;
    count = 1;
    return true;
  }
  if (timeout_ms == 0) return false;
  waiters.push_back(me);
  if (!wait_cv(cv, lk, timeout_ms, [&] { return owner == me; })) {
    waiters.erase(std::find(waiters.begin(), waiters.end(), me));
    return false;
  }
  return true;  // the releasing thread set owner/count for us
}

bool MutexObject::release() {
  uint32_t me = current_thread() ? current_thread()->id : 0;
  std::lock_guard<std::mutex> lk(m);
  if (owner != me) return false;
  if (--count == 0) {
    if (waiters.empty()) {
      owner = 0;
    } else {
      owner = waiters.front();
      waiters.pop_front();
      count = 1;
      cv.notify_all();
    }
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
  // The game writes Save\save.bin (OPEN_ALWAYS, no CreateDirectory) but the MSI never creates
  // the folder, so provide it.
  std::string game = g_host_root + "Game";
  if (vfs_is_dir(game)) mkdir((game + "/Save").c_str(), 0755);
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

// Case-insensitive lookup of one path component inside a host directory. Listings are cached:
// on the web, directory enumeration goes through the OPFS proxy and is far too slow to repeat
// for every CreateFile during level loads. The game only adds files through CreateFile, which
// registers them below.
static std::mutex g_dir_mutex;
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_dir_cache;

static std::unordered_map<std::string, std::string>& listing(const std::string& dir) {
  auto it = g_dir_cache.find(dir);
  if (it != g_dir_cache.end()) return it->second;
  auto& names = g_dir_cache[dir];
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) names.emplace(lower(e->d_name), e->d_name);
    closedir(d);
  }
  return names;
}

static std::string find_entry(const std::string& dir, const std::string& name) {
  std::lock_guard<std::mutex> lock(g_dir_mutex);
  auto& names = listing(dir);
  auto it = names.find(lower(name));
  return it == names.end() ? "" : it->second;
}

static void note_created(const std::string& dir, const std::string& name) {
  std::lock_guard<std::mutex> lock(g_dir_mutex);
  listing(dir).emplace(lower(name), name);
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
      if (!last || !for_create) return "";
      match = comp;
      note_created(host, comp);  // the caller is about to create it
    }
    host += "/" + match;
    i = j + 1;
  }
  return host;
}

}  // namespace rt
