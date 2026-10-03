#include "HelpClip.hpp"

#include "CommandHelp.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSettings>
#include <QTransform>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using V3 = QVector3D;
constexpr double kPi = 3.14159265358979323846, kCaption = 22, kRipple = 0.45;

enum class Ease { InOut, Linear, In, Out, Step };

// One keyframed property of an item: values at times, each segment eased by the key it arrives at.
struct Track {
  QString prop;
  QList<double> t;
  QList<QJsonValue> v;
  QList<Ease> ease;
};
struct Item {
  QString el;
  QJsonObject base;
  QList<Track> tracks;
  double from = -1e9, to = 1e9, fade = 0.12;
  QList<double> clicks;  // cursor: the times of its clicks
};
struct Bounds {
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  void add(const QPointF& p) { x0 = std::min(x0, p.x()); y0 = std::min(y0, p.y()); x1 = std::max(x1, p.x()); y1 = std::max(y1, p.y()); }
  bool valid() const { return x1 >= x0 && y1 >= y0; }
};
struct Clip {
  QString id;
  double duration = 4, still = 4, pad = 0.1;
  bool iso = false;
  double az = -45, el = 35.264;
  QRectF extent;     // view units; null = fitted once to what the clip draws
  bool fitted = false;
  QList<Item> items;
  QList<clips::Step> steps;
  QStringList texts;
};
struct Library {
  QHash<QString, Clip> clips;
  QStringList order, problems;
  bool loaded = false;
};
Library& lib() {
  static Library l;
  return l;
}
void ensureLoaded() {
  if (!lib().loaded) clips::load();
}

// ---- schema ---------------------------------------------------------------------------------------------------------
const QHash<QString, QStringList>& schema() {
  static const QHash<QString, QStringList> s = [] {
    const QStringList stroke{"color", "width", "dash", "fill", "fillOpacity", "progress", "glow", "hatch", "arrow", "dots", "smooth"};
    QHash<QString, QStringList> h{
        {"grid", {"spacing", "major", "area", "z", "color"}},
        {"axes", {"len", "at"}},
        {"poly", stroke + QStringList{"points", "closed"}},
        {"rect", stroke + QStringList{"corners"}},
        {"circle", stroke + QStringList{"center", "r", "plane"}},
        {"arc", stroke + QStringList{"center", "r", "start", "sweep", "through", "plane"}},
        {"slot", stroke + QStringList{"c1", "c2", "r"}},
        {"polygon", stroke + QStringList{"center", "r", "n", "corner"}},
        {"shape", stroke + QStringList{"profile", "holes", "plane", "at"}},
        {"plane", {"center", "size", "plane", "color", "fillOpacity"}},
        {"solid", {"profile", "holes", "plane", "dir", "at", "height", "tone", "toneAmount", "edges", "capColor", "hatch"}},
        {"lathe", {"profile", "at", "start", "sweep", "n", "tone", "toneAmount", "edges"}},
        {"arrow", {"at", "dir", "len", "color"}},
        {"dim", {"kind", "a", "b", "offset", "center", "r", "angle", "start", "sweep", "plane", "value", "color"}},
        {"label", {"at", "screen", "text", "value", "dx", "dy", "color", "size", "bold", "box", "align"}},
        {"chip", {"at", "screen", "text", "value", "dot", "icon", "close", "color", "dx", "dy", "hl"}},
        {"cursor", {"kind", "pos", "screen", "dx", "dy", "badge", "down"}},
        {"hud", {"at", "dx", "dy", "fields", "focus", "typed", "origin", "dec", "unit"}},
        {"key", {"caps", "corner", "press"}},
        {"glyph", {"kind", "at", "dx", "dy", "color"}},
        {"snap", {"kind", "at", "color"}},
        {"card", {"screen", "w", "title", "rows", "hl", "button", "press"}},
        {"picture", {"corners"}},
        {"camera", {"az", "el", "zoom", "center"}},
    };
    for (auto it = h.begin(); it != h.end(); ++it) *it << "el" << "from" << "to" << "fade" << "opacity" << "keys" << "offset" << "rot" << "pivot" << "scale" << "note";
    return h;
  }();
  return s;
}
const QStringList kColorProps{"color", "fill", "glow", "hatch", "tone", "edges", "capColor", "dot", "box"};
const QStringList kTokens{"bg", "bg2", "bg3", "bg4", "line", "fg", "fg2", "fg3", "vp", "sel", "selbg", "hov", "amber", "green", "red",
                          "mtop", "mleft", "mright", "medge", "cap", "onsel", "glow", "blue", "white", "black"};
const QHash<QString, QStringList> kKinds{
    {"cursor", {"arrow", "cross", "move"}},
    {"glyph", {"horizontal", "vertical", "parallel", "perpendicular", "coincident", "tangent", "equal", "concentric", "fix", "midpoint", "symmetric", "collinear", "smooth"}},
    {"snap", {"endpoint", "midpoint", "center", "quadrant", "intersection", "tangent", "nearest", "perpendicular"}},
    {"dim", {"linear", "radial", "diameter", "angular"}},
};

QColor token(const Tokens& t, const QString& name) {
  static const QHash<QString, QColor Tokens::*> fields{
      {"bg", &Tokens::bg}, {"bg2", &Tokens::bg2}, {"bg3", &Tokens::bg3}, {"bg4", &Tokens::bg4}, {"line", &Tokens::line}, {"fg", &Tokens::fg},
      {"fg2", &Tokens::fg2}, {"fg3", &Tokens::fg3}, {"vp", &Tokens::vp}, {"sel", &Tokens::sel}, {"selbg", &Tokens::selbg}, {"hov", &Tokens::hov},
      {"amber", &Tokens::amber}, {"green", &Tokens::green}, {"red", &Tokens::red}, {"mtop", &Tokens::mtop}, {"mleft", &Tokens::mleft},
      {"mright", &Tokens::mright}, {"medge", &Tokens::medge}, {"cap", &Tokens::cap}, {"onsel", &Tokens::onsel}, {"blue", &Tokens::sel}};
  if (const auto it = fields.find(name); it != fields.end()) return t.**it;
  if (name == "glow") return t.dark ? QColor(255, 255, 255, 150) : QColor(t.hov.red(), t.hov.green(), t.hov.blue(), 120);  // hover
  if (name == "white") return Qt::white;
  if (name == "black") return Qt::black;
  return t.fg;
}
QColor mix(const QColor& a, const QColor& b, double f) {
  f = std::clamp(f, 0.0, 1.0);
  return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * f), float(a.greenF() + (b.greenF() - a.greenF()) * f),
                          float(a.blueF() + (b.blueF() - a.blueF()) * f), float(a.alphaF() + (b.alphaF() - a.alphaF()) * f));
}
QColor alpha(QColor c, double a) {
  c.setAlphaF(float(std::clamp(c.alphaF() * a, 0.0, 1.0)));
  return c;
}

// ---- templates and numbers ------------------------------------------------------------------------------------------
// "1.2+0.3" left after a parameter was put in: + - * / and brackets over numbers.
struct Expr {
  const QString& s;
  int i = 0;
  bool ok = true;
  void ws() { while (i < s.size() && s[i].isSpace()) ++i; }
  double factor() {
    ws();
    if (i < s.size() && s[i] == '(') {
      ++i;
      const double v = sum();
      ws();
      if (i < s.size() && s[i] == ')') ++i; else ok = false;
      return v;
    }
    if (i < s.size() && s[i] == '-') { ++i; return -factor(); }
    int j = i;
    while (j < s.size() && (s[j].isDigit() || s[j] == '.')) ++j;
    if (j == i) { ok = false; return 0; }
    const double v = s.mid(i, j - i).toDouble(&ok);
    i = j;
    return v;
  }
  double product() {
    double v = factor();
    for (ws(); i < s.size() && (s[i] == '*' || s[i] == '/'); ws()) v = s[i++] == '*' ? v * factor() : v / factor();
    return v;
  }
  double sum() {
    double v = product();
    for (ws(); i < s.size() && (s[i] == '+' || s[i] == '-'); ws()) v = s[i++] == '+' ? v + product() : v - product();
    return v;
  }
};
QJsonValue arithmetic(const QString& s) {
  static const QRegularExpression shape(R"(^[-+*/().0-9\s]*\d[-+*/().0-9\s]*$)");
  if (!shape.match(s).hasMatch()) return s;
  Expr e{s};
  const double v = e.sum();
  return e.ok && e.i == s.size() ? QJsonValue(v) : QJsonValue(s);
}

// "$name" alone takes the argument's value (a number, a point, an object); inside a longer string its text.
QJsonValue substitute(const QJsonValue& v, const QJsonObject& args) {
  if (v.isString()) {
    const QString s = v.toString();
    if (!s.contains('$')) return v;
    if (s.startsWith('$') && args.contains(s.mid(1))) return args.value(s.mid(1));
    static const QRegularExpression name(R"(\$([A-Za-z_][A-Za-z0-9_]*))");
    QString out;
    qsizetype last = 0;
    for (const auto& m : name.globalMatch(s)) {
      out += s.mid(last, m.capturedStart() - last);
      const QJsonValue a = args.value(m.captured(1));
      out += a.isDouble() ? QString::number(a.toDouble(), 'g', 12) : a.isString() ? a.toString() : m.captured(0);
      last = m.capturedEnd();
    }
    return arithmetic(out + s.mid(last));
  }
  if (v.isArray()) {
    QJsonArray a;
    for (const QJsonValue& x : v.toArray()) a.append(substitute(x, args));
    return a;
  }
  if (v.isObject()) {
    QJsonObject o;
    const QJsonObject in = v.toObject();
    for (auto it = in.begin(); it != in.end(); ++it) o.insert(it.key(), substitute(it.value(), args));
    return o;
  }
  return v;
}

using Problem = std::function<void(const QString&)>;
// The template's parameters filled from `given` (defaults for the rest; a null default is required).
bool bindArgs(const QString& name, const QJsonObject& tpl, const QJsonObject& given, QJsonObject& args, const Problem& problem) {
  const QJsonObject params = tpl.value("params").toObject();
  bool ok = true;
  for (auto it = given.begin(); it != given.end(); ++it)
    if (!params.contains(it.key())) { problem(QString("template %1 has no parameter %2").arg(name, it.key())); ok = false; }
  for (auto it = params.begin(); it != params.end(); ++it) {
    if (given.contains(it.key())) args.insert(it.key(), given.value(it.key()));
    else if (!it.value().isNull()) args.insert(it.key(), it.value());
    else { problem(QString("template %1 needs %2").arg(name, it.key())); ok = false; }
  }
  return ok;
}

// Items with {"use": template, "args": {...}} replaced by the template's items, recursively.
QJsonArray expand(const QJsonArray& items, const QJsonObject& templates, const Problem& problem, int depth = 0) {
  QJsonArray out;
  for (const QJsonValue& v : items) {
    const QJsonObject o = v.toObject();
    if (!o.contains("use")) { out.append(o); continue; }
    const QString name = o.value("use").toString();
    const QJsonObject tpl = templates.value(name).toObject();
    QJsonObject args;
    if (tpl.isEmpty()) { problem("unknown template " + name); continue; }
    if (depth > 6 || !bindArgs(name, tpl, o.value("args").toObject(), args, problem)) continue;
    for (const QJsonValue& x : expand(substitute(tpl.value("items"), args).toArray(), templates, problem, depth + 1)) out.append(x);
  }
  return out;
}

