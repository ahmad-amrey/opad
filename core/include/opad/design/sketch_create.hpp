#pragma once
#include "sketch.hpp"
namespace opad::design {
// Advanced primitive variants. Coordinates are local millimetres; a conic's middle pick is its tangent
// control point, and rho (0..1) controls the rational quadratic's weight. Existing IDs are never rewritten.
// options.snaps (one per pick, null = free): {"point": id} reuses an existing point where the primitive puts a new one at
// that pick (or puts it on the new curve through the pick), {"holds": [["midpoint", line], ["coincident", curve],
// ["horizontal", point], ...]} constrains the new point there.
std::vector<int> create_primitive(Sketch& sketch, const std::string& kind,
    const std::vector<std::pair<double,double>>& picks, const json& options = json::object());
}
