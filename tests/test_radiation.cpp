// Radiation by rays (core/src/sim/radiation.hpp) against exact view factors (Incropera, Fundamentals of Heat and Mass
// Transfer, table 13.2) and the energy a closed box must keep.
#include <cmath>
#include <vector>

#include "check.hpp"
#include "../core/src/sim/radiation.hpp"

using opad::Vec3;
namespace rad = opad::sim::radiation;

namespace {
// A rectangle from `o` along u and v, n x n squares of two triangles each, facing `normal`.
void add_rect(std::vector<rad::Surface>& out, Vec3 o, Vec3 u, Vec3 v, Vec3 normal, int n, double eps = 0.9) {
  auto at = [&](double a, double b) { return Vec3{o[0] + a * u[0] + b * v[0], o[1] + a * u[1] + b * v[1], o[2] + a * u[2] + b * v[2]}; };
  const double lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), lv = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      const double a0 = double(i) / n, a1 = double(i + 1) / n, b0 = double(j) / n, b1 = double(j + 1) / n;
      for (const auto& tri : {std::array<Vec3, 3>{at(a0, b0), at(a1, b0), at(a1, b1)}, std::array<Vec3, 3>{at(a0, b0), at(a1, b1), at(a0, b1)}}) {
        rad::Surface s;
        s.corners = tri;
        s.normal = normal;
        s.area = 0.5 * lu * lv / (n * n);
        s.emissivity = eps;
        out.push_back(s);
      }
    }
}

// The area-weighted share of [a0, a1)'s view taken by [b0, b1).
double share(const rad::ViewFactors& vf, const std::vector<rad::Surface>& s, size_t a0, size_t a1, size_t b0, size_t b1) {
  double A = 0, AF = 0;
  for (size_t i = a0; i < a1; ++i) {
    A += s[i].area;
    for (const auto& [j, f] : vf.to[i])
      if (size_t(j) >= b0 && size_t(j) < b1) AF += s[i].area * f;
  }
  return AF / A;
}
}  // namespace

TEST(parallel_squares_view_factor) {
  // Two 1 x 1 squares facing each other 1 apart: X = Y = 1, F = 0.1998.
  std::vector<rad::Surface> s;
  add_rect(s, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 10);
  const size_t half = s.size();
  add_rect(s, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}, 10);
  const auto vf = rad::view_factors(s, 1024);
  const double X = 1, Y = 1;
  const double F = 2 / (M_PI * X * Y) *
                   (std::log(std::sqrt((1 + X * X) * (1 + Y * Y) / (1 + X * X + Y * Y))) + X * std::sqrt(1 + Y * Y) * std::atan(X / std::sqrt(1 + Y * Y)) +
                    Y * std::sqrt(1 + X * X) * std::atan(Y / std::sqrt(1 + X * X)) - X * std::atan(X) - Y * std::atan(Y));
  CHECK_NEAR(F, 0.1998, 1e-3);
  CHECK_NEAR(share(vf, s, 0, half, half, s.size()), F, 0.01);
  CHECK_NEAR(share(vf, s, half, s.size(), 0, half), F, 0.01);
}

TEST(perpendicular_squares_view_factor) {
  // Two 1 x 1 squares at right angles along a common edge: F = 0.2000.
  std::vector<rad::Surface> s;
  add_rect(s, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 10);
  const size_t half = s.size();
  add_rect(s, {0, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, 10);
  const auto vf = rad::view_factors(s, 1024);
  CHECK_NEAR(share(vf, s, 0, half, half, s.size()), 0.2000, 0.01);
}

TEST(closed_box_keeps_its_energy) {
  // The inside of a closed cube: nothing reaches the room; one face hot, the rest cold, the net exchange sums to zero, and with
  // every face at one temperature each sink is that temperature.
  std::vector<rad::Surface> s;
  add_rect(s, {0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, 6, 0.3);   // floor, facing up (into the box)
  const size_t floor = s.size();
  add_rect(s, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}, 6, 0.8);  // roof
  add_rect(s, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, 6, 0.5);
  add_rect(s, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, -1, 0}, 6, 0.5);
  add_rect(s, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}, 6, 0.9);
  add_rect(s, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, 6, 0.9);
  const auto vf = rad::view_factors(s, 512);
  double room = 0, A = 0;
  for (size_t i = 0; i < s.size(); ++i) room += s[i].area * vf.room[i], A += s[i].area;
  CHECK(room / A < 0.005);
  std::vector<double> T(s.size(), 40.0);
  for (double sink : rad::sinks(vf, s, T, 20.0)) CHECK_NEAR(sink, 40.0, 0.3);
  for (size_t i = 0; i < floor; ++i) T[i] = 120.0;
  const auto sink = rad::sinks(vf, s, T, 20.0);
  double net = 0, out = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const double q = s[i].emissivity * 5.670374419e-8 * s[i].area * (std::pow(T[i] + 273.15, 4) - std::pow(sink[i] + 273.15, 4));
    net += q;
    if (i < floor) out += q;
  }
  CHECK(out > 0);
  CHECK(std::fabs(net) < 0.02 * out);
}

CHECK_MAIN()
