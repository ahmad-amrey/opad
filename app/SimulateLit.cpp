// The Simulation folder's rows in the browser: a thermal study's setup (its box, boards, heatsinks and air) as rows of their own,
// and what a picked row stands for lit in the view in the colour of its nature, with its badge (a heat source red with a
// flame, a fan blue, a heatsink steel, a board green, the box sand, a support purple, a force orange). A hidden part is not lit.
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>
#include <TopoDS_Edge.hxx>

#include <QBitmap>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>

#include <map>
#include <set>

#include "AppDocument.hpp"
#include "BodyShape.hpp"
#include "DesignController.hpp"
#include "SimulateCooling.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "SimulateArea.hpp"
#include "Tracking.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/materials.hpp"
#include "opad/sim/cfd.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_geom.hpp"

namespace {
using opad::json;

// A round native child over the view (it cannot be translucent), the nature's icon in white on its colour.
class Badge : public QFrame {
 public:
  Badge(QWidget* parent, const QColor& color, const QString& icon) : QFrame(parent) {
    setObjectName("simBadge");
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(24, 24);
    setStyleSheet(QString("#simBadge { background: %1; border: none; }").arg(color.name()));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* label = new QLabel(this);
    label->setPixmap(icons::pixmap(icon, Qt::white, 16, devicePixelRatioF()));
    layout->addWidget(label);
    QBitmap mask(size());
    mask.fill(Qt::color0);
    QPainter p(&mask);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::color1);
    p.drawEllipse(QRectF(0, 0, width(), height()));
    p.end();
    setMask(mask);
  }
};

constexpr size_t kMaxBadges = 40;  // a heat source on hundreds of parts: every one lit, the first ones badged
}  // namespace

QColor Simulate::natureColor(const std::string& nature) {
  if (nature == "heat") return QColor("#e5484d");
  if (nature == "temperature") return QColor("#d6409f");
  if (nature == "convection" || nature == "natural") return QColor("#29a3c2");
  if (nature == "radiation") return QColor("#f5a623");
  if (nature == "fan") return QColor("#3e63dd");
  if (nature == "heatsink") return QColor("#7c8a9e");
  if (nature == "board") return QColor("#30a46c");
  if (nature == "box") return QColor("#b8956a");
  if (nature == "fixed" || nature == "displacement") return QColor("#8e4ec6");
  return QColor("#f76b15");  // force, pressure, moment, gravity, a bolt's preload
}

QString Simulate::natureIcon(const std::string& nature) {
  if (nature == "heat") return "simHeat";
  if (nature == "temperature") return "simTemperature";
  if (nature == "convection") return "simConvection";
  if (nature == "natural") return "simNatural";
  if (nature == "radiation") return "simRadiation";
  if (nature == "fan") return "simFan";
  if (nature == "heatsink") return "simHeatsink";
  if (nature == "board") return "simBoard";
  if (nature == "box") return "simBox";
  if (nature == "fixed" || nature == "displacement") return "simFixed";
  if (nature == "pressure") return "simPressure";
  if (nature == "bolt_preload") return "simBolt";
  if (nature == "gravity") return "simGravity";
  return "simForce";
}

