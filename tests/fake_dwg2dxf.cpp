// A stand-in DWG converter for the tests (OPAD_DWG2DXF): called as LibreDWG's dwg2dxf is (-v0 -y -o <out> <in>), it waits
// OPAD_FAKE_DWG_SLEEP milliseconds, copies <in> to <out> (a test's "DWG" is DXF text) and adds a line to OPAD_FAKE_DWG_LOG.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::filesystem::path out = argv[argc - 2], in = argv[argc - 1];
  if (const char* ms = std::getenv("OPAD_FAKE_DWG_SLEEP")) std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(ms)));
  std::error_code error;
  std::filesystem::copy_file(in, out, std::filesystem::copy_options::overwrite_existing, error);
  if (const char* log = std::getenv("OPAD_FAKE_DWG_LOG")) std::ofstream(log, std::ios::app) << in.filename().string() << "\n";
  return error ? 1 : 0;
}
