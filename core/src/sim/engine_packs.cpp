#include "opad/sim/engine_packs.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>

#include "../zip.hpp"
#include "opad/util.hpp"

namespace opad::sim {

namespace fs = std::filesystem;

namespace {

// Built by engines/<id>/make-pack.sh in .github/workflows/engines.yml and published by it on dev, one release per pack
// revision and system (tag engines-<id>-<version>-<platform>, never replaced: a change is a new revision). The size and
// SHA-256 are the release's (its SHA256SUMS), pinned here in the commit after it was published; until then they are
// empty and the pack is not offered, so no Install button points at a missing file.
constexpr const char* kReleases = "https://github.com/ahmad-amrey/opad/releases/download/";

struct Pin {
  const char* id;
  const char* name;
  const char* version;
  const char* platform;
  std::uint64_t size;
  const char* sha256;
};
constexpr Pin kPins[] = {
    {"openfoam", "OpenFOAM v2606", "v2606-1", "windows-x64", 43636466, "70562bb0c93a1173785669c9f5adf771a5ae8d76c66dfcf1828b155e5b8c1fb4"},
    {"openfoam", "OpenFOAM v2606", "v2606-1", "linux-x64", 60183982, "6732b2d0e486c24223871120846f001d0e1dc71c8985a21c864e9b1cbb5f8be2"},
    {"openfoam", "OpenFOAM v2606", "v2606-1", "macos-arm64", 33134002, "a00fa3286df7410a4eca44d54f9ab21dcda00f070707375d75224d5802d19fcd"},
    {"calculix", "CalculiX 2.24", "2.24-1", "windows-x64", 13143110, "77538ad64eb542bdba9f046b8b450b7094ca2c51d18e8a8d35581241e6aaa91c"},
    {"calculix", "CalculiX 2.24", "2.24-1", "linux-x64", 10086856, "621937940e494355a6253e026c39e4c27c8b5061e872ece42edae82763700bca"},
    {"calculix", "CalculiX 2.24", "2.24-1", "macos-arm64", 13352000, "7712a0af1689f4dca6cb1b805baf94edd8c053da1e9ed3e776f1d972705d57e4"},
};

std::vector<EnginePack> pinned() {
  std::vector<EnginePack> out;
  const std::string here = engine_pack_platform();
  for (auto& p : engine_pack_table())
    if (!here.empty() && p.platform == here && !p.sha256.empty()) out.push_back(std::move(p));  // published for this system
  return out;
}

bool from_environment() {
  const char* e = std::getenv("OPAD_ENGINE_PACKS");
  return e && *e;
}

bool local_url(const std::string& url) {
  for (const char* host : {"http://127.0.0.1:", "http://localhost:", "http://127.0.0.1/", "http://localhost/"})
    if (url.rfind(host, 0) == 0) return true;
  return false;
}

// A member's name as a path inside the pack's folder, or empty when it would land anywhere else (absolute, a drive, "..",
// backslashes, which Windows takes as separators) or names nothing.
fs::path member_path(const std::string& name) {
  if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) return {};
  fs::path out;
  size_t a = 0;
  while (a < name.size()) {
    const size_t b = std::min(name.find('/', a), name.size());
    const std::string part = name.substr(a, b - a);
    if (part == "..") return {};
    if (!part.empty() && part != ".") out /= path_from_utf8(part);
    a = b + 1;
  }
  return out;
}

void remove_quietly(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);  // a program of the old pack still running keeps its files: they go next time
}

std::string notice_of(const std::string& id) {
  if (id == "openfoam")
    return "OpenFOAM (installed by OPAD on request into the engines folder)\n"
           "  Programs from OpenFOAM, by OpenCFD Ltd, run by OPAD as separate programs for airflow and air cooling.\n"
           "  Licence: GNU General Public License, version 3 or later.\n"
           "  Source: the unmodified OpenFOAM source tarball, OPAD's build recipe (engines/openfoam in OPAD's repository)\n"
           "  and its patches, published next to the pack on https://github.com/ahmad-amrey/opad/releases (engines-openfoam-*).\n"
           "  This offering is not approved or endorsed by OpenCFD Limited, producer and distributor of the OpenFOAM software\n"
           "  via www.openfoam.com, and owner of the OPENFOAM(R) and OpenCFD(R) trade marks.\n";
  if (id == "calculix")
    return "CalculiX ccx (installed by OPAD on request into the engines folder)\n"
           "  CalculiX's solver, by Guido Dhondt, run by OPAD as a separate program for strength, vibration and heat.\n"
           "  Licence: GNU General Public License, version 2 or later. Linked into it: SPOOLES (public domain), ARPACK-NG and\n"
           "  OpenBLAS (BSD 3-clause), the GCC runtime (GPL 3 with the GCC Runtime Library Exception).\n"
           "  Source: the unmodified ccx and SPOOLES sources, OPAD's build recipe (engines/calculix in OPAD's repository) and\n"
           "  its patches, published next to the pack on https://github.com/ahmad-amrey/opad/releases (engines-calculix-*).\n";
  return {};
}

}  // namespace

