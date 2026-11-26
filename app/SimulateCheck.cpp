#include "SimulateCheck.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "AppDocument.hpp"
#include "GuidedSteps.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "EngineInstall.hpp"
#include "SimChecks.hpp"
#include "SimulatePrint.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "opad/geometry.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/fea.hpp"
#include "opad/sim/flow.hpp"
#include "opad/sim/joints.hpp"

using opad::json;

namespace {

QLabel* text(const QString& s, QWidget* parent, const char* object = "secondary") {
  auto* l = new QLabel(s, parent);
  l->setWordWrap(true);
  l->setObjectName(object);
  return l;
}

QLabel* heading(const QString& s, QWidget* parent) {
  auto* l = new QLabel(s.toUpper(), parent);
  l->setObjectName("secondary");
  QFont f = l->font();
  f.setBold(true);
  f.setPointSizeF(f.pointSizeF() * 0.85);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
  l->setFont(f);
  return l;
}

QDoubleSpinBox* spin(double lo, double hi, int decimals, const QString& suffix, QWidget* parent) {
  auto* s = new QDoubleSpinBox(parent);
  s->setRange(lo, hi);
  s->setDecimals(decimals);
  s->setSuffix(suffix.isEmpty() ? QString() : " " + suffix);
  s->setKeyboardTracking(false);  // typing writes once, when done
  return s;
}

// A number as people write it: no exponent, three figures at most after the point.
QString num(double v, int sig = 3) {
  if (std::fabs(v) < 1e-12) return QStringLiteral("0");
  const int digits = std::clamp(sig - 1 - int(std::floor(std::log10(std::fabs(v)))), 0, 3);
  QString t = QString::number(v, 'f', digits);
  if (t.contains('.')) {
    while (t.endsWith('0')) t.chop(1);
    if (t.endsWith('.')) t.chop(1);
  }
  return t;
}
// A strength margin: "3.2", "45", "more than 100".
QString times(double sf) { return sf > 100 ? GuidedCheck::tr("more than 100") : num(sf, 2); }
// How far it bends: "hardly at all" below a micrometre.
QString sagText(double mm) { return mm < 0.001 ? GuidedCheck::tr("hardly at all") : units::format(units::Kind::Length, mm); }
QString fixed1(double v) { return QString::number(v, 'f', v < 10 ? 1 : 0); }

std::vector<opad::Ref> refsOf(const json& j) {
  std::vector<opad::Ref> out;
  if (j.is_array())
    for (const auto& r : j) try {
        out.push_back(opad::Ref::from_json(r));
      } catch (const std::exception&) {
      }
  return out;
}

json jsonOf(const std::vector<opad::Ref>& refs) {
  json a = json::array();
  for (const auto& r : refs) a.push_back(r.to_json());
  return a;
}

opad::Vec3 dirVector(const QString& d) {
  if (d == "up") return {0, 0, 1};
  if (d == "x+") return {1, 0, 0};
  if (d == "x-") return {-1, 0, 0};
  if (d == "y+") return {0, 1, 0};
  if (d == "y-") return {0, -1, 0};
  return {0, 0, -1};
}

double toM3s(double v, const QString& unit) {
  if (unit == "m3/h") return v / 3600.0;
  if (unit == "l/s") return v / 1000.0;
  return v * 0.000471947;  // CFM
}

double fromM3s(double q, const QString& unit) {
  if (unit == "m3/h") return q * 3600.0;
  if (unit == "l/s") return q * 1000.0;
  return q / 0.000471947;
}

QString unitText(const QString& unit) { return unit == "m3/h" ? QStringLiteral("m³/h") : unit == "l/s" ? QStringLiteral("l/s") : QStringLiteral("CFM"); }

QString duration(double seconds) {
  if (seconds < 90) return GuidedCheck::tr("%1 s").arg(num(seconds, 2));
  if (seconds < 5400) return GuidedCheck::tr("%1 min").arg(fixed1(seconds / 60));
  return GuidedCheck::tr("%1 h").arg(fixed1(seconds / 3600));
}

// What a check's result says, in plain words first.
struct Verdict {
  enum State { None, Good, Warn, Bad, Info } state = None;
  QString head, big, text;
  std::vector<std::pair<QString, QString>> numbers;
  QString line() const { return head.isEmpty() ? QString() : head + ": " + big; }
};

}  // namespace

// ---------------------------------------------------------------- the choices
struct GuidedCheck::Setup {
  std::vector<std::string> bodies;
  std::vector<opad::Ref> supports, loadFaces, inlet, outlet;
  std::vector<std::pair<opad::Ref, double>> heat;  // a part (or a face) and its watts
  QString loadKind = "weight";  // weight (kg) | push (N) | pressure (kPa)
  double loadValue = 5;
  QString loadDir = "down";
  bool selfWeight = false;
  double need = 2;
  double rpm = 3000;
  QString cooling = "natural";  // natural | fan
  QString fan;                   // a library fan's id
  std::vector<std::string> heatsink;
  QString fanDir = "x+";
  double ambient = 25, limit = 70, minutes = 10;
  std::string joint;
  double travel = 90, seconds = 2, clearance = 1;
  bool gravity = true;
  QString airMode = "flow";  // flow | fan
  double flow = 350, freeFlow = 350, shutoff = 120;
  QString flowUnit = "cfm", fluid = "air", quality = "normal";
  std::map<std::string, std::string> loads;  // role -> the load's id

  json toJson() const {
    json heatRows = json::array();
    for (const auto& [r, w] : heat) heatRows.push_back({{"on", r.to_json()}, {"W", w}});
    json l = json::object();
    for (const auto& [k, v] : loads) l[k] = v;
    return {{"bodies", bodies}, {"supports", jsonOf(supports)}, {"load_faces", jsonOf(loadFaces)},
            {"load", {{"kind", loadKind.toStdString()}, {"value", loadValue}, {"dir", loadDir.toStdString()}}},
            {"self_weight", selfWeight}, {"need", need}, {"rpm", rpm}, {"heat", heatRows}, {"cooling", cooling.toStdString()},
            {"fan", fan.toStdString()}, {"heatsink", heatsink}, {"fan_dir", fanDir.toStdString()}, {"ambient", ambient},
            {"limit_C", limit}, {"minutes", minutes}, {"joint", joint}, {"travel", travel}, {"seconds", seconds},
            {"clearance_mm", clearance}, {"gravity", gravity}, {"inlet", jsonOf(inlet)}, {"outlet", jsonOf(outlet)},
            {"air", {{"mode", airMode.toStdString()}, {"value", flow}, {"unit", flowUnit.toStdString()}, {"free_flow", freeFlow}, {"shutoff_pa", shutoff}}},
            {"fluid", fluid.toStdString()}, {"quality", quality.toStdString()}, {"loads", l}};
  }
  void fromJson(const json& j) {
    if (!j.is_object()) return;
    auto str = [&](const json& o, const char* k, const QString& d) { return QString::fromStdString(o.value(k, d.toStdString())); };
    for (const auto& b : j.value("bodies", json::array()))
      if (b.is_string()) bodies.push_back(b.get<std::string>());
    supports = refsOf(j.value("supports", json()));
    loadFaces = refsOf(j.value("load_faces", json()));
    inlet = refsOf(j.value("inlet", json()));
    outlet = refsOf(j.value("outlet", json()));
    const json ld = j.value("load", json::object());
    loadKind = str(ld, "kind", loadKind), loadValue = ld.value("value", loadValue), loadDir = str(ld, "dir", loadDir);
    selfWeight = j.value("self_weight", selfWeight), need = j.value("need", need), rpm = j.value("rpm", rpm);
    for (const auto& h : j.value("heat", json::array())) try {
        heat.push_back({opad::Ref::from_json(h.at("on")), h.value("W", 0.0)});
      } catch (const std::exception&) {
      }
    cooling = str(j, "cooling", cooling), fan = str(j, "fan", fan), fanDir = str(j, "fan_dir", fanDir);
    for (const auto& b : j.value("heatsink", json::array()))
      if (b.is_string()) heatsink.push_back(b.get<std::string>());
    ambient = j.value("ambient", ambient), limit = j.value("limit_C", limit), minutes = j.value("minutes", minutes);
    joint = j.value("joint", joint), travel = j.value("travel", travel), seconds = j.value("seconds", seconds);
    clearance = j.value("clearance_mm", clearance), gravity = j.value("gravity", gravity);
    const json air = j.value("air", json::object());
    airMode = str(air, "mode", airMode), flow = air.value("value", flow), flowUnit = str(air, "unit", flowUnit);
    freeFlow = air.value("free_flow", freeFlow), shutoff = air.value("shutoff_pa", shutoff);
    fluid = str(j, "fluid", fluid), quality = str(j, "quality", quality);
    for (const auto& [k, v] : j.value("loads", json::object()).items())
      if (v.is_string()) loads[k] = v.get<std::string>();
  }
};

// A list the view fills: OPAD's proposal, picked by hand (the view's clicks add and take out), cleared.
struct GuidedCheck::PickList {
  QString id, filter, nature;
  QLabel* text = nullptr;
  QPushButton *use = nullptr, *pick = nullptr, *clear = nullptr;
  std::vector<opad::Ref> proposal, base;  // base: what it held when picking began (clicks add to it; Clear empties it)
  QString proposalText;
  std::function<std::vector<opad::Ref>()> get;
  std::function<void(const std::vector<opad::Ref>&)> set;
};

// ---------------------------------------------------------------- OPAD's proposals (a worker)
namespace {

struct FaceInfo {
  opad::Ref ref;
  gp_Dir normal;
  bool planar = false;
  int wires = 0;
  double area = 0, holeSize = 0;  // the largest inner loop's size (a ring's bore)
  double width = 0;               // the face's own size across (a disc's diameter)
  gp_Pnt centre;
};

std::vector<FaceInfo> faces(const opad::Document& doc, const opad::Scene& s, const std::string& body) {
  std::vector<FaceInfo> out;
  const TopoDS_Shape shape = opad::node_world_shape(doc, s, body);
  if (opad::is_mesh_shape(shape)) return out;
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(shape, TopAbs_FACE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    const TopoDS_Face f = TopoDS::Face(map(i));
    FaceInfo fi;
    fi.ref.body = body, fi.ref.kind = opad::Ref::Kind::Face, fi.ref.index = i - 1;
    BRepAdaptor_Surface surf(f);
    GProp_GProps g;
    BRepGProp::SurfaceProperties(f, g);
    fi.area = g.Mass();
    fi.centre = g.CentreOfMass();
    {
      Bnd_Box fb;
      BRepBndLib::Add(f, fb);
      if (!fb.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        fb.Get(x0, y0, z0, x1, y1, z1);
        fi.width = std::max({x1 - x0, y1 - y0, z1 - z0});
      }
    }
    if (surf.GetType() == GeomAbs_Plane) {
      fi.planar = true;
      gp_Dir n = surf.Plane().Axis().Direction();
      if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
      fi.normal = n;
      const TopoDS_Wire outer = BRepTools::OuterWire(f);
      for (TopExp_Explorer w(f, TopAbs_WIRE); w.More(); w.Next()) {
        ++fi.wires;
        if (w.Current().IsSame(outer)) continue;
        Bnd_Box b;
        BRepBndLib::Add(w.Current(), b);
        if (b.IsVoid()) continue;
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        fi.holeSize = std::max(fi.holeSize, std::max({x1 - x0, y1 - y0, z1 - z0}));
      }
    }
    out.push_back(fi);
  }
  return out;
}

// The faces it rests on (facing down, at the bottom), the faces on top (facing up, at the top), a duct's open ends.
json proposals(const opad::Document& doc, const opad::Scene& s, const std::vector<std::string>& bodies, bool ends, const Progress& p) {
  std::vector<FaceInfo> all;
  double zmin = 1e300, zmax = -1e300;
  for (const auto& b : bodies) {
    if (p.cancelled()) return json();
    try {
      const auto f = faces(doc, s, b);
      all.insert(all.end(), f.begin(), f.end());
      Bnd_Box box;
      BRepBndLib::Add(opad::node_world_shape(doc, s, b), box);
      if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        zmin = std::min(zmin, z0), zmax = std::max(zmax, z1);
      }
    } catch (const std::exception&) {
    } catch (const Standard_Failure&) {
    }
  }
  json out = json::object();
  const double tol = std::max(0.5, 0.02 * (zmax - zmin));
  json bottom = json::array(), top = json::array();
  for (const auto& f : all) {
    if (!f.planar) continue;
    if (f.normal.Z() < -0.95 && f.centre.Z() <= zmin + tol) bottom.push_back(f.ref.to_json());
    if (f.normal.Z() > 0.95 && f.centre.Z() >= zmax - tol) top.push_back(f.ref.to_json());
  }
  out["supports"] = bottom;
  out["load"] = top;
  if (ends) {
    // Rings first (a hollow duct's ends: an outer loop and its bore); else the two flat faces farthest apart.
    std::vector<const FaceInfo*> rings;
    for (const auto& f : all)
      if (f.planar && f.wires >= 2 && f.holeSize > 0) rings.push_back(&f);
    if (rings.size() < 2) {
      rings.clear();
      for (const auto& f : all)
        if (f.planar) rings.push_back(&f);
    }
    const FaceInfo *a = nullptr, *b = nullptr;
    double farthest = -1;
    for (size_t i = 0; i < rings.size(); ++i)
      for (size_t j = i + 1; j < rings.size(); ++j) {
        const double d = rings[i]->centre.Distance(rings[j]->centre);
        if (d > farthest) farthest = d, a = rings[i], b = rings[j];
      }
    if (a && b) {
      // The opening's size: a ring's bore, else the face itself (a body that is the fluid: its end disc).
      auto size = [](const FaceInfo* f) { return f->holeSize > 0 ? f->holeSize : f->width; };
      if (size(b) > size(a)) std::swap(a, b);  // the wider end first: the air goes in there
      out["ends"] = {{{"ref", a->ref.to_json()}, {"size", size(a)}}, {{"ref", b->ref.to_json()}, {"size", size(b)}}};
    }
  }
  return out;
}

