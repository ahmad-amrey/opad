#include "AgentSentences.hpp"

#include <QRegularExpression>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "I18n.hpp"
#include "opad/design/feature.hpp"
#include "opad/live.hpp"

using opad::json;

namespace agentnames {
Names display(const QString& raw) {
  using agentsay::Say;
  const QString name = raw.simplified(), lower = name.toLower();
  const auto starts = [&](std::initializer_list<const char*> prefixes) {
    for (const char* p : prefixes)
      if (lower.startsWith(QLatin1String(p))) return true;
    return false;
  };
  if (starts({"claude-code", "claude code", "claude_code"})) return {QStringLiteral("Claude Code"), QStringLiteral("Claude")};
  if (starts({"claude-ai", "claude desktop", "claude-desktop", "claude_desktop"})) return {QStringLiteral("Claude Desktop"), QStringLiteral("Claude")};
  if (starts({"claude"})) return {QStringLiteral("Claude"), QStringLiteral("Claude")};
  if (starts({"codex"})) return {QStringLiteral("Codex"), QStringLiteral("Codex")};
  if (starts({"cursor"})) return {QStringLiteral("Cursor"), QStringLiteral("Cursor")};
  QString spaced = name;
  spaced.replace(QLatin1Char('-'), QLatin1Char(' ')).replace(QLatin1Char('_'), QLatin1Char(' '));
  QStringList words;
  for (const QString& w : spaced.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
    const QString l = w.toLower();
    if (l == QLatin1String("mcp") || l == QLatin1String("client")) continue;
    words << w.left(1).toUpper() + w.mid(1);
  }
  QString shown = words.join(QLatin1Char(' '));
  if (shown.isEmpty()) return {Say::tr("AI assistant"), Say::tr("AI assistant")};
  if (shown.size() > 40) shown = shown.left(39) + QChar(0x2026);
  return {shown, shown};
}
}  // namespace agentnames

