// Stands in for the ODA File Converter in test_dxf (ODAFileConverter <in dir> <out dir> ACAD2018 DXF 0 1 <file>): writes
// one line as <out dir>/<file stem>.dxf. Linked statically, so a copy runs from any folder.
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  if (argc < 8) return 2;
  std::string name = argv[7];
  if (const auto dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
  FILE* out = std::fopen((std::string(argv[2]) + "/" + name + ".dxf").c_str(), "wb");
  if (!out) return 1;
  std::fputs("0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nODA\n10\n0\n20\n0\n11\n10\n21\n0\n0\nENDSEC\n0\nEOF\n", out);
  std::fclose(out);
  return 0;
}
