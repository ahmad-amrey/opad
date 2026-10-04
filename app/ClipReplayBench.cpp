// OPAD_BENCH_CLIPREPLAY=<ids> (TODO 11 wave 3, audit 6.3 test 8): the help clips of the sketch tools replayed into the tools,
// the strongest check that a clip's flow is the tool's flow. <ids>: clip ids, comma separated; "sketch.*" every sketch clip
// with a replay ("setup" and "expect" blocks, clips.json "@replay"). OPAD_BENCH_CLIPSHOT=<prefix>: a picture of the view
// after each clip, <prefix>.<id>.png, and the files the setups need (a backdrop picture, an SVG to import).
//
// Sketch1 opens on XY; clip by clip the sketch starts over with the clip's setup (its curves, constraints, a backdrop, the
// sketch plane, the tool and its options, a command run first), the view frames what the clip frames, and the clip's own
// input (clips::input) goes into the editor through the entry points the view uses: the pointer's moves, presses, releases,
// clicks and double clicks (SketchInput), keys sent to the widget that has the keyboard (the shortcut override hands the
// sketch's keys to the sketch first; a command's key triggers its action, as the window's shortcut would), values typed
// into the value boxes key by key, a card's rows set in the panel's fields of that label, its list row picked, its page
// switch turned, its button (or a chip, or a dialog's button) pressed by its text. A click on a body (an iso clip's
// reference tool, the plane picker) takes what the view finds there, else the nearest edge, face or body. Then what the
// tool made is compared with the clip's "expect" block. Along the way the Tool guide is checked: every click, key, value
// and button of the clip falls in a clip step that the guide (clips::guideRange) loops while the tool waits for the step
// it is waiting for then, so the panel never loops a segment that shows something else. Logs
// "bench: clip replay: <id>: <what> PASS/FAIL" and quits; tools/bench_cases/help.py runs it.
#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "HelpClip.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_pattern.hpp"
#include "opad/geometry.hpp"
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCheckBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <cmath>

using namespace opad::design;

class ClipReplay : public QObject {
 public:
  ClipReplay(MainWindow& window, DesignController* design, std::function<QAction*(const QString&)> action, QStringList ids, QString shots)
      : QObject(design), m_window(window), m_design(design), m_action(std::move(action)), m_ids(std::move(ids)), m_shots(std::move(shots)) {
    m_timer.setInterval(25);
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    connect(m_design->sketch(), &SketchEditor::status, this, [this](const QString& text) { m_status = text; });
  }
  void start() {
    trace::log(QString("bench: clip replay: %1 clips").arg(m_ids.size()));
    qApp->installEventFilter(this);  // after the bench's own (BenchQuiet): a message box the clip answers is answered first
    m_timer.start();
  }

 protected:
  // A message box the tool asks (Cancel sketch's "Discard the changes…?"): the clip's card answers it with the button it
  // presses next, on the box's first turn (BenchQuiet would cancel it on its second); a box the clip shows no answer to fails.
  bool eventFilter(QObject* o, QEvent* e) override {
    auto* box = e->type() == QEvent::Show ? qobject_cast<QMessageBox*>(o) : nullptr;
    if (!box || m_phase == Phase::Done) return false;
    for (int i = m_next; i < m_events.size(); ++i) {
      const clips::Input& in = m_events[i];
      if (in.kind == clips::Input::Kind::Move || in.kind == clips::Input::Kind::Click) continue;
      if (in.kind != clips::Input::Kind::Button) break;
      for (QAbstractButton* b : box->buttons())
        if (plain(b->text()).compare(plain(in.text), Qt::CaseInsensitive) == 0) {
          m_answered.insert(i);
          trace::log(QString("bench: clip replay: %1: the clip answers \"%2\" with %3").arg(m_id, box->text(), in.text));
          QTimer::singleShot(0, b, [b] { b->click(); });
          return false;
        }
      break;
    }
    fail("the tool asks \"" + box->text() + "\", which the clip answers");
    return false;
  }

 private:
  enum class Phase { Next, Open, Prepare, Play, Settle, Done };
  SketchEditor* ed() const { return m_design->sketch(); }
  Viewport* view() const { return ed()->m_viewport; }
  SketchPanel* panel() const { return m_window.findChild<SketchPanel*>(); }

  void check(bool pass, const QString& what) {
    trace::log(QString("bench: clip replay: %1: %2 %3").arg(m_id, what, pass ? "PASS" : "FAIL"));
    m_ok = m_ok && pass;
    m_clipOk = m_clipOk && pass;
  }
  void fail(const QString& what) { check(false, what); }

  // Jobs, the tool's preview timer, a solve: the next input waits for them, as a hand that pauses does.
  bool busy() const {
    SketchEditor* e = ed();
    return e->m_editJob || e->m_geometryJob || e->m_imageJob || e->m_toolPreviewTimer.isActive() || m_design->busy() || e->m_jobs->busy();
  }

  void tick() {
    if (m_inTick) return;
    m_inTick = true;
    step();
    m_inTick = false;
  }

  void step() {
    if (m_phase == Phase::Done) return;
    if (++m_ticks > 1600) {  // 40 s for one clip
      fail("the replay ended in time");
      return next();
    }
    if (m_wait > 0) {
      --m_wait;
      return;
    }
    switch (m_phase) {
      case Phase::Next: return next();
      case Phase::Open:
        if (ed()->active()) {
          m_phase = Phase::Prepare;
          return;
        }
        if (!m_reopening) {  // a clip before finished, cancelled or left the sketch: Sketch1 again
          m_reopening = true;
          m_design->benchSketch([this] { m_reopening = false; });
        }
        return;
      case Phase::Prepare:
        if (busy() || m_reopening) return;
        try {
          prepare();
        } catch (const std::exception& e) {
          fail(QString("the setup is made (%1)").arg(QString::fromUtf8(e.what())));
          return next();
        }
        m_phase = Phase::Play;
        m_wait = 4;
        return;
      case Phase::Play: {
        if (busy() && !QApplication::activeModalWidget()) return;
        int moves = 0;
        while (m_next < m_events.size()) {
          if (m_answered.contains(m_next)) {  // a dialog's button pressed when the dialog came
            ++m_next;
            continue;
          }
          const clips::Input& in = m_events[m_next];
          if (in.kind == clips::Input::Kind::Move) {
            if (++moves > 40) return;  // the view's events in between
            ++m_next;
            apply(in);
            continue;
          }
          if (moves) return;  // the moves first, then this one on its own tick
          ++m_next;
          apply(in);
          m_wait = 2;
          return;
        }
        m_phase = Phase::Settle;
        m_settled = 0;
        return;
      }
      case Phase::Settle:
        if (busy() || QApplication::activeModalWidget()) {
          m_settled = 0;
          if (QApplication::activeModalWidget() && ++m_modal > 40) {  // a dialog the clip never answers
            fail("a dialog the clip does not show is left open: " + QApplication::activeModalWidget()->windowTitle());
            QApplication::activeModalWidget()->close();
          }
          return;
        }
        if (++m_settled < 6) return;
        compare();
        return next();
      case Phase::Done: return;
    }
  }

  void next() {
    if (!m_id.isEmpty()) {
      if (!m_shots.isEmpty()) view()->grabImage().save(m_shots + "." + m_id + ".png");
      trace::log(QString("bench: clip replay: %1 %2").arg(m_id, m_clipOk ? "PASS" : "FAIL"));
      if (!m_clipOk) m_failed << m_id;
    }
    if (++m_clip >= m_ids.size()) {
      m_phase = Phase::Done;
      m_timer.stop();
      trace::log(QString("bench: clip replay: %1 clips replayed%2 %3").arg(m_ids.size()).arg(m_failed.isEmpty() ? QString() : ", failed: " + m_failed.join(", "), m_ok ? "PASS" : "FAIL"));
      QTimer::singleShot(0, qApp, [ok = m_ok] { QCoreApplication::exit(ok ? 0 : 1); });
      return;
    }
    m_id = m_ids[m_clip];
    m_events = clips::input(m_id);
    m_setup = clips::setup(m_id);
    m_expect = clips::expect(m_id);
    m_next = 0;
    m_ticks = 0;
    m_modal = 0;
    m_clipOk = true;
    m_snapSeen.clear();
    m_answered.clear();
    m_phase = Phase::Open;
    if (m_expect.isEmpty()) fail("the clip has an expect block");
  }

