// kernel32.dll file, mapping and volume APIs on top of the VFS.
#include <sys/stat.h>
#include <unistd.h>

#include "../core/objects.h"

using namespace rt;

namespace {
const char* kDll = "kernel32.dll";

constexpr uint32_t GENERIC_WRITE = 0x40000000;
constexpr uint32_t CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5;

uint32_t open_file(const std::string& guest, uint32_t access, uint32_t disposition) {
  bool want_write = (access & GENERIC_WRITE) != 0;
  bool create = disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS;
  std::string hostp = vfs_host_path(guest, create);
  if (hostp.empty()) {
    log(Log::Debug, "CreateFile(%s): path not mapped", guest.c_str());
    set_last_error(ERROR_PATH_NOT_FOUND);
    return kInvalidHandle;
  }
  struct stat st;
  bool exists = stat(hostp.c_str(), &st) == 0;
  if (exists && S_ISDIR(st.st_mode)) {
    set_last_error(ERROR_ACCESS_DENIED);
    return kInvalidHandle;
  }
  const char* mode = nullptr;
  switch (disposition) {
    case CREATE_NEW:
      if (exists) {
        set_last_error(80);  // ERROR_FILE_EXISTS
        return kInvalidHandle;
      }
      mode = "w+b";
      break;
    case CREATE_ALWAYS:
      mode = "w+b";
      break;
    case OPEN_EXISTING:
      if (!exists) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return kInvalidHandle;
      }
      mode = want_write ? "r+b" : "rb";
      break;
    case OPEN_ALWAYS:
      mode = exists ? (want_write ? "r+b" : "rb") : "w+b";
      break;
    case TRUNCATE_EXISTING:
      if (!exists) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return kInvalidHandle;
      }
      mode = "w+b";
      break;
    default:
      set_last_error(ERROR_INVALID_PARAMETER);
      return kInvalidHandle;
  }
  FILE* fp = fopen(hostp.c_str(), mode);
  if (!fp) {
    set_last_error(want_write ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
    return kInvalidHandle;
  }
  auto f = std::make_shared<FileObject>();
  f->fp = fp;
  f->guest_path = guest;
  f->host_path = hostp;
  f->writable = want_write;
  // CREATE_ALWAYS / OPEN_ALWAYS report ERROR_ALREADY_EXISTS when the file existed.
  set_last_error((disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) && exists ? ERROR_ALREADY_EXISTS : 0);
  if (trace_enabled("file")) log(Log::Info, "CreateFile(%s, %s) -> %s", guest.c_str(), mode, hostp.c_str());
  return handle_new(f);
}

int64_t file_size(FILE* fp) {
  struct stat st;
  return fstat(fileno(fp), &st) == 0 ? (int64_t)st.st_size : -1;
}
}  // namespace

HLE_STDCALL(kDll, CreateFileA, 7) { return open_file(read_cstr(arg(c, 0)), arg(c, 1), arg(c, 4)); }

HLE_STDCALL(kDll, CreateFileW, 7) {
  std::u16string w = read_wstr(arg(c, 0));
  std::string s(w.begin(), w.end());
  return open_file(s, arg(c, 1), arg(c, 4));
}

HLE_STDCALL(kDll, ReadFile, 5) {
  uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), nread = arg(c, 3), ov = arg(c, 4);
  auto f = handle_as<FileObject>(h);
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (ov) fseeko(f->fp, (off_t)((uint64_t)ld32(ov + 12) << 32 | ld32(ov + 8)), SEEK_SET);
  size_t got = n ? fread(host(buf), 1, n, f->fp) : 0;
  if (nread) st32(nread, (uint32_t)got);
  if (ov) {
    // Completed synchronously: OVERLAPPED.Internal = STATUS_SUCCESS, InternalHigh = bytes.
    st32(ov + 0, 0);
    st32(ov + 4, (uint32_t)got);
    if (got == 0 && n) {
      set_last_error(ERROR_HANDLE_EOF);
      return 0;
    }
  }
  return 1;
}

HLE_STDCALL(kDll, WriteFile, 5) {
  uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), nwritten = arg(c, 3), ov = arg(c, 4);
  auto f = handle_as<FileObject>(h);
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (ov) fseeko(f->fp, (off_t)((uint64_t)ld32(ov + 12) << 32 | ld32(ov + 8)), SEEK_SET);
  size_t put = n ? fwrite(host(buf), 1, n, f->fp) : 0;
  if (f->is_console) fflush(f->fp);
  if (nwritten) st32(nwritten, (uint32_t)put);
  if (ov) {
    st32(ov + 0, 0);
    st32(ov + 4, (uint32_t)put);
  }
  return put == n;
}

HLE_STDCALL(kDll, GetOverlappedResult, 4) {
  uint32_t ov = arg(c, 1), n = arg(c, 2);
  if (n) st32(n, ld32(ov + 4));
  return 1;
}

HLE_STDCALL(kDll, CancelIo, 1) { return 1; }

HLE_STDCALL(kDll, SetFilePointer, 4) {
  uint32_t h = arg(c, 0), hi_ptr = arg(c, 2), method = arg(c, 3);
  int64_t dist = (int32_t)arg(c, 1);
  if (hi_ptr) dist = (int64_t)((uint64_t)ld32(hi_ptr) << 32 | arg(c, 1));
  auto f = handle_as<FileObject>(h);
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0xffffffffu;
  }
  int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
  if (fseeko(f->fp, (off_t)dist, whence) != 0) {
    set_last_error(131);  // ERROR_NEGATIVE_SEEK
    return 0xffffffffu;
  }
  uint64_t pos = (uint64_t)ftello(f->fp);
  if (hi_ptr) st32(hi_ptr, (uint32_t)(pos >> 32));
  set_last_error(0);
  return (uint32_t)pos;
}