Verdict verdictOf(const QString& check, const json& s, const json& guided, const opad::sim::StudyRun* run) {
  Verdict v;
  if (!s.is_object() || s.empty()) return v;
  const double need = guided.value("need", 2.0), limit = guided.value("limit_C", 70.0), rpm = guided.value("rpm", 3000.0);
  using G = GuidedCheck;
  if (check == "bend" || check == "print") {
    double sf = s.value("min_safety_factor", 0.0);
    std::string weakest;
    double weakestSf = 1e300;
    for (const auto& [name, b] : s.value("bodies", json::object()).items())
      if (b.is_object() && b.value("safety_factor", json()).is_number() && b["safety_factor"].get<double>() < weakestSf)
        weakestSf = b["safety_factor"].get<double>(), weakest = name;
    if (sf <= 0 && weakestSf < 1e299) sf = weakestSf;
    if (sf <= 0) return v;
    const double sag = s.value("max_displacement_mm", 0.0);
    v.big = G::tr("holds %1 times this load").arg(times(sf));
    if (sf >= need) {
      v.state = Verdict::Good, v.head = G::tr("Safe");
      v.text = G::tr("You asked for at least %1 times. It bends %2.").arg(num(need, 2), sag < 0.001 ? sagText(sag) : G::tr("at most %1").arg(sagText(sag)));
    } else if (sf >= 1) {
      v.state = Verdict::Warn, v.head = G::tr("Holds, with little margin");
      v.text = G::tr("You asked for %1 times: make it thicker where the map is red, or use a stronger material.").arg(num(need, 2));
    } else {
      v.state = Verdict::Bad, v.head = G::tr("Breaks");
      v.big = G::tr("holds only %1 times this load").arg(num(sf, 2));
      v.text = G::tr("It would give way where the map is red: make it thicker there, or use a stronger material.");
    }
    if (const json pr = s.value("print", json()); pr.is_object())
      for (const auto& [name, b] : pr.items())
        if (b.is_object() && b.value("fails", json()).is_string())
          v.text += " " + G::tr("%1 gives first %2, at layer %3.").arg(QString::fromStdString(name), i18n::t(QString::fromStdString(b["fails"].get<std::string>())))
                              .arg(b.value("weakest_layer", 0));
    v.numbers = {{G::tr("Strength margin"), G::tr("%1 times").arg(times(sf))},
                 {G::tr("Highest stress"), num(s.value("max_von_mises_MPa", 0.0)) + " MPa"},
                 {G::tr("Bends at most"), sagText(sag)}};
    if (!weakest.empty()) v.numbers.push_back({G::tr("Weakest part"), QString::fromStdString(weakest)});
  } else if (check == "rattle") {
    const json f = s.value("frequencies_Hz", json::array());
    if (!f.is_array() || f.empty()) return v;
    const double shake = rpm / 60.0, f1 = f[0].get<double>();
    double nearest = f1;
    for (const auto& x : f)
      if (std::fabs(x.get<double>() - shake) < std::fabs(nearest - shake)) nearest = x.get<double>();
    if (std::fabs(nearest - shake) < 0.15 * shake) {
      v.state = Verdict::Bad, v.head = G::tr("Likely to hum");
      v.big = G::tr("shakes at %1 Hz, right at your %2 rpm").arg(num(nearest), num(rpm));
      v.text = G::tr("A stiffer part (thicker, ribbed, held in more places) or another speed keeps it quiet.");
    } else if (f1 > 1.4 * shake) {
      v.state = Verdict::Good, v.head = G::tr("Quiet");
      v.big = G::tr("lowest shake at %1 Hz").arg(num(f1));
      v.text = G::tr("Well above the %1 Hz of %2 rpm.").arg(num(shake), num(rpm));
    } else {
      v.state = Verdict::Warn, v.head = G::tr("Close");
      v.big = G::tr("lowest shake at %1 Hz").arg(num(f1));
      v.text = G::tr("Not far from the %1 Hz of %2 rpm: a stiffer part or another speed keeps it quieter.").arg(num(shake), num(rpm));
    }
    for (size_t i = 0; i < f.size() && i < 3; ++i) v.numbers.push_back({G::tr("Shake %1").arg(i + 1), num(f[i].get<double>()) + " Hz"});
    v.numbers.push_back({G::tr("Your speed"), G::tr("%1 rpm = %2 Hz").arg(num(rpm), num(shake))});
  } else if (check == "overheat" || check == "heatsink" || check == "warmup") {
    if (!s.value("max_temperature_C", json()).is_number()) return v;
    const double t = s["max_temperature_C"].get<double>();
    std::string hottest;
    double h = -1e300;
    for (const auto& [name, b] : s.value("bodies", json::object()).items())
      if (b.is_object() && b.value("max_temperature_C", -1e300) > h) h = b["max_temperature_C"].get<double>(), hottest = name;
    const QString who = hottest.empty() ? G::tr("The hottest part") : QString::fromStdString(hottest);
    if (check == "warmup") {
      // The first frame over the limit, from the run's temperatures (when it is in memory).
      double reach = -1, end = t;
      if (run && run->fea && !run->fea->temperature_frames.empty() && run->t.size() == run->fea->temperature_frames.size()) {
        for (size_t k = 0; k < run->t.size(); ++k) {
          const auto& fr = run->fea->temperature_frames[k];
          const double m = fr.empty() ? -1e300 : *std::max_element(fr.begin(), fr.end());
          end = m;
          if (reach < 0 && m >= limit) reach = run->t[k];
        }
      }
      const double total = s.value("duration_s", guided.value("minutes", 10.0) * 60);
      if (reach >= 0) {
        v.state = reach < 0.5 * total ? Verdict::Bad : Verdict::Warn;
        v.head = G::tr("Too hot after %1").arg(duration(reach));
        v.big = G::tr("reaches %1 °C after %2").arg(num(limit), duration(reach));
        v.text = G::tr("%1 gets there first. More metal to soak up the heat, or more cooling, buys time.").arg(who);
      } else if (end > limit) {
        v.state = Verdict::Bad, v.head = G::tr("Too hot");
        v.big = G::tr("%1 °C by the end").arg(num(end));
        v.text = G::tr("%1 goes over your %2 °C within %3.").arg(who, num(limit), duration(total));
      } else {
        v.state = Verdict::Good, v.head = G::tr("Stays under %1 °C").arg(num(limit));
        v.big = G::tr("%1 °C after %2").arg(num(end), duration(total));
        v.text = G::tr("%1 is the warmest part at the end.").arg(who);
      }
    } else if (t <= limit - 5) {
      v.state = Verdict::Good, v.head = G::tr("Cool enough");
      v.big = G::tr("hottest part %1 °C").arg(num(t));
      v.text = G::tr("%1 stays under your %2 °C.").arg(who, num(limit));
    } else if (t <= limit) {
      v.state = Verdict::Warn, v.head = G::tr("Just under");
      v.big = G::tr("hottest part %1 °C").arg(num(t));
      v.text = G::tr("%1 is within 5 °C of your %2 °C: a warmer room would take it over.").arg(who, num(limit));
    } else {
      v.state = Verdict::Bad, v.head = G::tr("Too hot");
      v.big = G::tr("hottest part %1 °C").arg(num(t));
      v.text = G::tr("%1 goes over your %2 °C: more vents, a fan or a bigger heatsink would help.").arg(who, num(limit));
    }
    v.numbers = {{G::tr("Hottest"), num(t) + " °C"}};
    if (s.value("heat_W", json()).is_number()) v.numbers.push_back({G::tr("Heat made"), num(s["heat_W"].get<double>()) + " W"});
    if (s.value("ambient_C", json()).is_number()) v.numbers.push_back({G::tr("Room"), num(s["ambient_C"].get<double>()) + " °C"});
    if (!hottest.empty()) v.numbers.push_back({G::tr("Hottest part"), QString::fromStdString(hottest)});
  } else if (check == "clearance") {
    const json c = s.value("clearance", json());
    if (!c.is_object()) return v;
    const std::string status = c.value("status", "");
    auto name = [](const json& o, const char* k) { return QString::fromStdString(o.value(k, std::string())); };
    if (status == "collision") {
      const json f = c.value("first_collision", json::object());
      v.state = Verdict::Bad, v.head = G::tr("Parts hit");
      v.big = G::tr("%1 hits %2").arg(name(f, "a_name"), name(f, "b_name"));
      v.text = G::tr("%1 s into the motion. Play it to see where, then give them more room.").arg(num(f.value("t", 0.0), 2));
    } else if (status == "too_close") {
      const json list = c.value("too_close", json::array());
      const json f = list.is_array() && !list.empty() ? list[0] : json::object();
      v.state = Verdict::Warn, v.head = G::tr("Tight");
      v.big = G::tr("%1 comes within %2 of %3").arg(name(f, "a_name"), units::format(units::Kind::Length, f.value("min_distance_mm", 0.0)), name(f, "b_name"));
      v.text = G::tr("Closer than the %1 you asked for: a print or a cut part may not be that exact.").arg(units::format(units::Kind::Length, guided.value("clearance_mm", 1.0)));
    } else {
      v.state = Verdict::Good, v.head = G::tr("Moves freely");
      const json closestPair = c.value("closest", json::object());
      v.big = c.value("min_distance_mm", json()).is_number() ? G::tr("closest gap %1").arg(units::format(units::Kind::Length, c["min_distance_mm"].get<double>()))
                                                             : G::tr("nothing comes close");
      if (closestPair.is_object() && closestPair.contains("a_name")) v.text = G::tr("Between %1 and %2.").arg(name(closestPair, "a_name"), name(closestPair, "b_name"));
    }
    if (c.value("min_distance_mm", json()).is_number()) v.numbers.push_back({G::tr("Closest gap"), units::format(units::Kind::Length, c["min_distance_mm"].get<double>())});
    v.numbers.push_back({G::tr("Pairs of parts followed"), QString::number(c.value("pairs", 0))});
    if (c.value("in_contact_at_start_pairs", 0) > 0)
      v.numbers.push_back({G::tr("Touching at the start (joined)"), QString::number(c.value("in_contact_at_start_pairs", 0))});
  } else if (check == "forces") {
    double torque = 0, force = 0;
    QString what;
    auto take = [&](const std::string& name, double peak, const std::string& unit) {
      if (unit == "N.mm" && std::fabs(peak) / 1000 > torque) torque = std::fabs(peak) / 1000, what = QString::fromStdString(name);
      if (unit == "N" && std::fabs(peak) > force && name.find("motor") != std::string::npos) force = std::fabs(peak), what = QString::fromStdString(name);
    };
    if (run)
      for (const auto& se : run->series)
        if (se.group == "motor" && !se.v.empty()) {
          double peak = 0;
          for (double x : se.v) peak = std::max(peak, std::fabs(x));
          take(se.name, peak, se.unit);
        }
    if (torque == 0 && force == 0)
      for (const auto& [name, o] : s.value("outputs", json::object()).items())
        if (o.is_object() && name.find("motor") != std::string::npos) take(name, o.value("max", 0.0), o.value("unit", std::string()));
    if (torque == 0 && force == 0) {
      v.state = Verdict::Info, v.head = G::tr("No motor"), v.big = G::tr("it moved by gravity alone");
      v.text = G::tr("Drive a joint (the motion step) to see the torque a motor needs.");
    } else {
      v.state = Verdict::Info, v.head = G::tr("The motor needs");
      v.big = torque > 0 ? G::tr("up to %1 N·m").arg(num(torque)) : G::tr("up to %1 N").arg(num(force));
      v.text = G::tr("Pick a motor rated above this, with some margin (half as much again).");
      v.numbers.push_back({what, torque > 0 ? num(torque) + " N·m" : num(force) + " N"});
    }
    if (s.value("duration", json()).is_number()) v.numbers.push_back({G::tr("Motion"), duration(s["duration"].get<double>())});
  } else if (check == "fitting" || check == "fan") {
    if (!s.value("pressure_drop_pa", json()).is_number()) return v;
    const double dp = s["pressure_drop_pa"].get<double>(), q = s.value("flow_m3s", 0.0);
    const QString unit = QString::fromStdString(guided.value("air", json::object()).value("unit", std::string("cfm")));
    // The study's own figure in that unit when it gives one (flow_cfm, flow_m3h), else from m³/s.
    double shown = fromM3s(q, unit);
    if (unit == "cfm" && s.value("flow_cfm", json()).is_number()) shown = s["flow_cfm"].get<double>();
    if (unit == "m3/h" && s.value("flow_m3h", json()).is_number()) shown = s["flow_m3h"].get<double>();
    const QString flow = num(shown) + " " + unitText(unit);
    if (check == "fitting") {
      v.state = dp < 10 ? Verdict::Good : dp < 50 ? Verdict::Warn : Verdict::Bad;
      v.head = dp < 10 ? G::tr("Small loss") : dp < 50 ? G::tr("Some loss") : G::tr("Large loss");
      v.big = G::tr("%1 Pa at %2").arg(num(dp, 2), flow);
    } else {
      v.state = Verdict::Info, v.head = G::tr("Real airflow");
      v.big = flow;
    }
    const json rec = s.value("recirculation", json::array());
    v.text = rec.is_array() && !rec.empty() ? G::tr("The air swirls in %n place(s): that is where most of the loss comes from.", "", int(rec.size()))
                                            : G::tr("The air goes through smoothly.");
    if (s.value("mesh", std::string()) == "axisymmetric") v.text += " " + G::tr("The part is round, so OPAD solved a thin slice of it and turned it round.");
    v.numbers = {{G::tr("Pressure loss"), num(dp, 3) + " Pa"}, {G::tr("Airflow"), flow}};
    if (s.value("max_speed_ms", json()).is_number()) v.numbers.push_back({G::tr("Fastest air"), num(s["max_speed_ms"].get<double>()) + " m/s"});
    if (s.value("loss_coefficient", json()).is_number())
      v.numbers.push_back({G::tr("Loss coefficient K"), num(s["loss_coefficient"].get<double>())});
    for (const auto& w : s.value("warnings", json::array()))
      if (w.is_string()) v.text += QStringLiteral("\n⚠ ") + i18n::message(QString::fromStdString(w.get<std::string>()));
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------- the panel
GuidedCheck::GuidedCheck(Hooks hooks, QWidget* parent) : QWidget(parent), m_hooks(std::move(hooks)), m_setup(std::make_unique<Setup>()) {
  setObjectName("guidedCheck");
  m_outer = new QVBoxLayout(this);
  m_outer->setContentsMargins(0, 0, 0, 0);
  m_write = new Debounce(400, [this] { write(); }, this);
  if (AppDocument* d = m_hooks.document ? m_hooks.document() : nullptr)
    connect(d, &AppDocument::changed, this, [this] {
      // Changed from elsewhere (undo, the Advanced tools, an agent): the pages follow, unless they hold a change still to write.
      if (m_writing || m_write->pending() || !isVisible() || !m_check) return;
      reload();
    });
}

GuidedCheck::~GuidedCheck() = default;

void GuidedCheck::open(const QString& check, const std::string& study) {
  flush();
  stopPicking();
  m_checkId = check;
  m_check = simchecks::find(check);
  m_study = study;
  m_run.reset();
  m_written.clear();
  m_proposedAt = ~0ull;
  build();
  reload();
  if (m_hooks.lastRun && !m_study.empty()) m_run = m_hooks.lastRun(m_study);
  m_frame->setCurrent(hasResult() ? stepOf("results") : 0);
  showResults();
  emit changed();
}

void GuidedCheck::flush() { m_write->flush(); }

int GuidedCheck::stepOf(const QString& name) const { return int(m_stepIds.indexOf(name)); }

QStringList GuidedCheck::stepNames() const { return m_stepIds; }

std::vector<std::string> GuidedCheck::shownSolids() const {
  std::vector<std::string> out;
  const AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return out;
  for (const auto& id : d->scene.all_bodies()) {
    const opad::Node* n = d->scene.node(id);
    if (n && n->kind == opad::Node::Kind::Body && !n->body_missing && n->representation == "solid" && d->scene.effectively_visible(id)) out.push_back(id);
  }
  return out;
}

QString GuidedCheck::bodyNames(const std::vector<std::string>& ids, int most) const {
  const AppDocument* d = m_hooks.document();
  QStringList names;
  for (const auto& id : ids) {
    if (names.size() == most) {
      names << tr("and %n more", "", int(ids.size()) - most);
      break;
    }
    const opad::Node* n = d ? d->scene.node(id) : nullptr;
    names << (n ? QString::fromStdString(n->name) : QString::fromStdString(id));
  }
  return names.join(", ");
}

// ---------------------------------------------------------------- the steps
void GuidedCheck::build() {
  if (m_frame) {  // out of this widget at once (its deletion waits for the event loop): nothing finds its parts any more
    m_outer->removeWidget(m_frame);
    m_frame->hide();
    m_frame->setParent(nullptr);
    m_frame->deleteLater();
  }
  m_lists.clear();
  m_refreshers.clear();
  m_stepIds.clear();
  m_frame = new GuidedSteps("check", this);
  m_outer->addWidget(m_frame);
  connect(m_frame, &GuidedSteps::stepShown, this, &GuidedCheck::stepShown);
  if (!m_check) return;
  const Tokens& k = theme::current();
  m_frame->setHeader(simchecks::goalName(m_check->goal), simchecks::goalColor(m_check->goal, k), m_check->question);
  const QString id = m_check->id;
  const bool strength = id == "bend" || id == "print" || id == "rattle";
  const bool heat = id == "overheat" || id == "heatsink" || id == "warmup";
  const bool motion = id == "clearance" || id == "forces";
  const bool air = id == "fitting" || id == "fan";
  auto changedNow = [this] {
    if (!m_loading) scheduleWrite();
  };

  // A list the view fills, with its buttons.
  auto list = [this](QVBoxLayout* v, const QString& listId, const QString& title, const QString& filter, const QString& nature,
                     std::function<std::vector<opad::Ref>()> get, std::function<void(const std::vector<opad::Ref>&)> set) {
    auto pl = std::make_unique<PickList>();
    pl->id = listId, pl->filter = filter, pl->nature = nature, pl->get = std::move(get), pl->set = std::move(set);
    QWidget* page = v->parentWidget() ? v->parentWidget() : this;
    auto* box = new QFrame(page);
    box->setObjectName("checkList");
    box->setFrameShape(QFrame::StyledPanel);
    auto* bl = new QVBoxLayout(box);
    bl->setContentsMargins(10, 8, 10, 8);
    auto* t = new QLabel(title, box);
    QFont f = t->font();
    f.setBold(true);
    t->setFont(f);
    bl->addWidget(t);
    pl->text = text({}, box);
    bl->addWidget(pl->text);
    auto* row = new QHBoxLayout();
    pl->use = new QPushButton(tr("Use these"), box);
    pl->use->setObjectName("checkUse_" + listId);
    pl->pick = new QPushButton(tr("Pick in the view"), box);
    pl->pick->setObjectName("checkPick_" + listId);
    pl->pick->setCheckable(true);
    pl->clear = new QPushButton(tr("Clear"), box);
    pl->clear->setFlat(true);
    pl->text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(pl->use);
    row->addWidget(pl->pick);
    row->addStretch(1);
    row->addWidget(pl->clear);
    bl->addLayout(row);
    v->addWidget(box);
    const int index = int(m_lists.size());
    connect(pl->use, &QPushButton::clicked, this, [this, index] { useProposal(m_lists[size_t(index)]->id); });
    connect(pl->pick, &QPushButton::toggled, this, [this, index](bool on) {
      if (on) startPicking(index);
      else if (m_picking == index) stopPicking();
    });
    connect(pl->clear, &QPushButton::clicked, this, [this, index] {
      m_lists[size_t(index)]->base.clear();
      if (m_hooks.select && m_picking == index) m_hooks.select({});
      m_lists[size_t(index)]->set({});
      showList(index);
      lightLists();
      scheduleWrite();
    });
    m_lists.push_back(std::move(pl));
    return index;
  };
  auto bodiesGet = [this] {
    std::vector<opad::Ref> out;
    for (const auto& b : m_setup->bodies) {
      opad::Ref r;
      r.body = b;
      out.push_back(r);
    }
    return out;
  };
  auto bodiesSet = [this](const std::vector<opad::Ref>& refs) {
    m_setup->bodies.clear();
    for (const auto& r : refs)
      if (!r.body.empty() && std::find(m_setup->bodies.begin(), m_setup->bodies.end(), r.body) == m_setup->bodies.end()) m_setup->bodies.push_back(r.body);
  };

  // ---- the part (every check but the motion ones: they move the whole mechanism)
  if (!motion) {
    m_stepIds << "part";
    QVBoxLayout* v = m_frame->addPage(tr("Part"), air ? tr("The part the air goes through: a duct, an adapter, a box with its fan.")
                                                  : heat ? tr("The parts to include: the box, the board, the chips and the heatsink.")
                                                         : tr("The part to check. OPAD takes its material from the part (Properties > Material)."));
    list(v, "bodies", tr("Parts in this check"), "select.bodies", "box", bodiesGet, bodiesSet);
    auto* material = new QPushButton(tr("Choose a material…"), v->parentWidget());
    material->setObjectName("checkMaterial");
    material->setVisible(strength || heat);
    connect(material, &QPushButton::clicked, this, [this] {
      if (m_hooks.command) m_hooks.command("inspect.material");
    });
    v->addWidget(material);
  }

  // ---- strength: where it is held, what pushes on it
  if (strength) {
    m_stepIds << "held";
    QVBoxLayout* v = m_frame->addPage(tr("Held by"), tr("Where is it fixed or resting? OPAD looked for the faces it rests on."));
    list(v, "supports", tr("Held on these faces"), "select.faces", "fixed", [this] { return m_setup->supports; },
         [this](const std::vector<opad::Ref>& r) { m_setup->supports = r; });
    v->addWidget(text(tr("Screwed to a wall? Pick the faces that touch the wall, or the holes' faces."), v->parentWidget()));
  }
  if (id == "bend" || id == "print") {
    m_stepIds << "load";
    QVBoxLayout* v = m_frame->addPage(tr("Load"), tr("What pushes on it, and how hard?"));
    QWidget* page = v->parentWidget();
    list(v, "load", tr("Pushed on these faces"), "select.faces", "force", [this] { return m_setup->loadFaces; },
         [this](const std::vector<opad::Ref>& r) { m_setup->loadFaces = r; });
    auto* kind = new QComboBox(page);
    kind->setObjectName("checkLoadKind");
    kind->addItem(tr("A weight resting on it"), "weight");
    kind->addItem(tr("A push"), "push");
    kind->addItem(tr("Pressure spread over the faces"), "pressure");
    auto* value = spin(0, 1e7, 2, {}, page);
    value->setObjectName("checkLoadValue");
    auto* dir = new QComboBox(page);
    dir->setObjectName("checkLoadDir");
    dir->addItem(tr("Straight down"), "down");
    dir->addItem(tr("Up"), "up");
    dir->addItem(tr("Sideways, along X"), "x+");
    dir->addItem(tr("Sideways, against X"), "x-");
    dir->addItem(tr("Sideways, along Y"), "y+");
    dir->addItem(tr("Sideways, against Y"), "y-");
    auto* scale = text({}, page);
    auto* row = new QHBoxLayout();
    row->addWidget(kind, 1);
    row->addWidget(value);
    v->addLayout(row);
    v->addWidget(dir);
    v->addWidget(scale);
    auto* self = new QCheckBox(tr("Count its own weight too"), page);
    v->addWidget(self);
    v->addWidget(heading(tr("How sure do you want to be?"), page));
    auto* need = new QComboBox(page);
    need->setObjectName("checkNeed");
    need->addItem(tr("1.5 times the load: light use, nobody hurt if it fails"), 1.5);
    need->addItem(tr("2 times: everyday things (the usual choice)"), 2.0);
    need->addItem(tr("3 times: people lean on it, or it gets knocked"), 3.0);
    need->addItem(tr("4 times: hanging over people, or shocks"), 4.0);
    v->addWidget(need);
    auto showKind = [this, kind, value, dir, scale] {
      const QString k = kind->currentData().toString();
      value->setSuffix(k == "weight" ? " kg" : k == "push" ? " N" : " kPa");
      dir->setVisible(k != "pressure");
      scale->setText(k == "weight" ? tr("For a sense of scale: a full mug is about 0.4 kg, a bag of shopping 5 kg, a person 75 kg.")
                     : k == "push" ? tr("For a sense of scale: 10 N is what 1 kg weighs; a firm push with a hand is about 100 N.")
                                   : tr("For a sense of scale: 1 kPa is 100 kg spread over a square metre; water 1 m deep presses 10 kPa."));
    };
    connect(kind, &QComboBox::currentIndexChanged, this, [this, kind, showKind, changedNow] {
      showKind();
      if (!m_loading) m_setup->loadKind = kind->currentData().toString();
      changedNow();
    });
    connect(value, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->loadValue = x;
      changedNow();
    });
    connect(dir, &QComboBox::currentIndexChanged, this, [this, dir, changedNow] {
      if (!m_loading) m_setup->loadDir = dir->currentData().toString();
      changedNow();
    });
    connect(self, &QCheckBox::toggled, this, [this, changedNow](bool on) {
      if (!m_loading) m_setup->selfWeight = on;
      changedNow();
    });
    connect(need, &QComboBox::currentIndexChanged, this, [this, need, changedNow] {
      if (!m_loading) m_setup->need = need->currentData().toDouble();
      changedNow();
    });
    m_refreshers.push_back([this, kind, value, dir, self, need, showKind] {
      kind->setCurrentIndex(std::max(0, kind->findData(m_setup->loadKind)));
      value->setValue(m_setup->loadValue);
      dir->setCurrentIndex(std::max(0, dir->findData(m_setup->loadDir)));
      self->setChecked(m_setup->selfWeight);
      int best = 0;
      for (int i = 0; i < need->count(); ++i)
        if (std::fabs(need->itemData(i).toDouble() - m_setup->need) < std::fabs(need->itemData(best).toDouble() - m_setup->need)) best = i;
      need->setCurrentIndex(best);
      showKind();
    });
  }
  if (id == "print") {
    m_stepIds << "print";
    QVBoxLayout* v = m_frame->addPage(tr("Printing"), tr("How it is printed: the filament, the layers, walls and infill. Your slicer's profile can fill this in."));
    auto* summary = text({}, v->parentWidget(), "");
    v->addWidget(summary);
    auto* edit = new QPushButton(tr("Print settings…"), v->parentWidget());
    edit->setObjectName("checkPrintSettings");
    v->addWidget(edit);
    v->addWidget(text(tr("Lay the part so the layers run along the load: a print is weakest between its layers."), v->parentWidget()));
    connect(edit, &QPushButton::clicked, this, [this] {
      flush();
      write();  // the study exists before its print settings
      const AppDocument* d = m_hooks.document();
      const opad::Study* st = d ? d->scene.study(m_study) : nullptr;
      PrintDialog dialog(st ? st->def.value("settings", json::object()).value("print", json()) : json(), this);
      if (dialog.exec() != QDialog::Accepted || m_study.empty()) return;
      m_writing = true;
      try {
        m_hooks.write("study", {{"id", m_study}, {"settings", {{"print", dialog.print()}}}, {"run", false}}, tr("Print settings"));
      } catch (const std::exception& e) {
        m_writeNote->setText(i18n::message(QString::fromStdString(e.what())));
      }
      m_writing = false;
      reload();
    });
    m_refreshers.push_back([this, summary] {
      const AppDocument* d = m_hooks.document();
      const opad::Study* st = d && !m_study.empty() ? d->scene.study(m_study) : nullptr;
      const json p = st ? st->def.value("settings", json::object()).value("print", json()) : json();
      summary->setText(p.is_object() ? tr("Printed in %1: %2 layers, %3 walls, %4 % infill.")
                                           .arg(QString::fromStdString(p.value("material", std::string("PLA"))),
                                                units::format(units::Kind::Length, p.value("layer_height", 0.2)))
                                           .arg(p.value("walls", 2))
                                           .arg(num(p.value("infill", 0.2) * (p.value("infill", 0.2) <= 1 ? 100 : 1)))
                                     : tr("Not set yet: OPAD treats it as solid plastic until you set it."));
    });
  }
  if (id == "rattle") {
    m_stepIds << "shake";
    QVBoxLayout* v = m_frame->addPage(tr("What shakes it"), tr("What makes it vibrate? A motor or a fan turning at a speed. OPAD finds the speeds at which the part shakes most."));
    auto* rpm = spin(1, 1e6, 0, tr("rpm"), v->parentWidget());
    rpm->setObjectName("checkRpm");
    v->addWidget(rpm);
    v->addWidget(text(tr("For a sense of scale: a PC fan turns at 1000 to 3000 rpm, a washing machine's drum up to 1400, a drill 3000."), v->parentWidget()));
    connect(rpm, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->rpm = x;
      changedNow();
    });
    m_refreshers.push_back([this, rpm] { rpm->setValue(m_setup->rpm); });
  }

  // ---- heat: what gets hot, how it is cooled
  if (heat) {
    m_stepIds << "heat";
    QVBoxLayout* v = m_frame->addPage(tr("What gets hot"), tr("Each part that gives off heat, and how much. Not sure? Pick the closest example."));
    QWidget* page = v->parentWidget();
    list(v, "heat", tr("Parts that give off heat"), "select.bodies", "heat",
         [this] {
           std::vector<opad::Ref> out;
           for (const auto& h : m_setup->heat) out.push_back(h.first);
           return out;
         },
         [this](const std::vector<opad::Ref>& refs) {
           std::vector<std::pair<opad::Ref, double>> next;
           for (const auto& r : refs) {
             double w = 5;
             for (const auto& h : m_setup->heat)
               if (h.first.str() == r.str()) w = h.second;
             next.push_back({r, w});
           }
           m_setup->heat = next;
         });
    auto* rows = new QWidget(page);
    rows->setObjectName("checkHeatRows");
    auto* grid = new QGridLayout(rows);
    grid->setContentsMargins(0, 0, 0, 0);
    v->addWidget(rows);
    v->addWidget(heading(tr("Examples"), page));
    auto* examples = new QWidget(page);
    auto* ex = new QHBoxLayout(examples);
    ex->setContentsMargins(0, 0, 0, 0);
    const std::vector<std::pair<QString, double>> known = {{tr("Raspberry Pi 4, ~6 W"), 6}, {tr("Arduino, ~0.5 W"), 0.5}, {tr("LED driver, ~3 W"), 3}, {tr("Motor driver, ~4 W"), 4}};
    auto* exGrid = new QGridLayout();
    ex->addLayout(exGrid);
    int n = 0;
    for (const auto& [label, watts] : known) {
      auto* b = new QPushButton(label, examples);
      exGrid->addWidget(b, n / 2, n % 2);
      ++n;
      connect(b, &QPushButton::clicked, this, [this, w = watts] {
        // The example's power onto the last part listed (the one just added), else the selection added with it.
        if (m_setup->heat.empty() && m_hooks.selection)
          for (const auto& r : m_hooks.selection())
            if (!r.body.empty()) {
              opad::Ref b;
              b.body = r.body;
              m_setup->heat.push_back({b, w});
              break;
            }
        if (m_setup->heat.empty()) {
          if (m_status) m_status->setText(tr("Pick the part first (Pick in the view), then its example."));
          return;
        }
        m_setup->heat.back().second = w;
        for (auto& f : m_refreshers) f();
        scheduleWrite();
      });
    }
    v->addWidget(examples);
    v->addWidget(heading(tr("How hot is too hot?"), page));
    auto* limitRow = new QHBoxLayout();
    limitRow->addWidget(new QLabel(tr("Warn me above"), page), 1);
    auto* limit = spin(-50, 1000, 0, "°C", page);
    limit->setObjectName("checkLimit");
    limitRow->addWidget(limit);
    v->addLayout(limitRow);
    v->addWidget(text(tr("Most boards slow down near 80 °C; PLA prints soften from about 55 °C; a surface over 60 °C hurts to touch."), page));
    connect(limit, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->limit = x;
      changedNow();
    });
    m_refreshers.push_back([this, grid, rows, limit] {
      while (QLayoutItem* it = grid->takeAt(0)) {
        delete it->widget();
        delete it;
      }
      const AppDocument* d = m_hooks.document();
      for (size_t i = 0; i < m_setup->heat.size(); ++i) {
        const opad::Ref& r = m_setup->heat[i].first;
        const opad::Node* node = d ? d->scene.node(r.body) : nullptr;
        QString name = node ? QString::fromStdString(node->name) : QString::fromStdString(r.body);
        if (r.kind == opad::Ref::Kind::Face) name = tr("%1, face %2").arg(name).arg(r.index + 1);
        grid->addWidget(new QLabel(name, rows), int(i), 0);
        auto* w = spin(0, 1e5, 2, "W", rows);
        w->setObjectName(QString("checkWatts%1").arg(i));
        w->setValue(m_setup->heat[i].second);
        grid->addWidget(w, int(i), 1);
        connect(w, &QDoubleSpinBox::valueChanged, this, [this, i](double x) {
          if (m_loading || i >= m_setup->heat.size()) return;
          m_setup->heat[i].second = x;
          scheduleWrite();
        });
      }
      grid->setColumnStretch(0, 1);
      limit->setValue(m_setup->limit);
    });

    m_stepIds << "cool";
    QVBoxLayout* c = m_frame->addPage(id == "warmup" ? tr("Cooling and time") : tr("Cooling"), tr("How does the heat get away?"));
    QWidget* cp = c->parentWidget();
    auto* natural = new QRadioButton(tr("Still air: no fan"), cp);
    natural->setObjectName("checkNatural");
    auto* withFan = new QRadioButton(tr("A fan blows through a heatsink"), cp);
    withFan->setObjectName("checkFan");
    auto* group = new QButtonGroup(cp);
    group->addButton(natural);
    group->addButton(withFan);
    c->addWidget(natural);
    c->addWidget(withFan);
    auto* fanBox = new QWidget(cp);
    auto* fl = new QVBoxLayout(fanBox);
    fl->setContentsMargins(20, 0, 0, 0);
    auto* model = new QComboBox(fanBox);
    model->setObjectName("checkFanModel");
    for (const auto& f : opad::sim::air::fans()) model->addItem(QString::fromStdString(f.name), QString::fromStdString(f.id));
    fl->addWidget(model);
    auto* fdir = new QComboBox(fanBox);
    fdir->addItem(tr("The air goes along X"), "x+");
    fdir->addItem(tr("The air goes against X"), "x-");
    fdir->addItem(tr("The air goes along Y"), "y+");
    fdir->addItem(tr("The air goes against Y"), "y-");
    fdir->addItem(tr("The air goes up"), "up");
    fdir->addItem(tr("The air goes down"), "down");
    fl->addWidget(fdir);
    c->addWidget(fanBox);
    {
      QVBoxLayout* hv = fl;
      list(hv, "heatsink", tr("The heatsink it blows through"), "select.bodies", "heatsink",
           [this] {
             std::vector<opad::Ref> out;
             for (const auto& b : m_setup->heatsink) {
               opad::Ref r;
               r.body = b;
               out.push_back(r);
             }
             return out;
           },
           [this](const std::vector<opad::Ref>& refs) {
             m_setup->heatsink.clear();
             for (const auto& r : refs)
               if (!r.body.empty()) m_setup->heatsink.push_back(r.body);
           });
    }
    auto* roomRow = new QHBoxLayout();
    roomRow->addWidget(new QLabel(tr("Room temperature"), cp), 1);
    auto* room = spin(-50, 100, 0, "°C", cp);
    room->setObjectName("checkRoom");
    roomRow->addWidget(room);
    c->addLayout(roomRow);
    QDoubleSpinBox* minutes = nullptr;
    if (id == "warmup") {
      auto* tr2 = new QHBoxLayout();
      tr2->addWidget(new QLabel(tr("Watch it for"), cp), 1);
      minutes = spin(0.1, 1e5, 1, tr("min"), cp);
      minutes->setObjectName("checkMinutes");
      tr2->addWidget(minutes);
      c->addLayout(tr2);
      connect(minutes, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
        if (!m_loading) m_setup->minutes = x;
        changedNow();
      });
    }
    m_cooling = new QPushButton(tr("Solve the air in the box too (Thermal setup)…"), cp);
    m_cooling->setObjectName("checkThermalSetup");
    m_cooling->setVisible(id == "overheat");
    c->addWidget(m_cooling);
    c->addWidget(text(id == "overheat" ? tr("With vents and fans, solving the air (OpenFOAM) shows where it flows and which part it misses. It takes longer.") : QString(), cp));
    connect(m_cooling, &QPushButton::clicked, this, [this] {
      if (m_hooks.command) m_hooks.command("simulate.cooling");
    });
    auto showFan = [this, withFan, fanBox] { fanBox->setVisible(withFan->isChecked()); };
    connect(withFan, &QRadioButton::toggled, this, [this, withFan, showFan, changedNow] {
      showFan();
      if (!m_loading) m_setup->cooling = withFan->isChecked() ? "fan" : "natural";
      changedNow();
    });
    connect(model, &QComboBox::currentIndexChanged, this, [this, model, changedNow] {
      if (!m_loading) m_setup->fan = model->currentData().toString();
      changedNow();
    });
    connect(fdir, &QComboBox::currentIndexChanged, this, [this, fdir, changedNow] {
      if (!m_loading) m_setup->fanDir = fdir->currentData().toString();
      changedNow();
    });
    connect(room, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->ambient = x;
      changedNow();
    });
    m_refreshers.push_back([this, natural, withFan, model, fdir, room, minutes, showFan] {
      (m_setup->cooling == "fan" ? withFan : natural)->setChecked(true);
      if (m_setup->fan.isEmpty() && model->count()) m_setup->fan = model->itemData(0).toString();
      model->setCurrentIndex(std::max(0, model->findData(m_setup->fan)));
      fdir->setCurrentIndex(std::max(0, fdir->findData(m_setup->fanDir)));
      room->setValue(m_setup->ambient);
      if (minutes) minutes->setValue(m_setup->minutes);
      showFan();
    });
  }

  // ---- movement: the joint to drive, how far, how close is too close / gravity
  if (motion) {
    m_stepIds << "motion";
    QVBoxLayout* v = m_frame->addPage(tr("The motion"), tr("Which joint moves, and how far? OPAD drives it and plays the motion."));
    QWidget* page = v->parentWidget();
    auto* joint = new QComboBox(page);
    joint->setObjectName("checkJoint");
    v->addWidget(joint);
    auto* none = text(tr("This model has no joint yet. Add one where the part turns or slides (a hinge's pin, a slider's rail)."), page);
    auto* addJoint = new QPushButton(tr("Add a joint…"), page);
    addJoint->setObjectName("checkAddJoint");
    v->addWidget(none);
    v->addWidget(addJoint);
    auto* travelRow = new QHBoxLayout();
    travelRow->addWidget(new QLabel(tr("How far it moves"), page), 1);
    auto* travel = spin(-1e6, 1e6, 1, {}, page);
    travel->setObjectName("checkTravel");
    travelRow->addWidget(travel);
    v->addLayout(travelRow);
    auto* secRow = new QHBoxLayout();
    secRow->addWidget(new QLabel(tr("In how long"), page), 1);
    auto* seconds = spin(0.01, 1e5, 2, tr("s"), page);
    seconds->setObjectName("checkSeconds");
    secRow->addWidget(seconds);
    v->addLayout(secRow);
    connect(addJoint, &QPushButton::clicked, this, [this] {
      if (m_hooks.command) m_hooks.command("simulate.joint");
    });
    auto angular = [this](const std::string& jid) {
      const AppDocument* d = m_hooks.document();
      const opad::Joint* j = d ? d->scene.joint(jid) : nullptr;
      const opad::sim::JointKind* k = j ? opad::sim::joint_kind(j->kind) : nullptr;
      return !k || k->coords.empty() || k->coords[0].angle;
    };
    connect(joint, &QComboBox::currentIndexChanged, this, [this, joint, travel, angular, changedNow] {
      if (m_loading) return;
      m_setup->joint = joint->currentData().toString().toStdString();
      const bool a = angular(m_setup->joint);
      travel->setSuffix(" " + units::symbol(a ? units::Kind::Angle : units::Kind::Length));
      changedNow();
    });
    connect(travel, &QDoubleSpinBox::valueChanged, this, [this, angular, changedNow](double x) {
      if (m_loading) return;
      m_setup->travel = units::fromDisplay(angular(m_setup->joint) ? units::Kind::Angle : units::Kind::Length, x);
      changedNow();
    });
    connect(seconds, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->seconds = x;
      changedNow();
    });
    m_refreshers.push_back([this, joint, none, addJoint, travel, seconds, angular] {
      joint->clear();
      const AppDocument* d = m_hooks.document();
      if (d)
        for (const auto& j : d->scene.joints) {
          const opad::sim::JointKind* k = opad::sim::joint_kind(j.kind);
          if (!k || k->relation || k->coords.empty()) continue;
          joint->addItem(QString::fromStdString(j.name) + "  ·  " + i18n::t(QString::fromStdString(j.kind)), QString::fromStdString(j.id));
        }
      if (m_setup->joint.empty() || joint->findData(QString::fromStdString(m_setup->joint)) < 0)
        m_setup->joint = joint->count() ? joint->itemData(0).toString().toStdString() : std::string();
      joint->setCurrentIndex(std::max(0, joint->findData(QString::fromStdString(m_setup->joint))));
      joint->setVisible(joint->count() > 0);
      none->setVisible(joint->count() == 0);
      addJoint->setVisible(joint->count() == 0);
      const units::Kind kind = angular(m_setup->joint) ? units::Kind::Angle : units::Kind::Length;
      travel->setSuffix(" " + units::symbol(kind));
      travel->setValue(units::toDisplay(kind, m_setup->travel));
      seconds->setValue(m_setup->seconds);
    });
    if (id == "clearance") {
      m_stepIds << "clear";
      QVBoxLayout* c = m_frame->addPage(tr("How close"), tr("How close may two parts come before it counts as too tight?"));
      auto* gap = spin(0, 1e4, 2, units::symbol(units::Kind::Length), c->parentWidget());
      gap->setObjectName("checkGap");
      c->addWidget(gap);
      c->addWidget(text(tr("For a sense of scale: a 3D print needs about 0.3 mm between moving parts; machined parts 0.05 mm; a lid on a box 1 mm."), c->parentWidget()));
      connect(gap, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
        if (!m_loading) m_setup->clearance = units::fromDisplay(units::Kind::Length, x);
        changedNow();
      });
      m_refreshers.push_back([this, gap] { gap->setValue(units::toDisplay(units::Kind::Length, m_setup->clearance)); });
    } else {
      m_stepIds << "forces";
      QVBoxLayout* c = m_frame->addPage(tr("Gravity"), tr("Real parts have weight: OPAD works out the force each joint feels and the torque a motor needs to move them."));
      auto* gravity = new QCheckBox(tr("Gravity pulls the parts down"), c->parentWidget());
      gravity->setObjectName("checkGravity");
      c->addWidget(gravity);
      c->addWidget(text(tr("The parts' weight comes from their size and material (Properties > Material)."), c->parentWidget()));
      connect(gravity, &QCheckBox::toggled, this, [this, changedNow](bool on) {
        if (!m_loading) m_setup->gravity = on;
        changedNow();
      });
      m_refreshers.push_back([this, gravity] { gravity->setChecked(m_setup->gravity); });
    }
  }

  // ---- air: in and out, how much air
  if (air) {
    m_stepIds << "ends";
    QVBoxLayout* v = m_frame->addPage(tr("In and out"), tr("Which way does the air go through it: the opening it enters by and the one it leaves by."));
    list(v, "inlet", tr("Air goes in here"), "select.faces", "fan", [this] { return m_setup->inlet; },
         [this](const std::vector<opad::Ref>& r) { m_setup->inlet = r; });
    list(v, "outlet", tr("Air comes out here"), "select.faces", "convection", [this] { return m_setup->outlet; },
         [this](const std::vector<opad::Ref>& r) { m_setup->outlet = r; });
    auto* swap = new QPushButton(tr("Swap in and out"), v->parentWidget());
    swap->setObjectName("checkSwap");
    v->addWidget(swap);
    m_planText = text({}, v->parentWidget(), "");  // how OPAD will solve it (the core's check of the openings), before Run
    m_planText->setObjectName("checkPlan");
    v->addWidget(m_planText);
    connect(swap, &QPushButton::clicked, this, [this] {
      std::swap(m_setup->inlet, m_setup->outlet);
      for (int i = 0; i < int(m_lists.size()); ++i) showList(i);
      lightLists();
      scheduleWrite();
    });
    v->addWidget(heading(tr("What flows"), v->parentWidget()));
    auto* fluid = new QComboBox(v->parentWidget());
    fluid->setObjectName("checkFluid");
    fluid->addItem(tr("Air"), "air");
    fluid->addItem(tr("Water"), "water");
    v->addWidget(fluid);
    connect(fluid, &QComboBox::currentIndexChanged, this, [this, fluid, changedNow] {
      if (!m_loading) m_setup->fluid = fluid->currentData().toString();
      changedNow();
    });
    m_refreshers.push_back([this, fluid] { fluid->setCurrentIndex(std::max(0, fluid->findData(m_setup->fluid))); });

    m_stepIds << "air";
    QVBoxLayout* a = m_frame->addPage(tr("How much air"), id == "fan" ? tr("Your fan: OPAD finds the airflow it really delivers through this part.")
                                                                      : tr("How much air goes through it? Choose whichever you know."));
    QWidget* ap = a->parentWidget();
    auto* known = new QRadioButton(tr("I know the airflow"), ap);
    known->setObjectName("checkKnownFlow");
    auto* fan = new QRadioButton(tr("I have a fan: the two numbers on its label"), ap);
    fan->setObjectName("checkFanLabel");
    auto* g = new QButtonGroup(ap);
    g->addButton(known);
    g->addButton(fan);
    a->addWidget(known);
    auto* flowRow = new QHBoxLayout();
    flowRow->setContentsMargins(20, 0, 0, 0);
    auto* flow = spin(0, 1e7, 1, {}, ap);
    flow->setObjectName("checkFlow");
    auto* unit = new QComboBox(ap);
    unit->setObjectName("checkFlowUnit");
    for (const char* u : {"cfm", "m3/h", "l/s"}) unit->addItem(unitText(u), u);
    flowRow->addWidget(flow, 1);
    flowRow->addWidget(unit);
    a->addLayout(flowRow);
    a->addWidget(text(tr("For a sense of scale: a bathroom fan moves about 50 to 100 CFM; an 8-inch inline duct fan about 300 to 450."), ap));
    a->addWidget(fan);
    auto* fanRow = new QGridLayout();
    fanRow->setContentsMargins(20, 0, 0, 0);
    auto* free = spin(0, 1e7, 1, {}, ap);
    free->setObjectName("checkFreeFlow");
    auto* shut = spin(0, 1e6, 1, "Pa", ap);
    shut->setObjectName("checkShutoff");
    fanRow->addWidget(new QLabel(tr("Max airflow"), ap), 0, 0);
    fanRow->addWidget(free, 0, 1);
    fanRow->addWidget(new QLabel(tr("Max pressure"), ap), 1, 0);
    fanRow->addWidget(shut, 1, 1);
    a->addLayout(fanRow);
    a->addWidget(text(tr("A label in inches of water: 1 inH₂O is 249 Pa; in mmH₂O: 1 mm is 9.8 Pa."), ap));
    a->addWidget(heading(tr("Accuracy"), ap));
    auto* quality = new QComboBox(ap);
    quality->setObjectName("checkQuality");
    quality->addItem(tr("Quick: a first answer"), "quick");
    quality->addItem(tr("Normal: the answer to trust"), "normal");
    quality->addItem(tr("Fine: to check it"), "fine");
    a->addWidget(quality);
    auto showMode = [known, flow, unit, free, shut] {
      const bool k = known->isChecked();
      flow->setEnabled(k), unit->setEnabled(true), free->setEnabled(!k), shut->setEnabled(!k);
      flow->setSuffix(QString()), free->setSuffix(" " + unit->currentText());
    };
    connect(known, &QRadioButton::toggled, this, [this, known, showMode, changedNow] {
      showMode();
      if (!m_loading) m_setup->airMode = known->isChecked() ? "flow" : "fan";
      changedNow();
    });
    connect(flow, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->flow = x;
      changedNow();
    });
    connect(free, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->freeFlow = x;
      changedNow();
    });
    connect(shut, &QDoubleSpinBox::valueChanged, this, [this, changedNow](double x) {
      if (!m_loading) m_setup->shutoff = x;
      changedNow();
    });
    connect(unit, &QComboBox::currentIndexChanged, this, [this, unit, showMode, changedNow] {
      showMode();
      if (!m_loading) m_setup->flowUnit = unit->currentData().toString();
      changedNow();
    });
    connect(quality, &QComboBox::currentIndexChanged, this, [this, quality, changedNow] {
      if (!m_loading) m_setup->quality = quality->currentData().toString();
      changedNow();
    });
    m_refreshers.push_back([this, known, fan, flow, unit, free, shut, quality, showMode, id] {
      if (id == "fan" && m_written.empty() && m_study.empty()) m_setup->airMode = "fan";
      (m_setup->airMode == "fan" ? fan : known)->setChecked(true);
      flow->setValue(m_setup->flow);
      unit->setCurrentIndex(std::max(0, unit->findData(m_setup->flowUnit)));
      free->setValue(m_setup->freeFlow);
      shut->setValue(m_setup->shutoff);
      quality->setCurrentIndex(std::max(0, quality->findData(m_setup->quality)));
      showMode();
    });
  }

  // ---- Run
  {
    m_stepIds << "run";
    QVBoxLayout* v = m_frame->addPage(tr("Run"), {});
    QWidget* page = v->parentWidget();
    m_estimate = text({}, page, "");
    v->addWidget(m_estimate);
    m_runNote = text({}, page, "");
    m_runNote->setObjectName("checkRunNote");
    v->addWidget(m_runNote);
    m_runPlan = text({}, page, "");
    m_runPlan->setObjectName("checkRunPlan");
    v->addWidget(m_runPlan);
    m_solverButton = new QPushButton(page);
    m_solverButton->setObjectName("checkSolver");
    v->addWidget(m_solverButton);
    m_runButton = new QPushButton(tr("Run the check"), page);
    m_runButton->setObjectName("checkRun");
    m_runButton->setDefault(true);
    v->addWidget(m_runButton);
    m_status = text({}, page, "");
    v->addWidget(m_status);
    m_writeNote = text({}, page);
    v->addWidget(m_writeNote);
    connect(m_runButton, &QPushButton::clicked, this, &GuidedCheck::runNow);
    connect(m_solverButton, &QPushButton::clicked, this, [this] {
      const simchecks::Readiness r = simchecks::readiness(*m_check, m_hooks.engines ? m_hooks.engines() : json::object());
      if (r.solver.isEmpty()) return;
      // OPAD's own pack of it: the button installs it (progress in the status bar); else the solver window says where to get it.
      if (m_hooks.installSolver && engineinstall::pack(r.solver)) m_hooks.installSolver(r.solver);
      else if (m_hooks.solverNeeded) m_hooks.solverNeeded(r.solver);
    });
  }

  // ---- Results
  {
    m_stepIds << "results";
    QVBoxLayout* v = m_frame->addPage(tr("Results"), {});
    QWidget* page = v->parentWidget();
    m_verdictCard = new QFrame(page);
    m_verdictCard->setObjectName("simVerdict");
    auto* vc = new QVBoxLayout(m_verdictCard);
    vc->setContentsMargins(14, 12, 14, 12);
    m_verdictHead = new QLabel(m_verdictCard);
    m_verdictHead->setObjectName("checkVerdict");
    QFont hf = m_verdictHead->font();
    hf.setBold(true);
    m_verdictHead->setFont(hf);
    m_verdictBig = new QLabel(m_verdictCard);
    m_verdictBig->setObjectName("checkVerdictBig");
    m_verdictBig->setWordWrap(true);
    m_verdictBig->setStyleSheet(QString("font-size: %1px; font-weight: 700;").arg(theme::px(20)));  // the stylesheet's size wins over a font
    m_verdictText = text({}, m_verdictCard, "");
    vc->addWidget(m_verdictHead);
    vc->addWidget(m_verdictBig);
    vc->addWidget(m_verdictText);
    v->addWidget(m_verdictCard);
    m_numbersBox = new QWidget(page);
    auto* nb = new QVBoxLayout(m_numbersBox);
    nb->setContentsMargins(0, 0, 0, 0);
    nb->addWidget(heading(tr("The numbers"), m_numbersBox));
    m_numbers = new QGridLayout();
    nb->addLayout(m_numbers);
    v->addWidget(m_numbersBox);
    m_fieldButtons = new QWidget(page);
    m_fieldButtons->setObjectName("checkFields");
    auto* fb = new QGridLayout(m_fieldButtons);
    fb->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_fieldButtons);
    m_compareBox = new QWidget(page);
    m_compareBox->setObjectName("checkCompare");
    auto* cb = new QVBoxLayout(m_compareBox);
    cb->setContentsMargins(0, 0, 0, 0);
    cb->addWidget(heading(tr("Compare designs"), m_compareBox));
    m_compareWith = new QComboBox(m_compareBox);
    m_compareWith->setObjectName("checkCompareWith");
    cb->addWidget(m_compareWith);
    m_compareGrid = new QGridLayout();
    cb->addLayout(m_compareGrid);
    m_compareText = text({}, m_compareBox);
    cb->addWidget(m_compareText);
    auto* keep = new QPushButton(tr("Try a change: keep this result to compare"), m_compareBox);
    keep->setObjectName("checkKeep");
    cb->addWidget(keep);
    v->addWidget(m_compareBox);
    connect(m_compareWith, &QComboBox::currentIndexChanged, this, [this] { showCompare(); });
    connect(keep, &QPushButton::clicked, this, &GuidedCheck::keepCopy);
  }
  for (int i = 0; i < int(m_lists.size()); ++i) showList(i);
}

