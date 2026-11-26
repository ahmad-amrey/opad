// Solver packs (opad/sim/engine_packs.hpp): a downloaded zip is checked against its pinned size and SHA-256 and unpacked
// into the engines folder, all or nothing; where it may come from; OPAD's discovery finds an installed pack.
#include <zlib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "opad/sim/engine_packs.hpp"
#include "opad/sim/fea.hpp"
#include "opad/util.hpp"

namespace fs = std::filesystem;
using opad::sim::EnginePack;

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) unsetenv(name);
  else setenv(name, value.c_str(), 1);
#endif
}

// A zip of `files` (name, data): stored or deflated by turns, so both of the reader's ways are used.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
  std::string out, dir;
  auto u16 = [](std::string& s, unsigned v) { s += char(v & 0xFF), s += char((v >> 8) & 0xFF); };
  auto u32 = [&](std::string& s, unsigned long v) { u16(s, unsigned(v & 0xFFFF)), u16(s, unsigned((v >> 16) & 0xFFFF)); };
  int n = 0;
  for (const auto& [name, data] : files) {
    const bool squeeze = n % 2 == 1 && !data.empty();
    std::string body = data;
    if (squeeze) {
      body.resize(compressBound(uLong(data.size())) + 64);
      z_stream s{};
      deflateInit2(&s, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
      s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
      s.avail_in = uInt(data.size());
      s.next_out = reinterpret_cast<Bytef*>(body.data());
      s.avail_out = uInt(body.size());
      deflate(&s, Z_FINISH);
      body.resize(s.total_out);
      deflateEnd(&s);
    }
    const unsigned long crc = crc32(0, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size()));
    const unsigned long at = static_cast<unsigned long>(out.size());
    auto header = [&](std::string& s, bool central) {
      u32(s, central ? 0x02014b50 : 0x04034b50);
      if (central) u16(s, 20);
      u16(s, 20), u16(s, 0), u16(s, squeeze ? 8 : 0), u16(s, 0), u16(s, 0);
      u32(s, crc), u32(s, static_cast<unsigned long>(body.size())), u32(s, static_cast<unsigned long>(data.size()));
      u16(s, unsigned(name.size())), u16(s, 0);
      if (central) u16(s, 0), u16(s, 0), u16(s, 0), u32(s, 0), u32(s, at);
      s += name;
    };
    header(out, false);
    out += body;
    header(dir, true);
    ++n;
  }
  const unsigned long dir_at = static_cast<unsigned long>(out.size());
  out += dir;
  u32(out, 0x06054b50), u16(out, 0), u16(out, 0), u16(out, unsigned(n)), u16(out, unsigned(n));
  u32(out, static_cast<unsigned long>(dir.size())), u32(out, dir_at), u16(out, 0);
  return out;
}

struct Sandbox {
  fs::path root;
  Sandbox() {
    root = fs::temp_directory_path() / ("opad-test-packs-" + opad::new_uuid().substr(0, 8));
    fs::create_directories(root / "engines");
    set_env("OPAD_ENGINES_DIR", opad::path_to_utf8(root / "engines"));
    set_env("OPAD_ENGINE_PACKS", "");
    set_env("OPAD_CCX", "");
  }
  ~Sandbox() {
    set_env("OPAD_ENGINES_DIR", "");
    set_env("OPAD_ENGINE_PACKS", "");
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  // The zip written to a file, and a pack whose pins match it.
  EnginePack pack(const std::string& zip, const std::string& id = "calculix", const std::string& version = "2.22-9") {
    const fs::path file = root / (id + "-" + version + ".download");
    std::ofstream(file, std::ios::binary) << zip;
    return {id, id == "calculix" ? "CalculiX 2.22" : id, version, "windows-x64", "http://127.0.0.1:1/x.zip", zip.size(), opad::sha256_hex(zip)};
  }
  fs::path download(const EnginePack& p) const { return root / (p.id + "-" + p.version + ".download"); }
  std::vector<std::string> folders() const {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(root / "engines")) out.push_back(opad::path_to_utf8(e.path().filename()));
    return out;
  }
};

std::string meta(const std::string& id, const std::string& version) {
  return R"({"id": ")" + id + R"(", "version": ")" + version + R"("})";
}