std::vector<browser::Item> Simulate::setupRows(const opad::Study& st) const {
  std::vector<browser::Item> rows;
  const AppDocument* doc = services().document();
  const opad::Scene& s = doc->scene;
  const json set = st.def.value("settings", json::object());
  if (set.value("air", std::string()) != "cfd") return rows;
  const json cfd = set.value("cfd", json::object());
  const json mats = set.value("materials", json::object());
  const std::string prefix = "sim:x:" + st.id + ":";
  const std::string loadCase = set.value("case", std::string());
  auto name = [&](const std::string& id) { return s.node(id) ? doc->nodeName(id) : QString::fromStdString(id); };
  const json e = cfd.value("enclosure", json());
  QStringList walls;
  for (const auto& w : e.is_array() ? e : json::array({e}))
    if (w.is_string()) walls << name(w.get<std::string>());
  if (!walls.isEmpty())
    rows.push_back({prefix + "box", tr("Box: %1").arg(walls.join(" + ")), "simBox", tr("The box the air is solved in, with a margin of the room around it: its holes are the vents"), {}});
  for (const auto& [id, m] : mats.items())
    if (m.is_object() && m.contains("pcb"))
      rows.push_back({prefix + "board:" + id, tr("Board: %1 · %n copper layer(s)", "", m["pcb"].value("layers", 4)).arg(name(id)), "simBoard",
                      tr("A printed circuit board: its copper spreads the heat along it far better than through it"), {}});
  int fans = 0;
  std::set<std::string> sinks;
  for (const auto& l : s.loads) {
    if (l.load_case != loadCase || l.kind != "fan") continue;
    ++fans;
    for (const auto& h : l.def.value("heatsink", json::array())) {
      if (!h.is_string() || !sinks.insert(h.get<std::string>()).second) continue;
      const std::string id = h.get<std::string>();
      const opad::Node* n = s.node(id);
      const std::string first = n && n->kind != opad::Node::Kind::Body && !s.bodies_under(id).empty() ? s.bodies_under(id).front() : id;
      QString text = tr("Heatsink: %1").arg(name(id));
      if (const opad::Material* m = opad::material(mats.value(first, json::object()).value("material", std::string()))) text += " · " + i18n::t(QString::fromStdString(m->name));
      rows.push_back({prefix + "sink:" + id, text, "simHeatsink", tr("What a fan blows through: its fins get fine cells"), {}});
    }
  }
  const bool natural = cfd.value("buoyancy", false) && fans == 0;
  rows.push_back({prefix + "air", natural ? tr("Air: natural convection") : tr("Air: moved by %n fan(s)", "", fans), natural ? "simNatural" : "simFan",
                  natural ? tr("No fans: the air rises where the parts warm it and leaves through the vents") : tr("The fans' loads are in the case's folder"), {}});
  return rows;
}

void Simulate::addTarget(std::vector<Lit>& out, const std::string& row, const opad::Ref& r, const std::string& nature) const {
  if (r.body.empty()) return;
  const opad::Scene& s = services().document()->scene;
  const opad::Node* n = s.node(r.body);
  if (n && n->kind != opad::Node::Kind::Body) {  // a component (a fan's own model): each of its bodies
    for (const auto& b : s.bodies_under(r.body)) {
      opad::Ref body;
      body.body = b;
      out.push_back({row, nature, body});
    }
    return;
  }
  out.push_back({row, nature, r});
}

std::vector<Simulate::Lit> Simulate::rowTargets(const std::string& row) const {
  std::vector<Lit> out;
  const opad::Scene& s = services().document()->scene;
  auto add = [&](const opad::Ref& r, const std::string& nature) { addTarget(out, row, r, nature); };
  auto body = [](const std::string& id) {
    opad::Ref r;
    r.body = id;
    return r;
  };
  auto ofLoad = [&](const opad::Load& l) {
    for (const auto& r : l.refs) add(r, l.kind);
    if (l.kind == "fan" && l.refs.empty())  // a fan by its openings: a face among them lit as the fan
      for (const char* k : {"outlet", "inlet"})
        if (const json o = l.def.value(k, json()); o.is_object() && o.contains("body")) try {
            add(opad::Ref::from_json(o), "fan");
          } catch (const std::exception&) {
          }
  };
  if (row.rfind("sim:l:", 0) == 0) {
    if (const opad::Load* l = s.load(row.substr(6))) ofLoad(*l);
  } else if (row.rfind("sim:c:", 0) == 0) {
    for (const auto& l : s.loads)
      if (l.load_case == row.substr(6)) ofLoad(l);
  } else if (row.rfind("sim:x:", 0) == 0) {
    const std::string rest = row.substr(6);
    const size_t colon = rest.find(':');
    const opad::Study* st = s.study(rest.substr(0, colon));
    if (!st || colon == std::string::npos) return out;
    const std::string what = rest.substr(colon + 1);
    const json cfd = st->def.value("settings", json::object()).value("cfd", json::object());
    if (what == "box" || what == "air") {  // the air: where it moves, the box
      const json e = cfd.value("enclosure", json());
      for (const auto& w : e.is_array() ? e : json::array({e}))
        if (w.is_string()) add(body(w.get<std::string>()), what == "box" ? "box" : cfd.value("buoyancy", false) ? "natural" : "fan");
    } else if (what.rfind("board:", 0) == 0) {
      add(body(what.substr(6)), "board");
    } else if (what.rfind("sink:", 0) == 0) {
      add(body(what.substr(5)), "heatsink");
    }
  }
  return out;
}

