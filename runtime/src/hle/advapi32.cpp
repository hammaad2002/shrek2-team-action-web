// advapi32.dll: a small virtual registry, persisted to a text file (OPFS on the web).
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "../core/objects.h"
#include "registry.h"

using namespace rt;

namespace {
const char* kDll = "advapi32.dll";

struct Value {
  uint32_t type;  // REG_SZ = 1, REG_DWORD = 4, REG_BINARY = 3
  std::vector<uint8_t> data;
};

std::mutex g_reg_mutex;
std::map<std::string, std::map<std::string, Value>> g_keys;  // lower-case full path -> values
std::map<uint32_t, std::string> g_open;                      // handle -> lower-case key path
uint32_t g_next_hkey = 0x2000;
std::string g_store_path;

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string root_name(uint32_t hkey) {
  switch (hkey) {
    case 0x80000000: return "hkcr";
    case 0x80000001: return "hkcu";
    case 0x80000002: return "hklm";
    case 0x80000003: return "hku";
    default: {
      auto it = g_open.find(hkey);
      return it == g_open.end() ? "" : it->second;
    }
  }
}

std::string join(const std::string& base, const std::string& sub) {
  if (sub.empty()) return base;
  std::string s = sub;
  while (!s.empty() && s[0] == '\\') s.erase(0, 1);
  while (!s.empty() && s.back() == '\\') s.pop_back();
  return lower(base + "\\" + s);
}

void set_dword(const std::string& key, const std::string& name, uint32_t v) {
  Value val{4, std::vector<uint8_t>(4)};
  memcpy(val.data.data(), &v, 4);
  g_keys[lower(key)][lower(name)] = val;
}

void set_string(const std::string& key, const std::string& name, const std::string& v) {
  Value val{1, std::vector<uint8_t>(v.begin(), v.end())};
  val.data.push_back(0);
  g_keys[lower(key)][lower(name)] = val;
}

void ensure_key(const std::string& key) { g_keys[lower(key)]; }

void save() {
  if (g_store_path.empty()) return;
  std::ofstream out(g_store_path);
  for (auto& [key, vals] : g_keys) {
    if (key.rfind("hkcu", 0) != 0) continue;  // only user settings persist
    for (auto& [name, v] : vals) {
      if (v.type == 4) {
        uint32_t d;
        memcpy(&d, v.data.data(), 4);
        out << key << "\t" << name << "\tdword\t" << d << "\n";
      } else if (v.type == 1) {
        out << key << "\t" << name << "\tsz\t" << std::string((const char*)v.data.data()) << "\n";
      }
    }
  }
}
}  // namespace

namespace rt {

void registry_init(const std::string& store_path) {
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  g_store_path = store_path;
  const std::string product = "SOFTWARE\\Activision\\Shrek 2 Team Action";
  // Values the original installer (MSI Registry table) writes.
  set_string("hklm\\" + product + "\\Install", "path", kInstallDir);
  set_string("hklm\\" + product, "InstallPath", kInstallDir);
  set_string("hklm\\" + product, "Language", "English");
  set_string("hklm\\" + product, "Version", "1.00.0000");
  set_string("hklm\\" + product, "EXEString", std::string(kGameDir) + "Shrek 2.exe");
  ensure_key("hkcu\\" + product + "\\Install");
  ensure_key("hkcu\\" + product + "\\Settings");
  // Keep D3DX on its plain x87 code paths (no CPU-specific MMX/SSE/3DNow! pipeline).
  set_dword("hklm\\Software\\Microsoft\\Direct3D", "DisableD3DXPSGP", 1);
  // Load persisted user settings.
  std::ifstream in(store_path);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string key, name, type, value;
    if (!std::getline(ls, key, '\t') || !std::getline(ls, name, '\t') || !std::getline(ls, type, '\t')) continue;
    std::getline(ls, value);
    if (type == "dword") set_dword(key, name, (uint32_t)std::stoul(value));
    else if (type == "sz") set_string(key, name, value);
  }
}

void registry_set_dword(const std::string& key, const std::string& name, uint32_t value) {
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  set_dword(key, name, value);
  save();
}

}  // namespace rt

static uint32_t open_key(uint32_t hkey, uint32_t subkey_ptr, uint32_t out, bool create) {
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  std::string base = root_name(hkey);
  if (base.empty()) return ERROR_INVALID_HANDLE;
  std::string path = join(base, subkey_ptr ? read_cstr(subkey_ptr) : "");
  if (!g_keys.count(path)) {
    if (!create) {
      if (trace_enabled("reg")) log(Log::Info, "RegOpenKey(%s): not found", path.c_str());
      return ERROR_FILE_NOT_FOUND;
    }
    g_keys[path];
  }
  uint32_t h = g_next_hkey;
  g_next_hkey += 4;
  g_open[h] = path;
  st32(out, h);
  return ERROR_SUCCESS;
}

HLE_STDCALL(kDll, RegOpenKeyA, 3) { return open_key(arg(c, 0), arg(c, 1), arg(c, 2), false); }
HLE_STDCALL(kDll, RegOpenKeyExA, 5) { return open_key(arg(c, 0), arg(c, 1), arg(c, 4), false); }
HLE_STDCALL(kDll, RegCreateKeyA, 3) { return open_key(arg(c, 0), arg(c, 1), arg(c, 2), true); }

HLE_STDCALL(kDll, RegCloseKey, 1) {
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  g_open.erase(arg(c, 0));
  return ERROR_SUCCESS;
}

HLE_STDCALL(kDll, RegQueryValueExA, 6) {
  uint32_t hkey = arg(c, 0), name = arg(c, 1), type_out = arg(c, 3), data = arg(c, 4), size_ptr = arg(c, 5);
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  std::string key = root_name(hkey);
  std::string vname = lower(name ? read_cstr(name) : "");
  auto k = g_keys.find(key);
  if (k == g_keys.end() || !k->second.count(vname)) {
    if (trace_enabled("reg")) log(Log::Info, "RegQueryValueEx(%s\\%s): not found", key.c_str(), vname.c_str());
    return ERROR_FILE_NOT_FOUND;
  }
  const Value& v = k->second[vname];
  if (type_out) st32(type_out, v.type);
  uint32_t need = (uint32_t)v.data.size();
  if (!size_ptr) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
  uint32_t have = ld32(size_ptr);
  st32(size_ptr, need);
  if (!data) return ERROR_SUCCESS;
  if (have < need) return 234;  // ERROR_MORE_DATA
  memcpy(host(data), v.data.data(), need);
  return ERROR_SUCCESS;
}

HLE_STDCALL(kDll, RegSetValueExA, 6) {
  uint32_t hkey = arg(c, 0), name = arg(c, 1), type = arg(c, 3), data = arg(c, 4), size = arg(c, 5);
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  std::string key = root_name(hkey);
  Value v{type, std::vector<uint8_t>(host(data), host(data) + size)};
  g_keys[key][lower(name ? read_cstr(name) : "")] = v;
  save();
  return ERROR_SUCCESS;
}
