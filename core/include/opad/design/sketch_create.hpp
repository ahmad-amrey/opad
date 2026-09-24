#pragma once
#include "sketch.hpp"
namespace opad::design {
// Advanced primitive variants. Coordinates are local millimetres; a conic's middle pick is its tangent
// control point, and rho (0..1) controls the rational quadratic's weight. Existing IDs are never rewritten.
std::vector<int> create_primitive(Sketch& sketch, const std::string& kind,
    const std::vector<std::pair<double,double>>& picks, const json& options = json::object());
}