// ---------------------------------------------------------------- the choices from the document
void GuidedCheck::reload() {
  m_loading = true;
  struct Done {
    bool& flag;
    ~Done() { flag = false; }
  } done{m_loading};
  AppDocument* d = m_hooks.document();
  const opad::Study* st = d && d->hasDocument && !m_study.empty() ? d->scene.study(m_study) : nullptr;
  if (!m_study.empty() && !st) m_study.clear();  // deleted (undone): a new check again
  Setup fresh;
  if (st) {
    fresh.fromJson(st->def.value("settings", json::object()).value("guided", json()));
  } else if (m_setup && !m_written.empty()) {
    fresh = *m_setup;  // not written yet (a coming check): kept
  } else {
    // A new check: what is selected, else every shown solid.
    if (m_hooks.selection)
      for (const auto& r : m_hooks.selection())
        if (!r.body.empty() && std::find(fresh.bodies.begin(), fresh.bodies.end(), r.body) == fresh.bodies.end()) fresh.bodies.push_back(r.body);
    if (fresh.bodies.empty()) fresh.bodies = shownSolids();
    if (m_checkId == "heatsink") fresh.cooling = "fan";
    if (m_checkId == "forces") fresh.seconds = 1;
    if (m_checkId == "warmup") fresh.limit = 60;
    // Heat sources by their names (a chip, a processor, a regulator): a guess to confirm.
    static const QRegularExpression chip("\\b(chip|cpu|processor|soc|ic|u\\d+|regulator|led|mosfet|driver|transistor|fpga|gpu|mcu|power)\\b",
                                         QRegularExpression::CaseInsensitiveOption);
    if (d && (m_checkId == "overheat" || m_checkId == "heatsink" || m_checkId == "warmup"))
      for (const auto& b : fresh.bodies)
        if (const opad::Node* n = d->scene.node(b); n && chip.match(QString::fromStdString(n->name)).hasMatch()) {
          opad::Ref r;
          r.body = b;
          fresh.heat.push_back({r, 5});
        }
  }
  // Only what is still there.
  if (d && d->hasDocument) {
    auto alive = [d](std::vector<opad::Ref>& refs) {
      refs.erase(std::remove_if(refs.begin(), refs.end(), [d](const opad::Ref& r) { return !d->scene.node(r.body); }), refs.end());
    };
    alive(fresh.supports), alive(fresh.loadFaces), alive(fresh.inlet), alive(fresh.outlet);
    fresh.bodies.erase(std::remove_if(fresh.bodies.begin(), fresh.bodies.end(), [d](const std::string& b) { return !d->scene.node(b); }), fresh.bodies.end());
    fresh.heat.erase(std::remove_if(fresh.heat.begin(), fresh.heat.end(), [d](const auto& h) { return !d->scene.node(h.first.body); }), fresh.heat.end());
  }
  *m_setup = fresh;
  for (auto& f : m_refreshers) f();
  for (int i = 0; i < int(m_lists.size()); ++i) showList(i);
  if (st) m_written.clear();  // the next change compares with the document itself (an unchanged load or setting writes nothing)
  showRunState();
  showResults();
  lightLists();
  propose();
  m_plan = json(), m_planError.clear();
  if (st && m_check && m_check->kind == "flow") validateFlow(st->def.value("settings", json::object()));
  else showPlan();
}

