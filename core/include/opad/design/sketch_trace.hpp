#pragma once
#include "sketch.hpp"
#include <functional>
namespace opad::design {
struct TraceOptions {
  int threshold=128, smoothing=1, noise=8;
  double tolerance=.5, corner_angle=45; // pixels, degrees
  bool invert=false;
};
// Grey pixels, row major. Output coordinates have their origin at the lower left.
// The caller controls image placement independently of the editable trace.
Sketch trace_bitmap(const std::vector<unsigned char>& grey,int width,int height,const TraceOptions& options,
                    const std::function<bool()>& cancel={});
}