  // ---- the setup ------------------------------------------------------------------------------------------------------
  QString tool() const {
    if (m_setup.contains("tool")) return m_setup.value("tool").toString();
    QString t = m_id.mid(7);  // "sketch."
    if (t.startsWith("c.")) t = "c:" + t.mid(2);
    return t;
  }

  struct Builder {
    Sketch& sk;
    std::map<std::pair<long long, long long>, int> points;
    std::vector<int> curves;  // the setup's curves in order (their first entity: a slot's or a polyline's first)
    std::vector<std::vector<int>> parts;
    int point(const QJsonValue& v, bool share = true) {
      const QJsonArray a = v.toArray();
      const double x = a.at(0).toDouble(), y = a.at(1).toDouble();
      const std::pair<long long, long long> key{std::llround(x * 1e6), std::llround(y * 1e6)};
      if (share)
        if (const auto it = points.find(key); it != points.end()) return it->second;
      for (const SkPoint& p : sk.points)  // a point already there (the drawn curves join the setup's)
        if (share && !p.fixed && std::hypot(p.x - x, p.y - y) < 1e-9) return points[key] = p.id;
      return points[key] = sk.add_point(x, y);
    }
    static double rad(double degrees) { return degrees * M_PI / 180; }
    QJsonArray at(double x, double y) const { return QJsonArray{x, y}; }
    void add(const QJsonObject& c) {
      std::vector<int> made;
      const bool construction = c.value("construction").toBool();
      if (c.contains("line")) {
        const QJsonArray l = c.value("line").toArray();
        made.push_back(sk.add_line(point(l.at(0)), point(l.at(1)), construction));
      } else if (c.contains("poly") || c.contains("rect")) {
        QJsonArray pts = c.value("poly").toArray();
        bool closed = c.value("closed").toBool();
        if (c.contains("rect")) {
          const QJsonArray r = c.value("rect").toArray();
          const double x0 = r.at(0).toArray().at(0).toDouble(), y0 = r.at(0).toArray().at(1).toDouble(), x1 = r.at(1).toArray().at(0).toDouble(), y1 = r.at(1).toArray().at(1).toDouble();
          pts = QJsonArray{at(x0, y0), at(x1, y0), at(x1, y1), at(x0, y1)};
          closed = true;
        }
        for (qsizetype i = 1; i < pts.size(); ++i) made.push_back(sk.add_line(point(pts.at(i - 1)), point(pts.at(i)), construction));
        if (closed && pts.size() > 2) made.push_back(sk.add_line(point(pts.last()), point(pts.first()), construction));
      } else if (c.contains("circle")) {
        made.push_back(sk.add_circle(point(c.value("circle")), c.value("r").toDouble(), construction));
      } else if (c.contains("arc")) {  // centre, radius, start and sweep in degrees (negative: clockwise)
        const QJsonArray o = c.value("arc").toArray();
        const double cx = o.at(0).toDouble(), cy = o.at(1).toDouble(), r = c.value("r").toDouble();
        double a0 = rad(c.value("start").toDouble()), a1 = a0 + rad(c.value("sweep").toDouble());
        if (a1 < a0) std::swap(a0, a1);
        made.push_back(sk.add_arc(point(c.value("arc")), point(at(cx + r * std::cos(a0), cy + r * std::sin(a0))), point(at(cx + r * std::cos(a1), cy + r * std::sin(a1))), construction));
      } else if (c.contains("slot")) {  // two cap centres and a radius: two lines and two arcs, as the slot tool makes them
        const QJsonArray s = c.value("slot").toArray();
        const double x0 = s.at(0).toArray().at(0).toDouble(), y0 = s.at(0).toArray().at(1).toDouble(), x1 = s.at(1).toArray().at(0).toDouble(),
                     y1 = s.at(1).toArray().at(1).toDouble(), r = c.value("r").toDouble();
        const double a = std::atan2(y1 - y0, x1 - x0), nx = -std::sin(a) * r, ny = std::cos(a) * r;
        const int p0 = point(at(x0 + nx, y0 + ny)), p1 = point(at(x1 + nx, y1 + ny)), q0 = point(at(x0 - nx, y0 - ny)), q1 = point(at(x1 - nx, y1 - ny));
        made.push_back(sk.add_line(p0, p1, construction));
        made.push_back(sk.add_arc(point(s.at(1)), q1, p1, construction));
        made.push_back(sk.add_line(q1, q0, construction));
        made.push_back(sk.add_arc(point(s.at(0)), p0, q0, construction));
      } else if (c.contains("spline")) {  // through its points, or (poles) a cubic Bezier on them
        SkEntity e;
        e.type = SkEntity::Type::Spline;
        for (const QJsonValue& p : c.value("spline").toArray()) e.p.push_back(point(p, false));
        if (c.value("poles").toBool()) {
          e.degree = int(e.p.size()) - 1;
          e.knots = {0, 1};
          e.multiplicities = {e.degree + 1, e.degree + 1};
          e.weights.assign(e.p.size(), 1);
        }
        e.construction = construction;
        e.id = sk.next_id();
        sk.entities.push_back(e);
        made.push_back(e.id);
      } else if (c.contains("point")) {
        SkEntity e;
        e.type = SkEntity::Type::Point;
        e.p = {point(c.value("point"))};
        e.id = sk.next_id();
        sk.entities.push_back(e);
        made.push_back(e.id);
      }
      for (int id : made)
        if (SkEntity* e = sk.entity(id); e && c.value("linked").toBool()) {  // a projection that follows a body
          e->source = opad::json{{"ref", {{"base", "x"}}}, {"mode", "project"}, {"slot", 0}, {"count", 1}};
          e->fixed = true;
          for (int p : e->p) sk.point(p)->fixed = true;
        }
      curves.push_back(made.empty() ? 0 : made.front());
      parts.push_back(made);
    }
    int ref(const QJsonValue& v) {  // a curve by its index in the setup ("i.k": its k-th piece), or a point by where it is
      if (v.isArray()) return point(v);
      if (v.isString()) {
        const QStringList ik = v.toString().split('.');
        const size_t i = size_t(ik.value(0).toInt()), k = size_t(ik.value(1).toInt());
        return i < parts.size() && k < parts[i].size() ? parts[i][k] : 0;
      }
      const int i = v.toInt(-1);
      return i >= 0 && i < int(curves.size()) ? curves[size_t(i)] : 0;
    }
    void constrain(const QJsonObject& c) {
      std::vector<int> refs;
      for (const QJsonValue& v : c.value("on").toArray()) refs.push_back(ref(v));
      const auto type = SkConstraint::type_from_name(c.value("type").toString().toStdString());
      const double value = type == SkConstraint::Type::Angle ? rad(c.value("value").toDouble()) : c.value("value").toDouble();
      const int id = sk.add_constraint(type, refs, value);
      if (SkConstraint* k = sk.constraint(id); k && c.contains("label")) {
        const QJsonArray at = c.value("label").toArray();
        k->pos[0] = at.at(0).toDouble();
        k->pos[1] = at.at(1).toDouble();
      }
    }
  };

