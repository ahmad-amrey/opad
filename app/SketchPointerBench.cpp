#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Jobs.hpp"
#include "Units.hpp"
#include "I18n.hpp"
#include "opad/design/sketch_edit.hpp"
#include "opad/design/sketch_geom.hpp"
#include <QApplication>
#include <QImage>
#include <QLineEdit>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <gp_Trsf.hxx>
#include <cmath>

using namespace opad::design;

namespace {
// An arc's counter-clockwise sweep from its start to its end, and the direction of its middle (radians).
std::pair<double, double> arcSpan(const Sketch& sk, const SkEntity& e) {
  const SkPoint *o = sk.point(e.p[0]), *a = sk.point(e.p[1]), *b = sk.point(e.p[2]);
  const double from = std::atan2(a->y - o->y, a->x - o->x), to = std::atan2(b->y - o->y, b->x - o->x);
  const double sweep = std::fmod(to - from + 4 * M_PI, 2 * M_PI);
  return {sweep, from + sweep / 2};
}
double degrees(double radians) { return radians * 180 / M_PI; }
// A direction's difference from `want` degrees, wrapped.
double off(double radians, double want) { return std::abs(std::remainder(degrees(radians) - want, 360.0)); }
// A cubic's first end: where, which way and how much it bends (its poles p0 p1 p2).
struct End { double x, y, fx, fy, k; };
End bezierStart(const Sketch& sk, const SkEntity& e) {
  const SkPoint p = *sk.point(e.p[0]), q = *sk.point(e.p[1]), r = *sk.point(e.p[2]);
  const double fx = 3 * (q.x - p.x), fy = 3 * (q.y - p.y), sx = 6 * (r.x - 2 * q.x + p.x), sy = 6 * (r.y - 2 * q.y + p.y);
  return {p.x, p.y, fx, fy, (fx * sy - fy * sx) / std::pow(std::hypot(fx, fy), 3)};
}
}  // namespace