// The core's check of a flow study's settings and openings (no OpenFOAM needed), on a worker: how it will be meshed, or why
// it cannot be solved like this.
void GuidedCheck::validateFlow(const json& settings) {
  AppDocument* d = m_hooks.document();
  if (!d || !m_hooks.jobs) return;
  if (m_validating) m_validating->cancel();
  auto plan = std::make_shared<json>();
  auto error = std::make_shared<std::string>();
  m_validating = d->readAsync(m_hooks.jobs(), tr("Checking the openings"),
                              [settings, plan, error](const opad::Document& doc, const opad::Scene& s, Progress) {
                                try {
                                  *plan = opad::sim::plan_flow(doc, s, opad::sim::flow_settings(settings)).to_json();
                                } catch (const std::exception& e) {
                                  *error = e.what();
                                }
                              },
                              [this, self = QPointer<GuidedCheck>(this), plan, error](bool ok, const QString&) {
                                if (!self || !ok) return;
                                m_plan = *plan;
                                m_planError = error->empty() ? QString() : i18n::message(QString::fromStdString(*error));
                                showPlan();
                                showRunState();
                              });
}

void GuidedCheck::showPlan() {
  QString t;
  if (!m_planError.isEmpty()) {
    t = tr("OPAD cannot solve it like this: %1").arg(m_planError);
  } else if (m_plan.is_object() && m_plan.contains("mesh")) {
    const QString cells = QLocale().toString(qlonglong(m_plan.value("cells_estimate", 0)));
    if (m_plan.value("mesh", std::string()) == "axisymmetric") {
      t = tr("The part is round, so OPAD solves a thin slice of it and turns it round (about %1 cells): quick.").arg(cells);
    } else {
      const QString why = QString::fromStdString(m_plan.value("why_3d", std::string()));
      t = why.isEmpty() ? tr("OPAD solves the whole part (about %1 cells): this takes longer than a round part.").arg(cells)
                        : tr("OPAD solves the whole part (about %1 cells), since %2. This takes longer than a round part.").arg(cells, i18n::message(why));
    }
    const json in = m_plan.value("inlet", json::object()), out = m_plan.value("outlet", json::object());
    if (in.contains("diameter_mm") && out.contains("diameter_mm"))
      t += " " + tr("In %1 across, out %2 across, %3 apart.")
                     .arg(units::format(units::Kind::Length, in.value("diameter_mm", 0.0)), units::format(units::Kind::Length, out.value("diameter_mm", 0.0)),
                          units::format(units::Kind::Length, m_plan.value("length_mm", 0.0)));
  }
  for (QLabel* l : {m_planText, m_runPlan})
    if (l) {
      l->setText(t);
      l->setVisible(!t.isEmpty());
      l->setStyleSheet(m_planError.isEmpty() ? QString() : QString("color: %1;").arg(theme::current().amber.name()));
    }
}

