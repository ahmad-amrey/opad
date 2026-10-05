// Third-party notices (TODO 11 UI-13): the package's file wins when it is OPAD's (the portable package's beside the exe,
// the AppImage's in usr/share/doc/opad, the app bundle's in Contents/Resources: each written from the libraries it
// ships), else the text cmake/notices.cmake compiled in from the link lines.
#include <vector>

#include "opad/util.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace opad {
namespace detail {
const char* compiled_notices();  // generated: <build>/core/third_party_notices.cpp
}

std::string third_party_notices() {
  std::filesystem::path dir;
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (n > 0 && n < buffer.size()) dir = std::filesystem::path(buffer.substr(0, n)).parent_path();
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (size > 0 && _NSGetExecutablePath(buffer.data(), &size) == 0) dir = std::filesystem::path(buffer.c_str()).parent_path();
#else
  std::error_code ignored;
  dir = std::filesystem::read_symlink("/proc/self/exe", ignored).parent_path();
#endif
  if (dir.empty()) return detail::compiled_notices();
  std::vector<std::filesystem::path> files{dir / "THIRD-PARTY-NOTICES.txt"};
#ifdef __APPLE__
  files.push_back(dir / "../Resources/THIRD-PARTY-NOTICES.txt");
#elif !defined(_WIN32)
  files.push_back(dir / "../share/doc/opad/THIRD-PARTY-NOTICES.txt");
#endif
  for (const auto& file : files) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) continue;
    try {
      // Only OPAD's ("OPAD <version>: third-party notices"): the single-file exe may land beside another product's file.
      const std::string text = read_text_file(file);
      if (text.rfind("OPAD ", 0) == 0 && text.substr(0, text.find('\n')).find(": third-party notices") != std::string::npos) return text;
    } catch (const std::exception&) {
    }
  }
  return detail::compiled_notices();
}
}  // namespace opad