void Simulate::lightRows(const std::vector<std::string>& rows) {
  m_litRefs.clear();
  std::vector<Lit> targets;
  std::vector<json> fans;  // a fan's row (or its case's): its plane and the air's way too
  const opad::Scene& s = services().document()->scene;
  for (const auto& row : rows) {
    const auto more = rowTargets(row);
    targets.insert(targets.end(), more.begin(), more.end());
    for (const auto& l : s.loads)
      if (l.kind == "fan" && (row == "sim:l:" + l.id || row == "sim:c:" + l.load_case)) fans.push_back(l.def);
  }
  lightTargets(std::move(targets));
  showFanGlyphs(fans);
}

// The plane a fan pushes the air through, as the solver will take it (cfd.cpp): its outlet as set, else a disk through the
// middle of what it is on, 0.47 of its width across (its body's box here: near enough to show), square to its way.
void Simulate::showFanGlyphs(const std::vector<json>& fans) {
  m_glyphFans = fans;
  Viewport* view = services().viewport();
  const AppDocument* doc = services().document();
  if (!view || !doc) return;
  std::vector<Viewport::SimGlyph> glyphs;
  auto unit = [](opad::Vec3 v) {
    const double l = std::hypot(v[0], v[1], v[2]);
    return l > 1e-12 ? opad::Vec3{v[0] / l, v[1] / l, v[2] / l} : opad::Vec3{0, 0, 0};
  };
  // A fan load drawn by its openings (an outlet, a blower's inlet: a sketch's region or a face): their outlines, read on a
  // worker once (m_openings), drawn as the setup draws them.
  std::vector<json> expanded;
  std::vector<json> unread;
  if (doc->revision != m_openingsRevision) m_openings.clear(), m_openingsRevision = doc->revision;  // a sketch changed: read again
  for (const json& f : fans) {
    const json out = f.value("outlet", json());
    if (f.contains("loops") || !(out.is_string() || (out.is_object() && !out.contains("centre")))) {
      expanded.push_back(f);
      continue;
    }
    const bool blower = f.value("type", std::string()) == "blower";
    const json in = blower ? f.value("inlet", json()) : json();
    auto known = [&](const json& ref) {
      const auto it = m_openings.find(ref.dump());
      if (it == m_openings.end() && !m_reading.count(ref.dump())) unread.push_back(ref);
      return it == m_openings.end() ? json() : it->second;
    };
    const json o = known(out), i = in.is_null() ? json() : known(in);
    if (o.is_null()) continue;
    const opad::Vec3 oc = o["centre"], on = o["normal"];
    opad::Vec3 way = f.value("vector", on);
    if (blower && !i.is_null()) {  // out of its side, away from its inlet; in through its inlet
      const opad::Vec3 ic = i["centre"];
      opad::Vec3 in_n = i["normal"];
      way = on;
      if (way[0] * (oc[0] - ic[0]) + way[1] * (oc[1] - ic[1]) + way[2] * (oc[2] - ic[2]) < 0) way = {-way[0], -way[1], -way[2]};
      if (in_n[0] * (ic[0] - oc[0]) + in_n[1] * (ic[1] - oc[1]) + in_n[2] * (ic[2] - oc[2]) < 0) in_n = {-in_n[0], -in_n[1], -in_n[2]};
      if (f.value("flip_outlet", false)) way = {-way[0], -way[1], -way[2]};  // turned by Flip
      if (f.value("flip_inlet", false)) in_n = {-in_n[0], -in_n[1], -in_n[2]};
      expanded.push_back({{"loops", i["loops"]}, {"centre", ic}, {"vector", opad::Vec3{-in_n[0], -in_n[1], -in_n[2]}}, {"inward", true}});
    }
    expanded.push_back({{"loops", o["loops"]}, {"centre", oc}, {"vector", way}});
  }
  if (!unread.empty() && services().jobs()) {
    for (const auto& r : unread) m_reading.insert(r.dump());
    auto docCopy = std::make_shared<const opad::Document>(doc->doc);
    auto scene = std::make_shared<const opad::Scene>(doc->scene);
    auto read = std::make_shared<std::map<std::string, json>>();
    services().jobs()->async(tr("Reading the fans' openings"), [docCopy, scene, unread, read](Progress) {
      for (const auto& r : unread) try {
          const opad::sim::FanProfile p = opad::sim::fan_profile(*docCopy, *scene, r);
          (*read)[r.dump()] = {{"loops", p.loops}, {"centre", p.centre}, {"normal", p.normal}};
        } catch (const std::exception&) {
        }
    }, [this, read, unread](bool, const QString&) {
      for (const auto& r : unread) m_reading.erase(r.dump());
      for (const auto& [k, v] : *read) m_openings[k] = v;
      if (!read->empty()) showFanGlyphs(m_glyphFans);  // drawn now
    });
  }
  for (const json& f : expanded) {
    Viewport::SimGlyph g;
    g.color = natureColor("convection");  // the air's cyan: apart from the fan body's blue under it
    const opad::Vec3 way = unit(f.value("vector", opad::Vec3{1, 0, 0}));
    if (const json loops = f.value("loops", json()); loops.is_array() && !loops.empty()) {  // a drawn opening, the air through it
      g.loops = loops.get<std::vector<std::vector<opad::Vec3>>>();
      g.centre = f.value("centre", opad::Vec3{0, 0, 0}), g.normal = way, g.inward = f.value("inward", false);
    } else if (const json o = f.value("outlet", json()); o.is_object()) {
      opad::Vec3 n = unit(o.value("normal", way));
      if (n[0] * way[0] + n[1] * way[1] + n[2] * way[2] < 0) n = {-n[0], -n[1], -n[2]};
      g.centre = o.value("centre", opad::Vec3{0, 0, 0}), g.normal = n, g.x = o.value("x", opad::Vec3{1, 0, 0});
      g.width = o.value("width", 0.0), g.height = o.value("height", 0.0);
    } else {
      Bnd_Box box;
      for (const auto& r : f.value("refs", json::array())) {
        const std::string id = r.is_string() ? r.get<std::string>() : r.value("body", std::string());
        try {
          if (doc->scene.node(id)) box.Add(opad::node_world_bbox(doc->doc, doc->scene, id));
        } catch (const std::exception&) {
        }
      }
      if (box.IsVoid()) continue;
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      const opad::Vec3 half{0.5 * (x1 - x0), 0.5 * (y1 - y0), 0.5 * (z1 - z0)};
      const opad::Vec3 u1 = unit(std::fabs(way[0]) < 0.9 ? opad::Vec3{0, way[2], -way[1]} : opad::Vec3{-way[2], 0, way[0]});
      const opad::Vec3 u2 = unit({way[1] * u1[2] - way[2] * u1[1], way[2] * u1[0] - way[0] * u1[2], way[0] * u1[1] - way[1] * u1[0]});
      auto across = [&](const opad::Vec3& u) { return 2 * (std::fabs(u[0]) * half[0] + std::fabs(u[1]) * half[1] + std::fabs(u[2]) * half[2]); };
      g.centre = {0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.5 * (z0 + z1)}, g.normal = way, g.x = u1;
      g.round = true;
      g.width = 0.94 * std::min(across(u1), across(u2));
    }
    glyphs.push_back(g);
  }
  view->showSimGlyphs(glyphs);
}