HLE_STDCALL(kDll, GetFileSize, 2) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0xffffffffu;
  }
  int64_t size = file_size(f->fp);
  if (arg(c, 1)) st32(arg(c, 1), (uint32_t)((uint64_t)size >> 32));
  return (uint32_t)size;
}

HLE_STDCALL(kDll, GetFileType, 1) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (!f) return 0;           // FILE_TYPE_UNKNOWN
  return f->is_console ? 2 : 1;  // FILE_TYPE_CHAR / FILE_TYPE_DISK
}

HLE_STDCALL(kDll, FlushFileBuffers, 1) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (f && f->fp) fflush(f->fp);
  return 1;
}

HLE_STDCALL(kDll, SetEndOfFile, 1) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (!f || !f->fp) return 0;
  fflush(f->fp);
  return ftruncate(fileno(f->fp), ftello(f->fp)) == 0;
}

HLE_STDCALL(kDll, GetFileAttributesA, 1) {
  std::string g = read_cstr(arg(c, 0));
  std::string hostp = vfs_host_path(g, false);
  struct stat st;
  if (hostp.empty() || stat(hostp.c_str(), &st) != 0) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return 0xffffffffu;  // INVALID_FILE_ATTRIBUTES
  }
  return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}

HLE_STDCALL(kDll, GetFullPathNameA, 4) {
  std::string full = vfs_full_path(read_cstr(arg(c, 0)));
  uint32_t n = arg(c, 1), buf = arg(c, 2), part = arg(c, 3);
  if (n < full.size() + 1) return (uint32_t)full.size() + 1;
  write_cstr(buf, full);
  if (part) {
    size_t slash = full.find_last_of('\\');
    st32(part, slash == std::string::npos || slash + 1 == full.size() ? 0 : buf + (uint32_t)slash + 1);
  }
  return (uint32_t)full.size();
}

HLE_STDCALL(kDll, GetDiskFreeSpaceExA, 4) {
  for (int i = 1; i <= 3; i++)
    if (arg(c, i)) st64(arg(c, i), 8ull << 30);  // 8 GiB
  return 1;
}

// ---- File mappings -------------------------------------------------------------------------

HLE_STDCALL(kDll, CreateFileMappingA, 6) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  auto m = std::make_shared<MappingObject>();
  m->file = f;
  uint32_t lo = arg(c, 4);
  m->size = lo ? lo : (uint32_t)file_size(f->fp);
  return handle_new(m);
}

HLE_STDCALL(kDll, MapViewOfFile, 5) {
  auto m = handle_as<MappingObject>(arg(c, 0));
  if (!m) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  uint64_t off = (uint64_t)arg(c, 2) << 32 | arg(c, 3);
  uint32_t size = arg(c, 4) ? arg(c, 4) : m->size - (uint32_t)off;
  uint32_t base = vm_reserve(0, size);
  if (!base || !vm_commit(base, size)) {
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  // Read-only snapshot of the file (the game only maps files for reading).
  long pos = ftell(m->file->fp);
  fseeko(m->file->fp, (off_t)off, SEEK_SET);
  size_t got = fread(host(base), 1, size, m->file->fp);
  fseek(m->file->fp, pos, SEEK_SET);
  log(Log::Debug, "MapViewOfFile(%s, %u bytes) -> %08x (%zu read)", m->file->guest_path.c_str(), size, base, got);
  return base;
}

HLE_STDCALL(kDll, UnmapViewOfFile, 1) { return vm_release(arg(c, 0)); }

// ---- Volumes: the player's own disc is mounted as D: ---------------------------------------

HLE_STDCALL(kDll, GetLogicalDrives, 0) { return (1u << 2) | (1u << 3); }

HLE_STDCALL(kDll, GetDriveTypeA, 1) {
  std::string root = read_cstr(arg(c, 0));
  char d = root.empty() ? 'C' : (char)toupper((unsigned char)root[0]);
  return d == 'C' ? 3 : d == 'D' ? 5 : 1;  // DRIVE_FIXED / DRIVE_CDROM / DRIVE_NO_ROOT_DIR
}

HLE_STDCALL(kDll, GetVolumeInformationA, 8) {
  std::string root = read_cstr(arg(c, 0));
  char d = root.empty() ? 'C' : (char)toupper((unsigned char)root[0]);
  const char* label = d == 'D' ? "SHRK2TMA_1" : d == 'C' ? "SYSTEM" : nullptr;
  if (!label) {
    set_last_error(21);  // ERROR_NOT_READY
    return 0;
  }
  if (arg(c, 1) && arg(c, 2)) write_cstr(arg(c, 1), label);
  if (arg(c, 3)) st32(arg(c, 3), d == 'D' ? 0x20041006 : 0x1234abcd);
  if (arg(c, 4)) st32(arg(c, 4), 255);
  if (arg(c, 5)) st32(arg(c, 5), 0);
  if (arg(c, 6) && arg(c, 7)) write_cstr(arg(c, 6), d == 'D' ? "CDFS" : "NTFS");
  return 1;
}