// ---- parsing --------------------------------------------------------------------------------------------------------
Ease easeOf(const QJsonValue& v) {
  const QString s = v.toString();
  return s == "linear" ? Ease::Linear : s == "in" ? Ease::In : s == "out" ? Ease::Out : s == "step" ? Ease::Step : Ease::InOut;
}
double eased(Ease e, double f) {
  switch (e) {
    case Ease::Linear: return f;
    case Ease::In: return f * f * f;
    case Ease::Out: return 1 - std::pow(1 - f, 3);
    case Ease::Step: return f >= 1 ? 1 : 0;
    default: return f < 0.5 ? 4 * f * f * f : 1 - std::pow(-2 * f + 2, 3) / 2;
  }
}
// Numbers, and arrays/objects of them, tween; anything else (text, colour names, flags) changes at the next key.
QJsonValue lerp(const QJsonValue& a, const QJsonValue& b, double f) {
  if (f <= 0) return a;
  if (f >= 1) return b;
  if (a.isDouble() && b.isDouble()) return a.toDouble() + (b.toDouble() - a.toDouble()) * f;
  if (a.isArray() && b.isArray() && a.toArray().size() == b.toArray().size()) {
    QJsonArray out;
    const QJsonArray x = a.toArray(), y = b.toArray();
    for (qsizetype i = 0; i < x.size(); ++i) out.append(lerp(x[i], y[i], f));
    return out;
  }
  if (a.isObject() && b.isObject()) {
    QJsonObject out = a.toObject();
    const QJsonObject y = b.toObject();
    for (auto it = y.begin(); it != y.end(); ++it) out.insert(it.key(), out.contains(it.key()) ? lerp(out.value(it.key()), it.value(), f) : it.value());
    return out;
  }
  return a;
}
QJsonValue valueAt(const Track& k, double t) {
  if (t <= k.t.first()) return k.v.first();
  for (qsizetype i = 1; i < k.t.size(); ++i)
    if (t < k.t[i]) return lerp(k.v[i - 1], k.v[i], eased(k.ease[i], (t - k.t[i - 1]) / std::max(1e-9, k.t[i] - k.t[i - 1])));
  return k.v.last();
}
// Before its first key a property keeps the item's own value (when it has one), else the first key's.
QJsonObject evaluate(const Item& it, double t) {
  QJsonObject o = it.base;
  for (const Track& k : it.tracks)
    if (t >= k.t.first() || !o.contains(k.prop)) o.insert(k.prop, valueAt(k, t));
  return o;
}
double visibility(const Item& it, double t) {
  if (t < it.from - 1e-9 || t > it.to + 1e-9) return 0;
  double v = 1;
  if (it.fade > 0 && it.from > -1e8) v = std::min(v, (t - it.from) / it.fade + (it.from <= 1e-9 ? 1 : 0));
  if (it.fade > 0 && it.to < 1e8) v = std::min(v, (it.to - t) / it.fade);
  return std::clamp(v, 0.0, 1.0);
}

// A card value made only of words ("Through all") is translated; one with a number ("6 mm") is shown as written.
bool wordy(const QString& s) {
  static const QRegularExpression digit(R"(\d)"), letters("[A-Za-z]{2}");
  return !digit.match(s).hasMatch() && letters.match(s).hasMatch();
}

void collectTexts(const QString& el, const QJsonObject& o, QStringList& out) {
  auto add = [&](const QJsonValue& v) { if (v.isString() && !v.toString().isEmpty() && !out.contains(v.toString())) out << v.toString(); };
  if (el == "label" || el == "chip") add(o.value("text"));
  if (el == "card") {
    add(o.value("title"));
    add(o.value("button"));
    for (const QJsonValue& r : o.value("rows").toArray()) {
      const QJsonValue text = r.isArray() ? r.toArray().first() : r.toObject().value("text"), value = r.isArray() ? r.toArray().at(1) : r.toObject().value("value");
      add(text);
      if (wordy(value.toString())) add(value);
    }
  }
}

void checkValue(const QString& el, const QString& prop, const QJsonValue& v, const Problem& problem) {
  if (kColorProps.contains(prop) && v.isString() && !kTokens.contains(v.toString())) problem(QString("%1.%2: unknown colour %3").arg(el, prop, v.toString()));
  if (prop == "kind" && kKinds.contains(el) && !kKinds.value(el).contains(v.toString())) problem(QString("%1: unknown kind %2").arg(el, v.toString()));
  if ((prop == "badge" || prop == "icon") && !icons::has(v.toString())) problem(QString("%1: no icon %2").arg(el, v.toString()));
}

void parseItem(const QJsonObject& o, Clip& c, const Problem& problem) {
  Item it;
  it.el = o.value("el").toString();
  if (!schema().contains(it.el)) return problem("unknown element " + it.el);
  const QStringList& allowed = schema().value(it.el);
  auto check = [&](const QJsonObject& props, bool key) {
    for (auto p = props.begin(); p != props.end(); ++p) {
      if (key && (p.key() == "ease" || (it.el == "cursor" && p.key() == "click"))) continue;
      if (!allowed.contains(p.key()) || (key && QStringList{"keys", "el", "from", "to", "fade", "note"}.contains(p.key())))
        problem(QString("%1: unknown property %2").arg(it.el, p.key()));
      else checkValue(it.el, p.key(), p.value(), problem);
    }
  };
  check(o, false);
  it.from = o.value("from").toDouble(-1e9);
  it.to = o.value("to").toDouble(1e9);
  it.fade = o.value("fade").toDouble(0.12);
  it.base = o;
  for (const char* k : {"el", "from", "to", "fade", "keys", "note"}) it.base.remove(k);
  collectTexts(it.el, it.base, c.texts);
  double last = -1e9;
  for (const QJsonValue& kv : o.value("keys").toArray()) {
    const QJsonArray pair = kv.toArray();
    const double t = pair.at(0).toDouble(-1);
    const QJsonObject props = pair.at(1).toObject();
    if (pair.size() != 2 || !pair.at(0).isDouble() || t < last) { problem(it.el + ": keys must be [time, {...}] in time order"); continue; }
    last = t;
    check(props, true);
    collectTexts(it.el, props, c.texts);
    const Ease e = easeOf(props.value("ease"));
    if (it.el == "cursor" && (props.value("click").toBool() || props.value("click").toDouble() != 0)) it.clicks << t;
    for (auto p = props.begin(); p != props.end(); ++p) {
      if (p.key() == "ease" || p.key() == "click") continue;
      auto k = std::find_if(it.tracks.begin(), it.tracks.end(), [&](const Track& x) { return x.prop == p.key(); });
      if (k == it.tracks.end()) k = it.tracks.insert(it.tracks.end(), Track{p.key(), {}, {}, {}});
      k->t << t;
      k->v << p.value();
      k->ease << e;
    }
  }
  c.items << it;
}

QJsonObject readJson(const QString& path, QStringList* problems) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &error);
  if (error.error != QJsonParseError::NoError && problems) *problems << QString("%1: %2 at %3").arg(path, error.errorString()).arg(error.offset);
  return doc.object();
}

void parseClip(QJsonObject raw, const QJsonObject& templates, Library& l) {
  const QString id = raw.value("id").toString();
  if (id.isEmpty()) { l.problems << "a clip without an id"; return; }
  const Problem problem = [&](const QString& what) { l.problems << id + ": " + what; };
  if (raw.contains("template")) {  // a whole clip from a template: its fields are the defaults, the clip's items come after
    const QString name = raw.value("template").toString();
    const QJsonObject tpl = templates.value(name).toObject();
    QJsonObject args;
    if (tpl.isEmpty()) return problem("unknown template " + name);
    if (!bindArgs(name, tpl, raw.value("args").toObject(), args, problem)) return;
    QJsonObject base = substitute(tpl, args).toObject();
    QJsonArray items = base.value("items").toArray();
    for (const QJsonValue& v : raw.value("items").toArray()) items.append(v);
    for (auto it = raw.begin(); it != raw.end(); ++it) if (it.key() != "items" && it.key() != "template" && it.key() != "args") base.insert(it.key(), it.value());
    base.insert("items", items);
    base.remove("params");
    raw = base;
  }
  static const QStringList fields{"id", "duration", "still", "view", "camera", "extent", "pad", "items", "steps", "note"};
  for (auto it = raw.begin(); it != raw.end(); ++it) if (!fields.contains(it.key())) problem("unknown field " + it.key());
  Clip c;
  c.id = id;
  c.duration = raw.value("duration").toDouble(4);
  c.still = raw.value("still").toDouble(c.duration);
  c.pad = raw.value("pad").toDouble(0.1);
  c.iso = raw.value("view").toString() == "iso";
  if (raw.contains("view") && !QStringList{"iso", "plane"}.contains(raw.value("view").toString())) problem("view is iso or plane");
  const QJsonObject cam = raw.value("camera").toObject();
  c.az = cam.value("az").toDouble(c.az);
  c.el = cam.value("el").toDouble(c.el);
  if (const QJsonArray e = raw.value("extent").toArray(); e.size() == 4) c.extent = QRectF(QPointF(e[0].toDouble(), e[1].toDouble()), QPointF(e[2].toDouble(), e[3].toDouble())).normalized();
  for (const QJsonValue& v : expand(raw.value("items").toArray(), templates, problem)) parseItem(v.toObject(), c, problem);
  double from = 0;
  for (const QJsonValue& v : raw.value("steps").toArray()) {
    const QJsonObject s = v.toObject();
    clips::Step step{s.value("from").toDouble(from), s.value("to").toDouble(c.duration), s.value("caption").toString()};
    if (step.caption.isEmpty() || step.to <= step.from) problem("a step needs a caption and to > from");
    if (!c.texts.contains(step.caption)) c.texts.prepend(step.caption);
    c.steps << step;
    from = step.to;
  }
  if (c.steps.isEmpty()) problem("no steps");
  l.clips.insert(id, c);
  if (!l.order.contains(id)) l.order << id;
}

// ---- painting -------------------------------------------------------------------------------------------------------
struct Xf {
  V3 off, pivot, sc{1, 1, 1};
  double rot = 0;
  V3 rotate(const V3& q) const {
    const double a = rot * kPi / 180, c = std::cos(a), s = std::sin(a);
    return V3(float(q.x() * c - q.y() * s), float(q.x() * s + q.y() * c), q.z());
  }
  V3 apply(const V3& q) const { return rotate((q - pivot) * sc) + pivot + off; }
  V3 normal(const V3& n) const { return rotate(V3(n.x() / sc.x(), n.y() / sc.y(), n.z() / sc.z())).normalized(); }
};
V3 vec(const QJsonValue& v, const V3& fallback = V3()) {
  const QJsonArray a = v.toArray();
  if (a.size() < 2) return fallback;
  return V3(float(a[0].toDouble()), float(a[1].toDouble()), float(a.size() > 2 ? a[2].toDouble() : 0));
}
Xf xform(const QJsonObject& o) {
  Xf x;
  x.off = vec(o.value("offset"));
  x.pivot = vec(o.value("pivot"));
  x.rot = o.value("rot").toDouble();
  const QJsonValue s = o.value("scale");
  if (s.isDouble()) x.sc = V3(float(s.toDouble()), float(s.toDouble()), float(s.toDouble()));
  else if (s.isArray()) x.sc = vec(s, V3(1, 1, 1));
  return x;
}
// The axes of a named plane: u, v and the normal u x v ("xz" faces the iso viewer, "yz" its right).
void planeAxes(const QString& name, V3& u, V3& v) {
  if (name == "xz") { u = V3(1, 0, 0); v = V3(0, 0, 1); }
  else if (name == "yz") { u = V3(0, 1, 0); v = V3(0, 0, 1); }
  else { u = V3(1, 0, 0); v = V3(0, 1, 0); }
}

struct Ctx {
  QPainter* p = nullptr;
  const Tokens* tk = nullptr;
  QRectF full, scene;
  double u = 1, t = 0;
  bool rtl = false, still = false, iso = false;
  V3 R{1, 0, 0}, U{0, 1, 0}, V{0, 0, 1};
  QPointF center;
  double s = 1;
  V3 cursor;
  bool hasCursor = false;
  Bounds* record = nullptr;  // measuring: the view-unit bounds of every model point
  QStringList* problems = nullptr;

  void camera(double az, double el) {
    const double a = az * kPi / 180, e = el * kPi / 180;
    V = V3(float(std::cos(e) * std::cos(a)), float(std::cos(e) * std::sin(a)), float(std::sin(e)));
    R = V3(float(-std::sin(a)), float(std::cos(a)), 0);
    U = V3::crossProduct(V, R);
  }
  QPointF view(const V3& q) const { return iso ? QPointF(V3::dotProduct(q, R), V3::dotProduct(q, U)) : QPointF(q.x(), q.y()); }
  QPointF map(const V3& q) const {
    const QPointF v = view(q);
    if (record) record->add(v);
    return {scene.center().x() + (v.x() - center.x()) * s, scene.center().y() - (v.y() - center.y()) * s};
  }
  double depth(const V3& q) const { return V3::dotProduct(q, V); }
  QColor col(const QJsonValue& v, const char* fallback) const { return token(*tk, v.isString() ? v.toString() : QString(fallback)); }
  QFont font(double px, int weight = QFont::Normal, bool mono = false) const {
    QFont f = mono ? theme::mono() : theme::ui(13, weight);
    f.setPixelSize(std::max(5, int(std::lround(px * u))));
    return f;
  }
  // A point: [x, y(, z)] through the item's transform, or "cursor" (where the cursor is now).
  V3 pt(const QJsonValue& v, const Xf& x) const { return v.isString() && v.toString() == "cursor" ? cursor : x.apply(vec(v)); }
  // Screen chrome (cards, chips, key caps) is laid out left to right and mirrored for right-to-left languages.
  QRectF mirror(const QRectF& r) const { return rtl ? QRectF(scene.left() + scene.right() - r.right(), r.top(), r.width(), r.height()) : r; }
  QPointF screen(const QJsonValue& v) const {
    const QJsonArray a = v.toArray();
    const double fx = a.at(0).toDouble(), fy = a.at(1).toDouble();
    return {scene.left() + (rtl ? 1 - fx : fx) * scene.width(), scene.top() + fy * scene.height()};
  }
};