void Simulate::showCheckOpenings(const std::vector<json>& openings) {
  Viewport* view = services().viewport();
  if (!view) return;
  std::vector<json> fans;
  std::vector<Viewport::Candidate> arrows;
  for (const json& o : openings) {
    fans.push_back({{"loops", o.value("loops", json::array())}, {"centre", o["centre"]}, {"vector", o["vector"]}, {"inward", o.value("inward", false)}});
    // The arrow as the glyph draws it (twice the opening's size, coming to an inlet, leaving an outlet), a rod to click.
    const opad::Vec3 c = o["centre"], v = o["vector"];
    gp_Vec n(v[0], v[1], v[2]);
    const double size = std::max(1.0, o.value("size", 0.0));
    if (n.Magnitude() < 1e-9) continue;
    n.Normalize();
    const gp_Pnt centre(c[0], c[1], c[2]);
    const gp_Pnt from = o.value("inward", false) ? centre.Translated(n * (-2 * size)) : centre;
    try {
      arrows.push_back({o.value("id", std::string()), BRepPrimAPI_MakeCylinder(gp_Ax2(from, gp_Dir(n)), 0.06 * size, 2 * size).Shape()});
    } catch (const Standard_Failure&) {
    }
  }
  showFanGlyphs(fans);
  if (!arrows.empty()) view->showCandidates(arrows);
  else if (m_checkArrows) view->clearCandidates();
  m_checkArrows = arrows.size();
}