void GuidedCheck::scheduleWrite() {
  if (m_loading) return;
  m_write->poke();
  showRunState();
}

// The study and its loads as the choices say: one step to undo, nothing when nothing changed.
void GuidedCheck::write() {
  if (!m_check) return;
  AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return;
  const simchecks::Readiness ready = simchecks::readiness(*m_check, m_hooks.engines ? m_hooks.engines() : json::object());
  if (ready.state == simchecks::Readiness::Coming) {
    m_written = "unsaved";
    if (m_writeNote) m_writeNote->setText(tr("This check comes with a later version of OPAD: what you set here is not saved yet."));
    return;
  }
  const opad::Scene& s = d->scene;
  const Setup& u = *m_setup;
  const QString id = m_check->id;
  // The loads it needs, by role.
  std::vector<std::pair<std::string, json>> loads;
  auto bodiesJson = [](const std::vector<std::string>& ids) {
    json a = json::array();
    for (const auto& b : ids) a.push_back(b);
    return a;
  };
  if (id == "bend" || id == "print" || id == "rattle") {
    if (!u.supports.empty()) loads.push_back({"support", {{"kind", "fixed"}, {"on", jsonOf(u.supports)}}});
    if (id != "rattle" && !u.loadFaces.empty()) {
      const opad::Vec3 dir = dirVector(u.loadDir);
      if (u.loadKind == "pressure") {
        loads.push_back({"load", {{"kind", "pressure"}, {"on", jsonOf(u.loadFaces)}, {"value", u.loadValue / 1000.0}}});
      } else {
        const double n = u.loadKind == "weight" ? u.loadValue * 9.80665 : u.loadValue;
        loads.push_back({"load", {{"kind", "force"}, {"on", jsonOf(u.loadFaces)}, {"vector", {dir[0] * n, dir[1] * n, dir[2] * n}}}});
      }
    }
    if (id != "rattle" && u.selfWeight) loads.push_back({"gravity", {{"kind", "gravity"}, {"on", json::array()}, {"vector", {0, 0, -9806.65}}}});
  } else if (id == "overheat" || id == "heatsink" || id == "warmup") {
    for (size_t i = 0; i < u.heat.size(); ++i)
      loads.push_back({"heat" + std::to_string(i), {{"kind", "heat"}, {"on", json::array({u.heat[i].first.kind == opad::Ref::Kind::Face ? u.heat[i].first.to_json() : json(u.heat[i].first.body)})}, {"value", u.heat[i].second}}});
    std::vector<std::string> air = u.bodies;
    if (u.cooling == "fan" && !u.heatsink.empty() && !u.fan.isEmpty()) {
      const opad::Vec3 dir = dirVector(u.fanDir);
      loads.push_back({"fan", {{"kind", "fan"}, {"on", bodiesJson(u.heatsink)}, {"fan", u.fan.toStdString()}, {"vector", {dir[0], dir[1], dir[2]}}, {"ambient", u.ambient}}});
      air.erase(std::remove_if(air.begin(), air.end(), [&](const std::string& b) { return std::find(u.heatsink.begin(), u.heatsink.end(), b) != u.heatsink.end(); }), air.end());
    }
    if (!air.empty()) loads.push_back({"air", {{"kind", "convection"}, {"on", bodiesJson(air)}, {"h", "natural"}, {"ambient", u.ambient}}});
  }
  // The study's settings.
  json settings = {{"check", id.toStdString()}};
  const std::string kind = m_check->kind.toStdString();
  if (kind == "static" || kind == "modal" || kind == "thermal") settings["bodies"] = bodiesJson(u.bodies);
  if (kind == "modal") settings["modes"] = 6;
  if (id == "warmup") settings["duration"] = u.minutes * 60, settings["frames"] = 61;
  if (kind == "thermal") settings["ambient"] = u.ambient;
  if (kind == "motion" || kind == "dynamic") {
    const opad::Joint* j = s.joint(u.joint);
    const double from = j && !j->values.empty() ? j->values[0] : 0.0;
    settings["duration"] = u.seconds;
    settings["frames"] = kind == "motion" ? 61 : 101;
    settings["drivers"] = u.joint.empty() ? json::array() : json::array({{{"joint", u.joint}, {"to", from + u.travel}}});
    if (kind == "motion") settings["clearance"] = {{"clearance_mm", u.clearance}};
    else settings["gravity"] = u.gravity;
  }
  if (kind == "flow") {
    settings["inlet"] = jsonOf(u.inlet);
    settings["outlet"] = jsonOf(u.outlet);
    // The flow study's fan: free flow in `unit` (cfm, m3/h, l/s), shut-off pressure in `pressure_unit`.
    if (u.airMode == "fan")
      settings["fan"] = {{"free_flow", u.freeFlow}, {"unit", u.flowUnit.toStdString()}, {"shutoff_pressure", u.shutoff}, {"pressure_unit", "Pa"}}, settings["flow"] = nullptr;
    else settings["flow"] = {{"value", u.flow}, {"unit", u.flowUnit.toStdString()}}, settings["fan"] = nullptr;
    settings["fluid"] = u.fluid.toStdString();
    settings["ambient"] = u.ambient;
    settings["quality"] = u.quality.toStdString();
  }
  if (kind == "flow" && (u.inlet.empty() || u.outlet.empty())) {
    // The flow study is checked as it is written (its openings first): saved once both are known.
    if (m_writeNote) m_writeNote->setText(tr("Say where the air goes in and out: the check is saved then."));
    m_plan = json(), m_planError.clear();
    showPlan();
    return;
  }
  const std::string key = json({{"settings", settings}, {"guided", u.toJson()}, {"loads", loads}}).dump();
  if (key == m_written && !m_study.empty() && s.study(m_study)) return;
  m_writing = true;
  struct Done {
    bool& flag;
    ~Done() { flag = false; }
  } done{m_writing};
  Setup next = u;
  std::string study = m_study;
  try {
    d->batch(tr("Check: %1").arg(m_check->question), [&] {
      std::string caseName;
      if (study.empty() || !s.study(study)) {
        std::set<std::string> taken;
        for (const auto& st : s.studies) taken.insert(st.name);
        const QString stem = simchecks::goalName(m_check->goal) + " · " + m_check->question.left(m_check->question.size() - 1);
        std::string name = stem.toStdString();
        for (int n = 2; taken.count(name); ++n) name = (stem + QString(" (%1)").arg(n)).toStdString();
        json first = {{"check", id.toStdString()}};
        if (kind == "flow") {  // checked as written: made with its openings and airflow at once (a null drops a key only in a change)
          first = settings;
          for (auto it = first.begin(); it != first.end();) it = it->is_null() ? first.erase(it) : std::next(it);
          first["guided"] = next.toJson();
        }
        const json out = m_hooks.write("study", {{"kind", kind}, {"name", name}, {"settings", first}, {"run", false}}, tr("New check"));
        if (out.contains("plan")) m_plan = out["plan"];
        study = out.value("id", std::string());
        caseName = name;
      } else {
        caseName = s.study(study)->name;
        const std::string was = s.study(study)->def.value("settings", json::object()).value("case", std::string());
        if (!was.empty()) caseName = was;
      }
      // Loads no longer needed go; the others are changed in place (only when they differ) or added.
      std::set<std::string> wanted;
      for (const auto& [role, args] : loads) wanted.insert(role);
      for (auto it = next.loads.begin(); it != next.loads.end();) {
        if (!wanted.count(it->first) && s.load(it->second)) m_hooks.write("delete", {{"target", it->second}}, tr("Check"));
        if (!wanted.count(it->first) || !s.load(it->second)) it = next.loads.erase(it);
        else ++it;
      }
      for (const auto& [role, args] : loads) {
        const auto at = next.loads.find(role);
        const opad::Load* l = at == next.loads.end() ? nullptr : s.load(at->second);
        if (l && l->kind == args.value("kind", std::string())) {
          bool same = l->load_case == caseName;
          for (const auto& [k, v] : args.items()) {
            if (k == "kind") continue;
            if (k == "on" ? l->def.value("refs", json()) != v : l->def.value(k, json()) != v) same = false;
          }
          if (same) continue;
          json change = args;
          change.erase("kind");
          change["id"] = l->id;
          change["case"] = caseName;
          m_hooks.write("load", change, tr("Check"));
        } else {
          if (l) m_hooks.write("delete", {{"target", l->id}}, tr("Check"));
          json add = args;
          add["case"] = caseName;
          add["name"] = (m_check->question.left(24) + " · " + QString::fromStdString(role)).toStdString();
          const json out = m_hooks.write("load", add, tr("Check"));
          next.loads[role] = out.value("id", std::string());
        }
      }
      if (kind == "static" || kind == "modal" || kind == "thermal") settings["case"] = caseName;
      settings["guided"] = next.toJson();
      const json out = m_hooks.write("study", {{"id", study}, {"settings", settings}, {"run", false}}, tr("Check"));
      if (out.contains("plan")) m_plan = out["plan"];
    });
    m_study = study;
    m_setup->loads = next.loads;
    settings.erase("guided");
    m_written = json({{"settings", settings}, {"guided", m_setup->toJson()}, {"loads", loads}}).dump();  // as the next write computes it
    if (m_writeNote) m_writeNote->clear();
    if (kind == "flow") m_planError.clear();
  } catch (const std::exception& e) {
    if (m_writeNote) m_writeNote->setText(tr("Not saved: %1").arg(i18n::message(QString::fromStdString(e.what()))));
    if (kind == "flow") m_plan = json(), m_planError = i18n::message(QString::fromStdString(e.what()));
  }
  if (kind == "flow") showPlan();
  showRunState();
  emit changed();
}

