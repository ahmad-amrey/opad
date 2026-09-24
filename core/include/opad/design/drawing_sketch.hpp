#pragma once
#include "sketch.hpp"
#include "../document.hpp"
#include "../scene.hpp"

namespace opad::design {
struct DrawingLayer { std::string id; bool construction=false; };
// Project selected drawing layers into one editable sketch. Analytic lines and
// circular arcs are retained; other curves use the requested chord tolerance.
Sketch drawing_sketch(const Document& doc,const Scene& scene,const std::vector<DrawingLayer>& layers,
                      const Frame& frame,double tolerance=0.01);
}
