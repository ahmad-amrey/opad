// Third-party notices (TODO 11 UI-13): the file beside the program wins (the portable package's, written from the DLLs it
// ships), else the text cmake/notices.cmake compiled in from the link lines.
#include "opad/util.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
#else
  std::error_code ignored;
  dir = std::filesystem::read_symlink("/proc/self/exe", ignored).parent_path();
#endif
  std::error_code error;
  if (!dir.empty() && std::filesystem::is_regular_file(dir / "THIRD-PARTY-NOTICES.txt", error)) {
    try {
      return read_text_file(dir / "THIRD-PARTY-NOTICES.txt");
    } catch (const std::exception&) {
    }
  }
  return detail::compiled_notices();
}
}  // namespace opad