// ---------------------------------------------------------------- lists and picking
void GuidedCheck::showList(int index) {
  if (index < 0 || size_t(index) >= m_lists.size()) return;
  PickList& l = *m_lists[size_t(index)];
  const std::vector<opad::Ref> refs = l.get();
  const AppDocument* d = m_hooks.document();
  QString t;
  if (refs.empty()) {
    t = m_picking == index ? tr("Waiting: click in the view") : tr("Nothing yet.");
  } else if (l.filter == "select.faces") {
    std::vector<std::string> bodies;
    for (const auto& r : refs)
      if (std::find(bodies.begin(), bodies.end(), r.body) == bodies.end()) bodies.push_back(r.body);
    t = tr("%n face(s) on %1", "", int(refs.size())).arg(bodyNames(bodies));
  } else {
    std::vector<std::string> ids;
    for (const auto& r : refs) ids.push_back(r.body);
    t = bodyNames(ids, 4);
    if (l.id == "bodies" && refs.size() > 20) t += " · " + tr("that is a lot to check at once: pick the part that matters");
  }
  if (!l.proposalText.isEmpty() && m_picking != index) t += "\n" + l.proposalText;
  (void)d;
  l.text->setText(t);
  l.use->setVisible(!l.proposal.empty());
  l.clear->setEnabled(!refs.empty());
  const QSignalBlocker quiet(l.pick);
  l.pick->setChecked(m_picking == index);
  l.pick->setText(m_picking == index ? tr("Done") : tr("Pick in the view"));
}

