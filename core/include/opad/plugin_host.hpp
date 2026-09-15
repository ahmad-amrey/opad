#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "json.hpp"

namespace opad {
struct LoadedPlugin {
  std::string name, version;
  std::filesystem::path path;
};
// Loads a native plugin (F35) implementing the C ABI in plugins/include/opad/plugin.h.
LoadedPlugin load_plugin(const std::filesystem::path& lib);
std::vector<LoadedPlugin> loaded_plugins();
}  // namespace opad
