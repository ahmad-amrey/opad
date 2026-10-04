// The view's interaction roles (UI-38, app/Highlight.cpp): the selection is hued and unlike the hover in both themes, a
// body in the selection's own colour gets an outline that stands off it, a picked edge is drawn thicker than the hover's in
// a halo that sits between the selection colour and the background.
#include "Highlight.hpp"
#include "Theme.hpp"
#include "check.hpp"

#include <QColor>

TEST(selection_is_hued_and_unlike_the_hover) {
  for (const bool dark : {true, false}) {
    const Tokens t = theme::tokens(dark);
    CHECK(theme::deltaE(t.selected3d, t.hover) > 30);  // selected never reads as hovered
    CHECK(t.selected3d.hsvSaturationF() > 0.5);        // hued, not the grey it was
    const highlight::Selection grey = highlight::selection(t, QColor::fromRgbF(0.75, 0.75, 0.78), true);
    CHECK(grey.fill == t.selected3d && grey.edge == t.selected3d && !grey.outline);
    CHECK(grey.fillAlpha > 0.25 && grey.fillAlpha < 0.7);  // the body's shading still shows through
    // The halo lies between the selection colour and the view's background, drawn opaque under the core.
    CHECK(theme::deltaE(grey.halo, t.selected3d) > 5 && theme::deltaE(grey.halo, t.vp) > 5 && grey.haloAlpha == 1);
    const QColor glow = highlight::hoverHalo(t);
    CHECK(glow.hsvSaturationF() < 0.05 && theme::deltaE(glow, t.vp) > 5);  // white-ish, and seen on the background
  }
}

TEST(a_body_in_the_selection_colour_is_outlined) {
  for (const bool dark : {true, false}) {
    const Tokens t = theme::tokens(dark);
    const QColor same = t.selected3d, near = t.selected3d.darker(110), far = QColor("#c0392b");
    CHECK(highlight::closeToSelection(t, same) && highlight::closeToSelection(t, near) && !highlight::closeToSelection(t, far));
    CHECK(!highlight::closeToSelection(t, QColor()));  // unknown colour: no outline
    for (const QColor& body : {same, near}) {
      const highlight::Selection s = highlight::selection(t, body, true);
      CHECK(s.outline && s.edge == highlight::outlineFor(body));
      CHECK(theme::deltaE(s.edge, body) > 40);  // stands off the body
      CHECK(s.edgeWidth >= highlight::selection(t, body, false).edgeWidth);  // the only cue: as thick as a picked edge
    }
    CHECK(!highlight::selection(t, far, true).outline);
  }
  CHECK(highlight::outlineFor(QColor("#ffffff")) != highlight::outlineFor(QColor("#000000")));
}

TEST(picked_edges_are_thicker_than_the_hover_in_a_halo) {
  const Tokens t = theme::tokens(true);
  const highlight::Selection edge = highlight::selection(t, QColor(), false), body = highlight::selection(t, QColor(), true);
  CHECK(edge.edgeWidth >= highlight::kHoverEdgeWidth && edge.haloWidth > edge.edgeWidth);
  CHECK(edge.edgeWidth > body.edgeWidth && edge.haloWidth > body.haloWidth);  // a body's face boundaries stay thinner
  CHECK(edge.haloWidth <= 8);  // drivers clamp wider lines: a wider halo would not be drawn wider
  CHECK(edge.pointHalo > edge.point);
}

// On the light background a hovered line's white gets a rim between the background and the ink; the dark theme needs none.
TEST(hovered_lines_have_a_rim_on_the_light_background) {
  CHECK(!highlight::hoverRim(theme::tokens(true)).isValid());
  const Tokens t = theme::tokens(false);
  const QColor rim = highlight::hoverRim(t);
  CHECK(rim.isValid() && rim.hsvSaturationF() < 0.1);  // a grey, not a role colour
  CHECK(rim.lightness() < t.vp.lightness() - 40 && rim.lightness() > t.fg.lightness() + 40 && theme::deltaE(rim, t.hover) > 30);
  CHECK(highlight::kHoverRimWidth > highlight::kHoverEdgeWidth + 2 && highlight::kHoverRimWidth <= 8);  // a rim each side, within the clamp
}

CHECK_MAIN()
