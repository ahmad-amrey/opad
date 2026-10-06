#pragma once
// Pictures of mechanisms and results for the render command: the parts at joint values, a motion or dynamic study's frame,
// or a structural study's result map (stress or displacement in colour on its deformed shape, with a legend), all without
// writing to the document.
#include <memory>
#include <string>
#include <vector>

#include "../render.hpp"

namespace opad::sim {

struct Picture {
  Scene scene;
  std::shared_ptr<std::vector<Mesh>> meshes;  // the result map's surface, kept alive for the render
  bool legend = false;
  std::string title, unit;
  double lo = 0, hi = 0;
  json info = json::object();  // what was drawn: the frame's time, the field's range, the deformation scale
};

// args: "joints": {joint: value}, or "study": {"id", "t" | "frame" | "mode", "field": von_mises | displacement | mode,
// "scale": deformation scale (default: the largest displacement shown as 5% of the model's size; 0: undeformed)}.
Picture picture(const Document& doc, Scene scene, const json& args, RenderOptions& options);

}  // namespace opad::sim
