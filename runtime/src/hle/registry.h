// Virtual registry used by advapi32 and the settings UI.
#pragma once
#include <cstdint>
#include <string>

namespace rt {
void registry_init(const std::string& store_path);
void registry_set_dword(const std::string& key, const std::string& name, uint32_t value);
}  // namespace rt
