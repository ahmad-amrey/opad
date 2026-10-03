// Explorer thumbnails (Windows): opad-thumbnails.dll driven as Windows' thumbnail cache drives it (a class object for
// its CLSID, a stream of the file, GetThumbnail) without registering anything: it renders through opad-cli beside it.
#include <windows.h>
#include <propsys.h>
#include <shlwapi.h>
#include <thumbcache.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "opad/util.hpp"

namespace {
const CLSID kClsid = {0x67dd85ae, 0x5101, 0x4c3a, {0x92, 0x31, 0x39, 0xd7, 0xb7, 0xef, 0xec, 0x39}};

std::filesystem::path beside(const wchar_t* name) {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  return std::filesystem::path(self).parent_path() / name;
}

struct Thumb {
  int width = 0, height = 0, opaque = 0, clear = 0;
  bool ok = false;
};

Thumb thumbnail(const std::filesystem::path& file, UINT size) {
  Thumb out;
  HMODULE dll = LoadLibraryW(beside(L"opad-thumbnails.dll").c_str());
  CHECK(dll != nullptr);
  using GetClassObject = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
  auto get = reinterpret_cast<GetClassObject>(reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
  CHECK(get != nullptr);
  IClassFactory* factory = nullptr;
  CHECK(SUCCEEDED(get(kClsid, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory))));
  IInitializeWithStream* init = nullptr;
  CHECK(SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(IInitializeWithStream), reinterpret_cast<void**>(&init))));
  IStream* stream = nullptr;
  CHECK(SUCCEEDED(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream)));
  CHECK(SUCCEEDED(init->Initialize(stream, STGM_READ)));
  IThumbnailProvider* provider = nullptr;
  CHECK(SUCCEEDED(init->QueryInterface(__uuidof(IThumbnailProvider), reinterpret_cast<void**>(&provider))));
  HBITMAP bitmap = nullptr;
  WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
  if (SUCCEEDED(provider->GetThumbnail(size, &bitmap, &alpha)) && bitmap) {
    DIBSECTION dib{};
    GetObjectW(bitmap, sizeof(dib), &dib);
    out.width = dib.dsBm.bmWidth;
    out.height = dib.dsBm.bmHeight;
    const auto* px = static_cast<const uint8_t*>(dib.dsBm.bmBits);
    for (int i = 0; px && i < out.width * out.height; ++i) {
      if (px[i * 4 + 3] == 255) ++out.opaque;
      if (px[i * 4 + 3] == 0) ++out.clear;
    }
    out.ok = alpha == WTSAT_ARGB;
    DeleteObject(bitmap);
  }
  provider->Release();
  stream->Release();
  init->Release();
  factory->Release();
  return out;
}
}  // namespace

TEST(models_and_drawings_get_thumbnails) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const auto dir = std::filesystem::temp_directory_path() / ("opad-thumbs-" + opad::new_uuid());
  std::filesystem::create_directory(dir);
  {
    std::ofstream stl(dir / "part.stl");
    stl << "solid t\n"
           "facet normal 0 0 -1\nouter loop\nvertex 0 0 0\nvertex 0 10 0\nvertex 10 0 0\nendloop\nendfacet\n"
           "facet normal 0 -1 0\nouter loop\nvertex 0 0 0\nvertex 10 0 0\nvertex 0 0 10\nendloop\nendfacet\n"
           "facet normal -1 0 0\nouter loop\nvertex 0 0 0\nvertex 0 0 10\nvertex 0 10 0\nendloop\nendfacet\n"
           "facet normal 1 1 1\nouter loop\nvertex 10 0 0\nvertex 0 10 0\nvertex 0 0 10\nendloop\nendfacet\n"
           "endsolid t\n";
    std::ofstream svg(dir / "sheet.svg");
    svg << "<svg width=\"40mm\" viewBox=\"0 0 40 40\"><rect width=\"20\" height=\"10\"/><circle cx=\"5\" cy=\"5\" r=\"2\"/></svg>";
    std::ofstream junk(dir / "notes.stl");
    junk << "not a mesh";
  }
  // A model: its shape opaque on a transparent background.
  const Thumb part = thumbnail(dir / "part.stl", 128);
  CHECK(part.ok && part.width == 128 && part.height == 128 && part.opaque > 500 && part.clear > 5000);
  // A drawing: on its white sheet.
  const Thumb sheet = thumbnail(dir / "sheet.svg", 96);
  CHECK(sheet.ok && sheet.width == 96 && sheet.opaque == 96 * 96);
  // A file it cannot read: no thumbnail, no crash (Windows shows the type's icon).
  CHECK(!thumbnail(dir / "notes.stl", 64).ok);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

CHECK_MAIN()
