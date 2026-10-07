#pragma once
// Studies: what a mechanism does over time and what a part's material does under load.
//
//   motion   kinematic: joints driven by functions of time, the parts placed by the kinematic solver at each frame
//            (sim/kinematics.hpp), with joint values, speeds and accelerations and the paths of traced points;
//   dynamic  Project Chrono: masses from the bodies' volumes and materials, gravity, joints with their drives
//            (position, speed, torque or force), springs, friction and contacts between parts; reactions, motor torques
//            and power, energies (sim/dynamics.hpp);
//   static   Netgen mesh + CalculiX: displacements and stresses for a load case (sim/fea.hpp);
//   modal    the same: natural frequencies and mode shapes with the case's supports;
//   thermal  the same mesh, heat transfer: temperatures from heat sources, convection (natural, forced, fans through
//            heatsinks: sim/airflow.hpp) and radiation, steady or over time; or the air solved (sim/cfd.hpp);
//   sweep    one of those again over values of the design's parameters (a vent's place, a fin count): the best design.
//
// A study op keeps its settings and the summary of its last run; the full results (every frame, every node) are
// recomputed on demand and kept in memory for the process (run_study_cached).
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../document.hpp"
#include "../scene.hpp"

namespace opad::sim {

struct Series {
  std::string id;     // what it is of: a joint, part, point or "energy"
  std::string name;   // "Crank rotation", "Piston speed", "Kinetic energy"
  std::string unit;   // deg, mm, deg/s, mm/s, N, N.mm, W, J, ...
  std::string group;  // value | speed | acceleration | trace | reaction | motor | energy | contact
  std::vector<double> v;
};

struct FeaResult;  // sim/fea.hpp

struct StudyRun {
  std::string kind;
  std::vector<double> t;                        // frame times (s); modal: one frame per mode
  std::vector<std::string> parts;               // the parts the frames place
  std::vector<std::vector<Mat4>> poses;         // [frame][part]: world placement
  std::vector<Series> series;
  json summary = json::object();                // what the study op's result keeps
  std::vector<std::string> warnings;
  std::shared_ptr<const FeaResult> fea;         // static and modal
};

using Progress = std::function<bool(double fraction, const std::string& phase)>;  // false: cancel

// Runs a study (its op's data: kind, settings) on the document as it is now. Throws Error for settings it cannot run.
StudyRun run_study(const Document& doc, const Scene& scene, const json& study, const Progress& progress = {});
// The same, remembered per document state and study settings for this process (the app and an agent asking again).
std::shared_ptr<const StudyRun> run_study_cached(const Document& doc, const Scene& scene, const json& study, const Progress& progress = {});
// What an agent gets: the summary, and with `series` the named series sampled to at most `samples` points.
json study_report(const StudyRun& run, const json& options);
// The scene posed at frame i of a run (parts at their places).
Scene posed_at(const Scene& scene, const StudyRun& run, size_t frame);

// Whether the engines are in this build / on this machine (dynamic: Chrono; static and modal: Netgen and CalculiX's ccx).
json engines();

// ---- internal, one per engine (sim/dynamics.cpp, sim/fea.cpp)
struct AirFace;
StudyRun run_motion(const Document& doc, const Scene& scene, const json& settings, const Progress& progress);
StudyRun run_dynamic(const Document& doc, const Scene& scene, const json& settings, const Progress& progress);
// sweep: another study run again over values of the design's parameters, the best found (sim/sweep.cpp).
StudyRun run_sweep(const Document& doc, const Scene& scene, const json& settings, const Progress& progress);
// air: a thermal study's films from the air solved around the parts (sim/fea.hpp, AirFilms); its convection, radiation and
// fan loads are then left out.
StudyRun run_structural(const Document& doc, const Scene& scene, const std::string& kind, const json& settings, const Progress& progress,
                        const std::function<void(std::vector<AirFace>&, int)>* air = nullptr);

}  // namespace opad::sim