// A fan's openings are picked among the sketches' closed regions (found on a worker, as a feature's profiles input finds
// them) or the faces under them.
void Simulate::showProfiles(bool on) {
  Viewport* view = services().viewport();
  if (Job* j = std::exchange(m_profileJob, nullptr)) j->cancel();
  if (!view) return;
  if (!on || !services().jobs() || !services().document()) return view->clearCandidates();
  // Each closed region of a shown sketch, and each of its curves standing for the smallest region it bounds: a click on a
  // curve takes that closed loop (a slot drawn inside a face's projected outline is picked by its own edge, not the ring
  // around it). Kernel work: on a worker, from copies of the sketches.
  struct Source {
    std::string id;
    opad::json geometry;
    opad::Frame frame;
  };
  auto sources = std::make_shared<std::vector<Source>>();
  for (const auto& s : services().document()->scene.sketches)
    if (s.visible || !s.consumed) sources->push_back({s.id, s.geometry, s.frame});
  auto found = std::make_shared<std::vector<Viewport::Candidate>>();
  m_profileJob = services().jobs()->async(tr("Finding the sketches' profiles"), [sources, found](Progress p) {
    using namespace opad::design;
    for (const auto& src : *sources) {
      if (p.cancelled()) return;
      const Sketch sk = Sketch::from_json(src.geometry);
      const std::vector<Region> regions = sketch_regions(sk, src.frame);
      std::vector<std::string> ids;
      for (const auto& r : regions) {
        BRepMesh_IncrementalMesh(r.face, 0.05, Standard_False, 0.3, Standard_False);
        Bnd_Box box;
        BRepBndLib::Add(r.face, box);
        ids.push_back(opad::json{{"sketch", src.id}, {"at", {r.u, r.v}}, {"boundary", r.boundary}}.dump());
        found->push_back({ids.back(), r.face, true, BodyPrs::build(r.face, box, true)});
      }
      for (const auto& e : sk.entities) {
        if (e.construction || e.type == SkEntity::Type::Point) continue;
        int best = -1;  // the smallest region this curve bounds
        for (size_t k = 0; k < regions.size(); ++k)
          if (std::any_of(regions[k].boundary.begin(), regions[k].boundary.end(), [&](int b) { return std::abs(b) == e.id; }) &&
              (best < 0 || regions[k].area < regions[size_t(best)].area))
            best = int(k);
        if (best < 0) continue;  // an open curve: no loop to take
        const TopoDS_Edge edge = entity_edge(sk, e, src.frame);
        if (edge.IsNull()) continue;
        Viewport::Candidate c{opad::json{{"sketch", src.id}, {"entity", e.id}, {"region", opad::json::parse(ids[size_t(best)])}}.dump(), edge, true};
        c.priority = 6;  // over the region faces it lies on
        found->push_back(c);
      }
    }
  }, [this, found](bool ok, const QString&) {
    m_profileJob = nullptr;
    trace::log(QString("simulate: fan openings: %1 sketch profile(s) and curve(s) offered").arg(found->size()));
    auto* setup = services().window()->findChild<CoolingAssistant*>();
    if (!ok || !setup || !setup->picking()) return;
    if (Viewport* v = services().viewport()) v->showCandidates(*found);
    setup->reselect();  // the opening already picked shown selected among them
  });
}

