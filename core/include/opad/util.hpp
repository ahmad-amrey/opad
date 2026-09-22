#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace opad {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string new_uuid();
bool is_uuid(std::string_view s);
std::string now_iso8601();
std::string sha256_hex(std::string_view data);
std::string default_author();
std::string version_string();
// Routes OCCT kernel messages to stderr (alarms only unless verbose or OPAD_VERBOSE=1) so stdout stays JSON.
void configure_kernel_logging(bool verbose = false);

std::string read_text_file(const std::filesystem::path& p);
void write_text_file(const std::filesystem::path& p, std::string_view text);

using Vec3 = std::array<double, 3>;

// Row-major 4x4 homogeneous transform; last row is always 0 0 0 1. Units are mm.
struct Mat4 {
  std::array<double, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  static Mat4 identity() { return Mat4{}; }
  static Mat4 translation(double x, double y, double z);
  bool is_identity(double eps = 1e-12) const;
  Mat4 operator*(const Mat4& o) const;
  Vec3 apply(const Vec3& p) const;
  Vec3 apply_dir(const Vec3& d) const;
  json to_json() const;
  static Mat4 from_json(const json& j);
  double& at(int r, int c) { return m[static_cast<size_t>(r * 4 + c)]; }
  double at(int r, int c) const { return m[static_cast<size_t>(r * 4 + c)]; }
};

// A reference to geometry: a body node, or a face/edge/vertex inside it (by ordinal in the immutable
// content-addressed body entry, hence stable), or a free 3D point.
struct Ref {
  enum class Kind { Body, Face, Edge, Vertex, Point, Center };
  std::string body;  // node uuid; empty for Point
  Kind kind = Kind::Body;
  int index = -1;    // ordinal for Face/Edge/Vertex; Center uses its circular edge ordinal
  Vec3 point{0, 0, 0};

  std::string str() const;
  json to_json() const;
  static Ref parse(std::string_view s);  // "uuid", "uuid/face/3", "uuid/edge/2", "uuid/vertex/1", "point/x,y,z"
  static Ref from_json(const json& j);
  static const char* kind_name(Kind k);
};

}  // namespace opad
