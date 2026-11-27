#pragma once
// A colour per vertex over the arrays a body is drawn with (Deviation's colour map, DeviationArea.cpp). The viewport draws a
// painted body from a coloured copy of its base mesh made on a worker (Viewport::setVertexPaints), and the zoom refinement
// paints its finer arrays the same way (refineTintKey), so the map stays on the triangles the body is drawn with.
#include <gp_Trsf.hxx>

#include <functional>
#include <memory>

struct BodyPrs;

class VertexPaint {
 public:
  virtual ~VertexPaint() = default;
  // Worker thread: a copy of `src` to draw, its triangles coloured per vertex (`src` is in the body's own frame, `placed`
  // puts it in the world); null when it cannot be painted. Never touches Qt widgets or the OCCT context.
  virtual std::shared_ptr<BodyPrs> paint(const BodyPrs& src, const gp_Trsf& placed, const std::function<bool()>& cancelled) const = 0;
};
