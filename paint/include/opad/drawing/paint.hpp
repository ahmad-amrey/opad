#pragma once
// Qt painting of 2D drawings (TODO 11 UI-86, UI-87): one QPainter backend over drawing::Display for PDF and PNG now,
// print and the sheet canvas later, so they all come out as the DXF and SVG writers draw the same list. Pens are paper
// millimetres times the display's pen scale with the layer's dashes, curves exact (arcs, ellipses and splines as cubic
// Béziers within a micrometre of paper), fills even-odd, text shaped by Qt (Arabic joins and runs right to left) with
// its cap height as the drawing's text height. Workers may paint: QPdfWriter and QImage need no GUI thread.
#include <QImage>
#include <QPainter>
#include <QRectF>

#include "opad/drawing/display.hpp"

namespace opad::drawing {

// Paints `d` with `window` (xmin, ymin, xmax, ymax in drawing units) fitted into `target` (device units), y up, one
// scale for both axes, centred. min_px > 0: no line thinner than that many device pixels (a sheet seen from afar on screen).
void paint(QPainter& p, const Display& d, const std::array<double, 4>& window, const QRectF& target, double min_px = 0);

// The paper a drawing is printed on, in mm: its own (Display::paper) when it has one, else the smallest ISO sheet (A4 to
// A0, landscape when wider than tall) that holds it at 1 : pen_scale with `margin` around it, else the drawing and its
// margins. window: the drawing units shown.
struct Page {
  double w = 0, h = 0;
  std::string paper;  // A4..A0 or ANSI-A..E, empty when not a standard size
  std::array<double, 4> window{0, 0, 0, 0};
};
Page page_for(const Display& d, double margin = 10, bool standard = true);

// PDF: a vector page each as page_for chooses (titled as the first, creator OPAD <version>), fonts embedded; returns
// {"page": [w, h] mm, "paper", "scale": "1:5"}, for several pages {"pages": [those]}. PNG: its paper, else the drawing
// with 2 mm of paper around it, on white at `dpi` (lowered so that no side passes 16384 pixels nor the picture 50
// million); returns {"pixels": [w, h], "dpi"}. Throw Error when the file cannot be written.
json write_pdf(const std::vector<const Display*>& pages, const std::filesystem::path& file);
QImage paint_image(const Display& d, double dpi);
json write_png(const Display& d, const std::filesystem::path& file, double dpi = 300);

// Makes write_drawing and the export command write "pdf" and "png" (drawing::set_paint_writer). A process without a Qt
// application (opad-cli) gets a QGuiApplication on the offscreen platform the first time it paints, on that thread.
void install_painter();

}  // namespace opad::drawing
