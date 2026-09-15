#include "opad/cache.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "opad/util.hpp"

namespace opad {

std::filesystem::path cache_dir() {
  static std::filesystem::path dir = [] {
    std::filesystem::path p;
    if (const char* e = std::getenv("OPAD_CACHE_DIR"); e && *e) {
      p = e;
    } else {
#if defined(_WIN32)
      if (const char* e = std::getenv("LOCALAPPDATA"); e && *e) p = std::filesystem::path(e) / "opad" / "cache";
      else if (const char* t = std::getenv("TEMP"); t && *t) p = std::filesystem::path(t) / "opad-cache";
#elif defined(__APPLE__)
      if (const char* h = std::getenv("HOME"); h && *h) p = std::filesystem::path(h) / "Library" / "Caches" / "opad";
#else
      if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) p = std::filesystem::path(x) / "opad";
      else if (const char* h = std::getenv("HOME"); h && *h) p = std::filesystem::path(h) / ".cache" / "opad";
#endif
      if (p.empty()) p = std::filesystem::temp_directory_path() / "opad-cache";
    }
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
  }();
  return dir;
}

static std::filesystem::path entry_path(const std::string& bucket, const std::string& key) {
  return cache_dir() / bucket / (key + ".bin");
}

std::optional<std::string> cache_get(const std::string& bucket, const std::string& key) {
  std::ifstream in(entry_path(bucket, key), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void cache_put(const std::string& bucket, const std::string& key, const std::string& blob) {
  try {
    write_text_file(entry_path(bucket, key), blob);
  } catch (const std::exception&) {
    // The cache is best-effort; a read-only or full disk must never fail the caller.
  }
}

std::uintmax_t cache_size_bytes() {
  std::uintmax_t total = 0;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(cache_dir(), ec); !ec && it != std::filesystem::end(it);
       it.increment(ec))
    if (it->is_regular_file(ec)) total += it->file_size(ec);
  return total;
}

void cache_clear() {
  std::error_code ec;
  std::filesystem::remove_all(cache_dir(), ec);
  std::filesystem::create_directories(cache_dir(), ec);
}

}  // namespace opad
