// OPAD's Explorer thumbnails (Windows): a thumbnail provider for the formats OPAD reads, so Explorer and the Open
// dialog show the model or drawing instead of a generic icon. Settings > File types registers it (current user only,
// app/FileAssociations.cpp).
//
// Windows runs thumbnail providers in an isolated helper process and hands them the file as a stream. This DLL stays
// small and links nothing of OPAD: it copies the stream to a scratch file under its own name and runs
// `opad-cli thumbnail` (beside this DLL), which reads the file as the viewer does and renders it on the CPU; the result
// is premultiplied BGRA with a transparent background (see cli/main.cpp), which becomes the bitmap Windows asks for.
#include <windows.h>
#include <propsys.h>
#include <shlwapi.h>
#include <thumbcache.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// {67DD85AE-5101-4C3A-9231-39D7B7EFEC39}
const CLSID kClsid = {0x67dd85ae, 0x5101, 0x4c3a, {0x92, 0x31, 0x39, 0xd7, 0xb7, 0xef, 0xec, 0x39}};
HMODULE g_module = nullptr;
std::atomic<long> g_objects{0}, g_locks{0};

const wchar_t* const kExtensions[] = {L".opad", L".step", L".stp", L".iges", L".igs", L".brep", L".brp", L".stl", L".3mf",
                                      L".obj", L".ply", L".gltf", L".glb", L".wrl", L".vrml", L".dxf", L".dwg", L".svg"};
constexpr unsigned long long kLargest = 400ull << 20;  // beyond this a thumbnail costs more than it is worth
constexpr DWORD kPatience = 45000;                      // ms the renderer gets

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}

std::wstring module_dir() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) return {};
    if (n < path.size()) { path.resize(n); break; }
    path.resize(path.size() * 2);
  }
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// The format from the stream's name, else from its first bytes (STEP, ASCII and binary STL).
std::wstring extension_of(IStream* stream, unsigned long long& size) {
  STATSTG stat{};
  std::wstring ext;
  if (SUCCEEDED(stream->Stat(&stat, STATFLAG_DEFAULT))) {
    size = stat.cbSize.QuadPart;
    if (stat.pwcsName) {
      const std::wstring name = stat.pwcsName;
      CoTaskMemFree(stat.pwcsName);
      const auto dot = name.find_last_of(L'.');
      if (dot != std::wstring::npos) ext = lower(name.substr(dot));
    }
  }
  for (const wchar_t* known : kExtensions)
    if (ext == known) return ext;
  char head[96] = {};
  ULONG got = 0;
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  stream->Read(head, sizeof(head), &got);
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  if (got >= 12 && std::strncmp(head, "ISO-10303-21", 12) == 0) return L".step";
  if (got >= 84) {
    uint32_t facets = 0;
    std::memcpy(&facets, head + 80, 4);
    if (size == 84ull + 50ull * facets) return L".stl";
  }
  if (got >= 6 && std::strncmp(head, "solid ", 6) == 0) return L".stl";
  return {};
}

bool copy_stream(IStream* stream, const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  std::vector<char> buffer(1 << 20);
  bool ok = true;
  for (;;) {
    ULONG got = 0;
    const HRESULT hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &got);
    if (FAILED(hr)) { ok = false; break; }
    if (got == 0) break;
    DWORD written = 0;
    if (!WriteFile(file, buffer.data(), got, &written, nullptr) || written != got) { ok = false; break; }
    if (hr == S_FALSE) break;
  }
  CloseHandle(file);
  return ok;
}

// Runs opad-cli hidden and waits for it; true when it exited with 0.
bool render(const std::wstring& cli, const std::wstring& model, const std::wstring& out, UINT size) {
  std::wstring command = L"\"" + cli + L"\" --compact thumbnail \"" + model + L"\" --out \"" + out + L"\" --size " + std::to_wstring(size);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  const std::wstring dir = module_dir();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                      dir.empty() ? nullptr : dir.c_str(), &startup, &process))
    return false;
  DWORD code = 1;
  if (WaitForSingleObject(process.hProcess, kPatience) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
  else TerminateProcess(process.hProcess, 1);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code == 0;
}