std::string error_of(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

}  // namespace

TEST(a_pack_whose_hash_matches_is_unpacked_with_names_as_written) {
  Sandbox box;
  const std::string exe(300000, 'x');  // a big member: deflated, several progress steps
  const EnginePack p = box.pack(make_zip({{"bin/", ""}, {"bin/ccx.exe", exe}, {"etc/caseDicts/README", "Read me"},
                                          {"licenses/CalculiX-gpl-2.0.txt", "GPL"}, {"opad-pack.json", meta("calculix", "2.22-9")}}));
  std::vector<double> seen;
  const fs::path dir = opad::sim::install_pack(p, box.download(p), [&](double f, const std::string&) { return seen.push_back(f), true; });
  CHECK(dir == box.root / "engines" / "calculix-2.22-9");
  CHECK(opad::read_text_file(dir / "bin" / "ccx.exe") == exe);
  CHECK(opad::read_text_file(dir / "etc" / "caseDicts" / "README") == "Read me");  // case kept (OpenFOAM's etc/ needs it)
  CHECK(opad::sim::installed_pack(p) == dir);
  CHECK(box.folders() == std::vector<std::string>{"calculix-2.22-9"});  // no .partial left behind
  CHECK(!seen.empty() && seen.back() == 1.0);
  // Found whatever this build's table names (an installed pack was checked when it was unpacked); a half-unpacked folder
  // or one that is not a pack is never taken.
  fs::create_directories(box.root / "engines" / "calculix-2.22-99.partial" / "bin");
  std::ofstream(box.root / "engines" / "calculix-2.22-99.partial" / "opad-pack.json") << meta("calculix", "2.22-99");
  fs::create_directories(box.root / "engines" / "calculix-zz");
  CHECK(opad::sim::installed_pack("calculix") == dir);
  fs::remove_all(box.root / "engines" / "calculix-2.22-99.partial");
  fs::remove_all(box.root / "engines" / "calculix-zz");
  // OPAD's discovery takes it once OPAD_CCX is not set.
  set_env("OPAD_ENGINE_PACKS", opad::path_to_utf8(box.root / "table.json"));
  std::ofstream(box.root / "table.json") << R"([{"id": "calculix", "name": "CalculiX 2.22", "version": "2.22-9", "url": "http://127.0.0.1:1/x.zip", "size": )"
                                         << p.size << R"(, "sha256": ")" << p.sha256 << R"("}])";
  CHECK(opad::sim::installed_pack("calculix") == dir);
#ifdef _WIN32
  CHECK(opad::sim::ccx_program() == dir / "bin" / "ccx.exe");
#endif
  CHECK(opad::sim::remove_pack("calculix") && opad::sim::installed_pack("calculix").empty() && box.folders().empty());
}

TEST(a_download_that_differs_is_refused_and_nothing_is_installed) {
  Sandbox box;
  const std::string zip = make_zip({{"bin/ccx.exe", "program"}, {"opad-pack.json", meta("calculix", "2.22-9")}});
  EnginePack p = box.pack(zip);
  EnginePack wrong_hash = p;
  wrong_hash.sha256[0] = wrong_hash.sha256[0] == '0' ? '1' : '0';
  CHECK(error_of([&] { opad::sim::install_pack(wrong_hash, box.download(p)); }).find("SHA-256 differs") != std::string::npos);
  EnginePack wrong_size = p;
  wrong_size.size += 1;
  CHECK(error_of([&] { opad::sim::install_pack(wrong_size, box.download(p)); }).find("bytes, not") != std::string::npos);
  CHECK(box.folders().empty());
  CHECK(opad::sim::installed_pack(p).empty());
}

TEST(a_member_outside_the_folder_is_refused) {
  for (const char* evil : {"../outside.txt", "bin/../../outside.txt", "/abs.txt", "C:/drive.txt", "bin\\..\\..\\back.txt"}) {
    Sandbox box;
    const EnginePack p = box.pack(make_zip({{"opad-pack.json", meta("calculix", "2.22-9")}, {evil, "boo"}}));
    const std::string why = error_of([&] { opad::sim::install_pack(p, box.download(p)); });
    CHECK(why.find("outside its folder") != std::string::npos);
    CHECK(!fs::exists(box.root / "outside.txt") && !fs::exists(box.root / "back.txt"));
    CHECK(box.folders().empty());  // the half-written folder is gone
  }
}