std::string engine_pack_platform() {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
  return "windows-x64";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return "macos-arm64";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux-x64";
#else
  return {};
#endif
}

std::vector<EnginePack> engine_pack_table() {
  std::vector<EnginePack> out;
  for (const Pin& p : kPins) {
    const std::string stem = std::string(p.id) + "-" + p.version + "-" + p.platform;
    out.push_back({p.id, p.name, p.version, p.platform, std::string(kReleases) + "engines-" + stem + "/opad-" + stem + ".zip", p.size, p.sha256});
  }
  return out;
}

std::vector<EnginePack> engine_packs() {
  if (!from_environment()) return pinned();
  std::vector<EnginePack> out;
  const json list = json::parse(read_text_file(path_from_utf8(std::getenv("OPAD_ENGINE_PACKS"))));
  for (const auto& p : list)
    out.push_back({p.value("id", ""), p.value("name", p.value("id", "")), p.value("version", ""), p.value("platform", ""), p.value("url", ""),
                   p.value("size", std::uint64_t(0)), p.value("sha256", "")});
  return out;
}

std::optional<EnginePack> engine_pack(const std::string& id) {
  for (auto& p : engine_packs())
    if (p.id == id) return p;
  return std::nullopt;
}

bool engine_pack_url_allowed(const std::string& url) {
  if (from_environment()) return local_url(url);
  return url.rfind(std::string(kReleases), 0) == 0 && url.find("..") == std::string::npos;
}

fs::path engines_dir() {
  if (const char* e = std::getenv("OPAD_ENGINES_DIR"); e && *e) return path_from_utf8(e);
  // Qt's AppLocalDataLocation for organisation "opad", application "OPAD" (app/AppData.hpp), so opad-cli, which may run
  // without Qt's paths, finds what the app installed.
  auto env = [](const char* name) -> fs::path {
    const char* v = std::getenv(name);
    return v && *v ? path_from_utf8(v) : fs::path();
  };
#if defined(_WIN32)
  const fs::path base = env("LOCALAPPDATA");
#elif defined(__APPLE__)
  const fs::path base = env("HOME").empty() ? fs::path() : env("HOME") / "Library" / "Application Support";
#else
  const fs::path base = !env("XDG_DATA_HOME").empty() ? env("XDG_DATA_HOME") : env("HOME").empty() ? fs::path() : env("HOME") / ".local" / "share";
#endif
  return base.empty() ? fs::path() : base / "opad" / "OPAD" / "engines";
}

fs::path installed_pack(const EnginePack& pack) {
  const fs::path root = engines_dir();
  if (root.empty() || pack.id.empty() || pack.version.empty()) return {};
  const fs::path dir = root / pack.folder();
  std::error_code ec;
  return fs::is_regular_file(dir / "opad-pack.json", ec) ? dir : fs::path();
}

fs::path installed_pack(const std::string& id) {
  try {
    if (const auto p = engine_pack(id))
      if (fs::path dir = installed_pack(*p); !dir.empty()) return dir;
  } catch (const std::exception&) {  // an unreadable OPAD_ENGINE_PACKS
  }
  // Else any complete pack of it (checked when it was installed: a build whose table names another version, or none).
  const fs::path root = engines_dir();
  if (root.empty()) return {};
  fs::path best;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    const std::string n = path_to_utf8(e.path().filename());
    if (n.rfind(id + "-", 0) != 0 || n.find(".partial") != std::string::npos || !fs::is_regular_file(e.path() / "opad-pack.json", ec)) continue;
    try {
      if (json::parse(read_text_file(e.path() / "opad-pack.json")).value("id", "") != id) continue;
    } catch (const std::exception&) {
      continue;
    }
    if (best.empty() || e.path().filename() > best.filename()) best = e.path();
  }
  return best;
}