QString literal(const QString& s) { return QChar(0x202A) + s + QChar(0x202C); }  // numbers and units keep their order in RTL
QString shown(const QJsonObject& o) {  // "text" is translated, "value" shown as written
  if (o.contains("text")) return i18n::t(o.value("text").toString());
  const QJsonValue v = o.value("value");
  return v.isDouble() ? literal(QString::number(v.toDouble(), 'f', 0)) : v.toString().isEmpty() ? QString() : literal(v.toString());
}

void arrowHead(QPainter& p, const QPointF& tip, QPointF dir, double len, double half) {
  const double l = std::hypot(dir.x(), dir.y());
  if (l < 1e-9) return;
  dir /= l;
  const QPointF n(-dir.y(), dir.x()), b = tip - dir * len;
  p.drawPolygon(QPolygonF{tip, b + n * half, b - n * half});
}

// Catmull-Rom through the points (spline tools), in screen space.
QPolygonF smooth(const QPolygonF& in, bool closed) {
  if (in.size() < 3) return in;
  QPolygonF out;
  const qsizetype n = in.size(), segs = closed ? n : n - 1;
  auto at = [&](qsizetype i) { return closed ? in[(i % n + n) % n] : in[std::clamp<qsizetype>(i, 0, n - 1)]; };
  for (qsizetype i = 0; i < segs; ++i)
    for (int k = 0; k < 10; ++k) {
      const double t = k / 10.0, t2 = t * t, t3 = t2 * t;
      const QPointF p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
      out << 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
    }
  out << (closed ? in.first() : in.last());
  return out;
}

QPolygonF truncated(const QPolygonF& pts, double fraction) {
  if (fraction >= 1 || pts.size() < 2) return pts;
  double total = 0;
  for (qsizetype i = 1; i < pts.size(); ++i) total += QLineF(pts[i - 1], pts[i]).length();
  double left = total * std::max(0.0, fraction);
  QPolygonF out{pts.first()};
  for (qsizetype i = 1; i < pts.size(); ++i) {
    const double l = QLineF(pts[i - 1], pts[i]).length();
    if (l >= left) { out << pts[i - 1] + (pts[i] - pts[i - 1]) * (l > 0 ? left / l : 0); break; }
    left -= l;
    out << pts[i];
  }
  return out;
}

void hatch(Ctx& c, const QPainterPath& area, const QColor& color) {
  QPainter& p = *c.p;
  p.save();
  p.setClipPath(area, Qt::IntersectClip);
  p.setPen(QPen(color, 1.0 * c.u));
  const QRectF b = area.boundingRect();
  const double step = 5 * c.u;
  for (double x = b.left() - b.height(); x < b.right(); x += step) p.drawLine(QPointF(x, b.bottom()), QPointF(x + b.height(), b.top()));
  p.restore();
}