TEST(a_zip_of_another_pack_is_refused) {
  Sandbox box;
  const EnginePack p = box.pack(make_zip({{"bin/simpleFoam.exe", "x"}, {"opad-pack.json", meta("openfoam", "v2512-1")}}));
  CHECK(error_of([&] { opad::sim::install_pack(p, box.download(p)); }).find("different pack") != std::string::npos);
  CHECK(box.folders().empty());
}

TEST(a_new_version_replaces_the_old_one_and_cancel_leaves_nothing) {
  Sandbox box;
  const EnginePack one = box.pack(make_zip({{"bin/ccx.exe", "one"}, {"opad-pack.json", meta("calculix", "2.22-1")}}), "calculix", "2.22-1");
  opad::sim::install_pack(one, box.download(one));
  const EnginePack two = box.pack(make_zip({{"bin/ccx.exe", "two"}, {"opad-pack.json", meta("calculix", "2.22-2")}}), "calculix", "2.22-2");
  // Cancelled half way: the installed version stays as it was.
  CHECK(error_of([&] { opad::sim::install_pack(two, box.download(two), [](double f, const std::string&) { return f < 0.5; }); }) == "cancelled");
  CHECK(box.folders() == std::vector<std::string>{"calculix-2.22-1"});
  opad::sim::install_pack(two, box.download(two));
  CHECK(box.folders() == std::vector<std::string>{"calculix-2.22-2"});
  CHECK(opad::read_text_file(box.root / "engines" / "calculix-2.22-2" / "bin" / "ccx.exe") == "two");
}

TEST(packs_come_only_from_opads_releases) {
  Sandbox box;
  // The table compiled in: only github.com/ahmad-amrey/opad/releases/download/.
  CHECK(opad::sim::engine_pack_url_allowed("https://github.com/ahmad-amrey/opad/releases/download/engines-openfoam-v2512-1/x.zip"));
  CHECK(!opad::sim::engine_pack_url_allowed("https://github.com/someone/opad/releases/download/engines-openfoam-v2512-1/x.zip"));
  CHECK(!opad::sim::engine_pack_url_allowed("https://github.com/ahmad-amrey/opad/releases/download/../../evil/x.zip"));
  CHECK(!opad::sim::engine_pack_url_allowed("http://github.com/ahmad-amrey/opad/releases/download/x/y.zip"));
  CHECK(!opad::sim::engine_pack_url_allowed("http://127.0.0.1:8080/x.zip"));
  for (const auto& p : opad::sim::engine_packs()) {
    CHECK(opad::sim::engine_pack_url_allowed(p.url));
    CHECK(p.sha256.size() == 64 && p.size > 0);
  }
  // A test's table (OPAD_ENGINE_PACKS): a local fake release only.
  std::ofstream(box.root / "table.json") << R"([{"id": "openfoam", "version": "v1", "url": "http://127.0.0.1:9/x.zip", "size": 1, "sha256": "00"}])";
  set_env("OPAD_ENGINE_PACKS", opad::path_to_utf8(box.root / "table.json"));
  CHECK(opad::sim::engine_pack("openfoam").has_value() && !opad::sim::engine_pack("calculix").has_value());
  CHECK(opad::sim::engine_pack_url_allowed("http://127.0.0.1:9/x.zip"));
  CHECK(!opad::sim::engine_pack_url_allowed("https://github.com/ahmad-amrey/opad/releases/download/engines-openfoam-v2512-1/x.zip"));
  CHECK(!opad::sim::engine_pack_url_allowed("http://example.com/x.zip"));
}

TEST(the_notices_name_the_licences_and_opencfds_statement) {
  Sandbox box;
  const std::string n = opad::sim::engine_pack_notices();
  CHECK(n.find("GNU General Public License, version 3") != std::string::npos);
  CHECK(n.find("version 2 or later") != std::string::npos);
  CHECK(n.find("not approved or endorsed by OpenCFD Limited") != std::string::npos);
  CHECK(n.find("github.com/ahmad-amrey/opad/releases") != std::string::npos);
}

CHECK_MAIN()
