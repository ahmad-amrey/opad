// PDF and PNG of 2D drawings (TODO 11 UI-86, UI-87): the Qt painter over drawing::Display, as opad-cli and the app
// install it: a Qt application made on first use, pens, dashes, fills and colours where the drawing puts them, text as
// tall as its cap height and aligned as the writers align it, one vector page on the smallest ISO sheet, and the export
// command writing a view of a part as PDF and PNG.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

#include <QBuffer>
#include <QGuiApplication>
#include <QImage>

#include <algorithm>
#include <filesystem>

#include "check.hpp"
#include "drawing_sample.hpp"
#include "opad/commands.hpp"
#include "opad/drawing/paint.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / new_uuid();
  Files() { std::filesystem::create_directory(dir); }
  ~Files() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
};

// The Qt application the painter makes on first use (a test run on its own has not painted yet).
QImage painted(const Display& d, double dpi) {
  if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    Files f;
    install_painter();
    write_drawing(d, f.dir / "first.png", "png", 6, {{"dpi", 10}});
  }
  return paint_image(d, dpi);
}

// The pixel a drawing point lands on in a picture painted by paint_image.
struct Picture {
  QImage img;
  std::array<double, 4> window;
  double s = 1;
  Picture(const Display& d, double dpi) : img(painted(d, dpi)), window(page_for(d, 2, false).window) { s = img.width() / (window[2] - window[0]); }
  QPoint at(double x, double y) const { return {static_cast<int>((x - window[0]) * s), static_cast<int>((window[3] - y) * s)}; }
  QColor pixel(double x, double y) const { return img.pixelColor(at(x, y)); }
  bool dark(double x, double y) const { return qGray(img.pixel(at(x, y))) < 110; }
  // xmin, ymin, xmax, ymax in drawing units of the pixels that are not white
  std::array<double, 4> ink() const {
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y)
      for (int x = 0; x < img.width(); ++x)
        if (qGray(img.pixel(x, y)) < 128) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    return {window[0] + x0 / s, window[3] - (y1 + 1) / s, window[0] + (x1 + 1) / s, window[3] - y0 / s};
  }
};

bool red(const QColor& c) { return c.red() > 180 && c.green() < 90 && c.blue() < 90; }
bool blue(const QColor& c) { return c.blue() > 180 && c.red() < 90 && c.green() < 90; }

}  // namespace

TEST(the_painter_makes_its_own_qt_application) {
  // opad-cli has no Qt application: the first picture makes one (offscreen), as the CLI's export does.
  Files f;
  CHECK(QCoreApplication::instance() == nullptr);
  install_painter();
  CHECK(can_paint());
  const json r = write_drawing(sample(), f.dir / "sample.png", "png", 6, {{"dpi", 100}});
  CHECK(qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr);
  CHECK_EQ(r["dpi"], 100);
  const QImage img(QString::fromStdU16String((f.dir / "sample.png").u16string()));
  CHECK_EQ(img.width(), r["pixels"][0].get<int>());
  CHECK_EQ(img.height(), r["pixels"][1].get<int>());
  const Page page = page_for(sample(), 2, false);
  CHECK_NEAR(img.width(), page.w * 100 / 25.4, 1);
  const auto formats = commands::exporter_formats();
  CHECK(std::find(formats.begin(), formats.end(), "pdf") != formats.end() && std::find(formats.begin(), formats.end(), "png") != formats.end());
}

TEST(lines_dashes_fills_and_colours_land_where_the_drawing_puts_them) {
  const Picture p(sample(), 254);  // 10 pixels a millimetre
  for (double x = 5; x <= 95; x += 0.5) CHECK(p.dark(x, 0));  // a visible line, unbroken
  int dark = 0, runs = 0;
  bool was = false;
  for (double x = 5; x <= 80; x += 0.1) {  // the hidden line: 3 mm dashes, 1.5 mm gaps
    const bool d = p.dark(x, 10);
    dark += d;
    runs += d && !was;
    was = d;
  }
  CHECK(runs >= 15 && runs <= 19);
  CHECK(dark > 400 && dark < 650);
  CHECK(blue(p.pixel(201, 1)) && blue(p.pixel(225, 3)));  // fills in their layer's colour
  CHECK(qGray(p.img.pixel(p.at(205, 5))) > 240);           // the hole in the square
  Display overlap;  // two fills over one another: each even-odd on its own, so where they overlap stays filled
  const int o = overlap.layer({"Fills"});
  overlap.fill(o, {{{0, 0}, {10, 0}, {10, 10}, {0, 10}}});
  overlap.fill(o, {{{5, 5}, {15, 5}, {15, 15}, {5, 15}}});
  const Picture q(overlap, 100);
  CHECK(q.dark(7.5, 7.5) && q.dark(2, 2) && q.dark(13, 13) && !q.dark(13, 2));
  int reds = 0;
  for (double a = 0; a < 2 * M_PI; a += 0.01) reds += red(p.pixel(50 + 10 * std::cos(a), 25 + 10 * std::sin(a)));
  CHECK(reds > 300 && reds < 620);  // a red centre line: long dashes, short ones, gaps
  // Text: "OPAD 1/3" from (0, -10) up to its cap height.
  bool text = false;
  for (double x = 0.2; x < 15; x += 0.1) text = text || p.dark(x, -8);
  CHECK(text);
}

