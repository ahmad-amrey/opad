#pragma once
// The simplified 3D pictures of Simulate's checks (SimChecks): one per check, the approved design's artwork (the Simulate
// canvas, start screen): isometric, a light-grey translucent model with a goal-coloured accent for the air, the heat or the
// load, on a 300 x 104 grid. Each is kept as its SVG markup with the design's colours, and each colour is drawn as a theme
// token (the model's greys, fg, the goal colours, amber, red, green), so the pictures read in the light, dark and
// high-contrast themes at any size. The start page's cards and the help clips (element "simpic") draw them.
#include <QRectF>
#include <QString>

class QPainter;
struct Tokens;

namespace simpic {
constexpr double kWidth = 300, kHeight = 104;  // the design grid: draw into a rectangle of this shape
// `id`: a check's id ("bend", "fitting", ...); an unknown one draws nothing. The picture keeps its shape, centred in `r`.
void paint(QPainter& p, const QRectF& r, const QString& id, const Tokens& t);
bool has(const QString& id);
QString description(const QString& id);  // what it shows, for screen readers (the design's alt text)
}  // namespace simpic
