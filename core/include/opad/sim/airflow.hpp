#pragma once
// The air in thermal studies (sim/fea.hpp, kind thermal): how much heat a surface gives to the air around it, from the
// correlations of heat-transfer handbooks rather than a flow solution.
//
//   natural convection  each face by its tilt against gravity and its size: a vertical plate (Churchill and Chu), a hot
//                       face looking up or a cold one looking down (Lloyd and Moran), the other way (Raithby and
//                       Hollands); a face that looks at another one close by (a heatsink's fins) as the channel between
//                       two plates (Bar-Cohen and Rohsenow), whose boundary layers meet;
//   forced convection   a face in a stream along it: the flat plate, laminar or turbulent (Churchill and Ozoe / the
//                       mixed-length correlation);
//   heatsink and fan    a plate-fin heatsink found in the geometry (fins, their thickness, gap, height and length), the
//                       fan's curve against the heatsink's pressure drop (developing flow in its channels, with the
//                       entrance and exit losses: Muzychka and Yovanovich, Kays and London) for the air it moves, and the
//                       channels' heat transfer (Teertstra, Yovanovich and Culham's composite model); the air warms along
//                       the fins as it takes their heat.
//
// SI units here (m, m/s, W/m2K, Pa, m3/s, degC); the study converts at its edge. Air is dry, at 1 atm by default.
#include <TopoDS_Shape.hxx>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../util.hpp"

namespace opad::sim::air {

struct Air {
  double T;       // degC
  double rho;     // kg/m3
  double mu;      // Pa.s
  double k;       // W/m.K
  double cp;      // J/kg.K
  double nu;      // m2/s
  double alpha;   // m2/s
  double Pr;
  double beta;    // 1/K
};
Air properties(double T_C, double p_Pa = 101325);  // dry air at its film temperature

// Natural convection from a face at Ts into still air at Tinf: up is the face's outward normal dotted with "up" (against
// gravity), L its height for a tilted face or its area over its perimeter for a flat one (m). gap: the distance to the face
// it looks at (m), 0 when none is near: the channel correlation then. g: m/s2.
double natural_h(double up, double L, double Ts, double Tinf, double gap = 0, double g = 9.80665);
// Forced convection, average over a face L long in the stream (m) at U (m/s), the air at T (degC).
double forced_plate_h(double U, double L, double T);

// A plate-fin heatsink: a base and parallel fins standing on it, the air along the fins.
struct FinArray {
  int fins = 0;
  double t = 0, gap = 0, height = 0, length = 0, width = 0, base = 0;  // m: fin thickness, between fins, fin height, along the flow, across, base thickness
  Vec3 flow{1, 0, 0}, across{0, 1, 0}, up{0, 0, 1};                  // unit: the air's way, the fins' spacing, from the base to the tips
  double open() const { return (fins > 1 ? fins - 1 : 0) * gap * height; }  // m2: the channels' frontal area
  double sigma() const { return width > 0 ? 1 - fins * t / width : 0; }  // open over frontal area
  json to_json() const;
};
// The fin array of a body (world coordinates, mm) for air along `flow`: lines across it count the fins; nullopt when it
// has fewer than three parallel fins across the flow.
std::optional<FinArray> fin_array(const TopoDS_Shape& body, const Vec3& flow);

// How thin a body gets (mm, its own units): from the middle of each face, the distance through the solid to its other side
// (a wall's thickness) and through the air in front of it to the next face (a slot's or a gap's width; 0 when none). The
// CFD sizes its cells by them, so that a board, an enclosure's wall or a vent keeps cells across.
struct Thinness {
  double wall = 0, gap = 0;
};
Thinness thinness(const TopoDS_Shape& body);

// The channels of a heatsink with Q (m3/s) through them, the air at T: the speed in the channels, their Reynolds number,
// the average heat transfer coefficient of their walls and the pressure drop across the heatsink.
struct Channel {
  double V, Re, h, dp, Dh;
};
Channel channel(const FinArray& f, double Q, double T);

// A fan: its curve, pressure (Pa) against flow (m3/s), from a few points or from its free-flow and shut-off values.
struct Fan {
  std::string id, name;
  double size = 0;            // mm
  double rpm = 0;
  double Qmax = 0, Pmax = 0;  // m3/s at no pressure, Pa at no flow
  std::vector<std::array<double, 2>> curve;  // [m3/s, Pa] points from free flow to shut-off, when given
  double pressure(double Q) const;
  json to_json() const;
};
const std::vector<Fan>& fans();
const Fan* fan(const std::string& id);
// A fan from a load's `fan`: a library id, or {"flow" m3/h | "cfm", "pressure" Pa | "mmH2O", "curve": [[m3/h, Pa], ...]}.
Fan fan_from(const json& spec);
// Where the fan's curve meets the system's (Pa against m3/s, rising with flow): the flow; 0 when the fan cannot move air.
double operating_point(const Fan& fan, const std::function<double(double)>& system, int fans_in_parallel = 1);

}  // namespace opad::sim::air