  // A backdrop picture where the setup puts it (corners in sketch millimetres): a grey plate, and for tracing the dark
  // outline the clip traces (a rounded plate with two holes), 20 pixels to the millimetre.
  opad::json picture(const QJsonObject& image) {
    const QJsonArray c = image.value("corners").toArray();
    const double x0 = c.at(0).toArray().at(0).toDouble(), y0 = c.at(0).toArray().at(1).toDouble(), x1 = c.at(1).toArray().at(0).toDouble(), y1 = c.at(1).toArray().at(1).toDouble();
    const double k = 20, w = x1 - x0, h = y1 - y0;
    QImage img(int(std::lround(w * k)), int(std::lround(h * k)), QImage::Format_RGB32);
    img.fill(QColor(235, 235, 235));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    auto mm = [&](double x, double y) { return QPointF((x - x0) * k, (y1 - y) * k); };
    for (const QJsonValue& v : image.value("shapes").toArray()) {  // {"rect": [cx, cy, w, h], "round": r, "holes": [[cx, cy, r]]}
      const QJsonObject s = v.toObject();
      const QJsonArray r = s.value("rect").toArray();
      const double cx = r.at(0).toDouble(), cy = r.at(1).toDouble(), sw = r.at(2).toDouble(), sh = r.at(3).toDouble(), round = s.value("round").toDouble();
      QPainterPath path;
      path.addRoundedRect(QRectF(mm(cx - sw / 2, cy + sh / 2), mm(cx + sw / 2, cy - sh / 2)), round * k, round * k);
      for (const QJsonValue& hv : s.value("holes").toArray()) {
        const QJsonArray hole = hv.toArray();
        QPainterPath circle;
        circle.addEllipse(mm(hole.at(0).toDouble(), hole.at(1).toDouble()), hole.at(2).toDouble() * k, hole.at(2).toDouble() * k);
        path = path.subtracted(circle);
      }
      p.fillPath(path, QColor(40, 40, 40));
    }
    p.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    if (!m_shots.isEmpty()) img.save(m_shots + "." + m_id + ".picture.png");
    return {{"name", "replay.png"}, {"data", bytes.toBase64().toStdString()}, {"position", {x0, y0}}, {"width", w}, {"height", h},
            {"angle", image.value("angle").toDouble() * M_PI / 180}, {"opacity", image.value("opacity").toDouble(0.5)}};
  }

  void prepare() {
    SketchEditor* e = ed();
    // The plane: XY, or the clip's (an iso clip whose sketch plane is not the ground).
    opad::Frame frame;
    opad::json plane = {{"base", "xy"}};
    if (m_setup.contains("plane")) {
      const QJsonArray o = m_setup.value("plane").toObject().value("origin").toArray();
      frame.origin = {o.at(0).toDouble(), o.at(1).toDouble(), o.at(2).toDouble()};
      plane = {{"origin", {frame.origin[0], frame.origin[1], frame.origin[2]}}, {"normal", {0, 0, 1}}};
    }
    plane["frame"] = frame.to_json();
    Sketch sk;
    sk.add_point(0, 0, true);  // the origin, as a new sketch has it
    Builder b{sk};
    for (const QJsonValue& c : m_setup.value("curves").toArray()) b.add(c.toObject());
    for (const QJsonValue& c : m_setup.value("constraints").toArray()) b.constrain(c.toObject());
    for (const QJsonValue& v : m_setup.value("patterns").toArray()) {
      const QJsonObject pattern = v.toObject();
      std::vector<int> seeds;
      for (const QJsonValue& s : pattern.value("on").toArray()) seeds.push_back(b.ref(s));
      create_pattern(sk, seeds, opad::json::parse(QJsonDocument(pattern.value("inputs").toObject()).toJson().toStdString()));
    }
    if (m_setup.contains("image")) {
      opad::json image = picture(m_setup.value("image").toObject());
      image["id"] = sk.next_id();
      sk.images.push_back(image);
    }
    try {
      solve(sk);
    } catch (const std::exception&) {
    }
    e->end();
    e->begin({}, "Sketch1", plane, frame, sk.to_json());
    m_frame = frame;
    m_sketches = int(e->m_doc->scene.sketches.size());
    // Curves drawn since the sketch opened: it is modified (Cancel sketch asks).
    if (m_setup.contains("drawn")) {
      e->begin_change();
      Builder d{e->m_sk};
      for (const QJsonValue& c : m_setup.value("drawn").toArray()) d.add(c.toObject());
      e->end_change(QStringLiteral("Bench"));
    }
    view()->setGridSnap(false);  // the clips place points where they show them, between grid nodes too
    frameView();
    const QString t = tool();
    if (t != "select") e->setTool(t);
    const QJsonObject options = m_setup.value("options").toObject();
    for (auto it = options.begin(); it != options.end(); ++it) {
      QString value = it.value().toString();
      if (value.contains("{shots}")) value.replace("{shots}", m_shots + "." + m_id);
      e->m_options[it.key()] = value;
    }
    if (m_setup.contains("file")) writeFile(m_setup.value("file").toString());
    e->m_panelFieldsDirty = true;
    e->rebuild();
    emit e->changed();
    emit e->workflowChanged();
    if (const QString command = m_setup.value("command").toString(); !command.isEmpty()) trigger(command);
    m_status.clear();
  }

  void writeFile(const QString& kind) {
    const QString path = m_shots + "." + m_id + "." + kind;
    if (kind == "png") {  // a photo to insert: 24 by 14
      QImage img(480, 280, QImage::Format_RGB32);
      img.fill(QColor(200, 205, 210));
      QPainter p(&img);
      p.fillRect(QRect(60, 60, 360, 160), QColor(90, 90, 95));
      p.end();
      img.save(path);
      ed()->m_options["imageFile"] = path;
      return;
    }
    if (kind == "svg") {  // a mark the clip imports: two letters' worth of closed paths
      QFile f(path);
      if (f.open(QIODevice::WriteOnly))
        f.write(R"(<svg xmlns="http://www.w3.org/2000/svg" width="20mm" height="10mm" viewBox="0 0 20 10"><path d="M1 1 H8 V9 H1 Z M3 3 V7 H6 V3 Z"/><path d="M11 1 H18 V5 H13 V9 H11 Z"/></svg>)");
    }
    ed()->m_options["vectorFile"] = path;
  }

  // The view square to the sketch, framing what the clip frames (its extent; an iso clip: its own iso camera, for picks).
  void frameView() {
    const QRectF ext = clips::extent(m_id);
    const double vw = std::max(1, view()->width()), vh = std::max(1, view()->height());
    const double scale = std::max(ext.height(), ext.width() * vh / vw) * 1.1;
    if (clips::iso(m_id)) {
      const double az = -45 * M_PI / 180, el = 35.264 * M_PI / 180;
      const opad::Vec3 target = {0, 0, m_frame.origin[2] + 5};
      view()->setCameraJson({{"eye", {target[0] + 100 * std::cos(el) * std::cos(az), target[1] + 100 * std::cos(el) * std::sin(az), target[2] + 100 * std::sin(el)}},
                             {"target", {target[0], target[1], target[2]}}, {"up", {0, 0, 1}}, {"scale", scale}, {"projection", "orthographic"}, {"absolute", true}});
      return;
    }
    const opad::Vec3 c = m_frame.to_world(ext.center().x(), -ext.center().y());  // view units: y down
    const opad::Vec3 n = m_frame.normal();
    view()->setCameraJson({{"eye", {c[0] + 100 * n[0], c[1] + 100 * n[1], c[2] + 100 * n[2]}}, {"target", {c[0], c[1], c[2]}}, {"up", {m_frame.y[0], m_frame.y[1], m_frame.y[2]}},
                           {"scale", scale}, {"projection", "orthographic"}, {"absolute", true}});
  }

  // ---- the input -------------------------------------------------------------------------------------------------------
  // A clip's point on the sketch: a plane clip draws in sketch coordinates, an iso clip in the model's.
  void local(const QVector3D& at, double& u, double& v) const {
    if (clips::iso(m_id)) m_frame.to_local({at.x(), at.y(), at.z()}, u, v);
    else u = at.x(), v = at.y();
  }

  // What has the keyboard: the window's focus widget (a value box being typed into, the dimension's box), else the view. A
  // hidden window is never active, so QApplication::focusWidget() is empty there; the window's own focus child is kept.
  QWidget* keyboard() const {
    QWidget* w = view()->window()->focusWidget();
    return w && !w->isHidden() && (w == view() || view()->isAncestorOf(w)) ? w : view();  // the view or a box over it
  }
  void send(int key, Qt::KeyboardModifiers mods, const QString& text) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);  // not spontaneous: Qt sends it as a shortcut override first
    QApplication::sendEvent(keyboard(), &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QApplication::sendEvent(keyboard(), &release);
  }

