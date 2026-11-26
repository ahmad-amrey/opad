#pragma once
// Solver packs: OpenFOAM and CalculiX built by OPAD's recipes (engines/<id>/make-pack.sh) from their pinned sources and
// published as zips on OPAD's own GitHub releases, one per pack revision and system (tags engines-<id>-<version>-<platform>,
// published by .github/workflows/engines.yml when dev gets a new revision). Each pack's URL, size and SHA-256 are compiled
// into OPAD; the app downloads one when asked (app/EngineInstall.cpp) and this checks and unpacks it into the user's
// engines folder, where openfoam() and ccx_program() look right after OPAD_OPENFOAM / OPAD_CCX.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "opad/sim/study.hpp"

namespace opad::sim {

struct EnginePack {
  std::string id;        // "openfoam", "calculix"
  std::string name;      // "OpenFOAM v2512", "CalculiX 2.22"
  std::string version;   // the pack's own: "v2512-1" (upstream version, pack revision)
  std::string platform;  // "windows-x64", "linux-x64", "macos-arm64"
  std::string url;       // the zip on a release of github.com/ahmad-amrey/opad
  std::uint64_t size = 0;
  std::string sha256;    // lower-case hex
  std::string folder() const { return id + "-" + version; }  // its folder in engines_dir()
};

// This system as packs name it: "windows-x64", "linux-x64", "macos-arm64", or empty where OPAD builds no pack.
std::string engine_pack_platform();
// Every pack OPAD knows of, for every system, published or not (an unpublished one has no size and no SHA-256): its URL is
// https://github.com/ahmad-amrey/opad/releases/download/engines-<id>-<version>-<platform>/opad-<id>-<version>-<platform>.zip.
std::vector<EnginePack> engine_pack_table();
// The packs this build can install on this system: the table's for engine_pack_platform() that are published (pinned). OPAD_ENGINE_PACKS=<json file> replaces
// the table (tests and benches: a fake release on 127.0.0.1); its URLs must then be on 127.0.0.1 or localhost.
std::vector<EnginePack> engine_packs();
// The pack of `id` ("openfoam", "calculix"), or none when this build has none for this system.
std::optional<EnginePack> engine_pack(const std::string& id);
// Whether `url` may be downloaded for a pack: https://github.com/ahmad-amrey/opad/releases/download/... for the pinned
// table; 127.0.0.1 / localhost only when the table came from OPAD_ENGINE_PACKS.
bool engine_pack_url_allowed(const std::string& url);

// The user's engines folder: OPAD_ENGINES_DIR (set by appdata::use in a portable copy: <exe dir>/data/engines; tests set it
// themselves), else <AppLocalDataLocation>/engines as Qt names it (%LOCALAPPDATA%\opad\OPAD\engines, ~/.local/share/opad/OPAD/engines).
std::filesystem::path engines_dir();
// Where the pack is unpacked (engines_dir()/<id>-<version>), or empty when it is not installed.
std::filesystem::path installed_pack(const EnginePack& pack);
// The pack of `id` this build names when it is installed, else any complete one of it in engines_dir() (the highest
// folder name): what discovery (openfoam(), ccx_program()) uses.
std::filesystem::path installed_pack(const std::string& id);

// Checks the downloaded `archive` against the pack's size and SHA-256 and unpacks it into engines_dir(): first into
// <folder>.partial, which is renamed once every file is written, so a folder named after the pack is always complete;
// other versions of the same pack are removed afterwards. Throws Error saying why (wrong size or hash: "the download
// does not match ..."; a member that would land outside the folder; no space). `progress` (0..1) may cancel ("cancelled").
// Returns the installed folder.
std::filesystem::path install_pack(const EnginePack& pack, const std::filesystem::path& archive, const Progress& progress = {});
// Removes the installed pack (every version of it). False when there was none.
bool remove_pack(const std::string& id);

// The notices for the packs (licence, where the source is, OpenCFD's trade mark statement), each followed by the
// installed pack's own NOTICE.txt when it is installed: for Help > Third-party licences.
std::string engine_pack_notices();

}  // namespace opad::sim