fs::path install_pack(const EnginePack& pack, const fs::path& archive, const Progress& progress) {
  auto report = [&](double f, const std::string& phase) {
    if (progress && !progress(f, phase)) throw Error("cancelled");
  };
  const fs::path root = engines_dir();
  if (root.empty()) throw Error("no engines folder is set (OPAD_ENGINES_DIR)");
  std::error_code ec;
  const auto size = fs::file_size(archive, ec);
  if (ec) throw Error("the download of " + pack.name + " is missing: " + ec.message());
  if (size != pack.size)
    throw Error("the download of " + pack.name + " does not match what this OPAD expects (" + std::to_string(size) + " bytes, not " +
                std::to_string(pack.size) + "): it was not installed");
  report(0.0, "Checking " + pack.name);
  bool stopped = false;
  const std::string sha = sha256_file(archive, [&] { return stopped = progress && !progress(0.05, "Checking " + pack.name); });
  if (stopped) throw Error("cancelled");
  if (sha != pack.sha256) throw Error("the download of " + pack.name + " does not match what this OPAD expects (its SHA-256 differs): it was not installed");

  const detail::Zip zip(read_text_file(archive));
  fs::create_directories(root, ec);
  const fs::path partial = root / (pack.folder() + ".partial"), dir = root / pack.folder();
  remove_quietly(partial);
  fs::create_directories(partial, ec);
  if (ec) throw Error("cannot make " + path_to_utf8(partial) + ": " + ec.message());
  try {
    std::uint64_t total = 0, done = 0;
    for (size_t i = 0; i < zip.count(); ++i) total += zip.size(i);
    for (size_t i = 0; i < zip.count(); ++i) {
      const std::string& name = zip.name(i);
      const fs::path rel = member_path(name);
      if (rel.empty()) {
        if (name == "/" || name == "./") continue;
        throw Error("the download of " + pack.name + " holds a file outside its folder (" + name + "): it was not installed");
      }
      const fs::path to = partial / rel;
      if (name.back() == '/') {
        fs::create_directories(to, ec);
        continue;
      }
      fs::create_directories(to.parent_path(), ec);
      const std::string data = zip.read(i);
      {
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        if (!out.good()) throw Error("cannot write " + path_to_utf8(to) + " (is the disk full?)");
      }
#ifndef _WIN32
      if (const uint32_t mode = zip.mode(i) & 0777) fs::permissions(to, fs::perms(mode), fs::perm_options::replace, ec);
#endif
      done += data.size();
      report(0.1 + 0.88 * double(done) / double(std::max<std::uint64_t>(total, 1)), "Unpacking " + pack.name);
    }
    // The pack says what it is: a zip of something else (or of another version) is refused.
    const json meta = json::parse(read_text_file(partial / "opad-pack.json"));
    if (meta.value("id", "") != pack.id || meta.value("version", "") != pack.version)
      throw Error("the download of " + pack.name + " is a different pack (" + meta.value("id", "?") + " " + meta.value("version", "?") + ")");
  } catch (...) {
    remove_quietly(partial);
    throw;
  }
  remove_quietly(dir);
  fs::rename(partial, dir, ec);
  if (ec) {
    remove_quietly(partial);
    throw Error("cannot put " + pack.name + " in place (" + path_to_utf8(dir) + "): " + ec.message());
  }
  for (const auto& e : fs::directory_iterator(root, ec)) {  // other versions of this pack, and halves of earlier tries
    const std::string n = path_to_utf8(e.path().filename());
    if (n != pack.folder() && n.rfind(pack.id + "-", 0) == 0) remove_quietly(e.path());
  }
  report(1.0, pack.name + " installed");
  return dir;
}

bool remove_pack(const std::string& id) {
  const fs::path root = engines_dir();
  if (root.empty()) return false;
  bool any = false;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(root, ec))
    if (path_to_utf8(e.path().filename()).rfind(id + "-", 0) == 0) remove_quietly(e.path()), any = true;
  return any;
}

std::string engine_pack_notices() {
  std::string out;
  for (const char* id : {"openfoam", "calculix"}) {
    out += notice_of(id);
    if (const fs::path dir = installed_pack(id); !dir.empty()) try {
        out += "\n" + read_text_file(dir / "NOTICE.txt");
      } catch (const std::exception&) {
      }
    out += "\n";
  }
  return out;
}

}  // namespace opad::sim