  static QString plain(QString s) { return s.remove('&').section(QStringLiteral("   "), 0, 0).trimmed(); }

  bool trigger(const QString& id) {
    QAction* a = m_action(id);
    if (!a || !a->isEnabled()) {
      fail("the command " + id + " runs");
      return false;
    }
    QTimer::singleShot(0, a, [a] { a->trigger(); });  // as the window's shortcut does, from the event loop (a dialog it opens is modal)
    return true;
  }

  // A key as the clip draws its caps: the sketch's own keys (Enter, Esc, Tab, Backspace, Del, a value) go to the widget that
  // has the keyboard; another key is a command's shortcut, which the window's shortcut map would trigger.
  void key(const QStringList& caps) {
    QStringList parts;
    for (QString c : caps) parts << (c == "Enter" ? "Return" : c == "Esc" ? "Escape" : c == "Del" ? "Delete" : c);
    const QKeySequence seq = QKeySequence::fromString(parts.join('+'), QKeySequence::PortableText);
    if (seq.isEmpty()) return fail("the key " + caps.join('+') + " is a key");
    const QKeyCombination combo = seq[0];
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    const int k = combo.key();
    const QString text = k >= Qt::Key_A && k <= Qt::Key_Z && !(mods & Qt::ShiftModifier) ? QString(QChar('a' + (k - Qt::Key_A)))
                         : k >= Qt::Key_Space && k < Qt::Key_A ? QString(QChar(k))
                         : k == Qt::Key_Return ? QString("\r") : k == Qt::Key_Escape ? QString(QChar(27)) : k == Qt::Key_Tab ? QString("\t") : QString();
    const bool sketchKey = !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
                           (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Escape || k == Qt::Key_Backspace || k == Qt::Key_Delete || k == Qt::Key_Tab ||
                            [&] { QKeyEvent probe(QEvent::KeyPress, k, mods, text); return ed()->typingKey(&probe); }());
    if (sketchKey) return send(k, mods, text);
    auto same = [](QKeySequence a, QKeySequence b) {
      auto norm = [](QKeySequence s) { return s.toString(QKeySequence::PortableText).replace("Enter", "Return"); };
      return norm(a) == norm(b);
    };
    for (QAction* a : m_window.findChildren<QAction*>())
      for (const QKeySequence& s : a->shortcuts())
        if (same(s, seq) && a->isEnabled() && !a->objectName().isEmpty()) {
          QTimer::singleShot(0, a, [a] { a->trigger(); });
          return;
        }
    fail("the key " + caps.join('+') + " triggers a command");
  }

  static QStringList fixedCaps(const QString& name) {
    static const QHash<QString, QStringList> caps{{"enter", {"Enter"}}, {"esc", {"Esc"}}, {"tab", {"Tab"}}, {"del", {"Del"}}, {"backspace", {"Backspace"}},
                                                  {"ctrlEnter", {"Ctrl", "Enter"}}};
    return caps.value(name, {name});
  }

  void type(const QString& text) {
    for (const QChar c : text) {
      const ushort u = c.unicode();
      const int k = u >= '0' && u <= '9' ? Qt::Key_0 + (u - '0') : u == '.' ? Qt::Key_Period : u == '-' ? Qt::Key_Minus : u == ',' ? Qt::Key_Comma
                    : u >= 'a' && u <= 'z' ? Qt::Key_A + (u - 'a') : u >= 'A' && u <= 'Z' ? Qt::Key_A + (u - 'A') : u == ' ' ? Qt::Key_Space : int(u);
      send(k, u >= 'A' && u <= 'Z' ? Qt::ShiftModifier : Qt::NoModifier, QString(c));
    }
  }

  // The panel's field a card row names: the words of the row's label among the field label's ("Sides": "Number of sides:",
  // "Opacity": "Opacity (0 to 1)"), on the panel's pages as they show; an exact label first.
  QList<QWidget*> fields(const QString& label) const {
    auto words = [](QString s) {
      s = s.toLower();
      s.remove(QRegularExpression(R"(\(.*\))"));
      s.remove(':');
      return s.split(QRegularExpression(R"(\s+)"), Qt::SkipEmptyParts);
    };
    const QStringList want = words(label);
    QList<QWidget*> exact, partial;
    QWidget* root = panel();
    if (m_design->pickingPlane()) root = m_design->planePanel();
    if (!root) return {};
    for (QFormLayout* form : root->findChildren<QFormLayout*>()) {
      for (int r = 0; r < form->rowCount(); ++r) {
        QLayoutItem* l = form->itemAt(r, QFormLayout::LabelRole);
        QLayoutItem* f = form->itemAt(r, QFormLayout::FieldRole);
        auto* name = l ? qobject_cast<QLabel*>(l->widget()) : nullptr;
        QWidget* field = f ? f->widget() : nullptr;
        if (!name || !field || !field->isVisibleTo(root)) continue;
        const QStringList have = words(name->text());
        if (have == want) exact << field;
        else if (std::all_of(want.begin(), want.end(), [&](const QString& w) { return have.contains(w); })) partial << field;
      }
    }
    for (QWidget* w : root->findChildren<QWidget*>())  // a field without a form label: by its accessible name (the Select page's filter)
      if (!w->accessibleName().isEmpty() && w->isVisibleTo(root)) {
        const QStringList have = words(w->accessibleName());
        if (have == want) exact << w;
        else if (std::all_of(want.begin(), want.end(), [&](const QString& x) { return have.contains(x); })) partial << w;
      }
    return exact.isEmpty() ? partial : exact;
  }

  // A field's label in its form (lower case).
  QString fieldLabel(QWidget* field) const {
    for (QFormLayout* form : panel()->findChildren<QFormLayout*>()) {
      int row = -1;
      QFormLayout::ItemRole role;
      form->getWidgetPosition(field, &row, &role);
      if (row >= 0)
        if (auto* l = qobject_cast<QLabel*>(form->itemAt(row, QFormLayout::LabelRole) ? form->itemAt(row, QFormLayout::LabelRole)->widget() : nullptr)) return l->text().toLower();
    }
    return field->accessibleName().toLower();
  }

