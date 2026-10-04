#pragma once
// What draw_view (sheet_display.cpp) draws of section, detail, auxiliary and broken views (views.cpp, UI-82); not a
// public header.
#include "opad/drawing/sheet.hpp"

namespace opad::drawing::detail {

// The section faces of g (shaped: folded view coordinates) hatched on paper, a pattern per body cut.
void draw_section_faces(Display& d, const ViewFrame& f, const ViewGeometry& g);
// The view's label (A-A, B (2:1), VIEW C), a detail view's circle, its break lines, and the cutting lines, circles and
// arrows of the section, detail and auxiliary views taken from it.
void draw_view_marks(Display& d, const ViewFrame& f, const SheetView& v, const Scene& scene);

}  // namespace opad::drawing::detail