TEST(text_is_as_tall_as_its_cap_height_and_aligned_as_the_writers_align_it) {
  Display d;
  const int l = d.layer({"Text"});
  d.text(l, "HEH", {0, 0}, 10, 0, 1, 0);  // centred on its baseline
  auto ink = Picture(d, 254).ink();
  CHECK_NEAR(ink[1], 0, 0.15);
  CHECK_NEAR(ink[3], 10, 0.3);
  CHECK_NEAR((ink[0] + ink[2]) / 2, 0, 0.3);
  d.prims.clear();
  d.text(l, "HEH", {0, 0}, 10, M_PI / 2, 0, 3);  // up the page, its top (on its left) at the anchor
  ink = Picture(d, 254).ink();
  CHECK_NEAR(ink[0], 0, 0.3);
  CHECK_NEAR(ink[2], 10, 0.3);
  CHECK(ink[1] > -0.2 && ink[3] > 15);
  d.prims.clear();
  d.text(l, "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A", {0, 0}, 10, 0, 2, 2);  // "عربي", right-aligned, its middle at the anchor
  ink = Picture(d, 254).ink();
  CHECK(ink[2] < 0.5 && ink[2] > -1.5);
  CHECK(ink[0] < -10);
  CHECK(ink[3] > 0 && ink[1] < -6);  // joined letters on a baseline 5 below the anchor, the last one reaching under it
}

TEST(images_are_painted_on_their_corners) {
  QImage red2(2, 2, QImage::Format_RGB32);
  red2.fill(Qt::red);
  QByteArray png;
  QBuffer buf(&png);
  buf.open(QIODevice::WriteOnly);
  red2.save(&buf, "PNG");
  Display d;
  Prim image;
  image.kind = Prim::Kind::Image;
  image.layer = d.layer({"Images"});
  image.text = "data:image/png;base64," + png.toBase64().toStdString();
  image.corners = {Vec2{0, 10}, Vec2{20, 10}, Vec2{0, 0}};  // 20 x 10, the picture square: centred, 10 x 10
  image.fit = "none";
  d.prims.push_back(image);
  Picture p(d, 100);
  CHECK(red(p.pixel(2, 5)) && red(p.pixel(18, 5)));
  d.prims[0].fit = "xMidYMid meet";
  p = Picture(d, 100);
  CHECK(red(p.pixel(10, 5)) && !red(p.pixel(2, 5)) && !red(p.pixel(18, 5)));
}

TEST(a_pdf_is_one_vector_page_on_the_smallest_iso_sheet_that_holds_it) {
  install_painter();
  Files f;
  const json r = write_drawing(sample(), f.dir / "sample.pdf", "pdf");
  CHECK_EQ(r["paper"], "A4");
  CHECK_NEAR(r["page"][0].get<double>(), 297, 1e-9);
  CHECK_NEAR(r["page"][1].get<double>(), 210, 1e-9);
  CHECK_EQ(r["scale"], "1:1");
  const std::string pdf = read_text_file(f.dir / "sample.pdf");
  CHECK(pdf.rfind("%PDF-", 0) == 0);
  size_t pages = 0;
  for (size_t at = pdf.find("/Type /Page"); at != std::string::npos; at = pdf.find("/Type /Page", at + 1)) pages += pdf.compare(at, 12, "/Type /Pages") != 0;
  CHECK_EQ(pages, 1u);
  CHECK(pdf.find("/MediaBox [0 0 842") != std::string::npos);  // A4 landscape in points
  CHECK(pdf.find("/FontFile2") != std::string::npos);            // its text with the font embedded
  CHECK(pdf.find("/Subtype /Image") == std::string::npos);       // lines, not a picture of them
  // Larger drawings: a bigger sheet at the drawing's scale, or their own size past A0.
  Display big = sample();
  big.pen_scale = 5;
  big.line(0, {0, 0}, {1500, 0});
  Page page = page_for(big);
  CHECK_EQ(page.paper, "A3");
  CHECK_NEAR(page.window[2] - page.window[0], 420 * 5, 1e-6);
  CHECK_EQ(write_drawing(big, f.dir / "big.pdf", "pdf")["scale"], "1:5");
  big.pen_scale = 1;
  page = page_for(big);
  CHECK(page.paper.empty());
  CHECK_NEAR(page.w, big.bounds()[2] - big.bounds()[0] + 20, 1e-6);
  std::filesystem::create_directory(f.dir / "taken.pdf");  // a folder of that name: the file cannot be written
  CHECK_THROWS(write_drawing(sample(), f.dir / "taken.pdf", "pdf"));
}