  void row(const QString& label, QString value) {
    if (value.isEmpty()) return;
    if (value.endsWith(" px")) value.chop(3);
    QList<QWidget*> found = fields(label);
    // An angle: "60°" is "60 deg", or 60 in a field that names its unit ("Preserve corners above (degrees)").
    const bool degrees = std::any_of(found.begin(), found.end(), [this](QWidget* f) { return fieldLabel(f).contains("degree"); });
    value.replace(QStringLiteral("°"), degrees ? QString() : QStringLiteral(" deg"));
    QStringList values{value};
    if (found.size() == 2 && value.contains(',')) values = value.split(',');  // "Centre: -4, -8" is Centre X and Centre Y
    if (found.isEmpty() || (found.size() > 1 && values.size() != found.size())) return fail(QString("the card's row \"%1\" is one field of the tool's panel").arg(label));
    for (int i = 0; i < found.size(); ++i) {
      const QString v = values.value(i, value).trimmed();
      if (auto* edit = qobject_cast<QLineEdit*>(found[i])) {  // typed: its text, then the edit finished (the plane picker's X and Y read it then)
        edit->setText(v);
        edit->setModified(true);
        QMetaObject::invokeMethod(edit, "editingFinished");
      } else if (auto* combo = qobject_cast<QComboBox*>(found[i])) {
        int index = -1;
        for (int j = 0; j < combo->count() && index < 0; ++j)
          if (plain(combo->itemText(j)).compare(v, Qt::CaseInsensitive) == 0) index = j;
        for (int j = 0; j < combo->count() && index < 0; ++j)
          if (combo->itemText(j).contains(v, Qt::CaseInsensitive)) index = j;
        if (index < 0) return fail(QString("the card's \"%1: %2\" is a choice of the panel's field").arg(label, v));
        combo->setCurrentIndex(index);
      } else if (auto* check = qobject_cast<QCheckBox*>(found[i])) {
        check->setChecked(v == "true" || v == "on" || v == "1");
      } else {
        return fail(QString("the card's row \"%1\" is a field the replay can set").arg(label));
      }
    }
  }

  void button(const QString& text, bool chip) {
    const QString want = plain(text);
    if (QWidget* modal = QApplication::activeModalWidget()) {  // a dialog the tool asked: its button
      for (QAbstractButton* b : modal->findChildren<QAbstractButton*>())
        if (plain(b->text()).compare(want, Qt::CaseInsensitive) == 0) {
          b->click();
          return;
        }
      return fail("the dialog \"" + modal->windowTitle() + "\" has the card's button " + text);
    }
    for (QWidget* root : {static_cast<QWidget*>(panel()), static_cast<QWidget*>(m_design->planePanel())})
      if (root)
        for (QPushButton* b : root->findChildren<QPushButton*>())
          if (b->isVisibleTo(root) && b->isEnabled() && plain(b->text()) == want) {
            b->click();
            return;
          }
    for (QAction* a : m_window.findChildren<QAction*>())  // a chip: the command of that name (a card's button is the panel's)
      if (chip && !a->objectName().isEmpty() && plain(a->text()) == want && a->isEnabled()) {
        QTimer::singleShot(0, a, [a] { a->trigger(); });
        return;
      }
    fail("the card's button \"" + text + "\" is a button of the tool's panel, a dialog or a command");
  }

  void page(const QString& name) {
    if (SketchPanel* p = panel())
      for (QToolButton* b : p->findChildren<QToolButton*>())
        if (b->property("sketchPage").isValid() && plain(b->text()) == name) {
          b->click();
          return;
        }
    fail("the card's page switch " + name + " is the panel's");
  }

  void pick(const clips::Input& in) {
    SketchPanel* p = panel();
    QTreeWidget* list = nullptr;  // the Constraints page's list: ID, Type, Value
    if (p)
      for (QTreeWidget* t : p->findChildren<QTreeWidget*>())
        if (t->columnCount() == 3) list = t;
    if (!list || in.index >= list->topLevelItemCount()) return fail(QString("the panel's list has the card's row %1").arg(in.index + 1));
    QTreeWidgetItem* item = list->topLevelItem(in.index);
    list->setCurrentItem(item);
    bool number = false;
    const double value = in.value.toDouble(&number);
    const QString shown = item->text(1) + " " + item->text(2);
    const bool same = number ? std::abs(QString(item->text(2)).remove(QRegularExpression("[^0-9.\\-]")).toDouble() - value) < 1e-6 : shown.contains(in.value, Qt::CaseInsensitive);
    check(same, QString("the panel's row %1 reads \"%2\" as the card's (%3)").arg(in.index + 1).arg(in.value, shown.trimmed()));
  }

  // The reference a click in an iso clip is on: what the view finds there (after a frame: the picker clips to the depth range
  // a redraw sets), else the nearest edge, face, vertex or body as the tool's pick filter takes them.
  bool referenceAt(const QVector3D& at, opad::Ref::Kind kind, opad::Ref& ref) {
    view()->grabImage();
    const opad::Vec3 p{at.x(), at.y(), at.z()};
    if (view()->referenceAt(view()->widgetPoint(p), ref) && ref.kind == kind) return true;
    const TopoDS_Vertex probe = BRepBuilderAPI_MakeVertex(gp_Pnt(p[0], p[1], p[2]));
    double best = 1e18;
    AppDocument* doc = ed()->m_doc;
    for (const std::string& id : doc->scene.all_bodies()) {
      const opad::Node* node = doc->scene.node(id);
      const TopoDS_Shape shape = node ? opad::body_shape(doc->doc, node->body_key) : TopoDS_Shape();
      if (shape.IsNull()) continue;
      const int n = kind == opad::Ref::Kind::Body ? 1 : opad::subshape_count(shape, kind);
      for (int i = 0; i < n; ++i) {
        BRepExtrema_DistShapeShape d(probe, kind == opad::Ref::Kind::Body ? shape : opad::subshape(shape, kind, i));
        if (d.IsDone() && d.Value() < best) {
          best = d.Value();
          ref = opad::Ref{};
          ref.body = id;
          ref.kind = kind;
          if (kind != opad::Ref::Kind::Body) ref.index = i;
        }
      }
    }
    return best < 1e17;
  }

  void apply(const clips::Input& in) {
    using K = clips::Input::Kind;
    SketchEditor* e = ed();
    // The guide at the hand's actions; a value set in the panel can come any time (a "Set" step a default already fills), a
    // click on the chrome is the card's or chip's input that comes with it.
    if (in.kind != K::Move && in.kind != K::Release && in.kind != K::Row && in.screen.x() < 0 && in.caps != QStringList{"Esc"} && in.text != "esc") guide(in);  // Esc steps back or leaves
    double u = 0, v = 0;
    local(in.at, u, v);
    const bool scene = in.screen.x() < 0;
    if (m_verbose && in.kind != K::Move)  // OPAD_BENCH_CLIPVERBOSE: every input but the moves, as it is applied
      trace::log(QString("bench: clip replay: %1: %2 s: %3 at %4, %5 %6 %7 (tool %8, %9 selected)")
                     .arg(m_id).arg(in.t, 0, 'f', 2).arg(int(in.kind)).arg(u, 0, 'f', 2).arg(v, 0, 'f', 2).arg(in.caps.join('+') + in.text, in.value, e->tool()).arg(e->m_sel.size()));
    switch (in.kind) {
      case K::Move:
        if (m_held) mouse(m_held, QEvent::MouseMove, u, v);  // dragging a handle over the view
        else if (!m_design->pickingPlane()) e->sketchMove(u, v, Qt::NoModifier, in.down);
        noteSnap(in);
        break;
      case K::Press:
        if (!scene) break;
        if ((m_held = handleAt(u, v, m_heldShift))) mouse(m_held, QEvent::MouseButtonPress, u, v);  // the offset's arrow: the press is its
        else e->sketchPress(u, v, Qt::NoModifier);
        break;
      case K::Release:
        if (!scene) break;
        if (m_held) mouse(m_held, QEvent::MouseButtonRelease, u, v);
        else e->sketchRelease(u, v, Qt::NoModifier);
        m_held = nullptr;
        break;
      case K::Click:
        if (!scene) break;  // on the chrome: the card or chip it presses is an input of its own
        if (m_design->pickingPlane()) {  // the plane picker: the face clicked, as a selection in the view
          opad::Ref ref;
          if (!referenceAt(in.at, opad::Ref::Kind::Face, ref)) return fail("the clip's click is on a face");
          view()->selectRefs({ref});
          m_design->planePicker()->selectionChanged();
          break;
        }
        if (clips::iso(m_id) && sketchkeys::referenceTool(e->tool().toStdString())) {
          const QString filter = e->option("projectionPick", "edge");
          const opad::Ref::Kind kind = e->tool() == "intersect_body" || e->tool() == "silhouette" || filter == "body" ? opad::Ref::Kind::Body
                                       : filter == "face" ? opad::Ref::Kind::Face : filter == "vertex" ? opad::Ref::Kind::Vertex : opad::Ref::Kind::Edge;
          opad::Ref ref;
          if (!referenceAt(in.at, kind, ref)) return fail("the clip's click is on a body");
          e->m_replayReference = ref;
        }
        e->sketchMove(u, v, Qt::NoModifier, false);
        e->sketchPress(u, v, Qt::NoModifier);
        e->sketchRelease(u, v, Qt::NoModifier);
        e->m_replayReference.reset();
        break;
      case K::DoubleClick:  // Qt: press, release, double click, release
        e->sketchDoubleClick(u, v);
        e->sketchRelease(u, v, Qt::NoModifier);
        break;
      case K::Key:
        if (!in.value.isEmpty()) trigger(in.value);  // the command's key, whatever it is now
        else key(in.text.isEmpty() ? in.caps : fixedCaps(in.text));
        break;
      case K::Type: type(in.text); break;
      case K::Row: row(in.text, in.value); break;
      case K::Pick: pick(in); break;
      case K::Page: page(in.text); break;
      case K::Button: button(in.text, in.value == "chip"); break;
    }
  }