// Draw…: Design's New sketch (its plane picked: the fan's face), drawn there (Project its edges, the opening inside them),
// Finish; then back to the Thermal setup's Air page with that sketch's opening (its smallest region) taken.
void Simulate::drawOpening(int fan, bool inlet) {
  DesignController* design = services().design();
  AppDocument* doc = services().document();
  if (!design || !doc || !services().action("design.sketch")) return;
  auto before = std::make_shared<std::set<std::string>>();
  for (const auto& k : doc->scene.sketches) before->insert(k.id);
  auto watch = std::make_shared<QMetaObject::Connection>();
  *watch = connect(design, &DesignController::stateChanged, this, [this, design, doc, before, watch, fan, inlet] {
    if (design->sketchActive() || design->pickingPlane()) return;  // still choosing its plane, or drawing
    QObject::disconnect(*watch);
    std::string made;
    for (const auto& k : doc->scene.sketches)
      if (!before->count(k.id)) made = k.id;
    services().setWorkspace("simulate");
    openCooling(2);
    if (auto* setup = services().window()->findChild<CoolingAssistant*>(); setup && !made.empty()) setup->setProfile(fan, inlet, {{"sketch", made}});
  });
  services().setWorkspace("design");
  services().action("design.sketch")->trigger();
  if (!design->pickingPlane() && !design->sketchActive()) {  // it did not start
    QObject::disconnect(*watch);
    services().setWorkspace("simulate");
    openCooling(2);
  }
}

size_t Simulate::fanGlyphsShown() const { return services().viewport() ? services().viewport()->simGlyphCount() : 0; }

void Simulate::lightRefs(const std::vector<std::pair<opad::Ref, std::string>>& refs) {
  m_litRefs = refs;
  std::vector<Lit> targets;
  for (const auto& [r, nature] : refs) addTarget(targets, "setup", r, nature);
  lightTargets(std::move(targets));
}

void Simulate::lightTargets(std::vector<Lit> targets) {
  Viewport* view = services().viewport();
  if (!view) return;
  view->clearSimTargets();
  for (auto& [at, badge] : std::exchange(m_badges, {})) delete badge;
  m_lit.clear();
  for (Lit t : targets) {
    opad::Vec3 centre{0, 0, 0};
    t.shown = view->showSimTarget(t.target, natureColor(t.nature), &centre);
    if (t.shown && m_badges.size() < kMaxBadges) m_badges.push_back({centre, new Badge(view, natureColor(t.nature), natureIcon(t.nature))});
    m_lit.push_back(t);
  }
  placeBadges();
}

void Simulate::placeBadges() {
  const Viewport* view = services().viewport();
  for (auto& [at, badge] : m_badges) {
    const QPoint p = view->widgetPoint(at);
    const bool in = view->rect().contains(p);
    badge->move(p + QPoint(10, -10 - badge->height()));  // beside its target's middle, up and to the right: the part stays seen
    badge->setVisible(in);
    if (in) badge->raise();
  }
}

