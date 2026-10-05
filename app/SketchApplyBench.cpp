#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Jobs.hpp"
#include "Units.hpp"
#include "opad/geometry.hpp"
#include "opad/design/sketch_edit.hpp"
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_APPLY=<prefix> (TODO 11 wave 3, P4): the sketch tools with an Apply button do what their guides show.
// Mirror: a click picks one curve and the tool stays on the curves (the preview after the first curve used to move it on
// to the line, so the second click became the line); a window picks the rest, Enter goes on to the line, a click on it
// previews the mirror image and Enter keeps it. Move: a click picks one curve, with the tool's own "Select connected chain
// on click" the whole outline (and a click again drops it; the option is the tool's, rotate and offset keep theirs), the
// preview follows and Enter moves it. Project (on the box): a click in the view adds a source and previews it, a source
// added in the panel previews both, the panel lists them, Backspace takes the last back, Enter adds them linked. Break
// link: a click picks a linked curve (an unlinked one is refused), a window picks the linked ones, Enter unlinks them.
// Project again: the view picks edges again; an edge picked (highlighted under the sketch, not X-ray), then Include: the pick
// and its highlight go. Include: an origin axis as a construction curve on Enter. A second calibration shows its own measured
// distance, and Enter with it unchanged scales nothing. Insert image: the frame at the width set follows the pointer,
// stays at the click, Enter places the picture there. Calibrate: the measure follows the pointer, the Known distance box
// takes the two points' distance, typing the real one and Enter scales the picture. Transform image: the panel shows the
// picture's place, an angle and an opacity typed there and Enter in that field turn and fade it in place. Node weights: a click
// loads the node's own weight, the spline's new shape is previewed while the weight typed in the panel changes, Enter in the
// field keeps it. Silhouette: the box's outline previewed, Enter adds it. Intersect
// with plane: a click on the post standing through the plane (the case's document: the box and Cylinder1) previews the
// circle where it crosses, Enter adds it linked. <prefix>.mirror.png, <prefix>.project.png (and the panel's list,
// <prefix>.panel.png), <prefix>.image.png, <prefix>.calibrate.png, <prefix>.intersect.png.
void SketchEditor::benchApply() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_APPLY");
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch apply: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  auto* panel = m_viewport->window()->findChild<SketchPanel*>();
  std::string box, post;  // Box1 on the sketch plane, Cylinder1 standing through it
  for (const std::string& id : m_doc->scene.all_bodies()) (m_doc->nodeName(id).startsWith("Cylinder") ? post : box) = id;
  if (!panel || box.empty() || post.empty()) {
    check(false, "the sketch panel, the box and the post exist");
    return QCoreApplication::exit(2);
  }
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {35, 5, 100}}, {"target", {35, 5, 0}}, {"up", {0, 1, 0}}, {"scale", 110}, {"projection", "orthographic"}, {"absolute", true}});
  // Beside the box (-15..15 x -10..10, 10 high): a C to mirror about the upright at x 35, a rectangle to move.
  begin_change();
  const int c0 = m_sk.add_point(40, 0), c1 = m_sk.add_point(55, 0), c2 = m_sk.add_point(55, 10), c3 = m_sk.add_point(40, 10);
  const int bottom = m_sk.add_line(c0, c1), side = m_sk.add_line(c1, c2), top = m_sk.add_line(c2, c3);
  const int axis = m_sk.add_line(m_sk.add_point(35, -5), m_sk.add_point(35, 15));
  const int r0 = m_sk.add_point(65, 0), r1 = m_sk.add_point(80, 0), r2 = m_sk.add_point(80, 10), r3 = m_sk.add_point(65, 10);
  const int r01 = m_sk.add_line(r0, r1);
  m_sk.add_line(r1, r2), m_sk.add_line(r2, r3), m_sk.add_line(r3, r0);
  end_change(QStringLiteral("Bench"));
  rebuild();
  auto keyboard = [this]() -> QWidget* { QWidget* w = QApplication::focusWidget(); return w ? w : m_viewport; };
  auto send = [keyboard](int key, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);  // not spontaneous: Qt sends it as a shortcut override first
    QApplication::sendEvent(keyboard(), &press);
  };
  // Enter in a panel's value field (the keyboard stays there after typing): it applies as Enter in the view does.
  auto enterIn = [check](QLineEdit* field) {
    check(field != nullptr, "the panel has the value field Enter is pressed in");
    if (!field) return;
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QApplication::sendEvent(field, &press);
  };
  auto place = [this](double u, double v) {  // a click, Alt: exactly there
    sketchMove(u, v, Qt::AltModifier, false);
    sketchPress(u, v, Qt::AltModifier);
    sketchRelease(u, v, Qt::AltModifier);
  };
  auto window = [this](double u0, double v0, double u1, double v1) {  // a selection window dragged from (u0, v0)
    sketchMove(u0, v0, Qt::NoModifier, false);
    sketchPress(u0, v0, Qt::NoModifier);
    sketchMove((u0 + u1) / 2, (v0 + v1) / 2, Qt::NoModifier, true);
    sketchMove(u1, v1, Qt::NoModifier, true);
    sketchRelease(u1, v1, Qt::NoModifier);
  };
  auto has = [this](int id) { return std::find(m_sel.begin(), m_sel.end(), id) != m_sel.end(); };
  auto linked = [this] { std::vector<int> out; for (const auto& e : m_sk.entities) if (!e.source.is_null()) out.push_back(e.id); return out; };
  auto previewed = [this] { return m_toolPreview && !m_toolPreviewOverlay.IsNull(); };
  // The view's selection: the picked sources, highlighted until the tool adds them (as the guides show the picks).
  auto highlighted = [this](const std::string& body, opad::Ref::Kind kind) {
    int n = 0;
    for (const opad::Ref& r : m_viewport->selection()) n += r.body == body && r.kind == kind;
    return n;
  };
  auto sources = [panel] { auto* list = panel->findChild<QListWidget*>("sketchSources"); int n = 0; if (list) for (int i = 0; i < list->count(); ++i) n += !list->item(i)->data(Qt::UserRole).toString().isEmpty(); return n; };
  auto addInPanel = [panel](const std::string& body) {  // the panel's Add source list, as a choice there makes
    auto* combo = panel->findChild<QComboBox*>("sketchSourceAdd");
    for (int i = 0; combo && i < combo->count(); ++i)
      if (const auto j = opad::json::parse(combo->itemData(i).toString().toStdString(), nullptr, false); j.is_object() && j.value("body", "") == body) {
        emit combo->activated(i);
        return true;
      }
    return false;
  };
  const QString picture = prefix + ".picture.png";
  {
    QImage image(64, 32, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 8; y < 24; ++y)
      for (int x = 8; x < 56; ++x) image.setPixelColor(x, y, Qt::darkGray);
    image.save(picture);
  }
  auto state = std::make_shared<std::map<std::string, int>>();
  auto phase = std::make_shared<int>(0), ticks = std::make_shared<int>(0);
  auto* timer = new QTimer(this);
  timer->setInterval(60);
  connect(timer, &QTimer::timeout, this, [=, this] {
    if (++*ticks > 1500) {
      check(false, QString("phase %1 in time").arg(*phase));
      timer->stop();
      return QCoreApplication::exit(2);
    }
    if (m_editJob || m_geometryJob || m_imageJob || m_toolPreviewTimer.isActive()) return;
    auto& st = *state;
    if (st.count("until") && *ticks < st["until"]) return;  // a pause asked for (the view's selection mode switching)
    switch ((*phase)++) {
      // ---- Mirror
      case 0:
        setTool("mirror");
        place(47.5, 0);
        check(m_sel == std::vector<int>{bottom}, "mirror: a click picks one curve");
        break;
      case 1:  // the preview timer has run
        check(option("mirrorStage", "seed") == "seed" && !m_toolPreview, "mirror: after the first curve the tool still takes curves (no line yet, no preview)");
        place(55, 5);
        check(has(bottom) && has(side) && m_picked.empty(), "mirror: the second click picks a second curve, not the mirror line");
        break;
      case 2:
        window(38, -3, 58, 13);
        check(has(bottom) && has(side) && has(top) && !has(axis) && option("mirrorStage", "seed") == "seed", "mirror: a window picks the C, the tool still takes curves");
        check(sketchkeys::enter(keyState()) == sketchkeys::Enter::PickMirrorLine, "mirror: Enter would go on to the line");
        send(Qt::Key_Return);
        check(option("mirrorStage") == "axis" && prompt().contains(tr("Pick the mirror line")), "mirror: Enter goes on to the line");
        sketchPress(30, 30, Qt::NoModifier);  // a miss: no window, the curves stay
        sketchRelease(30, 30, Qt::NoModifier);
        check(!m_boxSelecting && has(bottom) && has(top) && option("mirrorStage") == "axis", "mirror: a click on nothing while it waits for the line keeps the curves");
        st["entities"] = int(m_sk.entities.size());
        place(35, 5);
        check(m_picked == std::vector<int>{axis}, "mirror: a click picks the line");
        break;
      case 3:
        check(previewed() && int(m_toolPreview->entities.size()) == st["entities"] + 3 && int(m_sk.entities.size()) == st["entities"], "mirror: the line picked, the mirror image is previewed and the sketch is unchanged");
        check(sketchkeys::enter(keyState()) == sketchkeys::Enter::Apply && keyHints().contains(tr("Enter apply")), "mirror: Enter applies now, and the prompt says so");
        m_viewport->grabImage().save(prefix + ".mirror.png");
        send(Qt::Key_Return);
        break;
      case 4: {
        const int symmetric = int(std::count_if(m_sk.constraints.begin(), m_sk.constraints.end(), [](const SkConstraint& c) { return c.type == SkConstraint::Type::Symmetric; }));
        check(int(m_sk.entities.size()) == st["entities"] + 3 && symmetric >= 4 && m_solved.converged, "mirror: Enter adds the three copies, held symmetric");
        check(option("mirrorStage") == "seed" && m_sel.empty() && m_picked.empty(), "mirror: then it takes the next curves");
        // ---- Move and the per-tool chain option
        setTool("move");
        m_sel.clear();
        check(!chainOnClick(), "move: chain on click is off by default");
        place(72.5, 0);
        check(m_sel == std::vector<int>{r01}, "move: a click picks one curve");
        place(72.5, 0);
        check(m_sel.empty(), "move: a click on it again drops it");
        m_options["chain:move"] = "1";
        place(72.5, 0);
        check(m_sel.size() == 4 && has(r01), "move: with Select connected chain on click, a click picks the whole rectangle");
        place(80, 5);
        check(m_sel.empty(), "move: a click on a picked curve drops its chain");
        setTool("rotate");
        const bool rotateChain = chainOnClick();
        setTool("offset");
        check(!rotateChain && chainOnClick(), "the option is the tool's own: rotate stays off, offset stays on");
        setTool("move");
        m_sel.clear();
        place(72.5, 0);
        m_options["dx"] = "5 mm";
        m_options["dy"] = "0 mm";
        scheduleToolPreview();
        break;
      }
      case 5:
        check(m_sel.size() == 4 && previewed() && std::abs(m_sk.point(r0)->x - 65) < 1e-9, "move: the outline previewed at its new place, the sketch unchanged");
        send(Qt::Key_Return);
        break;
      case 6: {
        check(std::abs(m_sk.point(r0)->x - 70) < 1e-6 && std::abs(m_sk.point(r2)->x - 85) < 1e-6, "move: Enter moves the outline");
        // ---- Project: a click in the view adds a source (once the view picks edges: a sliced job).
        setTool("project");
        st["until"] = *ticks + 8;
        break;
      }
      case 7: {
        st["entities"] = int(m_sk.entities.size());
        m_viewport->grabImage();  // a frame: the picker clips to the camera's depth range, which a redraw sets
        opad::Ref ref;
        const bool hovered = m_viewport->referenceAt(m_viewport->widgetPoint({0, -10, 10}), ref) && ref.body == box && ref.kind == opad::Ref::Kind::Edge;
        st["viewPicks"] = hovered;  // the view picks here (a frame drawn): Project chosen again must too
        place(0, -10);  // the click in the view: what the pointer is on
        if (!hovered) {  // no frame in a hidden window: that edge by hand (the box's edge from (-15,-10,10) to (15,-10,10))
          m_sources.clear();
          const opad::Node* node = m_doc->scene.node(box);
          const TopoDS_Shape shape = node ? opad::body_shape(m_doc->doc, node->body_key) : TopoDS_Shape();
          for (int i = 0; !shape.IsNull() && i < opad::subshape_count(shape, opad::Ref::Kind::Edge); ++i) {
            const TopoDS_Edge edge = TopoDS::Edge(opad::subshape(shape, opad::Ref::Kind::Edge, i));
            const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(edge)), b = BRep_Tool::Pnt(TopExp::LastVertex(edge));
            if (std::abs(a.Z() - 10) < 1e-6 && std::abs(b.Z() - 10) < 1e-6 && std::abs(a.Y() + 10) < 1e-6 && std::abs(b.Y() + 10) < 1e-6) {
              toggleSource(QString::fromStdString(opad::json{{"body", box}, {"kind", "edge"}, {"index", i}}.dump()));
              break;
            }
          }
        }
        trace::log(QString("bench: sketch apply: the view pick %1").arg(hovered ? "found the box's top edge" : "found nothing (hidden window): the edge added by hand"));
        check(m_sources.size() == 1 && sketchkeys::enter(keyState()) == sketchkeys::Enter::Apply, "project: a pick adds a source, Enter would apply");
        break;
      }
      case 8:
        check(previewed() && int(m_toolPreview->entities.size()) > st["entities"] && int(m_sk.entities.size()) == st["entities"], "project: the source previewed before Enter, the sketch unchanged");
        check(m_viewport->selection().size() == 1 && highlighted(box, opad::Ref::Kind::Edge) == 1, "project: the picked edge stays highlighted (the view's selection)");
        st["one"] = m_toolPreview ? int(m_toolPreview->entities.size()) - st["entities"] : 0;
        check(addInPanel(box), "project: the panel lists the box to add");
        break;
      case 9:
        check(m_sources.size() == 2 && sources() == 2, "project: the box added in the panel joins the edge; the panel lists both");
        check(previewed() && int(m_toolPreview->entities.size()) - st["entities"] > st["one"], "project: the preview shows both sources");
        check(m_toolPreviewOverlay->ZLayer() == Graphic3d_ZLayerId_TopOSD, "project: the preview is drawn over the picks' highlight");
        check(highlighted(box, opad::Ref::Kind::Edge) == 1 && highlighted(box, opad::Ref::Kind::Body) == 1, "project: both picks highlighted");
        sketchMove(30, -30, Qt::AltModifier, false);  // the pointer off the edge: the pick's highlight, not the hover
        m_viewport->grabImage().save(prefix + ".project.png");
        panel->grab().save(prefix + ".panel.png");
        send(Qt::Key_Backspace);
        check(m_sources.size() == 1 && keyHints().contains(tr("⌫ undo pick")) && m_viewport->selection().size() == 1 && highlighted(box, opad::Ref::Kind::Edge) == 1,
              "project: Backspace takes the last source back, its highlight too");
        break;
      case 10:
        check(sources() == 1 && previewed(), "project: the panel and the preview follow");
        addInPanel(box);
        break;
      case 11:
        check(m_sources.size() == 2 && previewed(), "project: the box again");
        st["previewed"] = m_toolPreview ? int(m_toolPreview->entities.size()) : -1;
        send(Qt::Key_Return);
        break;
      case 12: {
        const auto now = linked();
        check(int(m_sk.entities.size()) == st["previewed"] && int(now.size()) == st["previewed"] - st["entities"] && m_sources.isEmpty() && sources() == 0,
              "project: Enter adds what the preview showed, linked; the tool asks for new sources");
        check(m_viewport->selection().empty(), "project: the picks added, nothing is highlighted any more");
        // ---- Break link
        setTool("break_link");
        m_sel.clear();
        place(0, -10);
        check(m_sel.size() == 1 && m_sk.entity(m_sel.front()) && !m_sk.entity(m_sel.front())->source.is_null(), "break link: a click picks a linked curve");
        place(70, 5);  // the moved rectangle: not linked
        check(m_sel.size() == 1, "break link: a click on an unlinked curve picks nothing");
        m_sel.clear();
        window(-20, -14, 18, 13);
        st["selected"] = int(m_sel.size());
        check(m_sel.size() >= 4 && std::all_of(m_sel.begin(), m_sel.end(), [this](int id) { const auto* e = m_sk.entity(id); return e && !e->source.is_null(); }),
              "break link: a window picks the linked curves only");
        check(sketchkeys::enter(keyState()) == sketchkeys::Enter::Apply, "break link: Enter would unlink them");
        st["linked"] = int(now.size());
        send(Qt::Key_Return);
        break;
      }
      case 13: {
        check(int(linked().size()) == st["linked"] - st["selected"], "break link: Enter unlinks them");
        // ---- Project again after Break link (which made the bodies unpickable), the view's filter Edges all along: the view
        // picks edges again (once the bodies are activated: a sliced job).
        setTool("project");
        st["until"] = *ticks + 8;
        break;
      }
      case 14: {
        m_viewport->grabImage();  // a frame: the picker clips to the camera's depth range, which a redraw sets
        opad::Ref ref;
        const bool picks = m_viewport->referenceAt(m_viewport->widgetPoint({0, -10, 10}), ref) && ref.body == box && ref.kind == opad::Ref::Kind::Edge;
        check(picks || !st["viewPicks"], "project: chosen again after another tool, the view picks the box's edges");
        // An edge picked, then Include: both pick edges (the filter stays as it is), and the edge's highlight goes with the
        // pick; while it is shown, the selection sits under the sketch (not X-ray).
        {
          const opad::Node* node = m_doc->scene.node(box);
          const TopoDS_Shape shape = node ? opad::body_shape(m_doc->doc, node->body_key) : TopoDS_Shape();
          if (!shape.IsNull() && opad::subshape_count(shape, opad::Ref::Kind::Edge) > 0) toggleSource(QString::fromStdString(opad::json{{"body", box}, {"kind", "edge"}, {"index", 0}}.dump()));
        }
        check(m_sources.size() == 1 && highlighted(box, opad::Ref::Kind::Edge) == 1 && !m_viewport->selectionXray(), "project: a picked edge is highlighted, under the sketch");
        // ---- Include: the X axis as a construction curve.
        setTool("include3d");
        check(m_sources.isEmpty() && m_viewport->selection().empty() && m_viewport->selectionXray(), "include: chosen after Project, Project's pick and its highlight are gone");
        st["entities"] = int(m_sk.entities.size());
        toggleSource(QStringLiteral("{\"base\":\"x\"}"));
        break;
      }
      case 15:
        check(previewed() && int(m_toolPreview->entities.size()) > st["entities"], "include: the source previewed");
        send(Qt::Key_Return);
        break;
      case 16: {
        const SkEntity* e = int(m_sk.entities.size()) > st["entities"] ? &m_sk.entities.back() : nullptr;
        check(e && e->construction && !e->source.is_null(), "include: Enter adds it as a construction curve on the sketch plane, linked");
        // ---- Insert image: the frame follows the pointer, then stays at the click.
        setTool("image_insert");
        m_options["imageFile"] = picture;
        m_options["imageWidth"] = "40 mm";
        sketchMove(50, 30, Qt::AltModifier, false);
        const QColor rb = m_viewport->tokens().hov;
        check(transientDashed(rb) == 4 && !appliesOnEnter(), "insert image: the frame at the width set follows the pointer; Enter waits for the click");
        place(50, 30);
        sketchMove(90, 60, Qt::AltModifier, false);
        check(transientDashed(rb) == 4 && appliesOnEnter() && keyHints().contains(tr("Enter apply")), "insert image: the frame stays at the click, Enter would place it");
        m_viewport->grabImage().save(prefix + ".image.png");
        send(Qt::Key_Return);
        break;
      }
      case 17: {
        const auto* image = m_sk.images.empty() ? nullptr : &m_sk.images.back();
        check(image && std::abs(image->at("width").get<double>() - 40) < 1e-9 && std::abs(image->at("height").get<double>() - 20) < 1e-9 &&
                  std::abs(image->at("position")[0].get<double>() - 50) < 1e-9 && std::abs(image->at("position")[1].get<double>() - 30) < 1e-9,
              "insert image: Enter places it, its lower left at the click, as wide as set");
        // ---- Calibrate: two points, the real distance typed, Enter.
        setTool("image_calibrate");
        place(50, 30);
        sketchMove(60, 30, Qt::AltModifier, false);
        check(transientDashed(m_viewport->tokens().amber) == 1, "calibrate: the distance being measured follows the pointer");
        place(70, 30);
        check(option("knownDistance") == units::editable(units::Kind::Length, 20) && transientSolid(m_viewport->tokens().amber) == 1 && appliesOnEnter(),
              "calibrate: the two points' distance is in the Known distance box; Enter would apply");
        m_viewport->grabImage().save(prefix + ".calibrate.png");
        for (const QChar c : QStringLiteral("40")) send(Qt::Key_0 + c.digitValue(), QString(c));
        send(Qt::Key_Return);
        break;
      }
      case 18: {
        const auto* image = m_sk.images.empty() ? nullptr : &m_sk.images.back();
        check(image && std::abs(image->at("width").get<double>() - 80) < 1e-6, "calibrate: the real distance typed and Enter scale the picture");
        // A second calibration, off any grid: the box shows these clicks' distance (not the 40 typed before), and Enter with it
        // as measured scales nothing (the box holds it rounded, so its factor is not quite 1).
        setTool("image_calibrate");
        place(50.123456, 30.000017);
        place(63.333337, 31.777771);
        const double measured = std::hypot(63.333337 - 50.123456, 31.777771 - 30.000017);
        check(option("knownDistance") == units::editable(units::Kind::Length, measured), "calibrate: a second calibration shows its own measured distance");
        const size_t steps = m_undo.size();
        send(Qt::Key_Return);
        image = m_sk.images.empty() ? nullptr : &m_sk.images.back();
        check(!m_editJob && m_undo.size() == steps && image && std::abs(image->at("width").get<double>() - 80) < 1e-9, "calibrate: Enter with the measured distance scales nothing");
        // ---- Transform image: the panel holds the picture's place, the angle and opacity typed there, Enter applies them.
        setTool("image_edit");
        break;
      }
      case 19: {
        auto* x = panel->findChild<QLineEdit*>("sketchOption-imageX");
        auto* width = panel->findChild<QLineEdit*>("sketchOption-imageWidth");
        auto* angle = panel->findChild<QLineEdit*>("sketchOption-imageAngle");
        auto* opacity = panel->findChild<QLineEdit*>("sketchOption-imageOpacity");
        check(x && width && angle && opacity && x->text() == units::editable(units::Kind::Length, 50) && width->text() == units::editable(units::Kind::Length, 80),
              "transform image: the panel shows the picture's place and width");
        if (angle) angle->setText(QStringLiteral("30 deg"));
        if (opacity) opacity->setText(QStringLiteral("0.8"));
        check(appliesOnEnter() && sketchkeys::enter(keyState()) == sketchkeys::Enter::Apply && keyHints().contains(tr("Enter apply")), "transform image: Enter would apply the values");
        enterIn(opacity);  // where the keyboard is after typing there, as the guide types and presses Enter
        break;
      }
      case 20: {
        const auto* image = m_sk.images.empty() ? nullptr : &m_sk.images.back();
        check(image && std::abs(image->value("angle", 0.0) - M_PI / 6) < 1e-9 && std::abs(image->value("opacity", 0.0) - 0.8) < 1e-9 &&
                  std::abs(image->at("width").get<double>() - 80) < 1e-6 && std::abs(image->at("position")[0].get<double>() - 50) < 1e-6 &&
                  std::abs(image->at("position")[1].get<double>() - 30) < 1e-6,
              "transform image: Enter turns and fades the picture about its corner, where it was and as wide");
        // ---- The region tools take Enter once both loops are picked.
        setTool("union");
        check(!appliesOnEnter(), "union: Enter waits for two loops");
        m_clicks.push_back({});
        m_clicks.push_back({});
        check(appliesOnEnter(), "union: two loops picked, Enter applies");
        m_clicks.clear();
        // ---- Spline node weights: the curve previews its new shape, Enter keeps it.
        setTool("select");
        begin_change();
        st["spline"] = add_cubic_spline(m_sk, {m_sk.add_point(40, -25), m_sk.add_point(50, -15), m_sk.add_point(60, -25)});
        end_change(QStringLiteral("Bench"));
        const SkEntity* spline = m_sk.entity(st["spline"]);
        st["node"] = spline && spline->p.size() > 3 ? spline->p[3] : 0;
        const SkPoint* node = m_sk.point(st["node"]);
        setTool("node");
        m_options["weight"] = "7";  // what the box held for another node
        if (node) place(node->x, node->y);
        check(m_sel == std::vector<int>{st["node"]}, "node: a click picks the spline's middle node");
        auto* weight = panel->findChild<QLineEdit*>("sketchOption-weight");
        check(option("weight") == "1" && weight && weight->text() == "1", "node: the pick loads the node's own weight into the box");
        if (weight) weight->setText(QStringLiteral("4"));  // typed in the panel, as the guide does
        break;
      }
      case 21: {
        const SkEntity* spline = m_sk.entity(st["spline"]);
        const SkEntity* shown = m_toolPreview ? m_toolPreview->entity(st["spline"]) : nullptr;
        check(previewed() && shown && shown->weights.size() > 3 && std::abs(shown->weights[3] - 4) < 1e-9 && spline && (spline->weights.size() <= 3 || std::abs(spline->weights[3] - 4) > 1e-9),
              "node: the weight set, the curve's new shape is previewed and the sketch unchanged");
        check(sketchkeys::enter(keyState()) == sketchkeys::Enter::Apply, "node: Enter would keep it");
        enterIn(panel->findChild<QLineEdit*>("sketchOption-weight"));
        break;
      }
      case 22: {
        const SkEntity* spline = m_sk.entity(st["spline"]);
        check(spline && spline->weights.size() > 3 && std::abs(spline->weights[3] - 4) < 1e-9, "node: Enter keeps the new weight");
        // ---- Silhouette: the box picked, its outline previewed, Enter adds it.
        setTool("silhouette");
        st["entities"] = int(m_sk.entities.size());
        addInPanel(box);
        break;
      }
      case 23:
        check(m_sources.size() == 1 && previewed() && int(m_toolPreview->entities.size()) >= st["entities"] + 4 && int(m_sk.entities.size()) == st["entities"],
              "silhouette: the box's outline previewed before Enter");
        check(m_viewport->selection().size() == 1 && highlighted(box, opad::Ref::Kind::Body) == 1, "silhouette: the picked body stays highlighted");
        m_viewport->grabImage().save(prefix + ".silhouette.png");
        st["previewed"] = m_toolPreview ? int(m_toolPreview->entities.size()) : -1;
        send(Qt::Key_Return);
        break;
      case 24:
        check(int(m_sk.entities.size()) == st["previewed"] && m_sources.isEmpty() && m_viewport->selection().empty(), "silhouette: Enter adds the outline, the highlight goes");
        // ---- Intersect with plane: a click on the post that stands through the plane (once the view picks bodies).
        setTool("intersect_body");
        st["entities"] = int(m_sk.entities.size());
        st["until"] = *ticks + 8;
        break;
      case 25: {
        m_viewport->grabImage();  // a frame, as for project's pick
        opad::Ref ref;
        const bool hovered = m_viewport->referenceAt(m_viewport->widgetPoint({20, -30, 5}), ref) && ref.body == post;
        place(20, -30);  // the post's top, seen from above
        if (!hovered) {  // no frame in a hidden window: the post by hand
          m_sources.clear();
          toggleSource(QString::fromStdString(opad::json{{"body", post}}.dump()));
        }
        trace::log(QString("bench: sketch apply: the view pick %1").arg(hovered ? "found the post" : "found nothing (hidden window): the post added by hand"));
        check(m_sources.size() == 1 && opad::json::parse(m_sources.front().toStdString(), nullptr, false).value("body", "") == post &&
                  sketchkeys::enter(keyState()) == sketchkeys::Enter::Apply && keyHints().contains(tr("Enter apply")),
              "intersect: a click on the post adds it, Enter would apply");
        break;
      }
      case 26: {
        // Where the post (12 mm across, its axis at 20, -30) crosses the plane: that circle and nothing else, dashed.
        int round = 0, other = 0;
        for (size_t i = st["entities"]; m_toolPreview && i < m_toolPreview->entities.size(); ++i) {
          const SkEntity& e = m_toolPreview->entities[i];
          const SkPoint* centre = e.p.empty() ? nullptr : m_toolPreview->point(e.p.front());
          const bool circle = (e.type == SkEntity::Type::Circle || e.type == SkEntity::Type::Arc) && std::abs(e.r - 6) < 1e-6 && centre &&
                              std::hypot(centre->x - 20, centre->y + 30) < 1e-6;
          if (e.type != SkEntity::Type::Point) ++(circle ? round : other);
        }
        check(previewed() && round >= 1 && other == 0 && int(m_sk.entities.size()) == st["entities"],
              "intersect: the circle where the post crosses the plane is previewed before Enter, the sketch unchanged");
        check(m_viewport->selection().size() == 1 && highlighted(post, opad::Ref::Kind::Body) == 1, "intersect: the picked post stays highlighted");
        sketchMove(60, -40, Qt::AltModifier, false);  // the pointer off the post: its hover goes, the pick's highlight stays
        m_viewport->grabImage().save(prefix + ".intersect.png");
        st["previewed"] = m_toolPreview ? int(m_toolPreview->entities.size()) : -1;
        send(Qt::Key_Return);
        break;
      }
      case 27: {
        bool linkedAll = int(m_sk.entities.size()) > st["entities"];
        for (size_t i = st["entities"]; i < m_sk.entities.size(); ++i) linkedAll = linkedAll && !m_sk.entities[i].source.is_null();
        check(int(m_sk.entities.size()) == st["previewed"] && linkedAll && m_sources.isEmpty() && m_viewport->selection().empty(),
              "intersect: Enter adds the circle, linked to the post; the highlight goes");
        // ---- Project with the Faces filter (as its guide switches it): the box's top face, highlighted, then Esc.
        setTool("project");
        m_options["projectionPick"] = "face";
        referenceHover();  // as the panel's Pick filter does
        st["entities"] = int(m_sk.entities.size());
        st["until"] = *ticks + 8;
        break;
      }
      case 28: {
        m_viewport->grabImage();
        opad::Ref ref;
        const bool hovered = m_viewport->referenceAt(m_viewport->widgetPoint({-5, 3, 10}), ref) && ref.body == box && ref.kind == opad::Ref::Kind::Face;
        place(-5, 3);
        if (!hovered) {  // no frame in a hidden window: the top face (every vertex at z 10) by hand
          m_sources.clear();
          const opad::Node* node = m_doc->scene.node(box);
          const TopoDS_Shape shape = node ? opad::body_shape(m_doc->doc, node->body_key) : TopoDS_Shape();
          for (int i = 0; !shape.IsNull() && i < opad::subshape_count(shape, opad::Ref::Kind::Face); ++i) {
            bool top = true;
            for (TopExp_Explorer v(opad::subshape(shape, opad::Ref::Kind::Face, i), TopAbs_VERTEX); v.More(); v.Next()) top = top && std::abs(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Z() - 10) < 1e-6;
            if (top) {
              toggleSource(QString::fromStdString(opad::json{{"body", box}, {"kind", "face"}, {"index", i}}.dump()));
              break;
            }
          }
        }
        trace::log(QString("bench: sketch apply: the view pick %1").arg(hovered ? "found the box's top face" : "found nothing (hidden window): the face added by hand"));
        check(m_sources.size() == 1 && m_viewport->selection().size() == 1 && highlighted(box, opad::Ref::Kind::Face) == 1,
              "project: with the Faces filter a click picks the box's top face, highlighted");
        break;
      }
      case 29:
        check(previewed() && int(m_toolPreview->entities.size()) >= st["entities"] + 4 && int(m_sk.entities.size()) == st["entities"], "project: the face's outline previewed");
        sketchMove(30, -30, Qt::AltModifier, false);
        m_viewport->grabImage().save(prefix + ".face.png");
        setVisible(false);  // the browser's Hide sketch: the pick's highlight goes with the preview, and comes back
        st["hidden"] = int(m_viewport->selection().size());
        setVisible(true);
        check(st["hidden"] == 0 && highlighted(box, opad::Ref::Kind::Face) == 1, "project: hiding the sketch hides the pick's highlight, showing it shows it again");
        send(Qt::Key_Escape);
        check(m_sources.isEmpty() && m_viewport->selection().empty() && !m_toolPreview && int(m_sk.entities.size()) == st["entities"],
              "project: Esc drops the pick, its highlight and its preview");
        setTool("select");
        break;
      default:
        timer->stop();
        trace::log(QString("bench: sketch apply %1").arg(*ok ? "PASS" : "FAIL"));
        QCoreApplication::exit(*ok ? 0 : 2);
    }
  });
  timer->start();
}
