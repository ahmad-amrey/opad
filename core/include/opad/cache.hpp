#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace opad {
// OS user-cache directory for OPAD (never next to the document, F10). Created on demand.
std::filesystem::path cache_dir();
std::optional<std::string> cache_get(const std::string& bucket, const std::string& key);
void cache_put(const std::string& bucket, const std::string& key, const std::string& blob);
std::uintmax_t cache_size_bytes();
void cache_clear();
}  // namespace opad