TEST(export_writes_a_view_of_a_part_as_pdf_and_png) {
  install_painter();
  Files f;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40, 30, 20).Shape();
  const TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1)), 4, 22).Shape();
  auto doc = Document::create();
  import_brep(doc, brep_from_shape(BRepAlgoAPI_Cut(box, hole).Shape()), "Block");
  const auto pdf = f.dir / "front.pdf";
  json r = commands::run("export", {{"format", "pdf"}, {"out", pdf.string()}, {"view", "front"}, {"hidden", true}}, &doc);
  CHECK_EQ(r["layers"]["Visible"], 4);
  CHECK_EQ(r["layers"]["Hidden"], 2);
  CHECK_EQ(r["paper"], "A4");
  CHECK(read_text_file(pdf).rfind("%PDF-", 0) == 0);
  const auto png = f.dir / "front.png";
  r = commands::run("export", {{"format", "png"}, {"out", png.string()}, {"view", "front"}, {"hidden", true}, {"dpi", 254}}, &doc);
  CHECK_EQ(r["pixels"][0], 440);  // 40 x 20 mm and 2 mm of paper around it, 10 pixels a millimetre
  CHECK_EQ(r["pixels"][1], 240);
  const QImage img(QString::fromStdU16String(png.u16string()));
  CHECK(qGray(img.pixel(20, 120)) < 110);   // the left side at x = 2 mm
  CHECK(qGray(img.pixel(220, 120)) > 240);  // inside the block, between the hole's hidden sides
  int hidden = 0;
  for (int y = 25; y < 215; ++y) hidden += qGray(img.pixel(180, y)) < 110;  // the hole's left side, x = 2 + 16 mm
  CHECK(hidden > 80 && hidden < 170);  // dashed
  // Drawings as drawn: an imported DXF's red quarter circle about (5, 5), radius 20, in its layer's red.
  write_text_file(f.dir / "in.dxf", "0\nSECTION\n2\nENTITIES\n0\nARC\n8\nArcs\n62\n1\n10\n5\n20\n5\n40\n20\n50\n0\n51\n90\n0\nENDSEC\n0\nEOF\n");
  auto drawing = Document::create();
  import_file(drawing, f.dir / "in.dxf");
  r = commands::run("export", {{"format", "png"}, {"out", (f.dir / "drawing.png").string()}, {"dpi", 254}}, &drawing);
  CHECK_EQ(r["entities"]["ARC"], 1);
  const QImage arc(QString::fromStdU16String((f.dir / "drawing.png").u16string()));
  CHECK_EQ(arc.width(), 240);
  CHECK(red(arc.pixelColor(161, 79)));  // at 45 degrees: (19.14, 19.14), 2 mm of paper around the arc's box
  CHECK(qGray(arc.pixel(60, 180)) > 240);
  r = commands::run("export", {{"format", "pdf"}, {"out", (f.dir / "drawing.pdf").string()}}, &drawing);
  CHECK_EQ(r["paper"], "A4");
}

TEST(a_sheet_prints_on_its_own_paper) {
  install_painter();
  Files f;
  auto doc = Document::create();
  commands::run("feature", {{"kind", "box"}, {"inputs", {{"length", "40 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}}, &doc);
  const std::string custom = commands::run("sheet", {{"width", 200}, {"height", 100}, {"name", "Strip"}}, &doc)["id"];
  commands::run("sheet_view", {{"sheet", custom}, {"orient", "top"}, {"at", {100, 50}}}, &doc);
  json r = commands::run("export", {{"format", "png"}, {"sheet", custom}, {"dpi", 254}, {"out", (f.dir / "strip.png").string()}}, &doc);
  CHECK_EQ(r["pixels"][0], 2000);  // the paper, no margin around it
  CHECK_EQ(r["pixels"][1], 1000);
  const QImage img(QString::fromStdU16String((f.dir / "strip.png").u16string()));
  CHECK(qGray(img.pixel(200, 300)) < 110 && qGray(img.pixel(1900, 300)) < 110);  // the frame 20 mm from the left, 10 from the right
  CHECK(qGray(img.pixel(800, 400)) < 110 && qGray(img.pixel(1000, 500)) > 240);  // the plate's outline at x = 80 mm; inside it
  CHECK(qGray(img.pixel(100, 300)) > 240);                                        // the filing margin
  r = commands::run("export", {{"format", "pdf"}, {"sheet", custom}, {"out", (f.dir / "strip.pdf").string()}}, &doc);
  CHECK(r["paper"] == "" && r["page"] == json::array({200.0, 100.0}));
  const std::string a2 = commands::run("sheet", {{"size", "A2"}, {"orientation", "portrait"}}, &doc)["id"];
  r = commands::run("export", {{"format", "pdf"}, {"sheet", a2}, {"out", (f.dir / "a2.pdf").string()}}, &doc);
  CHECK(r["paper"] == "A2" && r["page"] == json::array({420.0, 594.0}) && r["sheet"]["views"] == 0);
  CHECK(read_text_file(f.dir / "a2.pdf").find("/MediaBox [0 0 1191") != std::string::npos);
}

int main(int argc, char** argv) { return check::run_all(argc, argv); }
