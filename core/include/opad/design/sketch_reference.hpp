#pragma once
#include "sketch_geom.hpp"
#include <functional>
namespace opad::design {
// All coordinates are projected into the destination frame. Native curve bases
// are retained; silhouette and section calculations run on the caller's worker.
Sketch derive_sketch(const Document& doc,const Scene& scene,const Frame& frame,const json& source,
                     const std::string& mode="project",const std::map<std::string,TopoDS_Shape>& fresh={});
void append_reference(Sketch& destination,const Sketch& geometry,const json& source,const std::string& mode,bool linked);
void refresh_references(Sketch& sketch,const std::function<Sketch(const json&,const std::string&)>& derive);
void break_reference(Sketch& sketch,const std::vector<int>& entities);
}