bool GuidedCheck::proposed(const QString& list) const {
  for (const auto& l : m_lists)
    if (l->id == list) return !l->proposal.empty();
  return false;
}

void GuidedCheck::useProposal(const QString& list) {
  for (int i = 0; i < int(m_lists.size()); ++i) {
    PickList& l = *m_lists[size_t(i)];
    if (l.id != list || l.proposal.empty()) continue;
    if (list == "inlet" || list == "outlet") {  // the two ends at once, the wider one in
      for (auto& o : m_lists)
        if (o->id == "inlet") o->set({o->proposal.empty() ? opad::Ref() : o->proposal[0]});
        else if (o->id == "outlet") o->set({o->proposal.empty() ? opad::Ref() : o->proposal[0]});
    } else {
      l.set(l.proposal);
    }
    for (int k = 0; k < int(m_lists.size()); ++k) showList(k);
    for (auto& f : m_refreshers) f();
    lightLists();
    scheduleWrite();
  }
}

std::vector<opad::Ref> GuidedCheck::listed(const QString& list) const {
  for (const auto& l : m_lists)
    if (l->id == list) return l->get();
  return {};
}

void GuidedCheck::startPicking(int index) {
  if (m_picking == index) return;
  stopPicking();
  m_picking = index;
  PickList& l = *m_lists[size_t(index)];
  l.base = l.get();  // clicks add to what it holds (a click on a picked item takes back only that click)
  if (m_hooks.select) m_hooks.select({});
  if (m_hooks.filter) m_filterBefore = m_hooks.filter(l.filter);
  if (m_hooks.accumulate) m_hooks.accumulate(true);
  for (int i = 0; i < int(m_lists.size()); ++i) showList(i);
}

void GuidedCheck::stopPicking() {
  if (m_picking < 0) return;
  m_picking = -1;
  if (m_hooks.accumulate) m_hooks.accumulate(false);
  if (m_hooks.select) m_hooks.select({});
  if (m_hooks.filter && !m_filterBefore.isEmpty()) m_hooks.filter(m_filterBefore);
  m_filterBefore.clear();
  for (int i = 0; i < int(m_lists.size()); ++i) showList(i);
  lightLists();
}

void GuidedCheck::selectionChanged(const std::vector<opad::Ref>& refs) {
  if (m_picking < 0) return;
  PickList& l = *m_lists[size_t(m_picking)];
  std::vector<opad::Ref> picked = l.base;
  auto has = [&picked](const opad::Ref& r) { return std::any_of(picked.begin(), picked.end(), [&](const opad::Ref& x) { return x.str() == r.str(); }); };
  for (const auto& r : refs) {
    if (r.body.empty()) continue;
    if (l.filter == "select.faces") {
      if (r.kind == opad::Ref::Kind::Face && !has(r)) picked.push_back(r);
    } else {
      opad::Ref b;
      b.body = r.body;
      if (!has(b)) picked.push_back(b);
    }
  }
  l.set(picked);
  showList(m_picking);
  for (auto& f : m_refreshers) f();
  scheduleWrite();
}

void GuidedCheck::lightLists() {
  if (!m_hooks.light || !isVisible()) return;
  std::vector<std::pair<opad::Ref, std::string>> lit;
  const int step = m_frame ? m_frame->current() : -1;
  const QString page = step >= 0 && step < m_stepIds.size() ? m_stepIds[step] : QString();
  for (const auto& l : m_lists) {
    // The page's own lists, and on Run every input at once.
    const bool here = page == "run" || (page == "part" && l->id == "bodies") || (page == "held" && l->id == "supports") || (page == "load" && l->id == "load") ||
                      (page == "heat" && l->id == "heat") || (page == "cool" && l->id == "heatsink") || (page == "ends" && (l->id == "inlet" || l->id == "outlet"));
    if (!here || (page == "run" && l->id == "bodies")) continue;
    for (const auto& r : l->get()) lit.push_back({r, l->nature.toStdString()});
  }
  if (m_picking < 0) m_hooks.light(lit);
  else m_hooks.light({});  // the selection shows what is picked
}

void GuidedCheck::propose() {
  if (!m_check || !m_hooks.jobs) return;
  AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return;
  const bool faces = m_checkId == "bend" || m_checkId == "print" || m_checkId == "rattle" || m_checkId == "fitting" || m_checkId == "fan";
  if (!faces) return;
  std::string forBodies;
  for (const auto& b : m_setup->bodies) forBodies += b + ",";
  if (m_proposedAt == d->revision && m_proposedFor == forBodies) return;
  if (m_proposing) m_proposing->cancel();
  m_proposedAt = d->revision, m_proposedFor = forBodies;
  const std::vector<std::string> bodies = m_setup->bodies.size() > 40 ? std::vector<std::string>() : m_setup->bodies;  // a whole assembly: nothing proposed
  const bool ends = m_checkId == "fitting" || m_checkId == "fan";
  auto result = std::make_shared<json>();
  m_proposing = d->readAsync(m_hooks.jobs(), tr("Looking at the part"),
                             [bodies, ends, result](const opad::Document& doc, const opad::Scene& s, Progress p) { *result = proposals(doc, s, bodies, ends, p); },
                             [this, self = QPointer<GuidedCheck>(this), result](bool ok, const QString&) {
                               if (!self || !ok || !result->is_object()) return;
                               for (const auto& l : m_lists) {
                                 l->proposal.clear();
                                 l->proposalText.clear();
                                 if (l->id == "supports") l->proposal = refsOf(result->value("supports", json()));
                                 if (l->id == "load") l->proposal = refsOf(result->value("load", json()));
                                 if (l->id == "supports" && !l->proposal.empty())
                                   l->proposalText = tr("OPAD found %n face(s) it rests on (at the bottom, facing down).", "", int(l->proposal.size()));
                                 if (l->id == "load" && !l->proposal.empty())
                                   l->proposalText = tr("OPAD found %n face(s) on top, facing up.", "", int(l->proposal.size()));
                                 const json ends = result->value("ends", json());
                                 if ((l->id == "inlet" || l->id == "outlet") && ends.is_array() && ends.size() == 2) {
                                   const json& e = ends[l->id == "inlet" ? 0 : 1];
                                   l->proposal = {opad::Ref::from_json(e["ref"])};
                                   l->proposalText = tr("OPAD found 2 open ends: this is the %1 one, %2 across.")
                                                         .arg(l->id == "inlet" ? tr("wider") : tr("narrower"), units::format(units::Kind::Length, e.value("size", 0.0)));
                                 }
                               }
                               // A new check takes the proposals at once: the user sees them lit and changes what is wrong.
                               if (m_study.empty() && m_written.empty())
                                 for (const auto& l : m_lists)
                                   if (!l->proposal.empty() && l->get().empty() && l->id != "load") l->set(l->proposal);
                               for (int i = 0; i < int(m_lists.size()); ++i) showList(i);
                               lightLists();
                               showRunState();
                             });
}

void GuidedCheck::stepShown(int i) {
  flush();
  stopPicking();
  lightLists();
  const QString page = i >= 0 && i < m_stepIds.size() ? m_stepIds[i] : QString();
  if (page == "run") showRunState();
  if (page == "results") showResults();
  // Next names where it goes.
  for (int k = 0; k + 1 < m_stepIds.size(); ++k)
    m_frame->setNextText(k, m_stepIds[k + 1] == "run" ? tr("Next: run") : m_stepIds[k + 1] == "results" ? tr("Next: results") : QString());
  for (int k = 0; k < m_stepIds.size(); ++k) {
    const QString s = m_stepIds[k];
    bool done = false;
    if (s == "part") done = !m_setup->bodies.empty();
    else if (s == "held") done = !m_setup->supports.empty();
    else if (s == "load") done = !m_setup->loadFaces.empty() || m_setup->selfWeight;
    else if (s == "heat") done = !m_setup->heat.empty();
    else if (s == "motion") done = !m_setup->joint.empty();
    else if (s == "ends") done = !m_setup->inlet.empty() && !m_setup->outlet.empty();
    else if (s == "run") done = hasResult();
    else if (s != "results") done = k < i;
    m_frame->setDone(k, done);
  }
}

void GuidedCheck::escape() {
  if (m_picking >= 0) stopPicking();
  else emit closeRequested();
}

// ---------------------------------------------------------------- Run
bool GuidedCheck::canRun() const {
  if (!m_check || (m_hooks.busy && m_hooks.busy())) return false;
  const simchecks::Readiness r = simchecks::readiness(*m_check, m_hooks.engines ? m_hooks.engines() : json::object());
  if (r.state != simchecks::Readiness::Ready) return false;
  const Setup& u = *m_setup;
  const QString id = m_check->id;
  if ((id == "bend" || id == "print" || id == "rattle") && (u.bodies.empty() || u.supports.empty())) return false;
  if ((id == "bend" || id == "print") && u.loadFaces.empty() && !u.selfWeight) return false;
  if ((id == "overheat" || id == "heatsink" || id == "warmup") && (u.heat.empty() || u.bodies.empty())) return false;
  if ((id == "clearance" || id == "forces") && u.joint.empty()) return false;
  if ((id == "fitting" || id == "fan") && (u.inlet.empty() || u.outlet.empty() || !m_planError.isEmpty())) return false;
  return true;
}