// Curves: fill, hatch, hover glow, the stroke (dashed, drawn on by progress), arrow heads, point dots.
void stroke(Ctx& c, QPolygonF pts, bool closed, const QJsonObject& o) {
  if (pts.size() < 2 || c.record) return;
  QPainter& p = *c.p;
  const QPolygonF dots = pts;
  if (o.value("smooth").toBool()) pts = smooth(pts, closed);
  else if (closed) pts << pts.first();
  const double prog = o.value("progress").toDouble(1);
  if (o.contains("fill") && prog >= 0.999) {
    p.setPen(Qt::NoPen);
    QColor f = c.col(o.value("fill"), "selbg");
    f.setAlphaF(float(o.value("fillOpacity").toDouble(0.22)));
    p.setBrush(f);
    p.drawPolygon(pts);
  }
  if (o.value("hatch").isString() && prog >= 0.999) {
    QPainterPath area;
    area.addPolygon(pts);
    hatch(c, area, alpha(c.col(o.value("hatch"), "cap"), 0.8));
  }
  const QPolygonF line = truncated(pts, prog);
  const QJsonValue glow = o.value("glow");
  p.setBrush(Qt::NoBrush);
  if (glow.toBool() || glow.isString()) {
    p.setPen(QPen(c.col(glow.isString() ? glow : QJsonValue("glow"), "glow"), 6.5 * c.u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(line);
  }
  const QColor color = c.col(o.value("color"), "fg");
  const double width = o.value("width").toDouble(1.6);
  QPen pen(color, width * c.u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  if (o.value("dash").toBool()) pen.setDashPattern({2.6, 2.2});
  p.setPen(pen);
  if (width > 0) p.drawPolyline(line);
  const QString arrow = o.value("arrow").toString();
  p.setPen(Qt::NoPen);
  p.setBrush(color);
  if ((arrow == "end" || arrow == "both") && line.size() > 1) arrowHead(p, line.last(), line.last() - line[line.size() - 2], 8 * c.u, 3.4 * c.u);
  if ((arrow == "start" || arrow == "both") && line.size() > 1) arrowHead(p, line.first(), line.first() - line[1], 8 * c.u, 3.4 * c.u);
  if (o.value("dots").toBool()) {
    p.setPen(QPen(token(*c.tk, "vp"), 1.0 * c.u));
    for (const QPointF& d : dots) p.drawEllipse(d, 2.6 * c.u, 2.6 * c.u);
  }
}

QVector<V3> circlePoints(const V3& center, double r, double start, double sweep, const QString& plane, int n) {
  V3 u, v;
  planeAxes(plane, u, v);
  QVector<V3> out;
  for (int i = 0; i <= n; ++i) {
    const double a = (start + sweep * i / n) * kPi / 180;
    out << center + u * float(r * std::cos(a)) + v * float(r * std::sin(a));
  }
  return out;
}

// The arc from a through m to b (in the xy plane at a's height).
QVector<V3> arcThrough(const V3& a, const V3& m, const V3& b) {
  const double ax = a.x(), ay = a.y(), bx = m.x(), by = m.y(), cx = b.x(), cy = b.y();
  const double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::abs(d) < 1e-9) return {a, b};
  const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
  const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
  const double r = std::hypot(ax - ux, ay - uy);
  auto ang = [&](double x, double y) { return std::atan2(y - uy, x - ux); };
  const double sa = ang(ax, ay), sm = ang(bx, by), sb = ang(cx, cy);
  auto ccw = [](double from, double to) { double d = to - from; while (d < 0) d += 2 * kPi; return d; };
  double sweep = ccw(sa, sb);
  if (ccw(sa, sm) > sweep) sweep -= 2 * kPi;  // m is not on the counter-clockwise way: go clockwise
  QVector<V3> out;
  const int n = 40;
  for (int i = 0; i <= n; ++i) {
    const double t = sa + sweep * i / n;
    out << V3(float(ux + r * std::cos(t)), float(uy + r * std::sin(t)), a.z());
  }
  return out;
}

// ---- solids ---------------------------------------------------------------------------------------------------------
struct Loop {
  QVector<QPointF> p;
  QVector<bool> sharp;
};
double area(const QVector<QPointF>& p) {
  double a = 0;
  for (qsizetype i = 0; i < p.size(); ++i) a += p[i].x() * p[(i + 1) % p.size()].y() - p[(i + 1) % p.size()].x() * p[i].y();
  return a / 2;
}
QList<double> four(const QJsonValue& v) {
  if (v.isDouble()) return {v.toDouble(), v.toDouble(), v.toDouble(), v.toDouble()};
  QList<double> out;
  for (int i = 0; i < 4; ++i) out << v.toArray().at(i).toDouble();
  return out;
}
// A closed profile, counter-clockwise: {"rect": [cx, cy, w, h], "round": r|[bl, br, tr, tl], "chamfer": ...},
// {"circle": [cx, cy, r]}, {"slot": [x1, y1, x2, y2, r]}, {"points": [[u, v], ...], "smooth": bool}. Point counts never
// depend on the values, so profiles tween (a radius of 0 is a corner).
Loop profile(const QJsonObject& o) {
  Loop l;
  auto add = [&](const QPointF& q, bool sharp) { l.p << q; l.sharp << sharp; };
  if (o.contains("rect")) {
    const QJsonArray r = o.value("rect").toArray();
    const double cx = r.at(0).toDouble(), cy = r.at(1).toDouble(), w = r.at(2).toDouble() / 2, h = r.at(3).toDouble() / 2;
    const QList<QPointF> corner{{cx - w, cy - h}, {cx + w, cy - h}, {cx + w, cy + h}, {cx - w, cy + h}};
    const QList<QPointF> in{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}, out{{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    const bool round = o.contains("round"), chamfer = o.contains("chamfer");
    const QList<double> rad = four(o.value(round ? "round" : "chamfer"));
    for (int i = 0; i < 4; ++i) {
      const double k = std::clamp(rad.value(i), 0.0, std::min(w, h));
      if (round) {
        const QPointF centre = corner[i] - in[i] * k + out[i] * k;
        for (int s = 0; s <= 6; ++s) {
          const double a = (180 + 90 * i + 15 * s) * kPi / 180;
          add(centre + QPointF(std::cos(a), std::sin(a)) * k, s == 0 || s == 6);
        }
      } else if (chamfer) {
        add(corner[i] - in[i] * k, true);
        add(corner[i] + out[i] * k, true);
      } else {
        add(corner[i], true);
      }
    }
  } else if (o.contains("circle")) {
    const QJsonArray r = o.value("circle").toArray();
    const int n = o.value("n").toInt(40);
    for (int i = 0; i < n; ++i) add(QPointF(r.at(0).toDouble(), r.at(1).toDouble()) + QPointF(std::cos(2 * kPi * i / n), std::sin(2 * kPi * i / n)) * r.at(2).toDouble(), false);
  } else if (o.contains("slot")) {
    const QJsonArray r = o.value("slot").toArray();
    const QPointF a(r.at(0).toDouble(), r.at(1).toDouble()), b(r.at(2).toDouble(), r.at(3).toDouble());
    const double rad = r.at(4).toDouble(), dir = std::atan2(b.y() - a.y(), b.x() - a.x());
    for (int end = 0; end < 2; ++end)
      for (int s = 0; s <= 12; ++s) {
        const double ang = dir - kPi / 2 + end * kPi + kPi * s / 12;
        add((end ? a : b) + QPointF(std::cos(ang), std::sin(ang)) * rad, s == 0 || s == 12);
      }
  } else {
    for (const QJsonValue& v : o.value("points").toArray()) add(QPointF(v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble()), !o.value("smooth").toBool());
  }
  if (area(l.p) < 0) {
    std::reverse(l.p.begin(), l.p.end());
    std::reverse(l.sharp.begin(), l.sharp.end());
  }
  return l;
}

struct Face {
  QVector<V3> p;
  QVector<QVector<V3>> holes;  // caps: hole outlines
  V3 n;
  QVector<bool> edge;  // edge i = p[i] -> p[i + 1]
  int pass = 1;        // -1 pocket floors, 0 hole walls, 1 walls, 2 caps
  bool cap = false, front = false;
  double depth = 0;
};

QColor shade(const Ctx& c, const V3& n) {
  const Tokens& t = *c.tk;
  const QColor side = mix(t.mleft, t.mright, 0.5 + 0.75 * V3::dotProduct(n, c.R));
  return n.z() >= 0 ? mix(side, t.mtop, n.z()) : mix(side, t.mright.darker(125), -n.z());
}

// Painter's algorithm per solid: back faces culled, hole walls, then walls far to near, then caps; edges drawn with
// their face (sharp profile corners, silhouettes, cap outlines), so nearer faces cover them.
void drawFaces(Ctx& c, QVector<Face>& faces, const QJsonObject& o) {
  if (c.record) {  // measuring: only where the points land
    for (const Face& f : faces) for (const V3& q : f.p) c.map(q);
    return;
  }
  QPainter& p = *c.p;
  for (Face& f : faces) {
    V3 mid;
    for (const V3& q : f.p) mid += q;
    f.depth = c.depth(mid / float(std::max<qsizetype>(1, f.p.size())));
  }
  std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.pass != b.pass ? a.pass < b.pass : a.depth < b.depth; });
  const QColor edges = c.col(o.value("edges"), "medge"), tone = c.col(o.value("tone"), "sel");
  const double toneAmount = o.contains("tone") ? o.value("toneAmount").toDouble(0.45) : 0;
  const double ew = 1.15 * c.u;
  for (const Face& f : faces) {
    if (!f.front) continue;
    QPolygonF poly;
    for (const V3& q : f.p) poly << c.map(q);
    QColor fill = f.cap && o.value("capColor").isString() ? c.col(o.value("capColor"), "cap") : shade(c, f.n);
    if (f.pass == 0) fill = fill.darker(112);
    fill = mix(fill, tone, toneAmount);
    // Faces fill without anti-aliasing, so neighbours meet without seams; the anti-aliased edges drawn over the
    // silhouettes and outlines keep the outside smooth.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    QPainterPath path;
    if (f.holes.isEmpty()) {
      p.drawPolygon(poly);
    } else {
      path.setFillRule(Qt::OddEvenFill);
      path.addPolygon(poly);
      path.closeSubpath();
      for (const auto& h : f.holes) {
        QPolygonF hp;
        for (const V3& q : h) hp << c.map(q);
        path.addPolygon(hp);
        path.closeSubpath();
      }
      p.drawPath(path);
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    if (f.cap && o.value("hatch").toBool()) {
      if (path.isEmpty()) path.addPolygon(poly);
      hatch(c, path, alpha(edges, 0.55));
    }
    p.setPen(QPen(edges, ew, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    for (qsizetype i = 0; i < f.p.size(); ++i)
      if (f.edge.value(i)) p.drawLine(poly[i], poly[(i + 1) % poly.size()]);
    for (const auto& h : f.holes) {
      QPolygonF hp;
      for (const V3& q : h) hp << c.map(q);
      hp << hp.first();
      p.drawPolyline(hp);
    }
  }
}

void solid(Ctx& c, const QJsonObject& o, const Xf& x) {
  V3 ua, va;
  planeAxes(o.value("plane").toString(), ua, va);
  const V3 n = V3::crossProduct(ua, va), dir = vec(o.value("dir"), n).normalized(), at = vec(o.value("at"));
  const double h = o.value("height").toDouble(10);
  if (std::abs(h) < 1e-6) return;
  auto P = [&](const QPointF& q, double w) { return x.apply(at + ua * float(q.x()) + va * float(q.y()) + dir * float(w)); };
  QList<Loop> loops{profile(o.value("profile").toObject())};
  if (c.record) {  // measuring: the outline's two ends are enough
    for (const QPointF& q : loops[0].p) { c.map(P(q, 0)); c.map(P(q, h)); }
    return;
  }
  QList<double> depth{std::abs(h)};  // a hole with a "depth" under the height is a pocket from the top, with a floor
  for (const QJsonValue& v : o.value("holes").toArray()) {
    loops << profile(v.toObject());
    depth << std::clamp(v.toObject().value("depth").toDouble(std::abs(h)), 0.0, std::abs(h));
  }
  QVector<Face> faces;
  for (qsizetype li = 0; li < loops.size(); ++li) {
    const Loop& l = loops[li];
    const double low = li > 0 && h > 0 ? h - depth[li] : 0;
    if (li > 0 && depth[li] < std::abs(h) - 1e-9) {
      Face floor;
      for (const QPointF& q : l.p) floor.p << P(q, low);
      floor.n = x.normal(dir);
      floor.edge = QVector<bool>(floor.p.size(), true);
      floor.pass = -1;
      floor.front = V3::dotProduct(floor.n, c.V) > 1e-4;
      faces << floor;
    }
    const qsizetype first = faces.size();
    for (qsizetype i = 0; i < l.p.size(); ++i) {
      const qsizetype j = (i + 1) % l.p.size();
      const QPointF d = l.p[j] - l.p[i];
      if (std::hypot(d.x(), d.y()) < 1e-6) continue;
      V3 wall = V3::crossProduct(ua * float(d.x()) + va * float(d.y()), n).normalized();
      if (li > 0) wall = -wall;
      Face f;
      f.p = {P(l.p[i], low), P(l.p[j], low), P(l.p[j], h), P(l.p[i], h)};
      f.n = x.normal(wall);
      f.edge = {true, l.sharp[j], true, l.sharp[i]};
      f.pass = li > 0 ? 0 : 1;
      f.front = V3::dotProduct(f.n, c.V) > 1e-4;
      faces << f;
    }
    // Silhouettes: the vertical edge between a wall facing the viewer and one facing away.
    const qsizetype count = faces.size() - first;
    for (qsizetype k = 0; k < count; ++k) {
      Face &a = faces[first + k], &b = faces[first + (k + 1) % count];
      if (a.front != b.front && (a.p[1] - b.p[0]).lengthSquared() < 1e-8) (a.front ? a.edge[1] : b.edge[3]) = true;
    }
  }
  for (int top = 0; top < 2; ++top) {
    Face cap;
    cap.cap = top == 1;
    cap.pass = 2;
    for (const QPointF& q : loops[0].p) cap.p << P(q, top ? h : 0);
    for (qsizetype li = 1; li < loops.size(); ++li) {
      if (!top && depth[li] < std::abs(h) - 1e-9) continue;  // pockets do not reach the bottom
      QVector<V3> hole;
      for (const QPointF& q : loops[li].p) hole << P(q, top ? h : 0);
      cap.holes << hole;
    }
    cap.n = x.normal((top ? dir : -dir) * (h > 0 ? 1.f : -1.f));
    cap.edge = QVector<bool>(cap.p.size(), true);
    cap.front = V3::dotProduct(cap.n, c.V) > 1e-4;
    faces << cap;
  }
  drawFaces(c, faces, o);
}

// A profile [[r, z], ...] turned about the z axis through `at` by `sweep` degrees from `start` (+x towards +y).
void lathe(Ctx& c, const QJsonObject& o, const Xf& x) {
  QVector<QPointF> prof;
  for (const QJsonValue& v : o.value("profile").toArray()) prof << QPointF(v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble());
  if (prof.size() < 3) return;
  if (area(prof) < 0) std::reverse(prof.begin(), prof.end());
  const double start = o.value("start").toDouble(), sweep = std::clamp(o.value("sweep").toDouble(360), -360.0, 360.0);
  if (std::abs(sweep) < 0.5) return;
  const bool full = std::abs(sweep) > 359.9;
  const int n = std::max(4, int(std::lround(o.value("n").toInt(48) * std::abs(sweep) / 360)));
  const V3 at = vec(o.value("at"));
  auto P = [&](double deg, const QPointF& q) {
    const double a = deg * kPi / 180;
    return x.apply(at + V3(float(q.x() * std::cos(a)), float(q.x() * std::sin(a)), float(q.y())));
  };
  const qsizetype m = prof.size();
  if (c.record) {
    for (int k = 0; k <= 8; ++k) for (const QPointF& q : prof) c.map(P(start + sweep * k / 8, q));
    return;
  }
  QVector<Face> faces;
  QVector<QVector<bool>> front(m, QVector<bool>(n));
  for (qsizetype j = 0; j < m; ++j) {
    const QPointF a = prof[j], b = prof[(j + 1) % m], d = b - a;
    const double len = std::hypot(d.x(), d.y());
    for (int k = 0; k < n; ++k) {
      const double t0 = start + sweep * k / n, t1 = start + sweep * (k + 1) / n, mid = (t0 + t1) / 2 * kPi / 180;
      Face f;
      f.p = {P(t0, a), P(t1, a), P(t1, b), P(t0, b)};
      f.n = len < 1e-9 ? V3(0, 0, 1) : x.normal(V3(float(d.y() / len * std::cos(mid)), float(d.y() / len * std::sin(mid)), float(-d.x() / len)));
      f.front = len > 1e-9 && V3::dotProduct(f.n, c.V) > 1e-4;
      front[j][k] = f.front;
      f.edge = {true, false, true, false};
      faces << f;
    }
  }
  for (qsizetype j = 0; j < m; ++j)
    for (int k = 0; k < n; ++k) {
      Face& f = faces[j * n + k];
      const bool prev = full ? front[j][(k + n - 1) % n] : k > 0 ? front[j][k - 1] : !f.front;
      const bool next = full ? front[j][(k + 1) % n] : k + 1 < n ? front[j][k + 1] : !f.front;
      f.edge[3] = prev != f.front;
      f.edge[1] = next != f.front;
    }
  if (!full) {
    const double sign = sweep > 0 ? 1 : -1;
    for (int end = 0; end < 2; ++end) {
      const double deg = end ? start + sweep : start, a = deg * kPi / 180;
      Face cap;
      for (const QPointF& q : prof) cap.p << P(deg, q);
      cap.n = x.normal(V3(float(-std::sin(a)), float(std::cos(a)), 0) * float(end ? sign : -sign));
      cap.edge = QVector<bool>(cap.p.size(), true);
      cap.front = V3::dotProduct(cap.n, c.V) > 1e-4;
      cap.cap = true;
      faces << cap;
    }
  }
  drawFaces(c, faces, o);
}

// Paints into a layer and composites it, so a see-through solid does not show its own back faces.
template <class F>
void layered(Ctx& c, double opacity, F draw) {
  if (opacity >= 0.999 || c.record) return draw();
  const qreal dpr = c.p->device()->devicePixelRatioF();
  QImage img((c.full.size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
  img.setDevicePixelRatio(dpr);
  img.fill(Qt::transparent);
  QPainter q(&img);
  q.setRenderHint(QPainter::Antialiasing);
  q.translate(-c.full.topLeft());
  QPainter* outer = c.p;
  c.p = &q;
  draw();
  q.end();
  c.p = outer;
  outer->save();
  outer->setOpacity(outer->opacity() * opacity);
  outer->drawImage(c.full.topLeft(), img);
  outer->restore();
}

// ---- marks ----------------------------------------------------------------------------------------------------------
void glyph(Ctx& c, const QString& kind, const QPointF& at, const QColor& color) {
  QPainter& p = *c.p;
  const double u = c.u, b = 7.5 * u;
  const QRectF box(at - QPointF(b, b), QSizeF(2 * b, 2 * b));
  p.setPen(QPen(token(*c.tk, "line"), 1 * u));
  p.setBrush(token(*c.tk, "bg2"));
  p.drawRoundedRect(box, 3 * u, 3 * u);
  p.setPen(QPen(color, 1.4 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  const double k = 4 * u;
  const QPointF o = at;
  auto L = [&](double x1, double y1, double x2, double y2) { p.drawLine(o + QPointF(x1, y1) * k, o + QPointF(x2, y2) * k); };
  if (kind == "horizontal") L(-1, 0, 1, 0);
  else if (kind == "vertical") L(0, -1, 0, 1);
  else if (kind == "parallel") { L(-0.9, 0.8, -0.1, -0.8); L(0.1, 0.8, 0.9, -0.8); }
  else if (kind == "perpendicular") { L(-1, 0.9, 1, 0.9); L(0, 0.9, 0, -1); }
  else if (kind == "coincident") { p.setBrush(color); p.drawEllipse(o, 1.8 * u, 1.8 * u); }
  else if (kind == "tangent") { p.drawEllipse(o + QPointF(0, -0.25) * k, 0.65 * k, 0.65 * k); L(-1, 0.6, 1, 0.6); }
  else if (kind == "equal") { L(-0.9, -0.4, 0.9, -0.4); L(-0.9, 0.4, 0.9, 0.4); }
  else if (kind == "concentric") { p.drawEllipse(o, 0.95 * k, 0.95 * k); p.drawEllipse(o, 0.4 * k, 0.4 * k); }
  else if (kind == "fix") { p.drawRect(QRectF(o + QPointF(-0.75, -0.1) * k, QSizeF(1.5 * k, 1.05 * k))); p.drawArc(QRectF(o + QPointF(-0.5, -0.95) * k, QSizeF(k, 1.4 * k)), 0, 180 * 16); }
  else if (kind == "midpoint") { L(-1, 0.6, 1, 0.6); p.setBrush(color); p.drawPolygon(QPolygonF{o + QPointF(0, -0.6) * k, o + QPointF(0.5, 0.25) * k, o + QPointF(-0.5, 0.25) * k}); }
  else if (kind == "symmetric") { L(0, -1, 0, 1); p.setBrush(color); p.drawEllipse(o + QPointF(-0.6, 0) * k, 1.2 * u, 1.2 * u); p.drawEllipse(o + QPointF(0.6, 0) * k, 1.2 * u, 1.2 * u); }
  else if (kind == "collinear") { L(-1, 0.5, -0.2, 0.5); L(0.2, 0.5, 1, 0.5); L(-1, -0.5, 1, -0.5); }
  else if (kind == "smooth") { QPainterPath s; s.moveTo(o + QPointF(-1, 0.7) * k); s.cubicTo(o + QPointF(0, 0.7) * k, o + QPointF(0, -0.7) * k, o + QPointF(1, -0.7) * k); p.drawPath(s); }
}

void snap(Ctx& c, const QString& kind, const QPointF& o, const QColor& color) {
  QPainter& p = *c.p;
  const double r = 5 * c.u;
  p.setPen(QPen(color, 1.5 * c.u, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
  p.setBrush(Qt::NoBrush);
  if (kind == "endpoint") p.drawRect(QRectF(o - QPointF(r, r), QSizeF(2 * r, 2 * r)));
  else if (kind == "midpoint") p.drawPolygon(QPolygonF{o + QPointF(0, -r * 1.1), o + QPointF(r, r * 0.8), o + QPointF(-r, r * 0.8)});
  else if (kind == "center") { p.drawEllipse(o, r, r); p.drawPoint(o); }
  else if (kind == "quadrant") p.drawPolygon(QPolygonF{o + QPointF(0, -r), o + QPointF(r, 0), o + QPointF(0, r), o + QPointF(-r, 0)});
  else if (kind == "intersection") { p.drawLine(o + QPointF(-r, -r), o + QPointF(r, r)); p.drawLine(o + QPointF(-r, r), o + QPointF(r, -r)); }
  else if (kind == "tangent") { p.drawEllipse(o + QPointF(0, -r * 0.3), r * 0.7, r * 0.7); p.drawLine(o + QPointF(-r, r * 0.4), o + QPointF(r, r * 0.4)); }
  else if (kind == "nearest") p.drawPolygon(QPolygonF{o + QPointF(-r, -r), o + QPointF(r, -r), o + QPointF(-r, r), o + QPointF(r, r)});
  else if (kind == "perpendicular") { p.drawLine(o + QPointF(-r, r), o + QPointF(r, r)); p.drawLine(o + QPointF(-r, r), o + QPointF(-r, -r)); p.drawLine(o + QPointF(-r, 0), o + QPointF(0, 0)); p.drawLine(o, o + QPointF(0, r)); }
}

void cursorGlyph(Ctx& c, const QString& kind, const QPointF& o) {
  QPainter& p = *c.p;
  const double u = c.u;
  if (kind == "cross" || kind == "move") {
    const QColor fg = token(*c.tk, "fg"), halo = alpha(token(*c.tk, "vp"), 0.9);
    for (int pass = 0; pass < 2; ++pass) {
      p.setPen(QPen(pass ? fg : halo, (pass ? 1.3 : 3.4) * u, Qt::SolidLine, Qt::RoundCap));
      const double in = kind == "cross" ? 3 : 0, out = kind == "cross" ? 10 : 9;
      for (const QPointF& d : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) p.drawLine(o + d * in * u, o + d * out * u);
      if (kind == "move" && pass) {
        p.setBrush(fg);
        for (const QPointF& d : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) arrowHead(p, o + d * 10.5 * u, d, 4 * u, 2.8 * u);
      }
    }
    return;
  }
  QPolygonF arrow{{0, 0}, {0, 16}, {4.2, 12.3}, {7.1, 18.5}, {9.7, 17.4}, {6.9, 11.3}, {12.2, 11.3}};
  for (QPointF& q : arrow) q = o + q * u;
  p.setPen(QPen(Qt::black, 1.0 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::white);
  p.drawPolygon(arrow);
}

// Value boxes beside the cursor: numbers formatted, {len} {dia} {ang} {w} {h} measured from `origin` to the cursor, the focused
// box with a caret and what is being typed.
void hud(Ctx& c, const QJsonObject& o, const Xf& x) {
  const QJsonValue atv = o.contains("at") ? o.value("at") : QJsonValue("cursor");
  if (atv.isString() && !c.hasCursor) return;
  const QPointF anchor = c.map(c.pt(atv, x));
  const double dx = o.value("dx").toDouble(14) * c.u;
  QPointF at = anchor + QPointF(dx, o.value("dy").toDouble(-30) * c.u);
  const int dec = o.value("dec").toInt(1), focus = o.value("focus").toInt(-1);
  const QString unit = o.value("unit").toString();
  QHash<QString, double> vars;
  if (o.contains("origin") && c.hasCursor) {
    const V3 f = c.pt(o.value("origin"), x), d = c.cursor - f;
    vars = {{"len", d.length()}, {"dia", 2 * d.length()}, {"w", std::abs(d.x())}, {"h", std::abs(d.y())}, {"ang", std::fmod(std::atan2(d.y(), d.x()) * 180 / kPi + 360, 360)}};
  }
  QPainter& p = *c.p;
  const QFont f = c.font(10.5);
  p.setFont(f);
  const QFontMetricsF fm(f);
  const double h = 17 * c.u, pad = 5 * c.u, gap = 4 * c.u;
  const QJsonArray fields = o.value("fields").toArray();
  QStringList texts;
  QList<double> widths;
  double total = -gap;
  for (qsizetype i = 0; i < fields.size(); ++i) {
    QString text = fields[i].isDouble() ? QString::number(fields[i].toDouble(), 'f', dec) + unit : fields[i].toString();
    for (auto it = vars.begin(); it != vars.end(); ++it) text.replace("{" + it.key() + "}", QString::number(it.value(), 'f', dec));
    if (i == focus && !o.value("typed").toString().isEmpty()) text = o.value("typed").toString();
    texts << text;
    widths << std::max(fm.horizontalAdvance(text) + 2 * pad + (i == focus ? 3 * c.u : 0), 26 * c.u);
    total += widths.last() + gap;
  }
  // Inside the scene: to the other side of the anchor when there is no room, then clamped.
  if (at.x() + total > c.scene.right() - 4 * c.u) at.setX(anchor.x() - std::abs(dx) - total);
  at.setX(std::clamp(at.x(), c.scene.left() + 4 * c.u, std::max(c.scene.left() + 4 * c.u, c.scene.right() - 4 * c.u - total)));
  at.setY(std::clamp(at.y(), c.scene.top() + 4 * c.u, std::max(c.scene.top() + 4 * c.u, c.scene.bottom() - 4 * c.u - h)));
  double cx = at.x();
  for (qsizetype i = 0; i < texts.size(); ++i) {
    const QString& text = texts[i];
    const bool focused = i == focus;
    const double w = widths[i];
    const QRectF box(cx, at.y(), w, h);
    p.setPen(QPen(focused ? token(*c.tk, "sel") : token(*c.tk, "line"), (focused ? 1.5 : 1) * c.u));
    p.setBrush(alpha(token(*c.tk, "bg2"), 0.96));
    p.drawRoundedRect(box, 3 * c.u, 3 * c.u);
    p.setPen(focused ? token(*c.tk, "fg") : token(*c.tk, "fg2"));
    p.drawText(box.adjusted(pad, 0, -pad, 0), Qt::AlignAbsolute | Qt::AlignLeft | Qt::AlignVCenter, literal(text));
    if (focused && std::fmod(c.t, 1.0) < 0.6) {
      const double tx = box.left() + pad + fm.horizontalAdvance(text) + 1.5 * c.u;
      p.setPen(QPen(token(*c.tk, "sel"), 1.2 * c.u));
      p.drawLine(QPointF(tx, box.top() + 3.5 * c.u), QPointF(tx, box.bottom() - 3.5 * c.u));
    }
    cx += w + gap;
  }
}

void keycaps(Ctx& c, const QJsonObject& o) {
  QPainter& p = *c.p;
  const QJsonArray keys = o.value("caps").toArray();
  const QFont f = c.font(10, QFont::Medium, true);
  p.setFont(f);
  const QFontMetricsF fm(f);
  const double h = 19 * c.u, pad = 6 * c.u, gap = 4 * c.u, m = 8 * c.u;
  double total = 0;
  for (const QJsonValue& k : keys) total += std::max(fm.horizontalAdvance(k.toString()) + 2 * pad, h) + gap;
  total -= gap;
  const QString corner = o.value("corner").toString("br");
  double x = corner.endsWith('l') ? c.scene.left() + m : corner.endsWith('c') ? c.scene.center().x() - total / 2 : c.scene.right() - m - total;
  const double y = corner.startsWith('t') ? c.scene.top() + m : c.scene.bottom() - m - h;
  const double since = c.t - o.value("press").toDouble(-10);
  const bool pressed = since > -0.06 && since < 0.24;
  // Left to right ("Ctrl" before "Enter") in every language, mirrored as a group: the corner flips, not the order.
  if (c.rtl && !corner.endsWith('c')) x = c.scene.left() + c.scene.right() - x - total;
  for (const QJsonValue& k : keys) {
    const double w = std::max(fm.horizontalAdvance(k.toString()) + 2 * pad, h);
    const QRectF cap(x, y + (pressed ? 1.5 * c.u : 0), w, h);
    p.setPen(Qt::NoPen);
    p.setBrush(alpha(Qt::black, 0.25));
    if (!pressed) p.drawRoundedRect(cap.translated(0, 1.5 * c.u), 3.5 * c.u, 3.5 * c.u);
    p.setPen(QPen(pressed ? token(*c.tk, "sel") : token(*c.tk, "line"), 1 * c.u));
    p.setBrush(pressed ? token(*c.tk, "sel") : token(*c.tk, "bg4"));
    p.drawRoundedRect(cap, 3.5 * c.u, 3.5 * c.u);
    p.setPen(pressed ? token(*c.tk, "onsel") : token(*c.tk, "fg"));
    p.drawText(cap, Qt::AlignCenter, k.toString());
    x += w + gap;
  }
}

// A panel stub: title, rows (label and value, radio, check box, slider, indent, icon), a highlighted row, a button.
void card(Ctx& c, const QJsonObject& o) {
  QPainter& p = *c.p;
  const Tokens& t = *c.tk;
  const double u = c.u, w = o.value("w").toDouble(140) * u, pad = 7 * u, rowH = 17 * u, titleH = 21 * u;
  const QJsonArray rows = o.value("rows").toArray();
  const bool button = o.contains("button");
  const double h = titleH + rows.size() * rowH + (button ? 26 * u : 0) + 4 * u;
  const QJsonArray at = o.value("screen").toArray();
  const QRectF ltr(c.scene.left() + at.at(0).toDouble() * c.scene.width(), c.scene.top() + at.at(1).toDouble() * c.scene.height(), w, h);  // laid out here
  const QRectF box = c.mirror(ltr);
  p.setPen(Qt::NoPen);
  p.setBrush(alpha(Qt::black, t.dark ? 0.3 : 0.12));
  p.drawRoundedRect(box.translated(0, 2 * u), 5 * u, 5 * u);
  p.setPen(QPen(t.line, 1 * u));
  p.setBrush(t.bg2);
  p.drawRoundedRect(box, 5 * u, 5 * u);
  p.setFont(c.font(10.5, QFont::DemiBold));
  p.setPen(t.fg);
  p.drawText(c.mirror(QRectF(ltr.left() + pad, ltr.top(), w - 2 * pad, titleH)), Qt::AlignLeft | Qt::AlignVCenter, i18n::t(o.value("title").toString()));
  p.setPen(QPen(t.line, 1 * u));
  p.drawLine(QPointF(box.left() + 1, box.top() + titleH), QPointF(box.right() - 1, box.top() + titleH));
  const int hl = o.value("hl").toInt(-1);
  double y = ltr.top() + titleH + 2 * u;
  for (qsizetype i = 0; i < rows.size(); ++i, y += rowH) {
    // [label, value], or {"text" (translated) | "label" (as written), "value", "radio", "check", "indent", "icon", "slider", "dim", "color"}
    const QJsonObject r = rows[i].isArray() ? QJsonObject{{"text", rows[i].toArray().at(0)}, {"value", rows[i].toArray().at(1)}} : rows[i].toObject();
    const QRectF row(ltr.left() + 2 * u, y, w - 4 * u, rowH);
    if (i == hl) {
      p.setPen(Qt::NoPen);
      p.setBrush(t.selbg);
      p.drawRoundedRect(c.mirror(row), 3 * u, 3 * u);
    }
    double x = row.left() + pad - 2 * u + r.value("indent").toDouble() * 10 * u;
    const bool dim = r.value("dim").toBool();
    if (r.contains("radio") || r.contains("check")) {
      const bool on = r.value(r.contains("radio") ? "radio" : "check").toBool();
      const QRectF mark = c.mirror(QRectF(x, y + rowH / 2 - 4.5 * u, 9 * u, 9 * u));
      p.setPen(QPen(on ? t.sel : t.fg3, 1.2 * u));
      p.setBrush(Qt::NoBrush);
      if (r.contains("radio")) {
        p.drawEllipse(mark);
        if (on) { p.setPen(Qt::NoPen); p.setBrush(t.sel); p.drawEllipse(mark.center(), 2.4 * u, 2.4 * u); }
      } else {
        if (on) p.setBrush(t.sel);
        p.drawRoundedRect(mark, 2 * u, 2 * u);
        if (on) {
          p.setPen(QPen(t.onsel, 1.3 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
          const QPointF m0 = mark.center();
          p.drawPolyline(QPolygonF{m0 + QPointF(-2.4, 0) * u, m0 + QPointF(-0.6, 1.8) * u, m0 + QPointF(2.6, -1.8) * u});
        }
      }
      x += 14 * u;
    }
    if (const QString icon = r.value("icon").toString(); icons::has(icon)) {
      const QRectF ir = c.mirror(QRectF(x, y + rowH / 2 - 6 * u, 12 * u, 12 * u));
      p.drawPixmap(ir.toRect(), icons::pixmap(icon, dim ? t.fg3 : t.fg2, int(std::lround(12 * u)), p.device()->devicePixelRatioF()));
      x += 16 * u;
    }
    p.setFont(c.font(10));
    p.setPen(dim ? t.fg3 : t.fg2);
    if (r.contains("text") || r.contains("label"))
      p.drawText(c.mirror(QRectF(x, y, row.right() - x - pad, rowH)), Qt::AlignLeft | Qt::AlignVCenter,
                 r.contains("text") ? i18n::t(r.value("text").toString()) : literal(r.value("label").toString()));
    if (r.contains("slider")) {
      const double sx0 = row.left() + w * 0.42, sx1 = row.right() - pad, f = std::clamp(r.value("slider").toDouble(), 0.0, 1.0), cy = y + rowH / 2;
      const QRectF track = c.mirror(QRectF(sx0, cy - 1.5 * u, sx1 - sx0, 3 * u)), done = c.mirror(QRectF(sx0, cy - 1.5 * u, (sx1 - sx0) * f, 3 * u));
      p.setPen(Qt::NoPen);
      p.setBrush(t.bg4);
      p.drawRoundedRect(track, 1.5 * u, 1.5 * u);
      p.setBrush(t.sel);
      p.drawRoundedRect(done, 1.5 * u, 1.5 * u);
      const QPointF knob = c.mirror(QRectF(sx0 + (sx1 - sx0) * f - 0.5, cy - 0.5, 1, 1)).center();
      p.setBrush(t.fg);
      p.drawEllipse(knob, 4.5 * u, 4.5 * u);
    } else if (r.contains("value")) {
      p.setPen(dim ? t.fg3 : r.value("color").isString() ? c.col(r.value("color"), "fg") : t.fg);
      const QString value = r.value("value").toString();
      p.drawText(c.mirror(QRectF(x, y, row.right() - x - pad, rowH)), Qt::AlignRight | Qt::AlignVCenter, wordy(value) ? i18n::t(value) : literal(value));
    }
  }
  if (button) {
    const QString label = i18n::t(o.value("button").toString());
    const QFont f = c.font(10, QFont::DemiBold);
    const double bw = QFontMetricsF(f).horizontalAdvance(label) + 20 * u;
    const QRectF b = c.mirror(QRectF(ltr.right() - pad - bw, y + 4 * u, bw, 18 * u));
    const double since = c.t - o.value("press").toDouble(-10);
    p.setPen(Qt::NoPen);
    p.setBrush(since > -0.05 && since < 0.3 ? t.sel.darker(130) : t.sel);
    p.drawRoundedRect(b, 3.5 * u, 3.5 * u);
    p.setFont(f);
    p.setPen(t.onsel);
    p.drawText(b, Qt::AlignCenter, label);
  }
}

void chip(Ctx& c, const QJsonObject& o, const Xf& x) {
  QPainter& p = *c.p;
  const Tokens& t = *c.tk;
  const double u = c.u, h = 19 * u, pad = 7 * u;
  const QString text = shown(o), icon = o.value("icon").toString();
  const QFont f = c.font(10);
  const double w = pad * 2 + QFontMetricsF(f).horizontalAdvance(text) + (o.contains("dot") ? 11 * u : 0) + (icon.isEmpty() ? 0 : 16 * u) + (o.value("close").toBool() ? 14 * u : 0);
  QRectF r;
  if (o.contains("screen")) {
    const QPointF tl = c.screen(o.value("screen")) + QPointF(o.value("dx").toDouble() * (c.rtl ? -1 : 1), o.value("dy").toDouble()) * u;
    r = QRectF(c.rtl ? tl.x() - w : tl.x(), tl.y(), w, h);
  } else {
    const QPointF at = c.map(c.pt(o.value("at"), x)) + QPointF(o.value("dx").toDouble(), o.value("dy").toDouble()) * u;
    r = QRectF(at.x() - w / 2, at.y() - h / 2, w, h);
  }
  const bool hl = o.value("hl").toBool();
  p.setPen(QPen(hl ? t.sel : t.line, (hl ? 1.5 : 1) * u));
  p.setBrush(alpha(t.bg2, 0.97));
  p.drawRoundedRect(r, h / 2, h / 2);
  // Content left to right, mirrored inside the chip in RTL.
  const bool flip = c.rtl;
  double cx = flip ? r.right() - pad : r.left() + pad;
  const double step = flip ? -1 : 1;
  if (o.contains("dot")) {
    p.setPen(Qt::NoPen);
    p.setBrush(c.col(o.value("dot"), "sel"));
    p.drawEllipse(QPointF(cx + step * 3.5 * u, r.center().y()), 3.5 * u, 3.5 * u);
    cx += step * 11 * u;
  }
  if (!icon.isEmpty()) {
    const QRectF ir(flip ? cx - 12 * u : cx, r.center().y() - 6 * u, 12 * u, 12 * u);
    p.drawPixmap(ir.toRect(), icons::pixmap(icon, t.fg2, int(std::lround(12 * u)), p.device()->devicePixelRatioF()));
    cx += step * 16 * u;
  }
  const double tw = QFontMetricsF(f).horizontalAdvance(text);
  p.setFont(f);
  p.setPen(c.col(o.value("color"), "fg"));
  p.drawText(QRectF(flip ? cx - tw : cx, r.top(), tw + 1, h), Qt::AlignAbsolute | Qt::AlignLeft | Qt::AlignVCenter, text);
  cx += step * (tw + 4 * u);
  if (o.value("close").toBool()) {
    p.setPen(QPen(t.fg3, 1.2 * u, Qt::SolidLine, Qt::RoundCap));
    const QPointF m(cx + step * 4.5 * u, r.center().y());
    p.drawLine(m + QPointF(-2.8, -2.8) * u, m + QPointF(2.8, 2.8) * u);
    p.drawLine(m + QPointF(-2.8, 2.8) * u, m + QPointF(2.8, -2.8) * u);
  }
}

void label(Ctx& c, const QJsonObject& o, const Xf& x) {
  QPainter& p = *c.p;
  const QString text = shown(o);
  if (text.isEmpty()) return;
  const QFont f = c.font(o.value("size").toDouble(11), o.value("bold").toBool() ? QFont::DemiBold : QFont::Normal);
  p.setFont(f);
  const QFontMetricsF fm(f);
  const double w = fm.horizontalAdvance(text), h = fm.height();
  const bool screen = o.contains("screen");
  QString align = o.value("align").toString(screen ? "left" : "center");
  QPointF at = screen ? c.screen(o.value("screen")) : c.map(c.pt(o.value("at"), x));
  at += QPointF(o.value("dx").toDouble() * (screen && c.rtl ? -1 : 1), o.value("dy").toDouble()) * c.u;
  if (screen && c.rtl) align = align == "left" ? "right" : align == "right" ? "left" : align;
  const double left = align == "left" ? at.x() : align == "right" ? at.x() - w : at.x() - w / 2;
  const QRectF r(left, at.y() - h / 2, w, h);
  if (o.value("box").isString()) {
    p.setPen(QPen(token(*c.tk, "line"), 1 * c.u));
    p.setBrush(alpha(c.col(o.value("box"), "bg2"), 0.94));
    p.drawRoundedRect(r.adjusted(-5 * c.u, -1.5 * c.u, 5 * c.u, 1.5 * c.u), 3 * c.u, 3 * c.u);
  }
  p.setPen(c.col(o.value("color"), "fg2"));
  p.drawText(r.adjusted(-2, 0, 2, 0), Qt::AlignCenter, text);
}

void dimension(Ctx& c, const QJsonObject& o, const Xf& x) {
  QPainter& p = *c.p;
  const QString kind = o.value("kind").toString("linear");
  const QColor color = c.col(o.value("color"), "fg");
  const double u = c.u;
  p.setPen(QPen(color, 1.1 * u));
  p.setBrush(color);
  QPointF anchor;
  QPointF away(0, -1);  // the side the text goes to
  auto line = [&](const QPointF& a, const QPointF& b) { p.drawLine(a, b); };
  if (kind == "linear") {
    const QPointF a = c.map(c.pt(o.value("a"), x)), b = c.map(c.pt(o.value("b"), x));
    QPointF d = b - a;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-6) return;
    d /= len;
    const QPointF n(d.y(), -d.x());
    const double off = o.value("offset").toDouble(14) * u, sg = off < 0 ? -1 : 1;
    const QPointF a2 = a + n * off, b2 = b + n * off;
    if (std::abs(off) > 4 * u) { line(a + n * sg * 3 * u, a2 + n * sg * 4 * u); line(b + n * sg * 3 * u, b2 + n * sg * 4 * u); }
    line(a2, b2);
    p.setPen(Qt::NoPen);
    arrowHead(p, a2, a2 - b2, 7 * u, 2.8 * u);
    arrowHead(p, b2, b2 - a2, 7 * u, 2.8 * u);
    away = n * sg;
    anchor = (a2 + b2) / 2;
  } else {
    V3 ua, va;
    planeAxes(o.value("plane").toString(), ua, va);
    const V3 ctr = c.pt(o.value("center"), x);
    const double r = o.value("r").toDouble(5);
    const QPointF cm = c.map(ctr);
    if (kind == "angular") {
      QPolygonF arc;
      for (const V3& q : circlePoints(ctr, r, o.value("start").toDouble(), o.value("sweep").toDouble(90), o.value("plane").toString(), 32)) arc << c.map(q);
      p.setBrush(Qt::NoBrush);
      p.drawPolyline(arc);
      p.setPen(Qt::NoPen);
      p.setBrush(color);
      arrowHead(p, arc.first(), arc.first() - arc[1], 7 * u, 2.8 * u);
      arrowHead(p, arc.last(), arc.last() - arc[arc.size() - 2], 7 * u, 2.8 * u);
      const QPointF mid = arc[arc.size() / 2];
      away = mid - cm;
      const double l = std::hypot(away.x(), away.y());
      away = l > 0 ? away / l : QPointF(0, -1);
      anchor = mid;
    } else {
      const double a = o.value("angle").toDouble(45) * kPi / 180;
      const V3 dir = ua * float(std::cos(a)) + va * float(std::sin(a));
      const QPointF rim = c.map(ctr + dir * float(r));
      QPointF d = rim - cm;
      const double l = std::hypot(d.x(), d.y());
      if (l < 1e-6) return;
      d /= l;
      const QPointF from = kind == "diameter" ? c.map(ctr - dir * float(r)) : cm, end = rim + d * 16 * u;
      line(from, end);
      p.setPen(Qt::NoPen);
      arrowHead(p, rim, d, 7 * u, 2.8 * u);
      if (kind == "diameter") arrowHead(p, from, -d, 7 * u, 2.8 * u);
      else p.drawEllipse(cm, 1.8 * u, 1.8 * u);
      away = d;
      anchor = end;
    }
  }
  const QString text = shown(o);
  if (text.isEmpty()) return;
  const QFont f = c.font(10.5, QFont::Medium);
  p.setFont(f);
  const QFontMetricsF fm(f);
  const QSizeF sz(fm.horizontalAdvance(text) + 6 * u, fm.height());
  // Beside the anchor, pushed out along `away` so the box never sits on the line.
  const QPointF centre = anchor + away * 3 * u + QPointF(away.x() * sz.width() / 2, away.y() * sz.height() / 2);
  const QRectF r(centre - QPointF(sz.width() / 2, sz.height() / 2), sz);
  p.setPen(Qt::NoPen);
  p.setBrush(alpha(token(*c.tk, "vp"), 0.85));
  p.drawRoundedRect(r, 2.5 * u, 2.5 * u);
  p.setPen(color);
  p.drawText(r, Qt::AlignCenter, text);
}

// An original stand-in photograph for the canvas clips: a machined plate with two holes on a bench, soft light.
void picture(Ctx& c, const QJsonObject& o, const Xf& x) {
  const QJsonArray k = o.value("corners").toArray();
  const V3 a = c.pt(k.at(0), x), b = c.pt(k.at(1), x);
  const QPolygonF to{c.map(V3(a.x(), b.y(), a.z())), c.map(V3(b.x(), b.y(), a.z())), c.map(V3(b.x(), a.y(), a.z())), c.map(V3(a.x(), a.y(), a.z()))};
  QTransform tr;
  if (!QTransform::quadToQuad(QPolygonF{{0, 0}, {1, 0}, {1, 1}, {0, 1}}, to, tr)) return;
  QPainter& p = *c.p;
  p.save();
  p.setTransform(tr, true);
  p.setPen(Qt::NoPen);
  QLinearGradient bench(0, 0, 0, 1);
  bench.setColorAt(0, QColor(196, 186, 170));
  bench.setColorAt(1, QColor(146, 134, 118));
  p.setBrush(bench);
  p.drawRect(QRectF(0, 0, 1, 1));
  QRadialGradient shadow(0.53, 0.56, 0.42);
  shadow.setColorAt(0, QColor(40, 30, 20, 120));
  shadow.setColorAt(1, QColor(40, 30, 20, 0));
  p.setBrush(shadow);
  p.drawEllipse(QRectF(0.1, 0.2, 0.86, 0.66));
  QPainterPath plate;
  plate.addRoundedRect(QRectF(0.18, 0.26, 0.64, 0.46), 0.07, 0.1);
  for (const double cx : {0.33, 0.67}) plate.addEllipse(QPointF(cx, 0.49), 0.065, 0.09);
  plate.setFillRule(Qt::OddEvenFill);
  QLinearGradient metal(0.2, 0.25, 0.8, 0.75);
  metal.setColorAt(0, QColor(178, 184, 192));
  metal.setColorAt(0.55, QColor(128, 135, 145));
  metal.setColorAt(1, QColor(96, 102, 112));
  p.setBrush(metal);
  p.drawPath(plate);
  p.setBrush(QColor(255, 255, 255, 60));
  p.drawRoundedRect(QRectF(0.22, 0.29, 0.5, 0.05), 0.02, 0.02);
  p.restore();
}

// Where the cursor is: a model point ("pos"), or a place on the screen chrome ("screen", mirrored like cards), plus px.
QPointF cursorAt(const Ctx& c, const QJsonObject& o) {
  const QPointF at = o.contains("screen") ? c.screen(o.value("screen")) : c.map(c.pt(o.value("pos"), xform(o)));
  return at + QPointF(o.value("dx").toDouble() * (o.contains("screen") && c.rtl ? -1 : 1), o.value("dy").toDouble()) * c.u;
}

void ripples(Ctx& c, const Item& it) {
  QPainter& p = *c.p;
  const QColor sel = token(*c.tk, "sel");
  int number = 0;
  QList<QPointF> placed;
  for (const double tc : it.clicks) {
    ++number;
    const QPointF at = cursorAt(c, evaluate(it, tc));
    if (c.still) {  // the still frame numbers the clicks instead (side by side where clicks land on one place)
      if (tc > c.t + 1e-6) continue;
      QPointF b = at + QPointF(-9, -9) * c.u;
      while (std::any_of(placed.begin(), placed.end(), [&](const QPointF& q) { return QLineF(q, b).length() < 10 * c.u; })) b += QPointF(15, 0) * c.u;
      placed << b;
      p.setPen(QPen(token(*c.tk, "vp"), 1.2 * c.u));
      p.setBrush(sel);
      p.drawEllipse(b, 6.5 * c.u, 6.5 * c.u);
      p.setFont(c.font(9, QFont::Bold));
      p.setPen(token(*c.tk, "onsel"));
      p.drawText(QRectF(b - QPointF(7, 7) * c.u, QSizeF(14, 14) * c.u), Qt::AlignCenter, QString::number(number));
      continue;
    }
    const double k = (c.t - tc) / kRipple;
    if (k < 0 || k > 1) continue;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(alpha(sel, 1 - k), 2 * c.u));
    p.drawEllipse(at, (4 + 13 * k) * c.u, (4 + 13 * k) * c.u);
    if (k < 0.5) {
      p.setPen(Qt::NoPen);
      p.setBrush(alpha(sel, 0.8 * (1 - 2 * k)));
      p.drawEllipse(at, 3.5 * c.u, 3.5 * c.u);
    }
  }
}

void paintItem(Ctx& c, const Item& it, const QJsonObject& o) {
  QPainter& p = *c.p;
  const Xf x = xform(o);
  const QString& el = it.el;
  auto points = [&](const QJsonValue& v) {
    QPolygonF out;
    for (const QJsonValue& q : v.toArray()) out << c.map(c.pt(q, x));
    return out;
  };
  auto mapped = [&](const QVector<V3>& v) {
    QPolygonF out;
    for (const V3& q : v) out << c.map(q);
    return out;
  };
  if (el == "grid") {
    Bounds* rec = c.record;
    c.record = nullptr;  // the grid fills the view: it never sizes it
    const double sp = std::max(0.1, o.value("spacing").toDouble(5)), z = o.value("z").toDouble();
    const int major = o.value("major").toInt();
    QRectF area;
    if (const QJsonArray a = o.value("area").toArray(); a.size() == 4) area = QRectF(QPointF(a[0].toDouble(), a[1].toDouble()), QPointF(a[2].toDouble(), a[3].toDouble())).normalized();
    else if (!c.iso) area = QRectF(QPointF(c.center.x() - c.scene.width() / 2 / c.s, c.center.y() - c.scene.height() / 2 / c.s), QSizeF(c.scene.width() / c.s, c.scene.height() / c.s));
    else area = QRectF(-30, -30, 60, 60);
    const QColor minor = c.col(o.value("color"), "line"), strong = alpha(token(*c.tk, "fg3"), 0.55);
    for (int axis = 0; axis < 2; ++axis) {
      const double lo = axis ? area.top() : area.left(), hi = axis ? area.bottom() : area.right();
      for (double v = std::ceil(lo / sp) * sp; v <= hi + 1e-9; v += sp) {
        const int index = int(std::lround(v / sp));
        p.setPen(QPen(major && index % major == 0 ? strong : minor, 1.0));
        const V3 a = axis ? V3(float(area.left()), float(v), float(z)) : V3(float(v), float(area.top()), float(z));
        const V3 b = axis ? V3(float(area.right()), float(v), float(z)) : V3(float(v), float(area.bottom()), float(z));
        p.drawLine(c.map(a), c.map(b));
      }
    }
    c.record = rec;
  } else if (el == "axes") {
    const V3 at = c.pt(o.value("at"), x);
    if (!c.iso && !o.contains("len")) {
      Bounds* rec = c.record;
      c.record = nullptr;
      const QPointF o0 = c.map(at);
      p.setPen(QPen(alpha(token(*c.tk, "red"), 0.7), 1.2 * c.u));
      p.drawLine(QPointF(c.scene.left(), o0.y()), QPointF(c.scene.right(), o0.y()));
      p.setPen(QPen(alpha(token(*c.tk, "green"), 0.7), 1.2 * c.u));
      p.drawLine(QPointF(o0.x(), c.scene.top()), QPointF(o0.x(), c.scene.bottom()));
      c.record = rec;
    } else {
      const float len = float(o.value("len").toDouble(10));
      const QStringList colors{"red", "green", "blue"};
      for (int i = 0; i < (c.iso ? 3 : 2); ++i) {
        V3 d;
        d[i] = len;
        p.setPen(QPen(alpha(token(*c.tk, colors[i]), 0.85), 1.4 * c.u, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c.map(at), c.map(at + d));
      }
    }
  } else if (el == "poly") {
    stroke(c, points(o.value("points")), o.value("closed").toBool(), o);
  } else if (el == "rect") {
    const QJsonArray k = o.value("corners").toArray();
    const V3 a = c.pt(k.at(0), x), b = c.pt(k.at(1), x);
    stroke(c, mapped({a, V3(b.x(), a.y(), a.z()), V3(b.x(), b.y(), a.z()), V3(a.x(), b.y(), a.z())}), true, o);
  } else if (el == "circle" || el == "arc") {
    const V3 ctr = c.pt(o.value("center"), x);
    const QJsonValue rv = o.value("r");
    const double r = rv.isString() ? (c.cursor - ctr).length() : rv.toDouble(5);
    if (el == "arc" && o.contains("through")) {
      const QJsonArray k = o.value("through").toArray();
      stroke(c, mapped(arcThrough(c.pt(k.at(0), x), c.pt(k.at(2), x), c.pt(k.at(1), x))), false, o);
    } else if (r > 1e-6) {
      const bool circle = el == "circle";
      QVector<V3> pts = circlePoints(ctr, r, circle ? 90 : o.value("start").toDouble(), circle ? 360 : o.value("sweep").toDouble(90), o.value("plane").toString(), 64);
      if (circle) pts.removeLast();
      stroke(c, mapped(pts), circle, o);
    }
  } else if (el == "slot") {
    const V3 a = c.pt(o.value("c1"), x), b = c.pt(o.value("c2"), x), d = b - a;
    double r = o.value("r").toDouble(3);
    if (o.value("r").isString() && c.hasCursor) {
      const V3 n = d.length() > 1e-6 ? d.normalized() : V3(1, 0, 0), w = c.cursor - a;
      r = std::max(0.05, double((w - n * V3::dotProduct(w, n)).length()));
    }
    const double dir = std::atan2(d.y(), d.x());
    QVector<V3> pts;
    for (int end = 0; end < 2; ++end)
      for (int s = 0; s <= 16; ++s) {
        const double ang = dir - kPi / 2 + end * kPi + kPi * s / 16;
        pts << (end ? a : b) + V3(float(std::cos(ang) * r), float(std::sin(ang) * r), 0);
      }
    stroke(c, mapped(pts), true, o);
  } else if (el == "polygon") {
    const V3 ctr = c.pt(o.value("center"), x);
    const int n = std::max(3, o.value("n").toInt(6));
    double r = o.value("r").toDouble(5), start = 90;
    if (o.contains("corner")) {
      const V3 d = c.pt(o.value("corner"), x) - ctr;
      r = d.length();
      start = std::atan2(d.y(), d.x()) * 180 / kPi;
    }
    if (r < 1e-6) return;
    QVector<V3> pts = circlePoints(ctr, r, start, 360, "xy", n);
    pts.removeLast();
    stroke(c, mapped(pts), true, o);
  } else if (el == "shape") {  // a profile and its holes drawn in a plane: sketch regions in 3D, outlines
    V3 ua, va;
    planeAxes(o.value("plane").toString(), ua, va);
    const V3 at = vec(o.value("at"));
    QList<Loop> loops{profile(o.value("profile").toObject())};
    for (const QJsonValue& v : o.value("holes").toArray()) loops << profile(v.toObject());
    QPainterPath region;
    region.setFillRule(Qt::OddEvenFill);
    QList<QPolygonF> outlines;
    for (const Loop& l : loops) {
      QPolygonF poly;
      for (const QPointF& q : l.p) poly << c.map(x.apply(at + ua * float(q.x()) + va * float(q.y())));
      region.addPolygon(poly);
      region.closeSubpath();
      outlines << poly;
    }
    if (o.contains("fill") && o.value("progress").toDouble(1) >= 0.999) {
      QColor f = c.col(o.value("fill"), "selbg");
      f.setAlphaF(float(o.value("fillOpacity").toDouble(0.22)));
      p.setPen(Qt::NoPen);
      p.setBrush(f);
      p.drawPath(region);
    }
    QJsonObject line = o;
    line.remove("fill");
    for (const QPolygonF& poly : outlines) stroke(c, poly, true, line);
  } else if (el == "plane") {
    V3 ua, va;
    planeAxes(o.value("plane").toString(), ua, va);
    const V3 ctr = c.pt(o.value("center"), x), sz = vec(o.value("size"), V3(20, 20, 0));
    const V3 du = ua * sz.x() / 2, dv = va * sz.y() / 2;
    const QPolygonF quad = mapped({ctr - du - dv, ctr + du - dv, ctr + du + dv, ctr - du + dv});
    const QColor color = c.col(o.value("color"), "sel");
    p.setPen(QPen(alpha(color, 0.8), 1.1 * c.u));
    p.setBrush(alpha(color, o.value("fillOpacity").toDouble(0.14)));
    p.drawPolygon(quad);
  } else if (el == "solid") {
    solid(c, o, x);
  } else if (el == "lathe") {
    lathe(c, o, x);
  } else if (el == "arrow") {
    const V3 at = c.pt(o.value("at"), x), dir = vec(o.value("dir"), V3(0, 0, 1));
    const QPointF a = c.map(at);
    QPointF d = c.map(at + dir) - a;
    const double l = std::hypot(d.x(), d.y());
    if (l < 1e-9) return;
    d /= l;
    const QColor color = c.col(o.value("color"), "sel");
    const QPointF tip = a + d * o.value("len").toDouble(34) * c.u;
    p.setPen(QPen(color, 2.2 * c.u, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, tip - d * 6 * c.u);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    arrowHead(p, tip, d, 10 * c.u, 5 * c.u);
    p.setBrush(token(*c.tk, "vp"));
    p.setPen(QPen(color, 1.6 * c.u));
    p.drawEllipse(a, 3.2 * c.u, 3.2 * c.u);
  } else if (el == "dim") {
    dimension(c, o, x);
  } else if (el == "label") {
    label(c, o, x);
  } else if (el == "chip") {
    chip(c, o, x);
  } else if (el == "cursor") {
    ripples(c, it);
    if (c.still) return;  // the numbered clicks tell the story; a resting cursor would hide them
    const QPointF at = cursorAt(c, o);
    if (o.value("down").toBool()) {
      p.setPen(Qt::NoPen);
      p.setBrush(alpha(token(*c.tk, "sel"), 0.45));
      p.drawEllipse(at, 5 * c.u, 5 * c.u);
    }
    const QString kind = o.value("kind").toString("arrow");
    cursorGlyph(c, kind, at);
    if (const QString badge = o.value("badge").toString(); !badge.isEmpty()) {
      const QRectF b(at + QPointF(kind == "arrow" ? 11 : 8, kind == "arrow" ? 15 : 7) * c.u, QSizeF(16, 16) * c.u);
      p.setPen(QPen(token(*c.tk, "line"), 1 * c.u));
      p.setBrush(token(*c.tk, "bg2"));
      p.drawRoundedRect(b, 3.5 * c.u, 3.5 * c.u);
      p.drawPixmap(b.adjusted(2 * c.u, 2 * c.u, -2 * c.u, -2 * c.u).toRect(), icons::pixmap(badge, token(*c.tk, "sel"), int(std::lround(12 * c.u)), p.device()->devicePixelRatioF()));
    }
  } else if (el == "hud") {
    hud(c, o, x);
  } else if (el == "key") {
    keycaps(c, o);
  } else if (el == "glyph") {
    glyph(c, o.value("kind").toString(), c.map(c.pt(o.value("at"), x)) + QPointF(o.value("dx").toDouble(10), o.value("dy").toDouble(-12)) * c.u, c.col(o.value("color"), "green"));
  } else if (el == "snap") {
    snap(c, o.value("kind").toString(), c.map(c.pt(o.value("at"), x)), c.col(o.value("color"), "green"));
  } else if (el == "card") {
    card(c, o);
  } else if (el == "picture") {
    picture(c, o, x);
  }
}

void captionBar(Ctx& c, const Clip& clip, const QRectF& bar) {
  QPainter& p = *c.p;
  const Tokens& t = *c.tk;
  const double u = c.u;
  p.setPen(Qt::NoPen);
  p.setBrush(alpha(t.bg2, 0.94));
  p.drawRect(bar);
  p.setPen(QPen(t.line, 1));
  p.drawLine(bar.topLeft(), bar.topRight());
  const int n = int(clip.steps.size());
  int current = 0;
  for (int i = 0; i < n; ++i) if (c.t >= clip.steps[i].from - 1e-9) current = i;
  // Laid out left to right: badge, caption, step dots; mirrored in RTL.
  auto M = [&](const QRectF& r) { return c.rtl ? QRectF(bar.left() + bar.right() - r.right(), r.top(), r.width(), r.height()) : r; };
  const double dots = n > 1 ? n * 8 * u + 6 * u : 0;
  double x = bar.left() + 7 * u;
  QString text;
  if (c.still) {
    QStringList parts;
    for (int i = 0; i < n; ++i) parts << QString("%1 %2").arg(i + 1).arg(i18n::t(clip.steps[i].caption));
    text = parts.join(QString::fromUtf8("  ›  "));
  } else if (n) {
    const QRectF badge = M(QRectF(x, bar.center().y() - 7 * u, 14 * u, 14 * u));
    p.setPen(Qt::NoPen);
    p.setBrush(t.sel);
    p.drawEllipse(badge);
    p.setFont(c.font(9, QFont::Bold));
    p.setPen(t.onsel);
    p.drawText(badge, Qt::AlignCenter, QString::number(current + 1));
    x += 20 * u;
    text = i18n::t(clip.steps[current].caption);
  }
  const QFont f = c.font(10.5);
  p.setFont(f);
  p.setPen(t.fg);
  const QRectF tr = M(QRectF(x, bar.top(), bar.right() - x - dots - 4 * u, bar.height()));
  p.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(f).elidedText(text, Qt::ElideRight, tr.width()));
  if (c.still) return;
  for (int i = 0; i < n && n > 1; ++i) {
    const QRectF d = M(QRectF(bar.right() - dots + i * 8 * u, bar.center().y() - 2.5 * u, 5 * u, 5 * u));
    p.setPen(Qt::NoPen);
    p.setBrush(i == current ? t.sel : i < current ? t.fg2 : t.fg3);
    p.drawEllipse(d);
  }
}

void paintScene(Ctx& c, const Clip& clip) {
  for (const Item& it : clip.items) {
    const double vis = visibility(it, c.t);
    if (vis <= 0 || it.el == "camera") continue;
    const QJsonObject o = evaluate(it, c.t);
    const double a = vis * o.value("opacity").toDouble(1);
    if (a <= 0.003) continue;
    c.p->save();
    if (it.el == "solid" || it.el == "lathe") {
      layered(c, a, [&] { paintItem(c, it, o); });
    } else {
      c.p->setOpacity(c.p->opacity() * a);
      paintItem(c, it, o);
    }
    c.p->restore();
  }
}

// Camera and cursor first: every item may point at "cursor", the camera turns and zooms the whole scene.
void prepare(Ctx& c, const Clip& clip, double& zoom, QPointF& pan) {
  double az = clip.az, el = clip.el;
  zoom = 1;
  pan = {};
  for (const Item& it : clip.items) {
    if (it.el == "camera") {
      const QJsonObject o = evaluate(it, c.t);
      az = o.value("az").toDouble(az);
      el = o.value("el").toDouble(el);
      zoom = o.value("zoom").toDouble(1);
      // The pan makes room for the screen chrome (cards, chips), which flips sides in right-to-left languages.
      pan = QPointF(vec(o.value("center")).x() * (c.rtl ? -1 : 1), vec(o.value("center")).y());
    } else if (it.el == "cursor" && !c.hasCursor) {
      const QJsonObject o = evaluate(it, c.t);
      c.cursor = xform(o).apply(vec(o.value("pos")));
      c.hasCursor = true;
    }
  }
  if (c.iso) c.camera(az, el);
}

// The view-unit box of everything the clip draws over its whole time, padded: the framing of clips without "extent".
QRectF fit(Clip& clip, const Tokens& tokens) {
  if (!clip.extent.isNull() || clip.fitted) return clip.extent;
  clip.fitted = true;
  Bounds b;
  QImage scratch(4, 4, QImage::Format_ARGB32_Premultiplied);
  QPainter p(&scratch);
  for (int i = 0; i <= 12; ++i) {
    Ctx c;
    c.p = &p;
    c.tk = &tokens;
    c.full = c.scene = QRectF(0, 0, 4, 4);
    c.iso = clip.iso;
    c.t = clip.duration * i / 12;
    c.record = &b;
    double zoom;
    QPointF pan;
    prepare(c, clip, zoom, pan);
    c.center = QPointF(2, -2);
    paintScene(c, clip);
  }
  if (!b.valid()) return clip.extent = QRectF(-30, -15, 60, 30);
  QRectF r(QPointF(b.x0, b.y0), QPointF(b.x1, b.y1));
  r = r.adjusted(-r.width() * clip.pad - 1, -r.height() * clip.pad - 1, r.width() * clip.pad + 1, r.height() * clip.pad + 1);
  return clip.extent = r;
}
}  // namespace

namespace clips {

void load(const QString& path) {
  Library& l = lib();
  l = Library{};
  l.loaded = true;
  help::language();  // the help area's translations (captions) are installed with the help
  QList<QJsonObject> files;
  if (path.isEmpty()) files << readJson(":/help/clips.json", &l.problems) << readJson(QCoreApplication::applicationDirPath() + "/help/clips.json", &l.problems);
  else files << readJson(path, &l.problems);
  QJsonObject templates;
  for (const QJsonObject& f : files) {
    const QJsonObject t = f.value("templates").toObject();
    for (auto it = t.begin(); it != t.end(); ++it) templates.insert(it.key(), it.value());
  }
  for (const QJsonObject& f : files)
    for (const QJsonValue& v : f.value("clips").toArray()) parseClip(v.toObject(), templates, l);
}

QStringList ids() {
  ensureLoaded();
  return lib().order;
}
bool has(const QString& id) {
  ensureLoaded();
  return lib().clips.contains(id);
}
QStringList problems() {
  ensureLoaded();
  return lib().problems;
}
double duration(const QString& id) {
  ensureLoaded();
  return lib().clips.value(id).duration;
}
double stillTime(const QString& id) {
  ensureLoaded();
  return lib().clips.value(id).still;
}
QList<Step> steps(const QString& id) {
  ensureLoaded();
  return lib().clips.value(id).steps;
}
int stepAt(const QString& id, double t) {
  const QList<Step> s = steps(id);
  int out = 0;
  for (int i = 0; i < s.size(); ++i) if (t >= s[i].from - 1e-9) out = i;
  return out;
}
QStringList texts(const QString& id) {
  ensureLoaded();
  return lib().clips.value(id).texts;
}

void paint(QPainter& p, const QRectF& r, const QString& id, double t, const Options& o) {
  ensureLoaded();
  const Tokens& tokens = o.tokens ? *o.tokens : theme::current();
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.setLayoutDirection(o.rtl ? Qt::RightToLeft : Qt::LeftToRight);
  const double u = std::min(r.width() / 288.0, r.height() / 162.0);
  QPainterPath round;
  round.addRoundedRect(r, 4 * u, 4 * u);
  p.setClipPath(round, Qt::IntersectClip);
  p.fillRect(r, tokens.vp);
  auto it = lib().clips.find(id);
  if (it == lib().clips.end()) return p.restore();
  Clip& clip = *it;
  Ctx c;
  c.p = &p;
  c.tk = &tokens;
  c.full = r;
  c.u = u;
  c.t = std::clamp(t, 0.0, clip.duration);
  c.rtl = o.rtl;
  c.still = o.still;
  c.iso = clip.iso;
  const double bar = o.caption && !clip.steps.isEmpty() ? kCaption * u : 0;
  c.scene = r.adjusted(0, 0, 0, -bar);
  const QRectF ext = fit(clip, tokens);
  double zoom;
  QPointF pan;
  prepare(c, clip, zoom, pan);
  c.s = std::min(c.scene.width() / ext.width(), c.scene.height() / ext.height()) * zoom;
  c.center = ext.center() + pan;
  p.save();
  p.setClipRect(c.scene, Qt::IntersectClip);
  paintScene(c, clip);
  p.restore();
  if (bar > 0) captionBar(c, clip, QRectF(r.left(), r.bottom() - bar, r.width(), bar));
  p.restore();
}

QImage frame(const QString& id, double t, QSize size, qreal dpr, const Options& o) {
  QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
  img.setDevicePixelRatio(dpr);
  img.fill(Qt::transparent);
  QPainter p(&img);
  paint(p, QRectF(QPointF(0, 0), QSizeF(size)), id, t, o);
  return img;
}

bool animations() {
  bool on = true;
#ifdef Q_OS_WIN
  BOOL enabled = TRUE;
  if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) on = enabled;
#endif
  return QSettings().value("ui/tipAnimate", on).toBool();
}

}  // namespace clips

ClipView::ClipView(const QString& clip, QWidget* parent) : QWidget(parent) {
  setObjectName("clipView");
  m_timer.setInterval(33);
  connect(&m_timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
  setClip(clip);
}

void ClipView::setClip(const QString& id) {
  m_clip = id;
  m_step = -1;
  m_clock.restart();
  sync();
  update();
}

void ClipView::setStep(int step) {
  if (step == m_step) return;
  m_step = step;
  m_clock.restart();
  update();
}

void ClipView::sync() {
  const bool run = isVisible() && clips::has(m_clip) && !still();
  if (run && !m_timer.isActive()) {
    m_clock.restart();
    m_timer.start();
  } else if (!run) {
    m_timer.stop();
  }
}

void ClipView::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  sync();
}

void ClipView::hideEvent(QHideEvent* e) {
  QWidget::hideEvent(e);
  m_timer.stop();
}

// The frame shown now: the segment plays, holds its last frame, then fades (`fade` 0..1) back to its first.
double ClipView::phase(double& first, double& fade) const {
  const QList<clips::Step> steps = clips::steps(m_clip);
  const bool one = m_step >= 0 && m_step < steps.size();
  const double from = one ? steps[m_step].from : 0, to = one ? steps[m_step].to : clips::duration(m_clip), len = std::max(0.01, to - from);
  first = from;
  fade = 0;
  if (still()) return clips::stillTime(m_clip);
  const double e = std::fmod(m_clock.isValid() ? m_clock.elapsed() / 1000.0 : 0, len + kHold + kFade);
  if (e < len) return from + e;
  if (e >= len + kHold) fade = std::clamp((e - len - kHold) / kFade, 0.0, 1.0);
  return to;
}

double ClipView::time() const {
  double first, fade;
  return phase(first, fade);
}

void ClipView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  clips::Options o;
  o.rtl = layoutDirection() == Qt::RightToLeft;
  o.still = still();
  double first, fade;
  clips::paint(p, rect(), m_clip, phase(first, fade), o);
  if (fade <= 0) return;
  p.setOpacity(fade);
  p.drawImage(rect(), clips::frame(m_clip, first, size(), devicePixelRatioF(), o));
}
