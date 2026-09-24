#pragma once
#include "sketch.hpp"
#include "../document.hpp"
#include "../scene.hpp"

namespace opad::design {
void simplify_sketch(Sketch& sketch,double tolerance);
struct DrawingLayer { std::string id; bool construction=false; };
// Project selected drawing layers into one editable sketch. Analytic lines and
// circular arcs and native splines are retained exactly; segmented smooth chains
// are reconstructed within the requested geometric tolerance.
Sketch drawing_sketch(const Document& doc,const Scene& scene,const std::vector<DrawingLayer>& layers,
                      const Frame& frame,double tolerance=0.01);
}
