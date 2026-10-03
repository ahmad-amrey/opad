#pragma once
#include <functional>
#include "sketch.hpp"
#include "../document.hpp"
#include "../scene.hpp"

namespace opad::design {
void simplify_sketch(Sketch& sketch,double tolerance);
struct DrawingLayer { std::string id; bool construction=false; };
// Project selected drawing layers into one editable sketch. Analytic lines and
// circular arcs and native splines are retained exactly; segmented smooth chains
// are reconstructed within the requested geometric tolerance. O(n log n) in the curves; more than 100,000 (curves and
// free vertices) throw before any is converted; `cancelled` is polled and a true answer ends it with Error("cancelled").
Sketch drawing_sketch(const Document& doc,const Scene& scene,const std::vector<DrawingLayer>& layers,
                      const Frame& frame,double tolerance=0.01,const std::function<bool()>& cancelled={});
// The plane and origin the layers' drawing lies in: its own XY frame where its import placed it. A sketch converted
// from a drawing uses exactly this frame, so its coordinates are the drawing's. Throws when the layers come from
// drawings placed on different planes (they convert separately).
Frame drawing_frame(const Scene& scene,const std::vector<DrawingLayer>& layers);
}
