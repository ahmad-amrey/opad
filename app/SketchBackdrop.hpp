#pragma once
#include <AIS_InteractiveObject.hxx>
#include "Jobs.hpp"
#include "opad/scene.hpp"
// Worker-only texture preparation shared by the active editor and saved sketches.
std::vector<Handle(AIS_InteractiveObject)> prepareSketchBackdrops(const opad::json& images,const opad::Frame& frame,Progress progress);
