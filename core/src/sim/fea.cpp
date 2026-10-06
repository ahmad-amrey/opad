#include "opad/sim/study.hpp"

namespace opad::sim {

StudyRun run_structural(const Document&, const Scene&, const std::string&, const json&, const Progress&) {
  throw Error("static and modal studies need Netgen and CalculiX");
}

json engines() {
#ifdef OPAD_HAVE_CHRONO
  const bool chrono = true;
#else
  const bool chrono = false;
#endif
  return {{"motion", true}, {"dynamic", chrono}, {"static", false}, {"modal", false}};
}

}  // namespace opad::sim
