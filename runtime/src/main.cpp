// Native entry point: map the game executable, bind the HLE layer and run the CRT entry point.
//   shrek2 [install-dir] [registry-file]
#include <fstream>
#include <iterator>

#include "core/objects.h"
#include "core/rt.h"
#include "hle/registry.h"
#include "platform/platform.h"

int main(int argc, char** argv) {
  std::string install = argc > 1 ? argv[1] : "work/install";
  std::string reg = argc > 2 ? argv[2] : "work/registry.txt";
  rt::memory_init();
  rt::vfs_init(install);
  rt::registry_init(reg);
  if (!plat::init()) return 1;

  std::ifstream f(install + "/Game/Shrek 2.exe", std::ios::binary);
  if (!f) rt::fatal("cannot open %s/Game/Shrek 2.exe", install.c_str());
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  rt::load_image(exe);
  rt::dispatch_init();

  rt::GuestThread* t = rt::create_guest_thread(0x100000);
  rt::bind_host_thread(t);
  rt::log(rt::Log::Info, "starting guest at entry %08x", rt::image_entry());
  uint32_t code = rt::guest_call(&t->cpu, rt::image_entry(), {});
  rt::process_exit(code);
}
