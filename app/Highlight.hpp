#pragma once
// The interaction roles in the view (UI-38): what the pointer is over glows white (Tokens::hover), what is selected takes
// the hued Tokens::selected3d, the same over bodies, faces, edges and vertices, sketch curves, drawings and the view cube
// (whose face the view looks straight at is drawn as selected). A selection is a tint over its faces and edges thicker
// than the hover's in a halo. Over a body whose own colour is close to the selection colour (a blue part, a sketch's
// blue wire) the tint cannot be seen: its edges are drawn in an outline colour that stands off the body instead.
#include <QColor>

#include "Theme.hpp"

namespace highlight {
constexpr double kCloseDeltaE = 25;     // CIE76: under this a body's colour reads as the selection colour itself
constexpr double kHoverEdgeWidth = 3;   // the hover's edge line (white, no halo)
constexpr double kHoverRimWidth = 7;    // under a hovered line on a light background: the driver's widest line (device px)

struct Selection {
  QColor fill;  // over the selected faces, at fillAlpha
  double fillAlpha = 0.4;
  QColor edge;  // the edges' core line: the selection colour, or the outline colour
  double edgeWidth = 3;
  // A wider line under the core: the selection colour dimmed towards the view's background, opaque (drawn in order, under
  // the core; a translucent one is drawn after everything opaque, over the core).
  // Drivers draw lines up to about 7 device pixels wide, so the halo stays near that and the core narrower.
  QColor halo;
  double haloAlpha = 1, haloWidth = 7;
  bool outline = false;  // the body is close to the selection colour: `edge` is outlineFor(body)
  double point = 3, pointHalo = 7;  // vertex markers (OCCT marker scales), core and halo
};

bool closeToSelection(const Tokens& t, const QColor& body);
QColor outlineFor(const QColor& body);  // white on a dark colour, near black on a light one
// The hover's glow under a line drawn on the view's background (sketch curves): the white role, dimmed on the dark theme.
QColor hoverHalo(const Tokens& t);
// On a light background white alone hardly reads: a rim darker than the background under the hover's white line or glow
// (edges, curve bodies, sketch curves). Invalid on the dark theme, where the white stands out by itself.
QColor hoverRim(const Tokens& t);
// A whole body's look when selected (its face boundaries thinner) or a picked face's, edge's or vertex's (wholeBody false);
// body: its colour, invalid when unknown.
Selection selection(const Tokens& t, const QColor& body, bool wholeBody);
}  // namespace highlight
