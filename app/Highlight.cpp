#include "Highlight.hpp"

namespace highlight {

bool closeToSelection(const Tokens& t, const QColor& body) { return body.isValid() && theme::deltaE(body, t.selected3d) < kCloseDeltaE; }

QColor outlineFor(const QColor& body) {
  const double luminance = 0.2126 * body.redF() + 0.7152 * body.greenF() + 0.0722 * body.blueF();
  return luminance > 0.45 ? QColor("#0d1117") : QColor("#ffffff");
}

namespace {
QColor mix(const QColor& a, const QColor& b, double t) {
  return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}
}  // namespace

QColor hoverHalo(const Tokens& t) { return t.dark ? mix(t.hover, t.vp, 0.4) : t.hover; }

QColor hoverRim(const Tokens& t) { return t.dark ? QColor() : mix(t.fg, t.vp, 0.6); }

Selection selection(const Tokens& t, const QColor& body, bool wholeBody) {
  Selection s;
  s.fill = s.edge = t.selected3d;
  s.halo = mix(t.selected3d, t.vp, 0.45);
  if (wholeBody) {
    s.edgeWidth = 2;
    s.haloWidth = 5;
  }
  s.outline = closeToSelection(t, body);
  if (s.outline) {  // the only cue on such a body: as thick as a picked edge
    s.edge = outlineFor(body);
    s.edgeWidth = 3;
  }
  return s;
}

}  // namespace highlight