namespace agentsay {
namespace {
QString S(const std::string& s) { return QString::fromStdString(s); }
QString cap(QString s, int n) {
  s = s.simplified();
  if (s.size() > n) s = s.left(n - 1) + QChar(0x2026);
  return s;
}
// The name of a node, step, sketch or parameter, isolated; looked up after the call, then before it (a removed thing).
QString nameIn(const opad::Scene& s, const std::string& id) {
  if (const auto* n = s.node(id)) return n->name.empty() ? QString() : isolate(cap(S(n->name), 48));
  if (const auto* f = s.feature(id)) return isolate(cap(S(f->name), 48));
  if (const auto* k = s.sketch(id)) return isolate(cap(S(k->name), 48));
  for (const auto& p : s.params)
    if (p.id == id || p.name == id) return isolate(cap(S(p.name), 48));
  return {};
}
struct Scenes {
  const opad::Scene& before;
  const opad::Scene& after;
  const units::Display& d;
  QString name(const std::string& id) const {
    QString n = nameIn(after, id);
    if (n.isEmpty()) n = nameIn(before, id);
    return n.isEmpty() ? Say::tr("a part") : n;
  }
  bool known(const std::string& id) const { return after.node(id) || before.node(id); }
  QString bodies(const std::vector<std::string>& ids) const {
    std::vector<std::string> unique;
    for (const auto& id : ids)
      if (std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
    if (unique.empty()) return {};
    if (unique.size() == 1) return name(unique[0]);
    if (unique.size() == 2) return Say::tr("%1 and %2").arg(name(unique[0]), name(unique[1]));
    return Say::tr("%1 parts").arg(unique.size());
  }
};
// Body, face, edge and vertex references anywhere in a value: "uuid/edge/3", {"body":..,"kind":..,"index":..}, lists.
void collect(const Scenes& sc, const json& v, std::vector<opad::Ref>& out) {
  if (v.is_string()) {
    const auto& text = v.get_ref<const std::string&>();
    if (text.size() < 8 || text.find(' ') != std::string::npos) return;
    try {
      auto r = opad::Ref::parse(text);
      if (sc.known(r.body)) out.push_back(r);
    } catch (const std::exception&) {}
  } else if (v.is_object()) {
    if (v.contains("body") && v["body"].is_string()) {
      try {
        auto r = opad::Ref::from_json(v);
        if (sc.known(r.body)) out.push_back(r);
      } catch (const std::exception&) {}
      return;
    }
    for (const auto& [k, c] : v.items())
      if (k != "hint" && k != "references") collect(sc, c, out);
  } else if (v.is_array()) {
    for (const auto& c : v) collect(sc, c, out);
  }
}
std::vector<opad::Ref> refsOf(const Scenes& sc, const json& v) {
  std::vector<opad::Ref> out;
  collect(sc, v, out);
  return out;
}
QString countOf(opad::Ref::Kind k, int n) {
  using K = opad::Ref::Kind;
  switch (k) {
    case K::Face: return n == 1 ? Say::tr("a face") : n == 2 ? Say::tr("2 faces") : Say::tr("%1 faces").arg(n);
    case K::Edge: return n == 1 ? Say::tr("an edge") : n == 2 ? Say::tr("2 edges") : Say::tr("%1 edges").arg(n);
    case K::Vertex: return n == 1 ? Say::tr("a corner") : n == 2 ? Say::tr("2 corners") : Say::tr("%1 corners").arg(n);
    case K::Center: return n == 1 ? Say::tr("a circle's centre") : Say::tr("%1 circle centres").arg(n);
    default: return n == 1 ? Say::tr("a point") : Say::tr("%1 points").arg(n);
  }
}
// "4 edges of Bracket", "a face of Lid", "Bracket and Lid"; empty when nothing is named.
QString things(const Scenes& sc, const std::vector<opad::Ref>& refs) {
  if (refs.empty()) return {};
  std::vector<std::string> owners;
  bool whole = true, same = true;
  for (const auto& r : refs) {
    owners.push_back(r.body);
    whole = whole && r.kind == opad::Ref::Kind::Body;
    same = same && r.kind == refs.front().kind;
  }
  if (whole) return sc.bodies(owners);
  int parts = 0;
  for (const auto& r : refs)
    if (r.kind != opad::Ref::Kind::Body) ++parts;
  const QString what = same ? countOf(refs.front().kind, parts) : (parts == 2 ? Say::tr("2 things") : Say::tr("%1 things").arg(parts));
  return Say::tr("%1 of %2").arg(what, sc.bodies(owners));
}
std::vector<std::string> ownersOf(const std::vector<opad::Ref>& refs) {
  std::vector<std::string> out;
  for (const auto& r : refs)
    if (std::find(out.begin(), out.end(), r.body) == out.end()) out.push_back(r.body);
  return out;
}
// A value as the user reads it: a plain amount in the shown unit ("2 mm", "0.079 in", "90°"), an expression as written.
QString amount(const json& v, units::Kind kind, const units::Display& d) {
  if (v.is_number()) return isolate(units::compact(kind, v.get<double>(), d));
  if (v.is_string()) {
    const QString text = S(v.get<std::string>()).trimmed();
    static const QRegularExpression plainAmount(QStringLiteral("^-?[0-9]*\\.?[0-9]+\\s*(mm|deg|°)?$"));
    if (plainAmount.match(text).hasMatch()) {
      if (auto parsed = units::parse(kind, kind == units::Kind::Length && !text.endsWith(QLatin1String("mm")) ? text + " mm" : text, d))
        return isolate(units::compact(kind, *parsed, d));
    }
    return isolate(cap(text, 40));
  }
  return {};
}
const opad::design::FeatureSpec* specOf(const std::string& kind) { return opad::design::feature_spec(kind); }
json input(const json& inputs, const std::string& kind, const std::string& key) {
  if (inputs.is_object() && inputs.contains(key) && !inputs[key].is_null()) return inputs[key];
  if (const auto* spec = specOf(kind))
    for (const auto& in : spec->inputs)
      if (in.name == key) return in.def;
  return nullptr;
}
QString label(const std::string& kind) {
  if (const auto* spec = specOf(kind)) return i18n::t(S(spec->label));
  return S(kind);
}
QString planeText(const Scenes& sc, const json& p) {
  if (p.is_object()) {
    if (p.contains("base") && p["base"].is_string()) return Say::tr("the %1 plane").arg(S(p["base"].get<std::string>()).toUpper());
    if (p.contains("face")) {
      const auto refs = refsOf(sc, p["face"]);
      if (!refs.empty()) return Say::tr("a face of %1").arg(sc.name(refs.front().body));
    }
    if (p.contains("feature") && p["feature"].is_string()) return sc.name(p["feature"].get<std::string>());
  }
  return Say::tr("a plane");
}
QString axisText(const Scenes& sc, const json& a) {
  if (a.is_object()) {
    if (a.contains("base") && a["base"].is_string()) return Say::tr("the %1 axis").arg(S(a["base"].get<std::string>()).toUpper());
    if (a.contains("edge")) {
      const auto refs = refsOf(sc, a["edge"]);
      if (!refs.empty()) return Say::tr("an edge of %1").arg(sc.name(refs.front().body));
    }
    if (a.contains("sketch") && a["sketch"].is_string()) return Say::tr("a line of %1").arg(sc.name(a["sketch"].get<std::string>()));
    if (a.contains("feature") && a["feature"].is_string()) return sc.name(a["feature"].get<std::string>());
  }
  return Say::tr("an axis");
}
// Sketches named by profiles or a path: "Sketch 1", "Sketch 1 and Sketch 2", "3 sketches".
QString sketchesIn(const Scenes& sc, const json& v) {
  std::vector<std::string> ids;
  std::function<void(const json&)> walk = [&](const json& x) {
    if (x.is_object()) {
      if (x.contains("sketch") && x["sketch"].is_string()) {
        const auto id = x["sketch"].get<std::string>();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
      }
      for (const auto& [k, c] : x.items()) walk(c);
    } else if (x.is_array()) {
      for (const auto& c : x) walk(c);
    }
  };
  walk(v);
  if (ids.empty()) return {};
  if (ids.size() == 1) return sc.name(ids[0]);
  if (ids.size() == 2) return Say::tr("%1 and %2").arg(sc.name(ids[0]), sc.name(ids[1]));
  return Say::tr("%1 sketches").arg(ids.size());
}
std::vector<std::string> idsIn(const json& v) {
  std::vector<std::string> out;
  if (v.is_string()) out.push_back(v.get<std::string>());
  else if (v.is_array())
    for (const auto& x : v)
      if (x.is_string()) out.push_back(x.get<std::string>());
  return out;
}
std::vector<std::string> madeBodies(const opad::Feature& f) {
  std::vector<std::string> out;
  if (!f.result.is_object()) return out;
  for (const auto& b : f.result.value("bodies", json::array()))
    if (b.contains("id") && b["id"].is_string()) out.push_back(b["id"].get<std::string>());
  return out;
}
// Join, cut and intersect name what they changed.
QString withOperation(const Scenes& sc, QString text, const json& inputs, const std::string& kind) {
  const auto op = input(inputs, kind, "operation");
  if (!op.is_string()) return text;
  const auto targets = sc.bodies(ownersOf(refsOf(sc, input(inputs, kind, "targets"))));
  const auto o = op.get<std::string>();
  if (targets.isEmpty()) return text;
  if (o == "join") return Say::tr("%1, joined to %2").arg(text, targets);
  if (o == "cut") return Say::tr("%1, cut from %2").arg(text, targets);
  if (o == "intersect") return Say::tr("%1, intersected with %2").arg(text, targets);
  return text;
}
QString lenOf(const Scenes& sc, const json& inputs, const std::string& kind, const char* key) { return amount(input(inputs, kind, key), units::Kind::Length, sc.d); }
QString angOf(const Scenes& sc, const json& inputs, const std::string& kind, const char* key) { return amount(input(inputs, kind, key), units::Kind::Angle, sc.d); }
QString numOf(const json& inputs, const std::string& kind, const char* key) {
  const auto v = input(inputs, kind, key);
  if (v.is_number()) return QString::number(v.get<double>());
  if (v.is_string()) return isolate(cap(S(v.get<std::string>()), 20));
  return QStringLiteral("?");
}
// "40 × 20 × 10 mm" when every size is in the same unit, else each with its own ("40 mm × width").
QString dims(const QStringList& isolated, const QString& joiner) {
  QStringList sizes;
  for (const auto& v : isolated) sizes << plain(v);
  QString unit;
  bool same = true;
  for (const auto& v : sizes) {
    const int space = v.lastIndexOf(QLatin1Char(' '));
    const QString u = space < 0 ? QString() : v.mid(space + 1);
    if (u.isEmpty() || (!unit.isEmpty() && u != unit)) same = false;
    unit = u;
  }
  if (!same) return isolate(sizes.join(joiner));
  QStringList numbers;
  for (const auto& v : sizes) numbers << v.left(v.lastIndexOf(QLatin1Char(' ')));
  return isolate(numbers.join(joiner) + QLatin1Char(' ') + unit);
}
QString featureText(const Scenes& sc, const std::string& kind, const json& in, const std::vector<std::string>& made) {
  const auto subject = [&](const char* key) { return things(sc, refsOf(sc, input(in, kind, key))); };
  const auto bodiesOf = [&](const char* key) { return sc.bodies(ownersOf(refsOf(sc, input(in, kind, key)))); };
  if (kind == "box")
    return withOperation(sc, Say::tr("Added a %1 box").arg(dims({lenOf(sc, in, kind, "length"), lenOf(sc, in, kind, "width"), lenOf(sc, in, kind, "height")}, QStringLiteral(" × "))), in, kind);
  if (kind == "cylinder")
    return withOperation(sc, Say::tr("Added a Ø%1 cylinder").arg(dims({lenOf(sc, in, kind, "diameter"), lenOf(sc, in, kind, "height")}, QStringLiteral(" × "))), in, kind);
  if (kind == "sphere") return withOperation(sc, Say::tr("Added a Ø%1 sphere").arg(lenOf(sc, in, kind, "diameter")), in, kind);
  if (kind == "cone")
    return withOperation(sc, Say::tr("Added a cone, Ø%1 → Ø%2, %3 tall").arg(lenOf(sc, in, kind, "diameter"), lenOf(sc, in, kind, "top_diameter"), lenOf(sc, in, kind, "height")), in, kind);
  if (kind == "torus") return withOperation(sc, Say::tr("Added a torus, Ø%1").arg(lenOf(sc, in, kind, "diameter")), in, kind);
  if (kind == "coil")
    return withOperation(sc, Say::tr("Added a coil, %1 turns, Ø%2").arg(numOf(in, kind, "turns"), lenOf(sc, in, kind, "diameter")), in, kind);
  if (kind == "gear") return withOperation(sc, Say::tr("Added a %1-tooth gear (module %2)").arg(numOf(in, kind, "teeth"), lenOf(sc, in, kind, "module")), in, kind);
  if (kind == "extrude") {
    const QString profile = sketchesIn(sc, input(in, kind, "profiles"));
    const auto extent = input(in, kind, "extent").is_string() ? input(in, kind, "extent").get<std::string>() : std::string("distance");
    QString how;
    if (extent == "all") how = Say::tr("through all");
    else if (extent == "to_face") how = Say::tr("up to %1").arg(subject("extent_face"));
    else if (extent == "to_body") how = Say::tr("up to %1").arg(bodiesOf("extent_body"));
    else {
      how = lenOf(sc, in, kind, "distance");
      const auto dir = input(in, kind, "direction");
      if (dir.is_string() && dir.get<std::string>() == "symmetric") how = Say::tr("%1 each way").arg(how);
    }
    const auto op = input(in, kind, "operation");
    const auto o = op.is_string() ? op.get<std::string>() : std::string("new");
    const QString text = Say::tr("Extruded %1 %2").arg(profile.isEmpty() ? Say::tr("a profile") : profile, how);
    if (o == "new") return Say::tr("%1 as a new body").arg(text);
    return withOperation(sc, text, in, kind);
  }
  if (kind == "revolve") return withOperation(sc, Say::tr("Revolved %1 %2").arg(sketchesIn(sc, input(in, kind, "profiles")), angOf(sc, in, kind, "angle")), in, kind);
  if (kind == "sweep") return withOperation(sc, Say::tr("Swept %1 along %2").arg(sketchesIn(sc, input(in, kind, "profiles")), sketchesIn(sc, input(in, kind, "path"))), in, kind);
  if (kind == "loft") return withOperation(sc, Say::tr("Lofted %1").arg(sketchesIn(sc, input(in, kind, "profiles"))), in, kind);
  if (kind == "pipe") return withOperation(sc, Say::tr("Added a Ø%1 pipe along %2").arg(lenOf(sc, in, kind, "diameter"), sketchesIn(sc, input(in, kind, "path"))), in, kind);
  if (kind == "thicken") return withOperation(sc, Say::tr("Thickened %1 by %2").arg(subject("faces"), lenOf(sc, in, kind, "thickness")), in, kind);
  if (kind == "mesh_solid") return Say::tr("Converted the mesh %1 to a solid").arg(bodiesOf("bodies"));
  if (kind == "hole") {
    int n = 0;
    for (const auto& p : input(in, kind, "points")) (void)p, ++n;
    const QString target = sc.bodies(ownersOf(refsOf(sc, input(in, kind, "targets"))));
    const auto extent = input(in, kind, "extent");
    const QString size = extent.is_string() && extent.get<std::string>() == "all"
                             ? Say::tr("Ø%1 through all").arg(lenOf(sc, in, kind, "diameter"))
                             : Say::tr("Ø%1 × %2 deep").arg(lenOf(sc, in, kind, "diameter"), lenOf(sc, in, kind, "depth"));
    const QString holes = n <= 1 ? Say::tr("a hole") : n == 2 ? Say::tr("2 holes") : Say::tr("%1 holes").arg(n);
    return target.isEmpty() ? Say::tr("Added %1, %2").arg(holes, size) : Say::tr("Added %1, %2, to %3").arg(holes, size, target);
  }
  if (kind == "fillet") return Say::tr("Filleted %1, %2").arg(subject("edges"), lenOf(sc, in, kind, "radius"));
  if (kind == "chamfer") return Say::tr("Chamfered %1, %2").arg(subject("edges"), lenOf(sc, in, kind, "distance"));
  if (kind == "shell") {
    const auto open = refsOf(sc, input(in, kind, "faces"));
    QString who = bodiesOf("bodies");
    if (who.isEmpty()) who = sc.bodies(ownersOf(open));
    const QString faces = open.size() == 1 ? Say::tr("1 face open") : Say::tr("%1 faces open").arg(open.size());
    return open.empty() ? Say::tr("Shelled %1, %2 walls").arg(who, lenOf(sc, in, kind, "thickness"))
                        : Say::tr("Shelled %1, %2 walls, %3").arg(who, lenOf(sc, in, kind, "thickness"), faces);
  }
  if (kind == "draft") return Say::tr("Drafted %1 by %2").arg(subject("faces"), angOf(sc, in, kind, "angle"));
  if (kind == "offset_face") return Say::tr("Pushed or pulled %1 by %2").arg(subject("faces"), lenOf(sc, in, kind, "distance"));
  if (kind == "remove_faces") return Say::tr("Removed %1").arg(subject("faces"));
  if (kind == "scale") return Say::tr("Scaled %1 %2×").arg(bodiesOf("bodies"), numOf(in, kind, "factor"));
  if (kind == "combine") {
    const auto op = input(in, kind, "operation");
    const QString target = bodiesOf("target"), tools = bodiesOf("tools");
    const auto o = op.is_string() ? op.get<std::string>() : std::string("join");
    if (o == "cut") return Say::tr("Cut %1 from %2").arg(tools, target);
    if (o == "intersect") return Say::tr("Intersected %1 with %2").arg(target, tools);
    return Say::tr("Joined %1 into %2").arg(tools, target);
  }
  if (kind == "split") return Say::tr("Split %1 along %2").arg(bodiesOf("bodies"), planeText(sc, input(in, kind, "plane")));
  if (kind == "mirror") return withOperation(sc, Say::tr("Mirrored %1 across %2").arg(bodiesOf("bodies"), planeText(sc, input(in, kind, "plane"))), in, kind);
  if (kind == "pattern_rect") {
    const auto second = input(in, kind, "second");
    if (second.is_boolean() && second.get<bool>())
      return Say::tr("Patterned %1 %2 × %3").arg(bodiesOf("bodies"), numOf(in, kind, "count"), numOf(in, kind, "count2"));
    return Say::tr("Patterned %1 %2 times, %3 apart").arg(bodiesOf("bodies"), numOf(in, kind, "count"), lenOf(sc, in, kind, "spacing"));
  }
  if (kind == "pattern_circ") return Say::tr("Patterned %1 %2 times around %3").arg(bodiesOf("bodies"), numOf(in, kind, "count"), axisText(sc, input(in, kind, "axis")));
  if (kind == "pattern_points") return Say::tr("Copied %1 to points of %2").arg(bodiesOf("bodies"), sketchesIn(sc, input(in, kind, "points")));
  if (kind == "move") {
    const QString who = bodiesOf("bodies");
    const auto copy = input(in, kind, "copy");
    if (copy.is_boolean() && copy.get<bool>()) return Say::tr("Made a moved copy of %1").arg(who);
    const auto rotate = input(in, kind, "rotate");
    if (rotate.is_boolean() && rotate.get<bool>()) return Say::tr("Rotated %1 %2 about %3").arg(who, angOf(sc, in, kind, "angle"), axisText(sc, input(in, kind, "axis")));
    return Say::tr("Moved %1").arg(who);
  }
  if (kind == "align") return Say::tr("Aligned %1").arg(bodiesOf("bodies"));
  if (kind == "remove") return Say::tr("Removed %1 from the design").arg(bodiesOf("bodies"));
  if (kind == "plane") return Say::tr("Added a construction plane");
  if (kind == "axis") return Say::tr("Added a construction axis");
  if (kind == "interference") return Say::tr("Added an interference check on %1").arg(bodiesOf("bodies"));
  const QString target = sc.bodies(made);
  return target.isEmpty() ? Say::tr("Added %1").arg(label(kind)) : Say::tr("Added %1 to %2").arg(label(kind), target);
}
QString iconFor(const std::string& kind) {
  static const std::map<std::string, const char*> icons = {
      {"extrude", "extrude"}, {"revolve", "revolve"}, {"sweep", "sweep"}, {"loft", "loft"}, {"pipe", "pipe"}, {"hole", "hole"},
      {"fillet", "fillet"}, {"chamfer", "chamfer"}, {"shell", "shell"}, {"draft", "draft"}, {"offset_face", "presspull"},
      {"scale", "scale"}, {"combine", "combine"}, {"split", "split"}, {"mirror", "mirror"}, {"pattern_rect", "patternRect"},
      {"pattern_circ", "patternCirc"}, {"pattern_points", "patternPoints"}, {"move", "move"}, {"align", "align"}, {"remove", "delete"},
      {"plane", "plane"}, {"axis", "axis"}, {"box", "box"}, {"cylinder", "cylinder"}, {"sphere", "sphere"}, {"cone", "cone"},
      {"torus", "torus"}, {"coil", "coil"}, {"gear", "gear"}, {"thicken", "thicken"}, {"interference", "interference"}, {"remove_faces", "delete"}};
  const auto it = icons.find(kind);
  return it == icons.end() ? QStringLiteral("aiMark") : QString::fromLatin1(it->second);
}
QString second(const Scenes& sc, const QString& step, const std::vector<std::string>& bodies) {
  const QString who = sc.bodies(bodies);
  if (step.isEmpty()) return who;
  if (who.isEmpty() || plain(who) == plain(step)) return step;  // a body named as its step: once
  return Say::tr("%1 · %2").arg(step, who);
}
// The value of the first numeric key named so, anywhere in a result.
std::optional<std::pair<std::string, double>> numberIn(const json& v, std::initializer_list<const char*> keys, int depth = 0) {
  if (depth > 4) return std::nullopt;
  if (v.is_object()) {
    for (const char* k : keys)
      if (v.contains(k) && v[k].is_number()) return std::make_pair(std::string(k), v[k].get<double>());
    for (const auto& [k, c] : v.items())
      if (auto found = numberIn(c, keys, depth + 1)) return found;
  } else if (v.is_array()) {
    for (const auto& c : v)
      if (auto found = numberIn(c, keys, depth + 1)) return found;
  }
  return std::nullopt;
}
QString fileName(const json& v) {
  if (!v.is_string()) return {};
  QString path = S(v.get<std::string>());
  path.replace(QLatin1Char('\\'), QLatin1Char('/'));
  return isolate(cap(path.section(QLatin1Char('/'), -1), 60));
}
QString viewName(const std::string& view) {
  if (view == "front") return Say::tr("Looked at the model from the front");
  if (view == "back") return Say::tr("Looked at the model from the back");
  if (view == "top") return Say::tr("Looked at the model from above");
  if (view == "bottom") return Say::tr("Looked at the model from below");
  if (view == "left") return Say::tr("Looked at the model from the left");
  if (view == "right") return Say::tr("Looked at the model from the right");
  return Say::tr("Looked at the model");
}
QString sketchShapes(const json& geometry) {
  std::map<std::string, int> counts;
  for (const auto& e : geometry.value("entities", json::array())) {
    if (!e.is_object()) continue;
    if (e.value("construction", false)) continue;
    std::string type = e.value("type", e.value("kind", std::string()));
    if (type == "line" || type == "segment") type = "line";
    else if (type == "circle") type = "circle";
    else if (type.find("arc") != std::string::npos) type = "arc";
    else if (type.find("spline") != std::string::npos) type = "spline";
    else continue;
    ++counts[type];
  }
  QStringList parts;
  const auto phrase = [](const std::string& t, int n) -> QString {
    if (t == "line") return n == 1 ? Say::tr("a line") : n == 2 ? Say::tr("2 lines") : Say::tr("%1 lines").arg(n);
    if (t == "circle") return n == 1 ? Say::tr("a circle") : n == 2 ? Say::tr("2 circles") : Say::tr("%1 circles").arg(n);
    if (t == "arc") return n == 1 ? Say::tr("an arc") : n == 2 ? Say::tr("2 arcs") : Say::tr("%1 arcs").arg(n);
    return n == 1 ? Say::tr("a curve") : n == 2 ? Say::tr("2 curves") : Say::tr("%1 curves").arg(n);
  };
  for (const auto& [t, n] : counts) parts << phrase(t, n);
  if (parts.isEmpty()) return {};
  if (parts.size() == 1) return parts[0];
  const QString last = parts.takeLast();
  return Say::tr("%1 and %2").arg(parts.join(Say::tr(", ")), last);
}
const opad::Feature* featureOf(const opad::Scene& s, const json& output, const json& args) {
  if (output.is_object()) {
    if (output.contains("feature_id") && output["feature_id"].is_string())
      if (const auto* f = s.feature(output["feature_id"].get<std::string>())) return f;
    for (const auto& id : output.value("ids", json::array()))
      if (id.is_string())
        if (const auto* f = s.feature(id.get<std::string>())) return f;
  }
  if (args.contains("target") && args["target"].is_string()) return s.feature(args["target"].get<std::string>());
  return nullptr;
}
const opad::SketchItem* sketchOf(const opad::Scene& s, const json& output, const json& args) {
  if (output.is_object() && output.contains("sketch_id") && output["sketch_id"].is_string())
    if (const auto* k = s.sketch(output["sketch_id"].get<std::string>())) return k;
  for (const char* key : {"target", "sketch", "id"})
    if (args.contains(key) && args[key].is_string())
      if (const auto* k = s.sketch(args[key].get<std::string>())) return k;
  return nullptr;
}
std::string lastNewOp(const json& output) {
  if (output.is_object() && output.contains("operation_ids") && output["operation_ids"].is_array() && !output["operation_ids"].empty() &&
      output["operation_ids"].back().is_string())
    return output["operation_ids"].back().get<std::string>();
  return {};
}
// Bodies the call made, changed, moved or removed (the hover tint and Show), from the scenes.
std::vector<std::string> touched(const opad::Scene& before, const opad::Scene& after) {
  std::vector<std::string> out;
  for (const auto& [id, n] : after.nodes) {
    if (n.kind != opad::Node::Kind::Body) continue;
    const auto* p = before.node(id);
    if (!p || p->body_key != n.body_key || p->name != n.name || p->visible != n.visible || p->local.to_json() != n.local.to_json()) out.push_back(id);
    if (out.size() >= 50) break;
  }
  return out;
}
const std::set<std::string>& hidden() {
  static const std::set<std::string> h = {"live_state", "wait_for_idle", "request_status", "live_diagnostics", "live_instances", "live_bind",
                                          "resolve_reference", "feature_kinds", "feature_schema", "commands", "version", "stop",
                                          "transaction_begin", "transaction_cancel", "preview_cancel", "transaction_commit", "preview_commit",
                                          "undo", "redo", "open_document", "new_document"};
  return h;
}
const std::set<std::string>& folded() {
  static const std::set<std::string> f = {"tree", "inspect", "properties", "features", "params", "ops", "info", "entity_details",
                                          "query_entities", "sketch_details", "related", "mechanism", "git_status", "git_log",
                                          "git_branches", "git_diff", "annotations", "fit_reference", "scan_axes"};
  return f;
}
}  // namespace

QString isolate(const QString& text) { return QChar(0x2068) + text + QChar(0x2069); }
QString plain(QString text) {
  for (QChar c : {QChar(0x2066), QChar(0x2067), QChar(0x2068), QChar(0x2069), QChar(0x200E), QChar(0x200F)}) text.remove(c);
  return text;
}
QString quote(const QString& text, int n) { return Say::tr("“%1”").arg(isolate(cap(text, n))); }
QString bodies(const opad::Scene& scene, const std::vector<std::string>& ids) {
  const units::Display d;
  return Scenes{scene, scene, d}.bodies(ids);
}
QString lookups(int n) { return n == 1 ? Say::tr("1 lookup") : n == 2 ? Say::tr("2 lookups") : Say::tr("%1 lookups").arg(n); }
QString changesCount(int n) { return n == 1 ? Say::tr("1 change") : n == 2 ? Say::tr("2 changes") : Say::tr("%1 changes").arg(n); }
QString stepsSoFar(int n) { return n == 1 ? Say::tr("1 step so far") : n == 2 ? Say::tr("2 steps so far") : Say::tr("%1 steps so far").arg(n); }

Kind kindOf(const std::string& tool, const json& args) {
  if (hidden().count(tool)) return Kind::Hidden;
  if (tool == "context") return args.value("section", std::string()) == "ai_agent_notes" ? Kind::Note : Kind::Fold;
  if (tool == "annotations") return args.value("style", "") == "ai_agent" ? Kind::Note : Kind::Fold;
  if (folded().count(tool)) return Kind::Fold;
  if (tool == "explode" && !opad::agent::live_mutation(tool, args)) return Kind::Fold;
  if (tool == "joint_set" && args.value("preview", false)) return Kind::Fold;
  if (tool == "viewport_image") return Kind::Look;
  if (tool == "annotate" || tool == "delete_annotation") return Kind::Note;
  if (tool == "selection" || tool == "live_select" || tool == "measure" || tool == "clearance" || tool == "validate" || tool == "export") return Kind::Result;
  if (tool == "save") return Kind::Quiet;
  if (tool == "model_batch") return Kind::Group;
  return Kind::Change;
}

QString running(const std::string& tool, const json& args, const opad::Scene& scene, const units::Display& d) {
  const Scenes sc{scene, scene, d};
  switch (kindOf(tool, args)) {
    case Kind::Hidden:
    case Kind::Fold: return {};
    default: break;
  }
  if (tool == "viewport_image") return Say::tr("Taking a look…");
  if (tool == "context" || tool == "annotations") return Say::tr("Reading your notes…");
  if (tool == "live_select") return Say::tr("Selecting…");
  if (tool == "measure") return Say::tr("Measuring…");
  if (tool == "clearance") return Say::tr("Checking the gap…");
  if (tool == "validate") return Say::tr("Checking the model…");
  if (tool == "export") return Say::tr("Exporting…");
  if (tool == "save") return Say::tr("Saving…");
  if (tool == "model_batch") return Say::tr("Making changes…");
  if (tool == "regenerate") return Say::tr("Rebuilding…");
  if (tool == "import" || tool == "import_brep") {
    const QString file = fileName(args.value("file", json()));
    return file.isEmpty() ? Say::tr("Importing…") : Say::tr("Importing %1…").arg(file);
  }
  if (tool == "mesh_to_solid") return Say::tr("Converting a mesh to a solid…");
  if (tool == "study") return Say::tr("Running a study…");
  if (tool == "sketch") return Say::tr("Drawing a sketch on %1…").arg(planeText(sc, args.value("plane", json())));
  if (tool == "sketch_edit" || tool == "sketch_tool" || tool == "align_sketch") {
    for (const char* key : {"target", "sketch", "id"})
      if (args.contains(key) && args[key].is_string())
        if (const QString n = nameIn(scene, args[key].get<std::string>()); !n.isEmpty()) return Say::tr("Editing %1…").arg(n);
    return Say::tr("Editing a sketch…");
  }
  if (tool == "feature_edit") {
    if (args.contains("target") && args["target"].is_string())
      if (const QString n = nameIn(scene, args["target"].get<std::string>()); !n.isEmpty()) return Say::tr("Editing %1…").arg(n);
    return Say::tr("Editing a step…");
  }
  if (tool == "param") return Say::tr("Setting %1…").arg(isolate(cap(S(args.value("name", std::string("a parameter"))), 40)));
  if (tool == "feature") {
    const auto kind = args.value("kind", std::string());
    const auto in = args.value("inputs", json::object());
    QString target;
    for (const char* key : {"edges", "faces", "bodies", "targets", "target"}) {
      const auto refs = refsOf(sc, input(in, kind, key));
      if (!refs.empty()) {
        target = sc.bodies(ownersOf(refs));
        break;
      }
    }
    if (kind == "extrude") {
      const QString p = sketchesIn(sc, input(in, kind, "profiles"));
      return p.isEmpty() ? Say::tr("Extruding…") : Say::tr("Extruding %1…").arg(p);
    }
    if (kind == "fillet" && !target.isEmpty()) return Say::tr("Filleting edges of %1…").arg(target);
    if (kind == "chamfer" && !target.isEmpty()) return Say::tr("Chamfering edges of %1…").arg(target);
    if (kind == "hole" && !target.isEmpty()) return Say::tr("Adding holes to %1…").arg(target);
    if (kind == "shell" && !target.isEmpty()) return Say::tr("Shelling %1…").arg(target);
    return target.isEmpty() ? Say::tr("Adding %1…").arg(label(kind)) : Say::tr("Adding %1 to %2…").arg(label(kind), target);
  }
  if (tool.rfind("git_", 0) == 0) return Say::tr("Working with version control…");
  return Say::tr("Working…");
}

Sentence feature(const json& args, const opad::Feature& made, const opad::Scene& before, const opad::Scene& after, const units::Display& d) {
  const Scenes sc{before, after, d};
  const auto in = made.inputs.is_object() ? made.inputs : args.value("inputs", json::object());
  Sentence s;
  s.kind = Kind::Change;
  const auto bodies = madeBodies(made);
  s.text = featureText(sc, made.kind, in, bodies);
  std::vector<std::string> affected = bodies;
  for (const char* key : {"targets", "target", "bodies", "edges", "faces"})
    for (const auto& id : ownersOf(refsOf(sc, input(in, made.kind, key))))
      if (std::find(affected.begin(), affected.end(), id) == affected.end() && after.node(id)) affected.push_back(id);
  s.second = second(sc, isolate(cap(S(made.name), 48)), affected);
  s.icon = iconFor(made.kind);
  s.subjects = affected;
  s.op = made.id;
  return s;
}

Sentence done(const std::string& tool, const json& args, const json& output, const opad::Scene& before, const opad::Scene& after, const units::Display& d) {
  const Scenes sc{before, after, d};
  Sentence s;
  s.kind = kindOf(tool, args);
  s.icon = QStringLiteral("aiMark");
  s.op = lastNewOp(output);
  const auto refsArg = [&](const char* key) { return refsOf(sc, args.value(key, json())); };
  const auto nameArg = [&](const char* key) -> QString {
    if (args.contains(key) && args[key].is_string()) return sc.name(args[key].get<std::string>());
    return {};
  };
  // Reads (the fold's lines).
  if (s.kind == Kind::Fold) {
    s.icon = QStringLiteral("eye");
    if (tool == "context") {
      const std::string first = args.value("section", std::string());
      if (first == "nodes") s.text = Say::tr("Read the parts list");
      else if (first == "sketches") s.text = Say::tr("Read the sketches");
      else if (first == "features") s.text = Say::tr("Read the design history");
      else if (first == "parameters") s.text = Say::tr("Read the parameters");
      else if (first == "errors") s.text = Say::tr("Checked for problems");
      else s.text = Say::tr("Read the model overview");
    } else if (tool == "tree") s.text = Say::tr("Read the parts list");
    else if (tool == "inspect") {
      const QString who = nameArg("id");
      s.text = who.isEmpty() ? Say::tr("Looked inside the model") : Say::tr("Looked inside %1").arg(who);
    } else if (tool == "properties") {
      const QString who = things(sc, refsArg("refs")).isEmpty() ? nameArg("id") : things(sc, refsArg("refs"));
      s.text = who.isEmpty() ? Say::tr("Read some properties") : Say::tr("Read the properties of %1").arg(who);
    } else if (tool == "features") s.text = Say::tr("Read the design history");
    else if (tool == "params") s.text = Say::tr("Read the parameters");
    else if (tool == "ops") s.text = Say::tr("Read the change log");
    else if (tool == "info") s.text = Say::tr("Read the document summary");
    else if (tool == "entity_details") {
      const QString what = things(sc, refsArg("ref").empty() ? refsArg("refs") : refsArg("ref"));
      s.text = what.isEmpty() ? Say::tr("Looked closely at the model") : Say::tr("Looked closely at %1").arg(what);
    } else if (tool == "query_entities") {
      const auto total = numberIn(output, {"total", "count"});
      s.text = total ? Say::tr("Searched the model (found %1)").arg(int(total->second)) : Say::tr("Searched the model");
    } else if (tool == "sketch_details") {
      const auto* k = sketchOf(after, output, args);
      s.text = k ? Say::tr("Looked at %1").arg(isolate(S(k->name))) : Say::tr("Looked at a sketch");
    } else if (tool == "related") s.text = Say::tr("Found which step made a part of the model");
    else if (tool == "fit_reference") {
      const QString who = sc.name(args.value("ref", json::object()).value("fit", json::object()).value("body", std::string()));
      s.text = who.isEmpty() ? Say::tr("Fitted a plane or an axis") : Say::tr("Fitted a plane or an axis on %1").arg(who);
    } else if (tool == "scan_axes") {
      const QString who = nameArg("body");
      s.text = who.isEmpty() ? Say::tr("Worked out how a scan could stand on the axes") : Say::tr("Worked out how %1 could stand on the axes").arg(who);
    }
    else if (tool == "mechanism") s.text = Say::tr("Looked at how the parts move");
    else if (tool == "explode") s.text = Say::tr("Worked out an exploded view");
    else if (tool == "joint_set") s.text = Say::tr("Tried a joint position");
    else if (tool == "git_status") s.text = Say::tr("Checked the version control status");
    else if (tool == "git_log") s.text = Say::tr("Read the commit history");
    else if (tool == "git_branches") s.text = Say::tr("Read the branches");
    else if (tool == "git_diff") s.text = Say::tr("Compared two versions");
    else if (tool == "annotations") s.text = Say::tr("Read the notes");
    else s.text = Say::tr("Read the model");
    return s;
  }
  if (tool == "viewport_image") {
    s.icon = QStringLiteral("image");
    if (!args.value("preview_id", std::string()).empty() || !args.value("transaction", std::string()).empty()) s.text = Say::tr("Looked at the preview");
    else if (args.contains("views") && args["views"].is_array() && args["views"].size() > 1) s.text = Say::tr("Looked at it from %1 sides").arg(args["views"].size());
    else if (args.contains("select") && args["select"].is_array() && !args["select"].empty()) {
      s.subjects = idsIn(args["select"]);
      s.text = Say::tr("Took a close look at %1").arg(sc.bodies(s.subjects));
    } else s.text = viewName(args.value("view", std::string()));
    return s;
  }
  if (tool == "context" || tool == "annotations") {
    s.icon = QStringLiteral("annotate");
    int open = 0;
    QString one;
    for (const auto& a : after.annotations)
      if (a.style == "ai_agent" && a.unresolved) {
        ++open;
        one = S(a.text);
      }
    if (open == 1) s.text = Say::tr("Read your note %1").arg(quote(one));
    else if (open == 0) s.text = Say::tr("Read your notes for the AI (none open)");
    else s.text = Say::tr("Read your notes for the AI (%1 open)").arg(open);
    return s;
  }
  if (tool == "selection") {
    const auto refs = refsOf(sc, output);
    if (refs.empty()) {
      s.kind = Kind::Fold;
      s.icon = QStringLiteral("eye");
      s.text = Say::tr("Looked at what you selected (nothing)");
      return s;
    }
    s.icon = QStringLiteral("cursor");
    s.text = Say::tr("Looked at what you selected: %1").arg(things(sc, refs));
    s.subjects = ownersOf(refs);
    return s;
  }
  if (tool == "live_select") {
    const auto refs = refsArg("refs");
    s.icon = QStringLiteral("cursor");
    s.text = Say::tr("Selected %1 for you").arg(things(sc, refs));
    s.subjects = ownersOf(refs);
    return s;
  }
  if (tool == "measure") {
    s.icon = QStringLiteral("distance");
    const auto refs = refsArg("refs");
    s.subjects = ownersOf(refs);
    const auto value = numberIn(output, {"distance", "distance_mm", "min_distance_mm", "angle", "angle_deg", "radius", "length", "value"});
    QString shown;
    if (value) shown = value->first.find("angle") != std::string::npos ? units::compact(units::Kind::Angle, value->second, d) : units::compact(units::Kind::Length, value->second, d);
    const QString what = things(sc, refs);
    if (args.value("pin", false)) {
      s.kind = Kind::Change;
      s.text = shown.isEmpty() ? Say::tr("Pinned a measurement on %1").arg(what) : Say::tr("Pinned a measurement on %1: %2").arg(what, shown);
    } else if (what.isEmpty()) s.text = shown.isEmpty() ? Say::tr("Measured the model") : Say::tr("Measured the model: %1").arg(shown);
    else s.text = shown.isEmpty() ? Say::tr("Measured %1").arg(what) : Say::tr("Measured %1: %2").arg(what, shown);
    return s;
  }
  if (tool == "clearance") {
    s.icon = QStringLiteral("interference");
    const auto a = ownersOf(refsArg("a")), b = ownersOf(refsArg("b"));
    s.subjects = a;
    s.subjects.insert(s.subjects.end(), b.begin(), b.end());
    const std::string status = output.value("status", std::string());
    const auto gap = numberIn(output, {"min_distance_mm"});
    const QString between = a.empty() || b.empty() ? Say::tr("the parts") : Say::tr("%1 and %2").arg(sc.bodies(a), sc.bodies(b));
    if (status == "intersecting") {
      s.issue = true;
      s.text = Say::tr("Checked the gap between %1: they overlap").arg(between);
    } else if (gap) s.text = Say::tr("Checked the gap between %1: %2").arg(between, units::compact(units::Kind::Length, gap->second, d));
    else s.text = Say::tr("Checked the gap between %1").arg(between);
    return s;
  }
  if (tool == "validate") {
    s.icon = QStringLiteral("check");
    int pairs = 0;
    if (output.contains("interference")) {
      const auto& i = output["interference"];
      if (i.contains("total") && i["total"].is_number()) pairs = i["total"].get<int>();
      else if (i.contains("items") && i["items"].is_array()) pairs = int(i["items"].size());
    }
    const bool valid = output.value("valid_page", true);
    if (pairs > 0) {
      s.issue = true;
      s.icon = QStringLiteral("issue");
      s.text = pairs == 1 ? Say::tr("Checked for collisions: 1 overlap") : Say::tr("Checked for collisions: %1 overlaps").arg(pairs);
    } else if (!valid) {
      s.issue = true;
      s.icon = QStringLiteral("issue");
      s.text = Say::tr("Checked the model: some parts aren't valid solids");
    } else {
      s.kind = Kind::Fold;
      s.icon = QStringLiteral("eye");
      s.text = Say::tr("Checked the model is valid");
    }
    return s;
  }
  if (tool == "export") {
    s.icon = QStringLiteral("export");
    const QString file = fileName(args.value("out", args.value("file", args.value("path", json()))));
    s.text = file.isEmpty() ? Say::tr("Exported the model") : Say::tr("Exported the model as %1").arg(file);
    return s;
  }
  if (tool == "save") {
    s.icon = QStringLiteral("save");
    const QString file = fileName(output.value("path", args.value("path", json())));
    s.text = file.isEmpty() ? Say::tr("Saved the document") : Say::tr("Saved %1").arg(file);
    return s;
  }
  if (tool == "annotate") {
    s.icon = QStringLiteral("annotate");
    const auto refs = refsOf(sc, args.value("anchor", json()));
    s.subjects = ownersOf(refs);
    const QString on = things(sc, refs);
    const QString text = S(args.value("text", std::string()));
    if (!args.value("reply_to", std::string()).empty()) s.text = Say::tr("Replied to your note: %1").arg(quote(text));
    else if (args.contains("drawing") && text.isEmpty()) s.text = on.isEmpty() ? Say::tr("Drew a sketch note") : Say::tr("Drew a sketch note on %1").arg(on);
    else s.text = on.isEmpty() ? Say::tr("Left a note: %1").arg(quote(text)) : Say::tr("Left a note on %1: %2").arg(on, quote(text));
    return s;
  }
  if (tool == "delete_annotation") {
    const std::string id = args.value("id", args.value("target", std::string()));
    for (const auto& a : before.annotations)
      if (a.id == id) {
        if (a.style == "ai_agent") {
          s.icon = QStringLiteral("check");
          s.text = Say::tr("Done: your note %1").arg(quote(S(a.text)));
        } else {
          s.icon = QStringLiteral("annotate");
          s.text = Say::tr("Removed the note %1").arg(quote(S(a.text)));
        }
        return s;
      }
    s.icon = QStringLiteral("annotate");
    s.text = Say::tr("Removed a note");
    return s;
  }
  // Changes.
  s.subjects = touched(before, after);
  if (tool == "feature") {
    if (const auto* f = featureOf(after, output, args)) {
      auto said = feature(args, *f, before, after, d);
      for (const auto& id : s.subjects)
        if (std::find(said.subjects.begin(), said.subjects.end(), id) == said.subjects.end()) said.subjects.push_back(id);
      if (said.op.empty()) said.op = s.op;
      return said;
    }
    s.icon = iconFor(args.value("kind", std::string()));
    s.text = Say::tr("Added %1").arg(label(args.value("kind", std::string())));
    return s;
  }
  if (tool == "feature_edit") {
    const auto* f = featureOf(after, output, args);
    const auto* was = f ? before.feature(f->id) : nullptr;
    if (!f) {
      s.text = Say::tr("Changed a step");
      return s;
    }
    s.icon = iconFor(f->kind);
    s.op = f->id;
    const QString name = isolate(cap(S(f->name), 48));
    if (was && was->name != f->name) s.text = Say::tr("Renamed %1 to %2").arg(isolate(cap(S(was->name), 48)), name);
    else if (was && was->suppressed != f->suppressed) s.text = f->suppressed ? Say::tr("Suppressed %1").arg(name) : Say::tr("Unsuppressed %1").arg(name);
    else {
      QString changed;
      if (was && was->inputs.is_object() && f->inputs.is_object())
        if (const auto* spec = specOf(f->kind))
          for (const auto& in : spec->inputs) {
            const auto a = input(was->inputs, f->kind, in.name), b = input(f->inputs, f->kind, in.name);
            if (a == b) continue;
            const auto kind = in.type == "angle" ? units::Kind::Angle : units::Kind::Length;
            if (in.type == "length" || in.type == "angle") changed = Say::tr("Changed %1: %2 %3 → %4").arg(name, i18n::t(S(in.label)).toLower(), amount(a, kind, d), amount(b, kind, d));
            else changed = Say::tr("Changed %1: %2").arg(name, i18n::t(S(in.label)).toLower());
            break;
          }
      s.text = changed.isEmpty() ? Say::tr("Changed %1").arg(name) : changed;
    }
    s.second = second(sc, {}, s.subjects);
    return s;
  }
  if (tool == "sketch") {
    s.icon = QStringLiteral("sketch");
    if (const auto* k = sketchOf(after, output, args)) {
      const QString shapes = sketchShapes(k->geometry);
      const QString on = planeText(sc, k->plane);
      s.text = shapes.isEmpty() ? Say::tr("Drew %1 on %2").arg(isolate(S(k->name)), on) : Say::tr("Drew %1 on %2: %3").arg(isolate(S(k->name)), on, shapes);
      s.op = k->id;
    } else s.text = Say::tr("Drew a sketch");
    return s;
  }
  if (tool == "sketch_edit" || tool == "align_sketch" || tool == "sketch_tool" || tool == "drawing_to_sketch") {
    s.icon = QStringLiteral("sketch");
    const auto* k = sketchOf(after, output, args);
    const QString name = k ? isolate(S(k->name)) : Say::tr("a sketch");
    if (tool == "align_sketch") s.text = Say::tr("Aligned %1").arg(name);
    else if (tool == "drawing_to_sketch") s.text = Say::tr("Turned the drawing into %1").arg(name);
    else if (tool == "sketch_tool") {
      const auto t = args.value("tool", std::string());
      if (t == "offset") s.text = Say::tr("Offset curves in %1").arg(name);
      else if (t == "mirror") s.text = Say::tr("Mirrored curves in %1").arg(name);
      else if (t == "trim") s.text = Say::tr("Trimmed curves in %1").arg(name);
      else if (t == "extend") s.text = Say::tr("Extended curves in %1").arg(name);
      else if (t == "split") s.text = Say::tr("Split curves in %1").arg(name);
      else if (t == "delete") s.text = Say::tr("Deleted curves from %1").arg(name);
      else if (t == "project") s.text = Say::tr("Projected edges into %1").arg(name);
      else if (t == "fillet") s.text = Say::tr("Rounded corners in %1").arg(name);
      else s.text = Say::tr("Edited %1").arg(name);
    } else s.text = Say::tr("Edited %1").arg(name);
    if (k) s.op = k->id;
    return s;
  }
  if (tool == "param" || tool == "param_delete") {
    s.icon = QStringLiteral("fx");
    const std::string name = args.value("name", args.value("id", std::string()));
    const opad::Param* was = nullptr;
    const opad::Param* now = nullptr;
    for (const auto& p : before.params)
      if (p.name == name || p.id == name) was = &p;
    for (const auto& p : after.params)
      if (p.name == name || p.id == name) now = &p;
    const QString n = isolate(cap(S(name), 40));
    if (tool == "param_delete") s.text = Say::tr("Deleted the parameter %1").arg(n);
    else if (!was && now) s.text = Say::tr("Added the parameter %1 = %2").arg(n, isolate(S(now->shown.empty() ? now->expr : now->shown)));
    else if (was && now) s.text = Say::tr("Changed %1 from %2 to %3").arg(n, isolate(S(was->shown.empty() ? was->expr : was->shown)), isolate(S(now->shown.empty() ? now->expr : now->shown)));
    else s.text = Say::tr("Set the parameter %1").arg(n);
    return s;
  }
  if (tool == "regenerate") {
    s.icon = QStringLiteral("regen");
    s.text = Say::tr("Brought the design up to date");
    return s;
  }
  if (tool == "component") {
    s.icon = QStringLiteral("component");
    const std::string id = output.value("id", std::string());
    s.text = Say::tr("Added the component %1").arg(id.empty() ? isolate(S(args.value("name", std::string()))) : sc.name(id));
    return s;
  }
  if (tool == "rename") {
    s.icon = QStringLiteral("rename");
    std::vector<std::string> ids = idsIn(args.value("targets", args.value("target", json())));
    if (ids.size() == 1) s.text = Say::tr("Renamed %1 to %2").arg(nameIn(before, ids[0]).isEmpty() ? Say::tr("a part") : nameIn(before, ids[0]), sc.name(ids[0]));
    else s.text = Say::tr("Renamed %1 parts").arg(ids.size());
    s.subjects = ids;
    return s;
  }
  if (tool == "appearance") {
    s.icon = QStringLiteral("eye");
    std::vector<std::string> ids = idsIn(args.value("targets", args.value("target", args.value("ids", json()))));
    s.subjects = ids;
    const QString who = sc.bodies(ids).isEmpty() ? Say::tr("the parts") : sc.bodies(ids);
    if (args.contains("visible") && args["visible"].is_boolean()) {
      s.icon = args["visible"].get<bool>() ? QStringLiteral("eye") : QStringLiteral("hide");
      s.text = args["visible"].get<bool>() ? Say::tr("Showed %1").arg(who) : Say::tr("Hid %1").arg(who);
    } else if (args.contains("color") && !args["color"].is_null()) s.text = Say::tr("Coloured %1").arg(who);
    else if (args.contains("opacity") && args["opacity"].is_number())
      s.text = Say::tr("Made %1 %2% see-through").arg(who).arg(int(std::lround((1 - args["opacity"].get<double>()) * 100)));
    else if (args.contains("locked") && args["locked"].is_boolean()) {
      s.icon = QStringLiteral("lock");
      s.text = args["locked"].get<bool>() ? Say::tr("Locked %1").arg(who) : Say::tr("Unlocked %1").arg(who);
    } else s.text = Say::tr("Changed how %1 looks").arg(who);
    return s;
  }
  if (tool == "transform") {
    s.icon = QStringLiteral("move");
    std::vector<std::string> ids = idsIn(args.value("targets", args.value("target", args.value("ids", json()))));
    if (!ids.empty()) s.subjects = ids;
    s.text = Say::tr("Moved %1").arg(sc.bodies(s.subjects).isEmpty() ? Say::tr("the parts") : sc.bodies(s.subjects));
    return s;
  }
  if (tool == "reparent") {
    s.icon = QStringLiteral("reparent");
    std::vector<std::string> ids = idsIn(args.value("targets", args.value("target", args.value("ids", json()))));
    if (!ids.empty()) s.subjects = ids;
    const std::string parent = args.value("parent", std::string());
    const QString who = sc.bodies(s.subjects).isEmpty() ? Say::tr("the parts") : sc.bodies(s.subjects);
    s.text = parent.empty() ? Say::tr("Moved %1 to the top level").arg(who) : Say::tr("Moved %1 into %2").arg(who, sc.name(parent));
    return s;
  }
  if (tool == "delete") {
    s.icon = QStringLiteral("delete");
    const std::string target = args.value("target", args.value("id", std::string()));
    if (const auto* f = before.feature(target)) s.text = Say::tr("Removed the step %1").arg(isolate(S(f->name)));
    else if (const auto* k = before.sketch(target)) s.text = Say::tr("Removed the sketch %1").arg(isolate(S(k->name)));
    else if (const auto* n = before.node(target)) s.text = Say::tr("Removed %1").arg(isolate(S(n->name)));
    else s.text = Say::tr("Removed a step");
    return s;
  }
  if (tool == "import" || tool == "import_brep") {
    s.icon = QStringLiteral("import");
    const QString file = fileName(args.value("file", json()));
    if (tool == "import_brep") s.text = Say::tr("Added a shape made in code");
    else s.text = file.isEmpty() ? Say::tr("Imported a file") : Say::tr("Imported %1").arg(file);
    return s;
  }
  if (tool == "mesh_to_solid") {
    s.icon = QStringLiteral("mesh");
    s.text = Say::tr("Converted a mesh to a solid");
    return s;
  }
  if (tool == "section") {
    s.icon = QStringLiteral("section");
    s.text = Say::tr("Added a section plane");
    return s;
  }
  if (tool == "view") {
    s.icon = QStringLiteral("home");
    const QString name = isolate(cap(S(args.value("name", std::string())), 40));
    s.text = args.value("name", std::string()).empty() ? Say::tr("Saved a view") : Say::tr("Saved the view %1").arg(name);
    return s;
  }
  if (tool == "explode") {
    s.icon = QStringLiteral("explode");
    s.text = Say::tr("Saved an exploded view");
    return s;
  }
  if (tool == "joint" || tool == "joint_set") {
    s.icon = QStringLiteral("axis");
    s.text = tool == "joint" ? Say::tr("Added a joint") : Say::tr("Set a joint's position");
    return s;
  }
  if (tool == "study" || tool == "load") {
    s.icon = QStringLiteral("fx");
    s.text = tool == "study" ? Say::tr("Ran a study") : Say::tr("Added a load");
    return s;
  }
  if (tool == "model_batch") {
    s.icon = QStringLiteral("commit");
    const int n = int(output.value("steps", json::array()).size());
    s.text = n == 1 ? Say::tr("Made 1 change") : Say::tr("Made %1 changes").arg(n);
    return s;
  }
  if (tool.rfind("git_", 0) == 0) {
    s.icon = QStringLiteral("git");
    const QString branch = isolate(cap(S(args.value("branch", args.value("name", std::string()))), 40));
    const QString message = S(args.value("message", std::string()));
    if (tool == "git_commit") s.text = message.isEmpty() ? Say::tr("Committed the document") : Say::tr("Committed %1").arg(quote(message));
    else if (tool == "git_init") s.text = Say::tr("Set up version control for the document");
    else if (tool == "git_branch_create") s.text = Say::tr("Made the branch %1").arg(branch);
    else if (tool == "git_switch") s.text = Say::tr("Switched to %1").arg(branch);
    else if (tool == "git_tag") s.text = Say::tr("Marked this version as %1").arg(branch);
    else if (tool == "git_push") s.text = Say::tr("Uploaded the commits");
    else if (tool == "git_fetch") s.text = Say::tr("Checked for new commits");
    else if (tool == "git_pull") s.text = Say::tr("Brought in the latest commits");
    else if (tool == "git_merge") s.text = branch.size() > 2 ? Say::tr("Merged %1").arg(branch) : Say::tr("Merged a branch");
    else if (tool == "git_merge_abort") s.text = Say::tr("Called off the merge");
    else if (tool == "git_resolve") s.text = Say::tr("Settled the merge conflicts");
    else s.text = Say::tr("Worked with version control");
    return s;
  }
  // A plugin's command or one this table does not know yet: its name in words, never snake_case.
  QString words = S(tool);
  words.replace(QLatin1Char('_'), QLatin1Char(' '));
  s.text = Say::tr("Ran %1").arg(isolate(words));
  return s;
}

bool retryable(const std::string& code) {
  static const std::set<std::string> codes = {"busy", "edit_session_busy", "stale_revision", "cancelled", "invalid_arguments", "request_id_reused",
                                              "prepared_active", "transaction_active", "not_bound", "target_changed", "wrong_target",
                                              "selection_busy_or_stale", "response_cancelled", "snapshot_failed", "nothing_to_undo", "nothing_to_redo",
                                              "unchecked_reference", "stale_reference"};
  return codes.count(code) || code.rfind("unknown_", 0) == 0;
}

QString failure(const std::string& code, const QString& message, const QString& doing) {
  if (retryable(code)) return {};
  QString what = doing;
  if (what.endsWith(QChar(0x2026))) what.chop(1);
  QString why;
  const QString m = message.toLower();
  if (m.contains(QLatin1String("solid validation")) || m.contains(QLatin1String("not a valid solid"))) why = Say::tr("the result wasn't a valid solid");
  else if (m.contains(QLatin1String("unresolved"))) why = Say::tr("it would break later steps");
  else if (code == "save_failed") why = Say::tr("the file couldn't be written");
  else if (code == "batch_failed") why = Say::tr("one of the steps failed");
  if (what.isEmpty()) return why.isEmpty() ? Say::tr("A request couldn't be done. Nothing changed.") : Say::tr("Couldn't finish: %1. Nothing changed.").arg(why);
  return why.isEmpty() ? Say::tr("Couldn't finish “%1”. Nothing changed.").arg(what) : Say::tr("Couldn't finish “%1”: %2. Nothing changed.").arg(what, why);
}
QString author(const opad::json& opData) {
  const std::string by = opData.is_object() ? opData.value("by", std::string()) : std::string();
  if (by != "Agent") return QString::fromStdString(by);
  const std::string client = opData.value("ai_client", std::string());
  return Say::tr("%1 (AI)").arg(client.empty() ? Say::tr("AI") : QString::fromStdString(client));
}

QString authorLine(const opad::json& opData, const QDateTime& now) {
  const QString who = author(opData);
  const std::string ts = opData.is_object() ? opData.value("ts", std::string()) : std::string();
  if (!opData.is_object() || opData.value("by", std::string()) != "Agent") return Say::tr("%1 · %2").arg(who, i18n::localTime(ts));
  const QDateTime at = QDateTime::fromString(QString::fromStdString(ts), Qt::ISODate);
  if (!at.isValid()) return who;
  const QDateTime local = at.toLocalTime();
  return Say::tr("%1 · %2").arg(who, local.date() == now.toLocalTime().date() ? local.toString(QStringLiteral("HH:mm")) : i18n::localTime(ts));
}

}  // namespace agentsay