HBITMAP load_bgra(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return nullptr;
  char header[16] = {};
  DWORD got = 0;
  HBITMAP bitmap = nullptr;
  if (ReadFile(file, header, sizeof(header), &got, nullptr) && got == sizeof(header) && std::memcmp(header, "OPADTHMB", 8) == 0) {
    uint32_t w = 0, h = 0;
    std::memcpy(&w, header + 8, 4);
    std::memcpy(&h, header + 12, 4);
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(info.bmiHeader);
      info.bmiHeader.biWidth = static_cast<LONG>(w);
      info.bmiHeader.biHeight = -static_cast<LONG>(h);  // top-down rows, as written
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      const DWORD bytes = w * h * 4;
      if (bitmap && (!ReadFile(file, bits, bytes, &got, nullptr) || got != bytes)) {
        DeleteObject(bitmap);
        bitmap = nullptr;
      }
    }
  }
  CloseHandle(file);
  return bitmap;
}

class Provider final : public IInitializeWithStream, public IThumbnailProvider {
 public:
  Provider() { ++g_objects; }
  ~Provider() {
    if (m_stream) m_stream->Release();
    --g_objects;
  }
  IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IInitializeWithStream)) *out = static_cast<IInitializeWithStream*>(this);
    else if (riid == __uuidof(IThumbnailProvider)) *out = static_cast<IThumbnailProvider*>(this);
    else { *out = nullptr; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++m_refs); }
  IFACEMETHODIMP_(ULONG) Release() override {
    const long left = --m_refs;
    if (left == 0) delete this;
    return static_cast<ULONG>(left);
  }
  IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
    if (m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (!stream) return E_INVALIDARG;
    m_stream = stream;
    m_stream->AddRef();
    return S_OK;
  }
  IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
    if (!bitmap || !alpha) return E_POINTER;
    *bitmap = nullptr;
    if (!m_stream) return E_UNEXPECTED;
    unsigned long long size = 0;
    const std::wstring ext = extension_of(m_stream, size);
    if (ext.empty() || size > kLargest) return E_FAIL;
    const std::wstring dir = module_dir(), cli = dir + L"\\opad-cli.exe";
    if (GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES) return E_FAIL;
    wchar_t temp[MAX_PATH + 1] = {};
    if (!GetTempPathW(MAX_PATH, temp)) return E_FAIL;
    const std::wstring work = std::wstring(temp) + L"opad-thumb-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(work.c_str(), nullptr)) return E_FAIL;
    const std::wstring model = work + L"\\model" + ext, out = work + L"\\thumb.bgra";
    HRESULT hr = E_FAIL;
    if (copy_stream(m_stream, model) && render(cli, model, out, cx)) {
      *bitmap = load_bgra(out);
      if (*bitmap) {
        *alpha = WTSAT_ARGB;
        hr = S_OK;
      }
    }
    DeleteFileW(model.c_str());
    DeleteFileW(out.c_str());
    RemoveDirectoryW(work.c_str());
    return hr;
  }

 private:
  std::atomic<long> m_refs{1};
  IStream* m_stream = nullptr;
};

class Factory final : public IClassFactory {
 public:
  IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *out = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }  // static instance
  IFACEMETHODIMP_(ULONG) Release() override { return 1; }
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* provider = new (std::nothrow) Provider();
    if (!provider) return E_OUTOFMEMORY;
    const HRESULT hr = provider->QueryInterface(riid, out);
    provider->Release();
    return hr;
  }
  IFACEMETHODIMP LockServer(BOOL lock) override {
    lock ? ++g_locks : --g_locks;
    return S_OK;
  }
};
Factory g_factory;

}  // namespace

extern "C" {
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, out);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }
}