  // A press the clip makes on a handle's arrow (the offset's, a DimensionHandle drawn over the view): the clip draws the arrow
  // to its own scale, the view at a fixed size in pixels, so a press on the arrow's line beyond its base is a press on the
  // arrow; the drag that follows keeps the clip's distances (the press moved onto the arrow, the moves by as much).
  DimensionHandle* handleAt(double u, double v, QPointF& shift) const {
    const QPointF p = view()->widgetPoint(m_frame.to_world(u, v));
    for (DimensionHandle* h : view()->findChildren<DimensionHandle*>()) {
      if (m_verbose) trace::log(QString("bench: clip replay: %1: handle %2 hidden %3 arrow %4,%5 %6,%7 press %8,%9").arg(m_id).arg(quintptr(h)).arg(h->isHidden())
                                    .arg(h->arrowLine().p1().x()).arg(h->arrowLine().p1().y()).arg(h->arrowLine().p2().x()).arg(h->arrowLine().p2().y()).arg(p.x()).arg(p.y()));
      if (h->isHidden()) continue;
      const QLineF a = h->arrowLine();
      if (a.length() < 1) {
        if (h->grips(p)) return shift = {}, h;
        continue;
      }
      const QPointF d = (a.p2() - a.p1()) / a.length(), r = p - a.p1();
      const double along = QPointF::dotProduct(r, d), across = std::abs(d.x() * r.y() - d.y() * r.x());
      if (across < DimensionHandle::kHitRadius && along > -DimensionHandle::kHitRadius) {
        shift = a.p1() + d * std::clamp(along, 0.0, a.length()) - p;
        return h;
      }
    }
    return nullptr;
  }
  void mouse(QWidget* w, QEvent::Type type, double u, double v) {
    const QPointF at = QPointF(view()->widgetPoint(m_frame.to_world(u, v))) + m_heldShift, local = at - QPointF(w->pos());
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, local, view()->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &event);
  }

  // The Tool guide shows this input: the clip step it falls in is one the guide loops for the step the tool waits for.
  void guide(const clips::Input& in) {
    SketchEditor* e = ed();
    if (!e->active() || "sketch." + QString(e->tool()).replace(':', '.') != m_id) return;  // another tool's guide (or none) shows meanwhile
    const QList<ToolStep> steps = e->toolSteps();
    const int count = int(steps.size());
    const int waiting = int(std::find_if(steps.begin(), steps.end(), [](const ToolStep& s) { return s.picked.isEmpty(); }) - steps.begin());
    const auto [first, last] = clips::guideRange(m_id, waiting, count);
    const int shown = clips::stepAt(m_id, in.t), before = clips::stepAt(m_id, in.t - 0.06);  // a click on a step's end is that step's
    if ((shown >= first && shown <= last) || (before >= first && before <= last)) return;
    fail(QString("the guide: at %1 s the clip shows step %2 (\"%3\"), the tool waits for its step %4 of %5 (\"%6\"), whose guide loops clip steps %7 to %8")
             .arg(in.t, 0, 'f', 2).arg(shown + 1).arg(clips::steps(m_id).value(shown).caption).arg(waiting + 1).arg(count)
             .arg(waiting < count ? steps[waiting].label : QString("done")).arg(first + 1).arg(last + 1));
  }

  // The pointer's snap where the expect block names one ("snaps": [{"at": [x, y], "kind": "endpoint"}]): what the pointer
  // was pulled to as it rested there.
  void noteSnap(const clips::Input& in) {
    for (const QJsonValue& v : m_expect.value("snaps").toArray()) {
      const QJsonArray at = v.toObject().value("at").toArray();
      if (std::hypot(in.at.x() - at.at(0).toDouble(), in.at.y() - at.at(1).toDouble()) > 1e-6) continue;
      static const QHash<QString, snapmarkers::Marker> kinds{{"endpoint", snapmarkers::Marker::Endpoint}, {"midpoint", snapmarkers::Marker::Midpoint},
                                                             {"center", snapmarkers::Marker::Centre}, {"quadrant", snapmarkers::Marker::Quadrant},
                                                             {"intersection", snapmarkers::Marker::Intersection}, {"nearest", snapmarkers::Marker::Nearest},
                                                             {"tangent", snapmarkers::Marker::Tangent}, {"perpendicular", snapmarkers::Marker::Perpendicular}};
      const QString kind = v.toObject().value("kind").toString();
      if (ed()->m_marker && kinds.value(kind, snapmarkers::Marker::Grid) == *ed()->m_marker) m_snapSeen.insert(kind + QString::number(at.at(0).toDouble()) + "," + QString::number(at.at(1).toDouble()));
    }
  }

  // ---- the expect block ------------------------------------------------------------------------------------------------
  static QString kindName(SkEntity::Type t) {
    switch (t) {
      case SkEntity::Type::Point: return "point";
      case SkEntity::Type::Line: return "line";
      case SkEntity::Type::Circle: return "circle";
      case SkEntity::Type::Arc: return "arc";
      case SkEntity::Type::Ellipse: return "ellipse";
      case SkEntity::Type::Spline: return "spline";
    }
    return "?";
  }
  // A count as expected: a number, or [least, most].
  static bool counts(const QJsonValue& want, int have) {
    if (want.isArray()) return have >= want.toArray().at(0).toInt() && have <= want.toArray().at(1).toInt(1 << 30);
    return have == want.toInt(0);
  }
  static QString shown(const QJsonValue& v) { return v.isArray() ? QString("%1 to %2").arg(v.toArray().at(0).toInt()).arg(v.toArray().at(1).toInt()) : QString::number(v.toInt()); }

  void compare() {
    SketchEditor* e = ed();
    const QJsonObject& x = m_expect;
    if (x.contains("active")) check(e->active() == x.value("active").toBool(), x.value("active").toBool() ? "the sketch is still open" : "the sketch was left");
    if (x.contains("sketches"))
      check(int(e->m_doc->scene.sketches.size()) - m_sketches == x.value("sketches").toInt(), QString("the clip adds %1 sketches to the document (%2)").arg(x.value("sketches").toInt()).arg(int(e->m_doc->scene.sketches.size()) - m_sketches));
    if (!e->active()) return;
    const Sketch& sk = e->m_sk;
    {  // what the tool made, for the log (an expect block is written from what the clip shows, then read against this)
      std::map<std::string, int> kinds, constraints;
      QStringList dims;
      for (const SkEntity& c : sk.entities) ++kinds[(c.construction ? "construction " : "") + kindName(c.type).toStdString() + (c.source.is_null() ? "" : " linked")];
      for (const SkConstraint& c : sk.constraints) {
        if (c.is_dimension() && !c.reference) dims << QString::number(c.type == SkConstraint::Type::Angle ? c.value * 180 / M_PI : c.value, 'g', 6);
        else ++constraints[SkConstraint::type_name(c.type)];
      }
      QStringList made;
      for (const auto& [k, n] : kinds) made << QString("%1 %2").arg(n).arg(QString::fromStdString(k));
      QStringList held;
      for (const auto& [k, n] : constraints) held << QString("%1 %2").arg(n).arg(QString::fromStdString(k));
      for (const SkEntity& c : sk.entities) {
        const SkPoint* o = c.p.empty() ? nullptr : sk.point(c.p[0]);
        if (c.type == SkEntity::Type::Line && o && sk.entities.size() <= 8)
          made << QString("line (%1, %2) (%3, %4)").arg(o->x, 0, 'f', 2).arg(o->y, 0, 'f', 2).arg(sk.point(c.p[1])->x, 0, 'f', 2).arg(sk.point(c.p[1])->y, 0, 'f', 2);
        if (c.type == SkEntity::Type::Point && o) made << QString("point (%1, %2)").arg(o->x, 0, 'f', 2).arg(o->y, 0, 'f', 2);
        if (c.type == SkEntity::Type::Circle && o) made << QString("circle (%1, %2) r %3").arg(o->x, 0, 'f', 2).arg(o->y, 0, 'f', 2).arg(c.r, 0, 'f', 2);
        if (c.type == SkEntity::Type::Arc && o) {
          const SkPoint *a = sk.point(c.p[1]), *b = sk.point(c.p[2]);
          const double from = std::atan2(a->y - o->y, a->x - o->x) * 180 / M_PI, to = std::atan2(b->y - o->y, b->x - o->x) * 180 / M_PI;
          made << QString("arc (%1, %2) r %3 from %4 through %5").arg(o->x, 0, 'f', 2).arg(o->y, 0, 'f', 2).arg(std::hypot(a->x - o->x, a->y - o->y), 0, 'f', 2)
                      .arg(from, 0, 'f', 1).arg(std::fmod(to - from + 720, 360), 0, 'f', 1);
        }
      }
      trace::log(QString("bench: clip replay: %1: made %2; constraints %3; dimensions %4; selected %5; tool %6; status \"%7\"")
                     .arg(m_id, made.join(", "), held.join(", "), dims.join(", "), QString::number(e->m_sel.size()), e->tool(), m_status));
    }
    std::map<QString, int> made, construction;
    int linked = 0;
    for (const SkEntity& c : sk.entities) {
      ++(c.construction ? construction : made)[kindName(c.type)];
      linked += !c.source.is_null();
    }
    if (x.contains("entities") || x.contains("construction")) {  // every kind: one not named is none
      for (const auto& [block, have] : {std::pair{"entities", &made}, std::pair{"construction", &construction}}) {
        const QJsonObject want = x.value(block).toObject();
        QStringList wrong, all;
        for (const char* k : {"point", "line", "circle", "arc", "ellipse", "spline"}) {
          const int n = have->count(k) ? have->at(k) : 0;
          if (n) all << QString("%1 %2").arg(n).arg(k);
          if (!counts(want.value(k), n)) wrong << QString("%1 %2 (expected %3)").arg(n).arg(k).arg(shown(want.value(k)));
        }
        check(wrong.isEmpty(), QString("%1: %2").arg(block, wrong.isEmpty() ? (all.isEmpty() ? QString("none") : all.join(", ")) : wrong.join(", ")));
      }
    }
    if (x.contains("linked")) check(counts(x.value("linked"), linked), QString("%1 linked curves (expected %2)").arg(linked).arg(shown(x.value("linked"))));
    if (x.contains("constraints")) {
      const QJsonObject want = x.value("constraints").toObject();
      for (auto it = want.begin(); it != want.end(); ++it) {
        int n = 0;
        for (const SkConstraint& c : sk.constraints) n += it.key() == SkConstraint::type_name(c.type);
        check(counts(it.value(), n), QString("%1 %2 constraints (expected %3)").arg(n).arg(it.key(), shown(it.value())));
      }
    }
    if (x.contains("dimensions")) {
      QList<double> have, want;
      for (const SkConstraint& c : sk.constraints)
        if (c.is_dimension() && !c.reference) have << (c.type == SkConstraint::Type::Angle ? c.value * 180 / M_PI : c.value);
      for (const QJsonValue& v : x.value("dimensions").toArray()) want << v.toDouble();
      std::sort(have.begin(), have.end());
      std::sort(want.begin(), want.end());
      bool same = have.size() == want.size();
      for (qsizetype i = 0; same && i < have.size(); ++i) same = std::abs(have[i] - want[i]) < 0.01;
      QStringList h;
      for (double d : have) h << QString::number(d, 'g', 6);
      check(same, QString("driving dimensions %1").arg(h.isEmpty() ? QString("none") : h.join(", ")));
    }
    auto within = [](const SkPoint* p, const QJsonArray& at, double tol = 0.05) { return p && std::hypot(p->x - at.at(0).toDouble(), p->y - at.at(1).toDouble()) < tol; };
    for (const QJsonValue& v : x.value("arcs").toArray()) {  // centre, radius, and the way it runs: counter-clockwise from start through sweep
      const QJsonObject a = v.toObject();
      const bool found = std::any_of(sk.entities.begin(), sk.entities.end(), [&](const SkEntity& c) {
        if (c.type != SkEntity::Type::Arc) return false;
        const SkPoint *o = sk.point(c.p[0]), *s = sk.point(c.p[1]), *t = sk.point(c.p[2]);
        if (!within(o, a.value("center").toArray()) || !s || !t) return false;
        const double r = std::hypot(s->x - o->x, s->y - o->y);
        if (a.contains("r") && std::abs(r - a.value("r").toDouble()) > 0.05) return false;
        const double from = std::atan2(s->y - o->y, s->x - o->x) * 180 / M_PI, to = std::atan2(t->y - o->y, t->x - o->x) * 180 / M_PI;
        const double sweep = std::fmod(to - from + 720, 360);
        if (a.contains("sweep") && std::abs(sweep - a.value("sweep").toDouble()) > 0.5) return false;
        return !a.contains("start") || std::abs(std::remainder(from - a.value("start").toDouble(), 360.0)) < 0.5;
      });
      check(found, QString("an arc about %1, %2, radius %3, from %4 through %5 degrees counter-clockwise")
                       .arg(a.value("center").toArray().at(0).toDouble()).arg(a.value("center").toArray().at(1).toDouble()).arg(a.value("r").toDouble())
                       .arg(a.value("start").toDouble()).arg(a.value("sweep").toDouble()));
    }
    for (const QJsonValue& v : x.value("circles").toArray()) {
      const QJsonObject c = v.toObject();
      const bool found = std::any_of(sk.entities.begin(), sk.entities.end(), [&](const SkEntity& k) {
        return k.type == SkEntity::Type::Circle && within(sk.point(k.p[0]), c.value("center").toArray()) && (!c.contains("r") || std::abs(k.r - c.value("r").toDouble()) < 0.05);
      });
      check(found, QString("a circle about %1, %2, radius %3").arg(c.value("center").toArray().at(0).toDouble()).arg(c.value("center").toArray().at(1).toDouble()).arg(c.value("r").toDouble()));
    }
    for (const QJsonValue& v : x.value("points").toArray()) {
      const QJsonArray at = v.toArray();
      check(std::any_of(sk.points.begin(), sk.points.end(), [&](const SkPoint& p) { return within(&p, at); }),
            QString("a point at %1, %2").arg(at.at(0).toDouble()).arg(at.at(1).toDouble()));
    }
    if (x.contains("selected")) check(int(e->m_sel.size()) == x.value("selected").toInt(), QString("%1 selected (expected %2)").arg(e->m_sel.size()).arg(x.value("selected").toInt()));
    if (x.contains("tool")) check(e->tool() == x.value("tool").toString(), "the tool is " + x.value("tool").toString() + " (" + e->tool() + ")");
    if (x.contains("images")) check(int(sk.images.size()) == x.value("images").toInt(), QString("%1 backdrop pictures").arg(sk.images.size()));
    if (x.contains("image") && !sk.images.empty()) {
      const opad::json& image = sk.images.back();
      const QJsonObject want = x.value("image").toObject();
      const double values[] = {image.at("position")[0].get<double>(), image.at("position")[1].get<double>(), image.at("width").get<double>(),
                               image.value("angle", 0.0) * 180 / M_PI, image.value("opacity", 0.5)};
      const char* names[] = {"x", "y", "width", "angle", "opacity"};
      for (int i = 0; i < 5; ++i)
        if (want.contains(names[i])) check(std::abs(values[i] - want.value(names[i]).toDouble()) < 0.02, QString("the picture's %1 is %2").arg(names[i]).arg(values[i]));
    }
    if (x.contains("page")) {
      auto* tabs = panel() ? panel()->findChild<QTabWidget*>() : nullptr;
      check(tabs && plain(tabs->tabText(tabs->currentIndex())) == x.value("page").toString(), "the panel shows its " + x.value("page").toString() + " page");
    }
    if (x.contains("rings")) check(int(e->m_dangling.size()) == x.value("rings").toInt(), QString("%1 open ends ringed").arg(e->m_dangling.size()));
    for (const QJsonValue& v : x.value("snaps").toArray()) {
      const QString kind = v.toObject().value("kind").toString();
      const QJsonArray at = v.toObject().value("at").toArray();
      check(m_snapSeen.contains(kind + QString::number(at.at(0).toDouble()) + "," + QString::number(at.at(1).toDouble())),
            QString("the pointer at %1, %2 snaps to the %3").arg(at.at(0).toDouble()).arg(at.at(1).toDouble()).arg(kind));
    }
    if (x.contains("plane")) {
      const QJsonArray n = x.value("plane").toObject().value("normal").toArray(), o = x.value("plane").toObject().value("origin").toArray();
      const opad::Vec3 have = e->frame().normal(), at = e->frame().origin;
      check(std::abs(have[0] - n.at(0).toDouble()) + std::abs(have[1] - n.at(1).toDouble()) + std::abs(have[2] - n.at(2).toDouble()) < 1e-6,
            QString("the sketch plane faces %1, %2, %3").arg(have[0]).arg(have[1]).arg(have[2]));
      if (!o.isEmpty())
        check(std::abs(at[0] - o.at(0).toDouble()) + std::abs(at[1] - o.at(1).toDouble()) + std::abs(at[2] - o.at(2).toDouble()) < 1e-3,
              QString("the sketch origin is at %1, %2, %3").arg(at[0]).arg(at[1]).arg(at[2]));
    }
    if (x.contains("file")) {
      const QFileInfo file(e->option("vectorFile"));
      check(file.exists() && file.size() > 0, "the file " + file.fileName() + " was written");
    }
    if (x.contains("weights")) {  // the first rational spline's weights
      QStringList have;
      bool same = false;
      for (const SkEntity& c : sk.entities)
        if (c.type == SkEntity::Type::Spline && !c.weights.empty()) {
          const QJsonArray want = x.value("weights").toArray();
          same = int(c.weights.size()) == want.size();
          for (size_t i = 0; i < c.weights.size(); ++i) {
            have << QString::number(c.weights[i], 'g', 4);
            same = same && std::abs(c.weights[i] - want.at(int(i)).toDouble()) < 1e-6;
          }
          break;
        }
      check(same, "the spline's weights are " + have.join(", "));
    }
    if (x.contains("dark") || x.contains("light")) {  // what the view shows there: a backdrop the right way up (sketch points)
      const QImage shot = view()->grabImage();
      const double k = shot.width() / double(std::max(1, view()->width()));
      for (const char* shade : {"dark", "light"})
        for (const QJsonValue& v : x.value(shade).toArray()) {
          const QJsonArray at = v.toArray();
          const QPoint p = view()->widgetPoint(m_frame.to_world(at.at(0).toDouble(), at.at(1).toDouble()));
          const int grey = qGray(shot.pixel(std::clamp(int(p.x() * k), 0, shot.width() - 1), std::clamp(int(p.y() * k), 0, shot.height() - 1)));
          // A light picture is drawn a little greyer than it is (lit); a dark one, or a curve's fill over it, stays dark.
          check(std::string(shade) == "dark" ? grey < 110 : grey > 130, QString("the view is %1 at %2, %3 (grey %4)").arg(shade).arg(at.at(0).toDouble()).arg(at.at(1).toDouble()).arg(grey));
        }
    }
    if (x.contains("status")) check(m_status.contains(x.value("status").toString()), "the status says \"" + x.value("status").toString() + "\" (" + m_status + ")");
  }

  MainWindow& m_window;
  DesignController* m_design;
  std::function<QAction*(const QString&)> m_action;
  QStringList m_ids, m_failed;
  QString m_shots, m_id, m_status;
  QJsonObject m_setup, m_expect;
  QList<clips::Input> m_events;
  QSet<QString> m_snapSeen;
  QSet<int> m_answered;
  opad::Frame m_frame;
  QTimer m_timer;
  Phase m_phase = Phase::Next;
  int m_clip = -1, m_next = 0, m_ticks = 0, m_wait = 0, m_settled = 0, m_modal = 0, m_sketches = 0;
  bool m_ok = true, m_clipOk = true, m_inTick = false, m_reopening = false;
  QPointer<QWidget> m_held;  // the handle a drag holds
  QPointF m_heldShift;      // where on it the press went, from where the clip pressed (view pixels)
  const bool m_verbose = qEnvironmentVariableIsSet("OPAD_BENCH_CLIPVERBOSE");
};

OPAD_BENCH(OPAD_BENCH_CLIPREPLAY, clipReplay) {
  QStringList ids;
  for (const QString& id : value.split(',', Qt::SkipEmptyParts)) {
    if (!id.endsWith(".*")) {
      ids << id.trimmed();
      continue;
    }
    for (const QString& clip : clips::ids())
      if (clip.startsWith(id.chopped(1)) && !clips::expect(clip).isEmpty() && !clips::setup(clip).contains("bodies")) ids << clip;  // bodies: a case of its own
  }
  // The clips that leave the sketch or move it (finish, cancel, a new plane) last: the others share the sketch on XY.
  std::stable_partition(ids.begin(), ids.end(), [](const QString& id) { return clips::expect(id).value("active").toBool(true) && !clips::expect(id).contains("plane"); });
  w.setWorkspace("design");
  auto* replay = new ClipReplay(w, w.m_design, [&w](const QString& id) { return w.action(id); }, ids, qEnvironmentVariable("OPAD_BENCH_CLIPSHOT"));
  w.m_design->benchSketch([replay] { replay->start(); });
  QTimer::singleShot(230000, qApp, [] { trace::log("bench: clip replay: ended in time FAIL"); QCoreApplication::exit(2); });
  return true;
}