void GuidedCheck::showRunState() {
  if (!m_runButton || !m_check) return;
  const simchecks::Readiness r = simchecks::readiness(*m_check, m_hooks.engines ? m_hooks.engines() : json::object());
  const Setup& u = *m_setup;
  const QString id = m_check->id;
  QString missing;
  if ((id == "bend" || id == "print" || id == "rattle") && u.supports.empty()) missing = tr("Say where it is held (the Held by step).");
  else if ((id == "bend" || id == "print") && u.loadFaces.empty() && !u.selfWeight) missing = tr("Say what pushes on it (the Load step).");
  else if ((id == "overheat" || id == "heatsink" || id == "warmup") && u.heat.empty()) missing = tr("Add a part that gets hot (the What gets hot step).");
  else if ((id == "clearance" || id == "forces") && u.joint.empty()) missing = tr("Add a joint to drive (the Motion step).");
  else if ((id == "fitting" || id == "fan") && (u.inlet.empty() || u.outlet.empty())) missing = tr("Say where the air goes in and out (the In and out step).");
  else if (u.bodies.empty() && id != "clearance" && id != "forces") missing = tr("Choose the part to check (the Part step).");
  m_estimate->setText(tr("About how long: %1.").arg(m_check->time));
  QString note;
  m_solverButton->hide();
  if (r.state == simchecks::Readiness::Coming) {
    note = tr("This check comes with a later version of OPAD. You can look through its steps now.");
  } else if (r.state == simchecks::Readiness::NeedsSolver) {
    const simchecks::Solver* sv = simchecks::solver(r.solver);
    note = tr("%1: OPAD runs this check with %2, which is not on this computer yet. Set the check up now; it runs once the solver is in place.")
               .arg(r.chip, sv ? sv->name : r.solver);
    const auto pack = sv && sv->external && m_hooks.installSolver ? engineinstall::pack(r.solver) : std::nullopt;
    const bool installing = pack && m_hooks.installing && m_hooks.installing(r.solver);
    m_solverButton->setText(installing ? tr("Installing %1…").arg(sv->name)
                            : pack     ? tr("Install %1 (%2)").arg(sv->name, engineinstall::sizeText(pack->size))
                            : sv && sv->external ? tr("Get %1…").arg(sv->name)
                                                 : tr("About %1…").arg(sv ? sv->name : r.solver));
    m_solverButton->setEnabled(!installing);
    m_solverButton->show();
  } else if (!missing.isEmpty()) {
    note = missing;
  } else if (m_hooks.busy && m_hooks.busy()) {
    note = tr("Another study is running: wait for it, or cancel it in the status bar.");
  }
  m_runNote->setText(note);
  m_runNote->setVisible(!note.isEmpty());
  m_runButton->setEnabled(canRun());
  if (m_cooling) m_cooling->setEnabled(m_hooks.engines && m_hooks.engines().value("cfd", false));
}

void GuidedCheck::runNow() {
  flush();
  write();
  if (!canRun() || m_study.empty()) return showRunState();
  m_run.reset();
  m_status->setText(tr("Running… the status bar shows how far it is and can cancel it."));
  m_runButton->setEnabled(false);
  if (m_hooks.run) m_hooks.run(m_study);
}

void GuidedCheck::runFinished(const std::string& study, std::shared_ptr<const opad::sim::StudyRun> run, const QString& error) {
  if (study != m_study || !m_status) return;
  if (!run) {
    m_status->setText(error.isEmpty() ? tr("Cancelled.") : tr("It did not run: %1").arg(error));
    showRunState();
    return;
  }
  m_run = std::move(run);
  m_status->setText(tr("Done."));
  showRunState();
  m_frame->setCurrent(stepOf("results"));
  showResults();
  emit changed();
}

void GuidedCheck::enginesChanged() {
  showRunState();
  emit changed();
}

void GuidedCheck::showStep(const QString& which) {
  if (!m_frame) return;
  if (which == "compare") {
    m_frame->setCurrent(stepOf("results"));
    if (m_compareWith) m_compareWith->setFocus();
    return;
  }
  m_frame->setCurrent(stepOf(which));
}

// ---------------------------------------------------------------- Results
opad::json GuidedCheck::summary() const {
  if (m_run && !m_run->summary.empty()) return m_run->summary;
  const AppDocument* d = m_hooks.document();
  const opad::Study* st = d && !m_study.empty() ? d->scene.study(m_study) : nullptr;
  return st && st->result.is_object() ? st->result : json::object();
}

bool GuidedCheck::hasResult() const { return !summary().empty(); }

QString GuidedCheck::verdict() const { return m_verdict; }

void GuidedCheck::showResults() {
  if (!m_verdictCard || !m_check) return;
  const json s = summary();
  const Verdict v = verdictOf(m_check->id, s, m_setup->toJson(), m_run.get());
  m_verdict = v.line();
  const Tokens& k = theme::current();
  const QColor c = v.state == Verdict::Good ? k.green : v.state == Verdict::Warn ? k.amber : v.state == Verdict::Bad ? k.red : k.sel;
  QColor bg = c;
  bg.setAlphaF(0.12f);
  m_verdictCard->setStyleSheet(QString("QFrame#simVerdict { border: 1px solid %1; border-radius: 8px; background: %2; }").arg(c.name(), bg.name(QColor::HexArgb)));
  m_verdictHead->setStyleSheet(QString("color: %1;").arg(c.name()));
  if (v.state == Verdict::None) {
    m_verdictHead->setText(tr("No result yet"));
    m_verdictBig->setText(s.empty() ? tr("Run the check to see it here.") : tr("This result does not say enough for a verdict: see the numbers in the Simulation panel."));
    m_verdictText->clear();
  } else {
    m_verdictHead->setText(v.head);
    m_verdictBig->setText(v.big.left(1).toUpper() + v.big.mid(1));
    m_verdictText->setText(v.text);
  }
  while (QLayoutItem* it = m_numbers->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  int row = 0;
  for (const auto& [label, value] : v.numbers) {
    auto* l = new QLabel(label, m_numbersBox);
    l->setObjectName("secondary");
    auto* x = new QLabel(value, m_numbersBox);
    QFont f = x->font();
    f.setBold(true);
    x->setFont(f);
    m_numbers->addWidget(l, row, 0);
    m_numbers->addWidget(x, row++, 1);
  }
  m_numbersBox->setVisible(!v.numbers.empty());
  // The colour maps and the motion: buttons for what this result has.
  auto* grid = static_cast<QGridLayout*>(m_fieldButtons->layout());
  while (QLayoutItem* it = grid->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  std::vector<std::pair<QString, QString>> fields;
  const QString kind = m_check->kind;
  if (!s.empty()) {
    if (kind == "static") {
      fields = {{tr("Colour by stress"), "von_mises"}, {tr("How far it bends"), "displacement"}};
      if (m_check->id == "print") fields.push_back({tr("Where the print fails"), "failure_index"});
    } else if (kind == "modal") {
      const json f = s.value("frequencies_Hz", json::array());
      for (size_t i = 0; i < f.size() && i < 4; ++i) fields.push_back({tr("Shape %1 (%2 Hz)").arg(i + 1).arg(num(f[i].get<double>())), QString("mode:%1").arg(i)});
    } else if (kind == "thermal") {
      fields = {{tr("Temperatures"), "temperature"}};
      if (m_check->id == "warmup") fields.push_back({tr("Play the warm-up"), "@simulate.play"});
    } else if (kind == "motion" || kind == "dynamic") {
      fields = {{tr("Play the motion"), "@simulate.play"}};
    } else if (kind == "flow") {
      fields = {{tr("Air speed"), "speed"}, {tr("Pressure"), "pressure"}, {tr("Streamlines"), "streamlines"}};
    }
  }
  int n = 0;
  for (const auto& [label, field] : fields) {
    auto* b = new QPushButton(label, m_fieldButtons);
    b->setObjectName("checkField_" + field);
    grid->addWidget(b, n / 2, n % 2);
    ++n;
    connect(b, &QPushButton::clicked, this, [this, f = field] {
      if (f.startsWith('@')) {
        if (m_hooks.command) m_hooks.command(f.mid(1));
      } else if (m_hooks.showField) {
        m_hooks.showField(f);
      }
    });
  }
  m_fieldButtons->setVisible(n > 0);
  showCompare();
}

void GuidedCheck::showCompare() {
  if (!m_compareBox || !m_check) return;
  const AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return;
  // The other runs of the same check (copies kept before a design change), with a result.
  const QString was = m_compareWith->currentData().toString();
  {
    const QSignalBlocker quiet(m_compareWith);
    m_compareWith->clear();
    for (const auto& st : d->scene.studies)
      if (st.id != m_study && QString::fromStdString(st.def.value("settings", json::object()).value("check", std::string())) == m_checkId &&
          st.result.is_object() && !st.result.empty())
        m_compareWith->addItem(QString::fromStdString(st.name), QString::fromStdString(st.id));
    m_compareWith->setCurrentIndex(std::max(0, m_compareWith->findData(was)));
  }
  while (QLayoutItem* it = m_compareGrid->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  m_compareWith->setVisible(m_compareWith->count() > 0);
  if (m_compareWith->count() == 0) {
    m_compareText->setText(hasResult() ? tr("Change the design, then compare: Try a change keeps this result as a copy and runs the changed design as this check.")
                                       : tr("Run the check first."));
    return;
  }
  const opad::Study* other = d->scene.study(m_compareWith->currentData().toString().toStdString());
  if (!other) return;
  const json guidedB = other->def.value("settings", json::object()).value("guided", json());
  const Verdict a = verdictOf(m_checkId, summary(), m_setup->toJson(), m_run.get());
  const Verdict b = verdictOf(m_checkId, other->result, guidedB, nullptr);
  m_compareGrid->addWidget(new QLabel(QString(), m_compareBox), 0, 0);
  auto bold = [this](const QString& s) {
    auto* l = new QLabel(s, m_compareBox);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
  };
  m_compareGrid->addWidget(bold(tr("A: this one")), 0, 1);
  m_compareGrid->addWidget(bold(tr("B: %1").arg(QString::fromStdString(other->name))), 0, 2);
  int row = 1;
  for (const auto& [label, value] : a.numbers) {
    QString vb;
    for (const auto& [lb, x] : b.numbers)
      if (lb == label) vb = x;
    m_compareGrid->addWidget(new QLabel(label, m_compareBox), row, 0);
    m_compareGrid->addWidget(new QLabel(value, m_compareBox), row, 1);
    m_compareGrid->addWidget(new QLabel(vb.isEmpty() ? QStringLiteral("–") : vb, m_compareBox), row++, 2);
  }
  m_compareText->setText(b.head.isEmpty() ? QString() : tr("B: %1").arg(b.line()));
}

void GuidedCheck::keepCopy() {
  flush();
  AppDocument* d = m_hooks.document();
  const opad::Study* st = d && !m_study.empty() ? d->scene.study(m_study) : nullptr;
  if (!st || !st->result.is_object() || st->result.empty()) {
    m_compareText->setText(tr("Run the check first: there is nothing to keep yet."));
    return;
  }
  // The copy keeps this result under a new name; this check (its id, its loads) goes on with the changed design.
  std::set<std::string> taken;
  for (const auto& x : d->scene.studies) taken.insert(x.name);
  std::string name;
  for (int n = 1; name.empty() || taken.count(name); ++n) name = st->name + " · " + tr("before change %1").arg(n).toStdString();
  const json settings = st->def.value("settings", json::object());
  const json result = st->result;
  const std::string kind = st->kind;
  std::string copy;
  m_writing = true;
  try {
    copy = m_hooks.write("study", {{"kind", kind}, {"name", name}, {"settings", settings}, {"run", false}}, tr("Keep the result")).value("id", std::string());
  } catch (const std::exception& e) {
    m_compareText->setText(i18n::message(QString::fromStdString(e.what())));
  }
  m_writing = false;
  // Its result is this run again (the cache answers at once while the design has not changed), on a worker as every run.
  if (!copy.empty() && m_hooks.run) m_hooks.run(copy);
  (void)result;
  showCompare();
  m_compareText->setText(tr("Kept as \"%1\". Change the design, run this check again, then compare.").arg(QString::fromStdString(name)));
}

QString GuidedCheck::verdictLine(const QString& check, const opad::json& summary, const opad::json& guided) {
  return verdictOf(check, summary, guided.is_object() ? guided : json::object(), nullptr).line();
}

void GuidedCheck::hideEvent(QHideEvent* e) {
  flush();         // a change still to write
  stopPicking();   // the view's clicks select again
  if (m_hooks.light) m_hooks.light({});
  QWidget::hideEvent(e);
}

void GuidedCheck::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  lightLists();
}

void GuidedCheck::setValue(const QString& field, double v) {
  if (field == "weight") m_setup->loadKind = "weight", m_setup->loadValue = v;
  else if (field == "need") m_setup->need = v;
  else if (field == "limit") m_setup->limit = v;
  else if (field == "flow") m_setup->flow = v, m_setup->airMode = "flow";
  else if (field == "travel") m_setup->travel = v;
  else if (field == "watts" && !m_setup->heat.empty()) m_setup->heat.front().second = v;
  else if (field == "freeFlow") m_setup->freeFlow = v, m_setup->airMode = "fan";
  else if (field == "shutoff") m_setup->shutoff = v, m_setup->airMode = "fan";
  else if (field == "quick") m_setup->quality = v > 0 ? "quick" : "normal";
  m_loading = true;
  for (auto& f : m_refreshers) f();
  m_loading = false;
  scheduleWrite();
}