bool Simulate::removeRows(const std::vector<std::string>& rows) {
  AppDocument* doc = services().document();
  const opad::Scene& s = doc->scene;
  std::vector<std::string> deleted;  // op ids: loads, studies, joints
  auto drop = [&](const std::string& id) {
    if (std::find(deleted.begin(), deleted.end(), id) == deleted.end()) deleted.push_back(id);
  };
  std::map<std::string, json> settings;        // a study's settings as its setup rows leave them
  std::map<std::string, json> heatsinks;       // a fan load's heatsink list without the heatsink deleted
  for (const auto& row : rows) {
    if (row.rfind("sim:l:", 0) == 0 && s.load(row.substr(6))) drop(row.substr(6));
    else if (row.rfind("sim:s:", 0) == 0 && s.study(row.substr(6))) drop(row.substr(6));
    else if (row.rfind("sim:j:", 0) == 0 && s.joint(row.substr(6))) drop(row.substr(6));
    else if (row.rfind("sim:c:", 0) == 0) {
      for (const auto& l : s.loads)
        if (l.load_case == row.substr(6)) drop(l.id);
    } else if (row.rfind("sim:x:", 0) == 0) {
      const std::string rest = row.substr(6);
      const size_t colon = rest.find(':');
      const opad::Study* st = colon == std::string::npos ? nullptr : s.study(rest.substr(0, colon));
      if (!st) continue;
      const std::string what = rest.substr(colon + 1);
      json& set = settings.try_emplace(st->id, st->def.value("settings", json::object())).first->second;
      if (what == "box") {
        if (set.contains("cfd")) set["cfd"].erase("enclosure");
      } else if (what.rfind("board:", 0) == 0) {
        const std::string id = what.substr(6);
        if (set.contains("materials") && set["materials"].contains(id)) {
          set["materials"][id].erase("pcb");
          if (set["materials"][id].empty()) set["materials"].erase(id);
        }
      } else if (what.rfind("sink:", 0) == 0) {
        const std::string id = what.substr(5);
        for (const auto& l : s.loads) {  // off the fans that blow through it
          if (l.load_case != set.value("case", std::string()) || l.kind != "fan") continue;
          json& list = heatsinks.try_emplace(l.id, l.def.value("heatsink", json::array())).first->second;
          for (auto it = list.begin(); it != list.end();) it = *it == id ? list.erase(it) : it + 1;
        }
        std::vector<std::string> bodies{id};  // its material, set for it as one part
        if (const opad::Node* n = s.node(id); n && n->kind != opad::Node::Kind::Body) bodies = s.bodies_under(id);
        for (const auto& b : bodies)
          if (set.contains("materials") && set["materials"].contains(b) && set["materials"][b].contains("material")) set["materials"].erase(b);
      }
    }
  }
  std::vector<std::pair<std::string, json>> writes;
  for (const auto& id : deleted) writes.push_back({"delete", {{"target", id}}});
  for (const auto& [id, set] : settings)
    if (std::find(deleted.begin(), deleted.end(), id) == deleted.end() && set != s.study(id)->def.value("settings", json::object()))
      writes.push_back({"study", {{"id", id}, {"settings", set}, {"run", false}}});
  for (const auto& [id, list] : heatsinks)
    if (std::find(deleted.begin(), deleted.end(), id) == deleted.end() && list != s.load(id)->def.value("heatsink", json::array()))
      writes.push_back({"load", {{"id", id}, {"heatsink", list}}});
  if (writes.empty()) return false;
  if (std::find(deleted.begin(), deleted.end(), m_study) != deleted.end()) chooseStudy(std::string());  // its results put away
  doc->batch(rows.size() == 1 ? tr("Delete") : tr("Delete %n item(s)", "", int(rows.size())), [&] {
    for (const auto& [command, args] : writes) doc->run(command, args);
  });
  lightRows({});
  m_picked.clear();
  return true;
}