// OPAD_BENCH_SKETCH_POINTER=<prefix> (TODO 11 wave 3, P5 and P6): the sketch tools whose guides show the pointer driving the
// preview, and results that are what their names say. Tangent circle: with both lines picked the circle is previewed on the
// pointer's side and moves to the other side with it, the click makes that one. Arc slot: the slot runs the way the pointer
// went round its centre, clockwise over the top (the guide's -120 degrees, read out signed) or counter-clockwise past half a
// turn. Circumscribed polygon: its circle is inside, touching every side, as big as the pointer's distance. Smooth (G2) joins a
// line to a spline and Curvature an arc to a spline, picked as the guides pick them. The Constraints page, opened with the
// panel's page switch, lights a picked row's geometry. Open ends: an end dragged onto the other is merged, the profile closes.
// Transform image: a press on the picture and a drag move it, the picture following; the release keeps the place.
// <prefix>.tangent.png, .arcslot.png, .polygon.png, .constraints.png (the panel), .image.png.
void SketchEditor::benchPointer() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_POINTER");
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch pointer: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  auto* panel = m_viewport->window()->findChild<SketchPanel*>();
  if (!panel) {
    check(false, "the sketch panel exists");
    return QCoreApplication::exit(2);
  }
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {40, -10, 100}}, {"target", {40, -10, 0}}, {"up", {0, 1, 0}}, {"scale", 200}, {"projection", "orthographic"}, {"absolute", true}});
  const Tokens& t = m_viewport->tokens();
  auto bezier = [this](std::initializer_list<std::pair<double, double>> poles) {
    SkEntity e;
    e.type = SkEntity::Type::Spline;
    e.degree = 3;
    e.knots = {0, 1};
    e.multiplicities = {4, 4};
    for (auto [x, y] : poles) {
      e.p.push_back(m_sk.add_point(x, y));
      e.weights.push_back(1);
    }
    e.id = m_sk.next_id();
    m_sk.entities.push_back(e);
    return e.id;
  };
  // The drawing: two crossing lines (at (-45, -20)) for the tangent circle; a line and a spline beside its end; an arc and a
  // spline beside its start; a rectangle whose last side stops short of its first corner.
  begin_change();
  const int l1 = m_sk.add_line(m_sk.add_point(-60, -20), m_sk.add_point(-10, -20));
  const int l2 = m_sk.add_line(m_sk.add_point(-55, -32), m_sk.add_point(-30, -2));
  const int straight = m_sk.add_line(m_sk.add_point(40, -20), m_sk.add_point(60, -20));
  const int spline1 = bezier({{60.4, -19.4}, {66, -6}, {78, -6}, {84, -19}});
  const int arc = m_sk.add_arc(m_sk.add_point(130, -10), m_sk.add_point(130, -20), m_sk.add_point(140, -10));
  const int spline2 = bezier({{129.6, -19.7}, {122, -26}, {110, -26}, {104, -18}});
  const int first = m_sk.add_point(0, 20), loose = m_sk.add_point(3, 22);
  const int c1 = m_sk.add_point(20, 20), c2 = m_sk.add_point(20, 30), c3 = m_sk.add_point(0, 30);
  m_sk.add_line(first, c1), m_sk.add_line(c1, c2), m_sk.add_line(c2, c3);
  const int last = m_sk.add_line(c3, loose);
  end_change(QStringLiteral("Bench"));
  rebuild();
  auto place = [this](double u, double v) {  // a click, Alt: exactly there
    sketchMove(u, v, Qt::AltModifier, false);
    sketchPress(u, v, Qt::AltModifier);
    sketchRelease(u, v, Qt::AltModifier);
  };
  auto field = [this](const char* key) {  // the live text of the step's box
    for (const auto& f : inputStage())
      if (f.key == key) return f.live;
    return QString();
  };
  const QString picture = prefix + ".picture.png";
  {
    QImage image(64, 32, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 8; y < 24; ++y)
      for (int x = 8; x < 56; ++x) image.setPixelColor(x, y, Qt::darkGray);
    image.save(picture);
  }
  auto state = std::make_shared<std::map<std::string, double>>();
  auto phase = std::make_shared<int>(0), ticks = std::make_shared<int>(0);
  auto* timer = new QTimer(this);
  timer->setInterval(60);
  connect(timer, &QTimer::timeout, this, [=, this] {
    if (++*ticks > 1200) {
      check(false, QString("phase %1 in time").arg(*phase));
      timer->stop();
      return QCoreApplication::exit(2);
    }
    if (m_editJob || m_geometryJob || m_imageJob || m_toolPreviewTimer.isActive()) return;
    auto& st = *state;
    if (st.count("until") && *ticks < st["until"]) return;
    switch ((*phase)++) {
      // ---- Tangent circle: the pointer chooses the side before the click.
      case 0: {
        setTool("tangent_circle");
        m_options["radius"] = "3 mm";
        sketchMove(-35, -14, Qt::AltModifier, false);
        check(primitivePreview().entities.empty(), "tangent circle: nothing previewed before the lines are picked");
        place(-40, -20);
        place(-30 - 25 * 0.2, -2 - 30 * 0.2);  // on the second line
        check(m_picked == std::vector<int>{l1, l2}, "tangent circle: both lines picked");
        auto centreAt = [this](double u, double v, double& x, double& y) {
          sketchMove(u, v, Qt::AltModifier, false);
          const Sketch shown = primitivePreview();
          if (shown.entities.size() != 1 || shown.entities[0].type != SkEntity::Type::Circle) return false;
          const SkPoint* c = shown.point(shown.entities[0].p[0]);
          x = c->x, y = c->y;
          return std::abs(shown.entities[0].r - 3) < 1e-9;
        };
        // Sides: above the first line (y > -20) or below; left or right of the second (cross of its direction).
        auto side = [](double x, double y) { return std::make_pair(y > -20, (25 * (y + 32) - 30 * (x + 55)) > 0); };
        double ax = 0, ay = 0, bx = 0, by = 0;
        const bool a = centreAt(-35, -14, ax, ay), b = centreAt(-55, -26, bx, by);
        auto tangent = [](double x, double y) {
          const double d1 = std::abs(y + 20), d2 = std::abs(25 * (y + 32) - 30 * (x + 55)) / std::hypot(25.0, 30.0);
          return std::abs(d1 - 3) < 1e-6 && std::abs(d2 - 3) < 1e-6;
        };
        check(a && b && tangent(ax, ay) && tangent(bx, by) && side(ax, ay) == side(-35, -14) && side(bx, by) == side(-55, -26) && side(ax, ay) != side(bx, by),
              QString("tangent circle: previewed tangent to both lines on the pointer's side, (%1, %2) then (%3, %4)").arg(ax).arg(ay).arg(bx).arg(by));
        check(transientSolid(t.hov) >= 8, "tangent circle: the preview is drawn");
        m_viewport->grabImage().save(prefix + ".tangent.png");
        const size_t before = m_sk.entities.size();
        place(-55, -26);
        const SkEntity* made = m_sk.entities.size() == before + 1 ? &m_sk.entities.back() : nullptr;
        const SkPoint* centre = made ? m_sk.point(made->p[0]) : nullptr;
        check(made && made->type == SkEntity::Type::Circle && centre && std::hypot(centre->x - bx, centre->y - by) < 1e-6 && m_solved.converged,
              "tangent circle: the click makes the circle the preview showed");
        // ---- Arc slot: clockwise over the top, as the guide sweeps it.
        setTool("arcslot");
        m_options["width"] = "4 mm";
        const double r = 14;
        auto at = [r](double cx, double cy, double angle) { return std::make_pair(cx + r * std::cos(angle * M_PI / 180), cy + r * std::sin(angle * M_PI / 180)); };
        place(0, -60);
        const auto s = at(0, -60, 150);
        place(s.first, s.second);
        for (double angle : {135.0, 110.0, 90.0, 70.0, 50.0, 30.0}) {
          const auto p = at(0, -60, angle);
          sketchMove(p.first, p.second, Qt::AltModifier, false);
        }
        const Sketch shown = primitivePreview();
        const auto span = shown.entities.empty() ? std::make_pair(0.0, 0.0) : arcSpan(shown, shown.entities[0]);
        check(off(slotSweep(m_cursor.u, m_cursor.v), -120) < 1e-6 && field("sweep").startsWith("-120"),
              "arc slot: going round clockwise the sweep is -120 degrees, read out signed: " + field("sweep"));
        check(!shown.entities.empty() && off(span.first, 120) < 1e-6 && off(span.second, 90) < 1e-6, "arc slot: the preview runs over the top");
        m_viewport->grabImage().save(prefix + ".arcslot.png");
        const size_t slot = m_sk.entities.size();
        const auto e = at(0, -60, 30);
        place(e.first, e.second);
        const auto made1 = m_sk.entities.size() > slot ? arcSpan(m_sk, m_sk.entities[slot]) : std::make_pair(0.0, 0.0);
        check(m_sk.entities.size() == slot + 4 && off(made1.first, 120) < 1e-6 && off(made1.second, 90) < 1e-6 && m_solved.converged,
              "arc slot: the click makes it over the top, 120 degrees from 150 down to 30");
        // Counter-clockwise past half a turn: from 150 round under the centre to 30.
        place(40, -60);
        const auto s2 = at(40, -60, 150);
        place(s2.first, s2.second);
        for (double angle = 170; angle <= 390; angle += 20) {
          const auto p = at(40, -60, angle);
          sketchMove(p.first, p.second, Qt::AltModifier, false);
        }
        check(off(slotSweep(m_cursor.u, m_cursor.v), 240) < 1e-6, "arc slot: round counter-clockwise past half a turn the sweep is 240 degrees: " + field("sweep"));
        const size_t slot2 = m_sk.entities.size();
        const auto e2 = at(40, -60, 30);
        place(e2.first, e2.second);
        const auto made2 = m_sk.entities.size() > slot2 ? arcSpan(m_sk, m_sk.entities[slot2]) : std::make_pair(0.0, 0.0);
        check(m_sk.entities.size() == slot2 + 4 && off(made2.first, 240) < 1e-6 && off(made2.second, 270) < 1e-6, "arc slot: made under the centre, 240 degrees");
        // ---- Circumscribed polygon: the circle inside, touching the sides.
        setTool("polygon_outer");
        m_options["sides"] = "6";
        place(80, -60);
        sketchMove(92, -60, Qt::AltModifier, false);
        const Sketch polygon = primitivePreview();
        bool inside = false;
        for (const auto& k : polygon.entities) inside = inside || (k.type == SkEntity::Type::Circle && k.construction && std::abs(k.r - 12) < 1e-9);
        check(inside, "circumscribed polygon: the preview's circle reaches the pointer, the middle of a side");
        const size_t sides = m_sk.entities.size();
        place(92, -60);
        int circle = 0, touching = 0;
        for (size_t i = sides; i < m_sk.entities.size(); ++i)
          if (m_sk.entities[i].type == SkEntity::Type::Circle) circle = m_sk.entities[i].id;
        bool apart = circle != 0;
        for (const auto& c : m_sk.constraints) touching += c.type == SkConstraint::Type::Tangent && c.refs.size() == 2 && c.refs[1] == circle;
        for (size_t i = sides; circle && i < m_sk.entities.size(); ++i) {
          const SkEntity& k = m_sk.entities[i];
          if (k.type != SkEntity::Type::Line || k.construction) continue;
          const SkPoint *a = m_sk.point(k.p[0]), *b = m_sk.point(k.p[1]);
          const double d = std::abs((b->x - a->x) * (-60 - a->y) - (b->y - a->y) * (80 - a->x)) / std::hypot(b->x - a->x, b->y - a->y);
          apart = apart && std::abs(d - 12) < 1e-6;
        }
        check(circle && std::abs(m_sk.entity(circle)->r - 12) < 1e-9 && touching == 6 && apart && m_solved.converged,
              "circumscribed polygon: its circle 24 across touches all six sides");
        m_viewport->grabImage().save(prefix + ".polygon.png");
        // ---- Smooth: the line, then the spline, as the guide picks them.
        setTool("c:smooth");
        check(!toolSteps().isEmpty() && toolSteps().front().label == i18n::t("Pick a line, circle, arc or spline"), "smooth: its first step asks for a line, circle, arc or spline");
        place(50, -20);
        place((60.4 + 3 * 66 + 3 * 78 + 84) / 8, (-19.4 + 3 * -6 + 3 * -6 - 19) / 8);  // its middle, clear of its poles
        int smooth = 0;
        for (const auto& c : m_sk.constraints)
          if (c.type == SkConstraint::Type::Smooth) smooth = c.id;
        const End j = bezierStart(m_sk, *m_sk.entity(spline1));
        const SkPoint *la = m_sk.point(m_sk.entity(straight)->p[0]), *lb = m_sk.point(m_sk.entity(straight)->p[1]);
        const double lx = lb->x - la->x, ly = lb->y - la->y, ll = std::hypot(lx, ly);
        check(smooth && m_sk.constraint(smooth)->refs == std::vector<int>{straight, spline1} && m_solved.converged && std::abs((lx * (j.y - la->y) - ly * (j.x - la->x)) / ll) < 1e-6 &&
                  std::abs(lx * j.fy - ly * j.fx) / (ll * std::hypot(j.fx, j.fy)) < 1e-6 && std::abs(j.k) < 1e-6,
              "smooth: a line and a spline join G2 (the spline's end on the line, along it, straight)");
        st["smooth"] = smooth;
        // ---- Curvature: the arc, then the spline.
        setTool("c:curvature");
        place(130 + 10 * std::cos(-M_PI / 4), -10 + 10 * std::sin(-M_PI / 4));
        place((129.6 + 3 * 122 + 3 * 110 + 104) / 8, (-19.7 + 3 * -26 + 3 * -26 - 18) / 8);
        int curvature = 0;
        for (const auto& c : m_sk.constraints)
          if (c.type == SkConstraint::Type::Curvature) curvature = c.id;
        const End k = bezierStart(m_sk, *m_sk.entity(spline2));
        const SkEntity* round = m_sk.entity(arc);
        const double radius = std::hypot(m_sk.point(round->p[1])->x - m_sk.point(round->p[0])->x, m_sk.point(round->p[1])->y - m_sk.point(round->p[0])->y);
        check(curvature && m_sk.constraint(curvature)->refs == std::vector<int>{arc, spline2} && m_solved.converged && std::abs(std::abs(k.k) - 1 / radius) < 1e-6,
              QString("curvature: an arc and a spline, the spline's end bending as the arc (1/%1)").arg(radius));
        // ---- The panel's page switch and the Constraints page: a picked row lights its geometry.
        setTool("select");
        m_sel.clear();
        rebuild();
        st["lit"] = double(sketchSolid(t.hov));
        QToolButton* constraints = nullptr;
        int switches = 0;
        for (auto* b : panel->findChildren<QToolButton*>())
          if (b->property("sketchPage").isValid()) {
            ++switches;
            if (b->property("sketchPage").toInt() == 2) constraints = b;
          }
        check(switches == 4 && constraints && constraints->isVisible() == panel->isVisible(), "the panel shows its four pages' switch");
        if (constraints) constraints->click();
        auto* pages = panel->findChild<QTabWidget*>();
        check(pages && pages->currentIndex() == 2 && constraints && constraints->isChecked(), "a click on Constraints opens that page");
        break;
      }
      case 1: {
        QTreeWidget* tree = nullptr;  // the Constraints page's list: its rows hold the constraints' ids
        QTreeWidgetItem* row = nullptr;
        for (auto* list : panel->findChildren<QTreeWidget*>())
          for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (list->topLevelItem(i)->data(0, Qt::UserRole).toInt() == int(st["smooth"])) tree = list, row = list->topLevelItem(i);
        if (row) tree->setCurrentItem(row);
        check(row && m_sel == std::vector<int>{int(st["smooth"])} && double(sketchSolid(t.hov)) > st["lit"] + 10,
              QString("a row picked in the list lights what it holds (%1 lit segments, %2 before)").arg(sketchSolid(t.hov)).arg(st["lit"]));
        panel->grab().save(prefix + ".constraints.png");
        for (auto* b : panel->findChildren<QToolButton*>())
          if (b->property("sketchPage").toInt() == 0 && b->property("sketchPage").isValid()) b->click();
        auto* pages = panel->findChild<QTabWidget*>();
        check(pages && pages->currentIndex() == 0, "the switch's Tool goes back to the tool page");
        m_sel.clear();
        // ---- Open ends: ringed, then one dragged onto the other.
        findOpenVertices();
        st["until"] = *ticks + 10;
        break;
      }
      case 2: {
        check(m_dangling.count(first) && m_dangling.count(loose), "open ends: the gap's two ends are ringed");
        const size_t points = m_sk.points.size();
        auto open = [this](int id) { const auto ends = dangling_vertices(m_sk); return std::find(ends.begin(), ends.end(), id) != ends.end(); };
        sketchMove(3, 22, Qt::NoModifier, false);
        sketchPress(3, 22, Qt::NoModifier);
        sketchMove(1.6, 21, Qt::NoModifier, true);
        sketchMove(0.2, 20.1, Qt::NoModifier, true);
        check(m_dropPoint == first, "open ends: the dragged end is held to the other");
        sketchRelease(0.2, 20.1, Qt::NoModifier);
        bool closed = false;
        for (const auto& region : sketch_regions(m_sk, opad::Frame{})) closed = closed || std::abs(region.area - 200) < 1e-6;
        check(!m_sk.point(loose) && m_sk.entity(last)->p[1] == first && m_sk.points.size() == points - 1 && !open(first) && closed,
              "open ends: dropped there the ends are one point, nothing is open and the profile closes");
        // ---- Transform image: a picture to drag.
        setTool("image_insert");
        m_options["imageFile"] = picture;
        m_options["imageWidth"] = "40 mm";
        place(-60, 40);
        applyTool();
        break;
      }
      case 3:
        check(m_sk.images.size() == 1, "transform image: a picture inserted");
        setTool("image_edit");
        break;
      case 4: {
        if (m_imagePrs.size() != 1) {  // its texture still being prepared
          --*phase;
          return;
        }
        const opad::json& image = m_sk.images.front();
        st["steps"] = double(m_undo.size());
        sketchMove(-70, 35, Qt::NoModifier, false);
        sketchPress(-70, 35, Qt::NoModifier);
        check(!m_imageDrag, "transform image: a press off the picture takes nothing");
        sketchRelease(-70, 35, Qt::NoModifier);
        sketchMove(-50, 45, Qt::NoModifier, false);
        check(transientDashed(t.hov) == 4, "transform image: the picture under the pointer shows its frame");
        sketchPress(-50, 45, Qt::NoModifier);
        sketchMove(-47, 47, Qt::NoModifier, true);
        sketchMove(-45, 48, Qt::NoModifier, true);
        const gp_XYZ moved = m_imagePrs.front()->LocalTransformation().TranslationPart();
        const opad::Vec3 a = m_frame.to_world(0, 0), b = m_frame.to_world(5, 3);
        check(m_imageDrag && m_imageDrag->moved && std::abs(moved.X() - (b[0] - a[0])) < 1e-9 && std::abs(moved.Y() - (b[1] - a[1])) < 1e-9 && std::abs(moved.Z() - (b[2] - a[2])) < 1e-9 &&
                  transientDashed(t.hov) == 4 && std::abs(image.at("position")[0].get<double>() + 60) < 1e-9,
              "transform image: dragged, the picture follows the pointer with its frame, the sketch unchanged yet");
        m_viewport->grabImage().save(prefix + ".image.png");
        sketchRelease(-40, 50, Qt::NoModifier);
        break;
      }
      case 5: {
        const opad::json& image = m_sk.images.front();
        auto* x = panel->findChild<QLineEdit*>("sketchOption-imageX");
        check(std::abs(image.at("position")[0].get<double>() + 50) < 1e-9 && std::abs(image.at("position")[1].get<double>() - 45) < 1e-9 &&
                  std::abs(image.at("width").get<double>() - 40) < 1e-9 && m_undo.size() == size_t(st["steps"]) + 1 && x && x->text() == units::editable(units::Kind::Length, -50),
              "transform image: the release keeps it 10 right and 5 up (one undo step, as wide), the panel's X shows it");
        setTool("select");
        break;
      }
      default:
        timer->stop();
        trace::log(QString("bench: sketch pointer %1").arg(*ok ? "PASS" : "FAIL"));
        QCoreApplication::exit(*ok ? 0 : 2);
    }
  });
  timer->start();
}
