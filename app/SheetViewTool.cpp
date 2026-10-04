#include "SheetViewTool.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QRegularExpression>

#include <cmath>

#include "AppDocument.hpp"
#include "SheetValueCard.hpp"
#include "Theme.hpp"

using opad::drawing::Display;
using opad::drawing::Vec2;

namespace {
Vec2 sub(Vec2 a, Vec2 b) { return {a[0] - b[0], a[1] - b[1]}; }
Vec2 add(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }
Vec2 mul(Vec2 a, double s) { return {a[0] * s, a[1] * s}; }
double dot(Vec2 a, Vec2 b) { return a[0] * b[0] + a[1] * b[1]; }
double len(Vec2 a) { return std::hypot(a[0], a[1]); }
Vec2 unit(Vec2 a) {
  const double l = len(a);
  return l > 1e-12 ? mul(a, 1 / l) : Vec2{1, 0};
}
Vec2 left(Vec2 a) { return {-a[1], a[0]}; }
double r4(double v) { return std::round(v * 1e4) / 1e4; }
opad::json js(Vec2 v) { return opad::json::array({r4(v[0]), r4(v[1])}); }
}  // namespace

SheetViewTool::SheetViewTool(AppDocument* doc, SheetCanvas* canvas, QObject* parent) : QObject(parent), m_doc(doc), m_canvas(canvas) {
  canvas->addInteraction(this);
  m_card = new SheetValueCard(canvas->viewport());
}

SheetViewTool::~SheetViewTool() {
  ++*m_generation;
  if (m_canvas) m_canvas->removeInteraction(this);
}

QString SheetViewTool::title(Tool tool) {
  switch (tool) {
    case Tool::Section: return tr("Section view");
    case Tool::Detail: return tr("Detail view");
    case Tool::Auxiliary: return tr("Auxiliary view");
    case Tool::Crop: return tr("Crop view");
    case Tool::Break: return tr("Break view");
    case Tool::Uncut: return tr("Bodies left uncut");
    case Tool::Breakout: return tr("Broken-out section");
    default: return {};
  }
}

bool SheetViewTool::start(Tool tool, const std::string& view) {
  cancel();
  if (tool == Tool::None || !m_canvas) return false;
  const opad::SheetView* v = m_doc->scene.sheet_view(view);
  const opad::drawing::ViewFrame* f = m_canvas->frame(view);
  if (!v || v->sheet != m_canvas->sheet() || !v->error.empty() || !f || !f->error.empty()) {
    emit message(tr("Select a view that is drawn on the sheet first."));
    return false;
  }
  if ((tool == Tool::Crop || tool == Tool::Break) && v->kind == "detail") {
    emit message(tr("A detail view is cropped by its circle already."));
    return false;
  }
  if (tool == Tool::Breakout && v->kind != "base" && v->kind != "projected" && v->kind != "auxiliary") {
    emit message(tr("A broken-out section goes on a base, projected or auxiliary view."));
    return false;
  }
  if (tool == Tool::Uncut && v->kind != "section" && !v->def.contains("breakouts")) {
    emit message(tr("Bodies are left uncut in a section view."));
    return false;
  }
  m_tool = tool;
  m_view = view;
  m_stage = Stage::Pick;
  promptForStage();
  updatePreview();
  m_canvas->setFocus();
  emit toolChanged();
  return true;
}

void SheetViewTool::cancel() {
  const bool was = m_tool != Tool::None;
  ++*m_generation;
  m_tool = Tool::None;
  m_stage = Stage::Pick;
  m_points.clear();
  m_extents.clear();
  m_measuring = m_pressed = m_haveDepths = false;
  m_ghost = QRectF();
  clearInputs();
  if (m_canvas && was) {
    m_canvas->setPreview(nullptr);
    m_canvas->setGhost(QRectF());
  }
  if (was) setPrompt(QString());
  if (was) emit toolChanged();
}

// ---------------------------------------------------------------- coordinates
const opad::drawing::ViewFrame* SheetViewTool::frame() const { return m_canvas ? m_canvas->frame(m_view) : nullptr; }

Vec2 SheetViewTool::toView(const QPointF& scene) const {
  const auto* f = frame();
  if (!f || !m_canvas) return {0, 0};
  const Vec2 p = m_canvas->toPaper(scene);
  return f->unfold({f->centre[0] + (p[0] - f->at[0]) / f->scale, f->centre[1] + (p[1] - f->at[1]) / f->scale});
}

QPointF SheetViewTool::toScene(Vec2 v) const {
  const auto* f = frame();
  if (!f || !m_canvas) return {};
  const Vec2 l = f->local(v);
  return m_canvas->toScene({f->at[0] + l[0], f->at[1] + l[1]});
}

QPointF SheetViewTool::snapped(const QPointF& scene) const {
  if (!m_canvas) return scene;
  const auto s = m_canvas->snapAt(scene);
  return s ? m_canvas->toScene(s->at) : scene;
}

double SheetViewTool::detailScale() const {  // the first standard scale at least twice the parent's
  const auto* f = frame();
  const double want = 2 * (f ? f->scale : 1);
  const auto& all = opad::drawing::standard_scales();
  for (auto it = all.rbegin(); it != all.rend(); ++it)
    if (*it >= want * (1 - 1e-9)) return *it;
  return all.front();
}

// ---------------------------------------------------------------- stages
void SheetViewTool::setPrompt(const QString& text) {
  m_prompt = text;
  if (m_canvas) m_canvas->setPrompt(text);
}

void SheetViewTool::promptForStage() {
  const QString esc = tr("Esc steps back");
  QString text;
  switch (m_tool) {
    case Tool::Section:
      text = m_stage == Stage::Place ? tr("Move to the side the section goes and click")
             : m_points.size() < 2   ? tr("Click the cutting line's points on the view: two for a full section, more for an offset, half or aligned section")
                                     : tr("Click more points, or press Enter to end the cutting line");
      break;
    case Tool::Detail:
      text = m_stage == Stage::Pick ? tr("Click the centre of the detail") : m_stage == Stage::Size ? tr("Click to set the detail's radius") : tr("Click where the detail view goes");
      break;
    case Tool::Auxiliary:
      text = m_stage == Stage::Pick ? tr("Click a straight edge of the view: the auxiliary view looks square to it") : tr("Move to the side the view goes and click");
      break;
    case Tool::Crop: text = m_stage == Stage::Pick ? tr("Drag a box around the part of the view to keep") : tr("Click the box's other corner"); break;
    case Tool::Break: text = m_stage == Stage::Pick ? tr("Click where the break starts") : tr("Click where it ends: the band between is taken out"); break;
    case Tool::Uncut: text = tr("Click a body in the section to draw it whole (shafts, fasteners); click it again to cut it"); break;
    case Tool::Breakout:
      text = m_stage == Stage::Depth ? tr("Click a point in a view beside it for the depth (the cut goes through it), or press Enter for the part's middle")
             : m_points.size() < 3   ? tr("Click points round what to open up: a smooth closed curve goes through them")
                                     : tr("Click more points, or press Enter to close the outline");
      break;
    default: break;
  }
  if (m_measuring) text = tr("Measuring the view…");
  else if (!inputKeys().empty()) text += QString::fromUtf8(" · ") + tr("or type the values, Tab to the next, Enter");
  setPrompt(text.isEmpty() ? text : text + QString::fromUtf8(" · ") + esc);
  syncInputs();
}

opad::json SheetViewTool::probe(int side) const {
  opad::json r = {{"op", "sheet_view"}, {"sheet", m_canvas ? m_canvas->sheet() : std::string()}, {"parent", m_view}};
  if (m_tool == Tool::Section) {
    opad::json cut = opad::json::array();
    for (const auto& p : m_points) cut.push_back(js(p));
    r["kind"] = "section";
    r["cut"] = cut;
    if (side < 0) r["flip"] = true;
    if (opad::drawing::inclined_cut(m_points)) r["aligned"] = true;  // its inclined segments revolved onto the first one's line
  } else if (m_tool == Tool::Auxiliary) {
    const Vec2 d = mul(left(m_edge), side);
    r["kind"] = "auxiliary";
    r["angle"] = r4(std::atan2(d[1], d[0]) * 180 / M_PI);
  }
  return r;
}

void SheetViewTool::measure() {
  if (!m_canvas) return;
  m_measuring = true;
  promptForStage();
  const int generation = ++*m_generation;
  auto out = std::make_shared<std::map<int, Extent>>();
  const opad::json probes[2] = {probe(1), probe(-1)};
  QPointer<SheetViewTool> self(this);
  auto alive = m_generation;
  m_canvas->read(
      tr("Measuring the view"),
      [out, p0 = probes[0], p1 = probes[1]](const opad::Document& doc, const opad::Scene& scene, Progress) {
        for (const auto& [side, def] : {std::pair{1, p0}, std::pair{-1, p1}}) {
          opad::SheetView v;
          v.id = "probe";
          v.sheet = def["sheet"].get<std::string>();
          v.parent = def["parent"].get<std::string>();
          v.kind = def["kind"].get<std::string>();
          v.def = def;
          const auto e = opad::drawing::view_extent(doc, scene, opad::drawing::view_spec(scene, v));
          (*out)[side] = {e[2] - e[0], e[3] - e[1], {(e[0] + e[2]) / 2, (e[1] + e[3]) / 2}};
        }
      },
      [self, alive, generation, out](bool ok, const QString& error) {
        if (!self || *alive != generation) return;
        self->m_measuring = false;
        if (!ok) {
          if (error != "cancelled") emit self->message(error);
          self->back();
          return;
        }
        self->m_extents = *out;
        self->m_stage = Stage::Place;
        self->promptForStage();
        self->place(self->m_mouse);
      });
}

void SheetViewTool::place(const QPointF& scene) {
  const auto* f = frame();
  if (!f || !m_canvas || m_stage != Stage::Place) return;
  const Vec2 m = m_canvas->toPaper(scene);
  if (m_tool == Tool::Detail) {
    const double size = 2 * m_radius * scaleNow();
    const Vec2 c{std::round(m[0]), std::round(m[1])};
    m_ghost = QRectF(m_canvas->toScene(c) - QPointF(size / 2, size / 2), QSizeF(size, size));
    m_canvas->setGhost(m_ghost, tr("Detail %1").arg(scaleLabel()));
    updatePreview();
    return;
  }
  // Section and auxiliary views: on the side the pointer is, lined up across the way they go (as layout() places them).
  Vec2 n;
  if (m_tool == Tool::Section) {
    n = left(unit(sub(m_points[1], m_points[0])));
    m_side = dot(sub(toView(scene), m_points[0]), n) >= 0 ? 1 : -1;
  } else {
    n = left(m_edge);
    const Vec2 at = m_canvas->toPaper(toScene(m_points[0]));
    m_side = dot(sub(m, at), n) >= 0 ? 1 : -1;
  }
  if (!m_extents.count(m_side)) return;
  const Extent& e = m_extents[m_side];
  const Vec2 d = mul(n, m_side), a{-d[1], d[0]};
  const double w = e.w * f->scale, h = e.h * f->scale;
  double reach = 0;
  for (int k = 0; k < 4; ++k) reach = std::max(reach, dot({(k & 1 ? f->box[2] : f->box[0]) - f->at[0], (k & 2 ? f->box[3] : f->box[1]) - f->at[1]}, d));
  const double back = (std::fabs(d[0]) * w + std::fabs(d[1]) * h) / 2, from = dot(f->at, d) + reach + back;
  const auto gap = typed("gap");  // typed: that far from the parent, on the pointer's side
  const double t = gap ? from + *gap : std::max(dot(m, d), from + 2);
  const double s = dot(f->at, a) + f->scale * dot(sub(e.centre, f->centre), a);
  const Vec2 c = add(mul(a, s), mul(d, t));
  m_gap = gap ? *gap : std::round((t - from) * 10) / 10;
  m_ghost = QRectF(m_canvas->toScene(c) - QPointF(w / 2, h / 2), QSizeF(w, h));
  m_canvas->setGhost(m_ghost, title(m_tool));
  updatePreview();
}

QString SheetViewTool::scaleLabel() const { return QString::fromStdString(opad::drawing::scale_text(scaleNow())); }

// ---------------------------------------------------------------- the value card
double SheetViewTool::scaleNow() const { return typed("scale").value_or(detailScale()); }

std::optional<double> SheetViewTool::typed(const std::string& key) const {
  for (const auto& in : m_inputs) {
    if (in.key != key || in.typed.isEmpty()) continue;
    if (key == "scale") {  // 5 or 5:1 (or 1:2, 0.5)
      try {
        const double s = in.typed.contains(':') ? opad::drawing::parse_scale(in.typed.toStdString()) : in.typed.toDouble();
        if (s > 0) return s;
      } catch (const std::exception&) {
      }
      return std::nullopt;
    }
    bool ok = false;
    const double v = in.typed.toDouble(&ok);
    if (ok && (v > 0 || ((key == "gap" || key == "depth") && v >= 0))) return v;
  }
  return std::nullopt;
}

std::vector<std::string> SheetViewTool::inputKeys() const {
  using Keys = std::vector<std::string>;
  switch (m_tool) {
    case Tool::Section:
    case Tool::Auxiliary: return m_stage == Stage::Place ? Keys{"gap"} : Keys{};
    case Tool::Detail: return m_stage == Stage::Size ? Keys{"radius", "scale"} : m_stage == Stage::Place ? Keys{"scale"} : Keys{};
    case Tool::Crop: return m_stage == Stage::Size ? Keys{"width", "height"} : Keys{};
    case Tool::Break: return m_stage == Stage::Size ? Keys{"length"} : Keys{};
    case Tool::Breakout: return m_stage == Stage::Depth ? Keys{"depth"} : Keys{};
    default: return {};
  }
}

std::vector<std::string> SheetViewTool::inputs() const {
  std::vector<std::string> out;
  for (const auto& in : m_inputs) out.push_back(in.key);
  return out;
}

bool SheetViewTool::cardShown() const { return m_card && m_card->isVisibleTo(m_card->parentWidget()); }

QString SheetViewTool::inputText(const std::string& key) const {
  for (const auto& in : m_inputs)
    if (in.key == key && !in.typed.isEmpty()) return in.typed;
  const auto mm = [](double v) { return QString::number(std::round(v * 10) / 10, 'f', 1); };
  if (key == "gap") return mm(m_gap);
  if (key == "scale") return QString::fromStdString(opad::drawing::scale_text(scaleNow()));
  if (key == "radius") return mm(m_stage == Stage::Size && !m_points.empty() ? len(sub(toView(m_mouse), m_points[0])) : m_radius);
  if (key == "depth") {
    if (!m_haveDepths) return {};
    const auto at = depthAt(m_mouse);
    return mm(m_depths[1] - (at ? *at : (m_depths[0] + m_depths[1]) / 2));
  }
  if (m_points.empty()) return {};
  const Vec2 d = sub(sized(m_mouse), m_points[0]);
  if (key == "width") return mm(std::fabs(d[0]));
  if (key == "height") return mm(std::fabs(d[1]));
  if (key == "length") return mm(std::max(std::fabs(d[0]), std::fabs(d[1])));
  return {};
}

Vec2 SheetViewTool::sized(const QPointF& scene) const {
  if (m_points.empty()) return toView(scene);
  const Vec2 a = m_points[0], p = toView(snapped(scene)), d = sub(p, a);
  const auto sign = [](double v) { return v < 0 ? -1.0 : 1.0; };
  if (m_tool == Tool::Crop) {
    const auto w = typed("width"), h = typed("height");
    return {w ? a[0] + sign(d[0]) * *w : p[0], h ? a[1] + sign(d[1]) * *h : p[1]};
  }
  if (m_tool == Tool::Break) {  // along the longer way: the length typed or the pointer's
    const size_t axis = std::fabs(d[0]) >= std::fabs(d[1]) ? 0 : 1;
    Vec2 b = a;
    b[axis] += sign(d[axis]) * typed("length").value_or(std::fabs(d[axis]));
    return b;
  }
  return p;
}

void SheetViewTool::syncInputs() {
  const auto keys = inputKeys();
  bool same = keys.size() == m_inputs.size();
  for (size_t i = 0; same && i < keys.size(); ++i) same = keys[i] == m_inputs[i].key;
  if (!same) {  // a stage with other fields: what was typed for the same field stays (a detail's scale)
    std::vector<Input> next;
    for (const auto& k : keys) {
      Input in{k, {}, {}};
      for (const auto& old : m_inputs)
        if (old.key == k) in = old;
      in.label = k == "gap" ? tr("Gap") : k == "radius" ? tr("Radius") : k == "scale" ? tr("View scale") : k == "width" ? tr("Width") : k == "height" ? tr("Height")
                 : k == "length" ? tr("Length") : tr("Depth");
      next.push_back(in);
    }
    m_inputs = std::move(next);
    m_focus = 0;
  }
  if (!m_card || !m_canvas) return;
  if (m_inputs.empty() || m_tool == Tool::None || m_measuring) return m_card->hide();
  std::vector<SheetValueCard::Cell> cells;
  for (size_t i = 0; i < m_inputs.size(); ++i) cells.push_back({m_inputs[i].label, inputText(m_inputs[i].key), !m_inputs[i].typed.isEmpty(), i == m_focus});
  m_card->set(std::move(cells));
  const QRect area = m_canvas->viewport()->rect();
  QPoint at = m_canvas->mapFromScene(m_mouse) + QPoint(18, 22);
  at.setX(std::clamp(at.x(), 4, std::max(4, area.width() - m_card->width() - 4)));
  at.setY(std::clamp(at.y(), 4, std::max(4, area.height() - m_card->height() - 4)));
  m_card->move(at);
  m_card->show();
  m_card->raise();
}

void SheetViewTool::clearInputs() {
  m_inputs.clear();
  m_focus = 0;
  if (m_card) m_card->hide();
}

bool SheetViewTool::inputKey(QKeyEvent* e) {
  if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
  const int key = e->key();  // the keypad's digits come with KeypadModifier, the same keys
  const bool digit = key >= Qt::Key_0 && key <= Qt::Key_9;
  if (digit || key == Qt::Key_Period || key == Qt::Key_Comma || key == Qt::Key_Colon || key == Qt::Key_Minus || key == Qt::Key_Plus) {
    if (m_inputs.empty() || key == Qt::Key_Minus || key == Qt::Key_Plus) return true;  // nothing takes it (sizes have no sign): still not a shortcut
    Input& in = m_inputs[m_focus];
    const QString next = in.typed + (digit ? QChar('0' + (key - Qt::Key_0)) : key == Qt::Key_Colon ? QChar(':') : QChar('.'));
    static const QRegularExpression size("^\\d*\\.?\\d*$"), ratio("^\\d*\\.?\\d*(:\\d*\\.?\\d*)?$");
    if (!(in.key == "scale" ? ratio : size).match(next).hasMatch()) return true;
    in.typed = next;
  } else if (key == Qt::Key_Backspace) {
    if (m_inputs.empty() || m_inputs[m_focus].typed.isEmpty()) return false;  // nothing typed there: steps back
    m_inputs[m_focus].typed.chop(1);
  } else if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
    if (m_inputs.empty()) return true;
    const bool back = key == Qt::Key_Backtab || (e->modifiers() & Qt::ShiftModifier);
    m_focus = (m_focus + (back ? m_inputs.size() - 1 : 1)) % m_inputs.size();
  } else if (key == Qt::Key_Escape && std::any_of(m_inputs.begin(), m_inputs.end(), [](const Input& in) { return !in.typed.isEmpty(); })) {
    for (auto& in : m_inputs) in.typed.clear();  // typed values first (from the first field again), then the stage
    m_focus = 0;
  } else {
    return false;
  }
  if (m_stage == Stage::Place) place(m_mouse);
  else updatePreview();
  return true;
}

// ---------------------------------------------------------------- broken-out sections
std::vector<Vec2> SheetViewTool::outline() const {
  std::vector<Vec2> pts = m_points;
  if (m_stage == Stage::Pick && m_canvas) pts.push_back(toView(m_mouse));
  try {
    return opad::drawing::breakout_outline(pts, 0.02);
  } catch (const std::exception&) {
    return {};
  }
}

std::optional<double> SheetViewTool::depthAt(const QPointF& scene) const {
  const auto* f = frame();
  const opad::Sheet* sheet = m_canvas ? m_doc->scene.sheet(m_canvas->sheet()) : nullptr;
  if (!f || !sheet) return std::nullopt;
  const Vec2 p = m_canvas->toPaper(snapped(scene));
  for (const auto& id : sheet->views) {
    const opad::drawing::ViewFrame* o = m_canvas->frame(id);
    if (!o || id == m_view || !o->error.empty() || p[0] < o->box[0] - 2 || p[0] > o->box[2] + 2 || p[1] < o->box[1] - 2 || p[1] > o->box[3] + 2) continue;
    const auto dot3 = [](const opad::Vec3& a, const opad::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    if (std::fabs(dot3(o->dir, f->dir)) > 1e-6) continue;  // it sees the depth along itself: not square to this view
    const Vec2 v = o->unfold({o->centre[0] + (p[0] - o->at[0]) / o->scale, o->centre[1] + (p[1] - o->at[1]) / o->scale});
    return dot3(f->dir, o->x) * v[0] + dot3(f->dir, o->y) * v[1];
  }
  return std::nullopt;
}

void SheetViewTool::measureDepth() {
  if (!m_canvas) return;
  m_haveDepths = false;
  const int generation = ++*m_generation;
  auto out = std::make_shared<std::array<double, 2>>();
  const std::string view = m_view;
  QPointer<SheetViewTool> self(this);
  auto alive = m_generation;
  m_canvas->read(
      tr("Measuring the view"),
      [out, view](const opad::Document& doc, const opad::Scene& scene, Progress) {
        if (const opad::SheetView* v = scene.sheet_view(view)) *out = opad::drawing::view_depth(doc, scene, opad::drawing::view_spec(scene, *v));
      },
      [self, alive, generation, out](bool ok, const QString& error) {
        if (!self || *alive != generation) return;
        if (!ok && error != "cancelled") emit self->message(error);
        self->m_depths = *out;
        self->m_haveDepths = ok;
      });
}

void SheetViewTool::commit() {
  const auto* f = frame();
  if (!f || !m_runner || !m_canvas) return;
  std::string command = "sheet_view";
  opad::json args = {{"sheet", m_canvas->sheet()}};
  if (m_tool == Tool::Section || m_tool == Tool::Auxiliary) {
    args.update(probe(m_side));
    args.erase("op");
    args["gap"] = m_gap;
  } else if (m_tool == Tool::Detail) {
    const Vec2 at = m_canvas->toPaper(m_ghost.center());
    args.update({{"kind", "detail"}, {"parent", m_view}, {"center", js(m_points[0])}, {"radius", r4(m_radius)}, {"scale", opad::drawing::scale_text(scaleNow())},
                 {"at", js(at)}});
  } else if (m_tool == Tool::Breakout) {
    const opad::SheetView* v = m_doc->scene.sheet_view(m_view);
    if (!v) return cancel();
    command = "sheet_edit";
    opad::json breakouts = v->def.value("breakouts", opad::json::array()), outline = opad::json::array();
    for (const auto& p : m_points) outline.push_back(js(p));
    breakouts.push_back({{"outline", outline}, {"depth", r4(m_depth)}});
    args = {{"target", m_view}, {"set", {{"breakouts", breakouts}}}};
  } else {
    const opad::SheetView* v = m_doc->scene.sheet_view(m_view);
    if (!v) return cancel();
    command = "sheet_edit";
    opad::json set;
    const Vec2 a = m_points[0], b = m_points[1];
    if (m_tool == Tool::Crop) {
      set["crop"] = {r4(std::min(a[0], b[0])), r4(std::min(a[1], b[1])), r4(std::max(a[0], b[0])), r4(std::max(a[1], b[1]))};
    } else {
      const int axis = std::fabs(b[0] - a[0]) >= std::fabs(b[1] - a[1]) ? 0 : 1;
      opad::json breaks = v->def.value("breaks", opad::json::array());
      breaks.push_back({{"axis", axis ? "y" : "x"}, {"from", r4(std::min(a[axis], b[axis]))}, {"to", r4(std::max(a[axis], b[axis]))}});
      set["breaks"] = breaks;
    }
    args = {{"target", m_view}, {"set", set}};
  }
  const std::string view = m_view;
  QPointer<SheetViewTool> self(this);
  cancel();
  m_runner(command, args, [self, command, view](const opad::json& out) {
    if (!self || out.is_null()) return;
    emit self->added(command == "sheet_view" ? out.value("id", "") : view);
  });
}

// ---------------------------------------------------------------- input
void SheetViewTool::clickAt(const QPointF& scene) {
  if (m_tool == Tool::None || m_measuring) return;
  m_mouse = scene;
  switch (m_tool) {
    case Tool::Section:
      if (m_stage == Stage::Place) return place(scene), commit();
      {
        Vec2 p = toView(snapped(scene));
        if (!m_points.empty()) {
          const Vec2 last = m_points.back(), d = sub(p, last);
          const double k = std::tan(6 * M_PI / 180);
          if (std::fabs(d[1]) <= k * std::fabs(d[0])) p[1] = last[1];        // level
          else if (std::fabs(d[0]) <= k * std::fabs(d[1])) p[0] = last[0];   // upright
          if (len(sub(p, last)) * frame()->scale < 0.2) return finish();     // the same point again (a double click): done
        }
        m_points.push_back(p);
      }
      break;
    case Tool::Detail:
      if (m_stage == Stage::Pick) {
        m_points = {toView(snapped(scene))};
        m_stage = Stage::Size;
      } else if (m_stage == Stage::Size) {
        const double r = typed("radius").value_or(len(sub(toView(scene), m_points[0])));
        if (r * frame()->scale < 1) return;
        m_radius = r;
        m_stage = Stage::Place;
        place(scene);
      } else {
        place(scene);
        return commit();
      }
      break;
    case Tool::Auxiliary:
      if (m_stage == Stage::Place) return place(scene), commit();
      {
        const auto pick = m_canvas ? m_canvas->pickAt(scene) : std::nullopt;
        if (!pick || !pick->line || pick->view != m_view || pick->curve.pts.size() < 2) {
          emit message(tr("Click a straight edge of the view."));
          return;
        }
        m_edge = unit(sub(pick->curve.pts.back(), pick->curve.pts.front()));
        m_points = {toView(m_canvas->toScene(pick->at))};
        measure();
      }
      break;
    case Tool::Uncut: {
      const auto [node, in] = m_canvas ? m_canvas->bodyAt(scene) : std::pair<std::string, std::string>{};
      const opad::SheetView* v = m_doc->scene.sheet_view(m_view);
      if (node.empty() || in != m_view || !v || !m_runner) {
        emit message(tr("Click a body in the section."));
        return;
      }
      opad::json whole = opad::json::array();
      bool was = false;
      for (const auto& n : v->def.value("whole", opad::json::array()))
        if (n == node) was = true;
        else whole.push_back(n);
      if (!was) whole.push_back(node);
      const std::string view = m_view;
      QPointer<SheetViewTool> self(this);
      m_runner("sheet_edit", {{"target", view}, {"set", {{"whole", whole.empty() ? opad::json() : whole}}}}, [self, view](const opad::json& out) {
        if (self && !out.is_null()) emit self->added(view);
      });
      return;  // the tool stays for the next body
    }
    case Tool::Breakout:
      if (m_stage == Stage::Depth) {
        if (const auto below = typed("depth"); below && m_haveDepths) {  // typed: that far below the part's front
          m_depth = m_depths[1] - *below;
          return commit();
        }
        const auto depth = depthAt(scene);
        if (!depth) {
          emit message(tr("Click a point in a view beside this one (square to it): the cut goes through it."));
          return;
        }
        m_depth = *depth;
        return commit();
      } else {
        const Vec2 p = toView(snapped(scene));
        if (!m_points.empty() && len(sub(p, m_points.back())) * frame()->scale < 0.2) return finish();  // a double click: closed
        m_points.push_back(p);
      }
      break;
    case Tool::Crop:
    case Tool::Break:
      if (m_stage == Stage::Pick) {
        m_points = {toView(snapped(scene))};
        m_stage = Stage::Size;
      } else {
        const Vec2 b = sized(scene), a = m_points[0];
        if (len(sub(b, a)) * frame()->scale < 1 || (m_tool == Tool::Crop && (std::fabs(b[0] - a[0]) * frame()->scale < 1 || std::fabs(b[1] - a[1]) * frame()->scale < 1))) return;
        m_points.push_back(b);
        return commit();
      }
      break;
    default: break;
  }
  promptForStage();
  updatePreview();
}

void SheetViewTool::moveTo(const QPointF& scene) {
  if (m_tool == Tool::None) return;
  m_mouse = scene;
  if (m_stage == Stage::Place) place(scene);
  else updatePreview();
}

void SheetViewTool::finish() {
  if (m_tool == Tool::Breakout) {
    if (m_stage == Stage::Pick) {
      try {
        opad::drawing::breakout_outline(m_points);
      } catch (const std::exception&) {
        emit message(tr("An outline needs three points at least."));
        return;
      }
      m_stage = Stage::Depth;
      measureDepth();
      promptForStage();
      updatePreview();
    } else if (m_haveDepths) {  // the depth typed below the part's front, else through its middle
      const auto below = typed("depth");
      m_depth = below ? m_depths[1] - *below : (m_depths[0] + m_depths[1]) / 2;
      commit();
    }
    return;
  }
  if (m_stage == Stage::Size && (m_tool == Tool::Detail || m_tool == Tool::Crop || m_tool == Tool::Break)) return clickAt(m_mouse);  // typed sizes
  if (m_tool == Tool::Section && m_stage == Stage::Pick && !m_measuring) {
    if (m_points.size() < 2) {
      emit message(tr("A cutting line needs two points at least."));
      return;
    }
    measure();
  } else if (m_stage == Stage::Place && !m_measuring) {
    place(m_mouse);
    commit();
  }
}

void SheetViewTool::back() {
  if (m_tool == Tool::None) return;
  ++*m_generation;  // a measure on its way is for what is taken back
  m_measuring = false;
  m_ghost = QRectF();
  if (m_canvas) m_canvas->setGhost(QRectF());
  if (m_stage == Stage::Place) {
    m_stage = m_tool == Tool::Detail ? Stage::Size : Stage::Pick;
    if (m_tool == Tool::Auxiliary) m_points.clear();
  } else if (m_stage == Stage::Size) {
    m_stage = Stage::Pick;
    m_points.clear();
  } else if (m_stage == Stage::Depth) {
    m_stage = Stage::Pick;
    m_haveDepths = false;
  } else if (!m_points.empty() && (m_tool == Tool::Section || m_tool == Tool::Breakout)) {
    m_points.pop_back();
  } else {
    return cancel();
  }
  promptForStage();
  updatePreview();
}

void SheetViewTool::updatePreview() {
  if (!m_canvas || m_tool == Tool::None) return;
  const auto* f = frame();
  if (!f) return;
  auto d = std::make_shared<Display>();
  const auto paper = [&](Vec2 v) { return m_canvas->toPaper(toScene(v)); };
  const Vec2 mouse = m_canvas->toPaper(m_mouse), mv = toView(m_mouse);
  using opad::drawing::kInk;
  using opad::drawing::LineType;
  const uint32_t ink = theme::current().sel.rgb() & 0xFFFFFFu;  // the selection colour: what the tool would add
  const int thin = d->layer({"Tool", ink, LineType::Continuous, 0.25});
  if (m_tool == Tool::Section) {
    std::vector<Vec2> pts;
    for (const auto& p : m_points) pts.push_back(paper(p));
    if (m_stage == Stage::Pick && !m_points.empty()) {
      Vec2 p = mv;
      const Vec2 last = m_points.back(), dd = sub(p, last);
      const double k = std::tan(6 * M_PI / 180);
      if (std::fabs(dd[1]) <= k * std::fabs(dd[0])) p[1] = last[1];
      else if (std::fabs(dd[0]) <= k * std::fabs(dd[1])) p[0] = last[0];
      pts.push_back(paper(p));
    }
    if (pts.size() >= 2) d->polyline(d->layer({"Section line", ink, LineType::Center, 0.35}), pts);
    for (const auto& p : pts) d->circle(thin, p, 0.8);
  } else if (m_tool == Tool::Detail && !m_points.empty()) {
    const Vec2 c = paper(m_points[0]);
    const auto r = typed("radius");
    d->circle(thin, c, m_stage == Stage::Size ? (r ? *r * f->scale : len(sub(mouse, c))) : m_radius * f->scale);
  } else if (m_tool == Tool::Auxiliary || m_tool == Tool::Uncut) {
    if (m_stage == Stage::Pick) {
      if (const auto pick = m_canvas->pickAt(m_mouse); pick && pick->line && pick->view == m_view)
        d->curve(d->layer({"Edge", ink, LineType::Continuous, 0.7}), pick->curve);
    } else if (!m_points.empty()) {
      const Vec2 at = paper(m_points[0]);
      d->line(d->layer({"Edge", ink, LineType::Continuous, 0.7}), sub(at, mul(m_edge, 10)), add(at, mul(m_edge, 10)));
    }
  } else if (m_tool == Tool::Breakout) {  // the closed curve through the points (and the pointer while picking)
    if (const auto c = outline(); !c.empty()) {
      std::vector<Vec2> pts;
      for (const auto& p : c) pts.push_back(paper(p));
      d->polyline(d->layer({"Outline", ink, LineType::Continuous, 0.35}), pts, true);
    } else if (!m_points.empty()) {
      d->polyline(thin, {paper(m_points.back()), mouse});
    }
    for (const auto& p : m_points) d->circle(thin, paper(p), 0.8);
    if (m_stage == Stage::Depth)  // where the cut would go through, on the view under the pointer
      if (const auto depth = depthAt(m_mouse); depth) d->circle(d->layer({"Depth", ink, LineType::Continuous, 0.5}), m_canvas->toPaper(snapped(m_mouse)), 1.2);
  } else if ((m_tool == Tool::Crop || m_tool == Tool::Break) && !m_points.empty()) {
    const Vec2 a = paper(m_points[0]), b = paper(sized(m_mouse));
    if (m_tool == Tool::Crop) {
      d->polyline(thin, {a, {b[0], a[1]}, b, {a[0], b[1]}}, true);
    } else {  // the band's two edges across the view
      const int axis = std::fabs(b[0] - a[0]) >= std::fabs(b[1] - a[1]) ? 0 : 1;
      for (const Vec2& p : {a, b})
        if (axis == 0) d->line(thin, {p[0], f->box[1] - 3}, {p[0], f->box[3] + 3});
        else d->line(thin, {f->box[0] - 3, p[1]}, {f->box[2] + 3, p[1]});
    }
  }
  m_canvas->setPreview(d->prims.empty() ? nullptr : d);
  syncInputs();
}

bool SheetViewTool::mousePress(QMouseEvent* e, const QPointF& scene) {
  if (m_tool == Tool::None) return false;
  if (e->button() == Qt::RightButton) {
    if ((m_tool == Tool::Section && m_stage == Stage::Pick && m_points.size() >= 2) || (m_tool == Tool::Breakout && m_stage == Stage::Pick && m_points.size() >= 3)) finish();
    else back();
    return true;
  }
  if (e->button() != Qt::LeftButton) return false;
  m_pressed = true;
  m_press = scene;
  clickAt(scene);
  return true;
}

bool SheetViewTool::mouseMove(QMouseEvent*, const QPointF& scene) {
  if (m_tool == Tool::None) return false;
  moveTo(scene);
  return true;
}

bool SheetViewTool::mouseRelease(QMouseEvent* e, const QPointF& scene) {
  if (m_tool == Tool::None) return false;
  // A box dragged out: the release is its other corner.
  if (m_pressed && e->button() == Qt::LeftButton && m_tool == Tool::Crop && m_stage == Stage::Size && m_canvas &&
      QLineF(m_press, scene).length() * m_canvas->pixelsPerMm() > 6)
    clickAt(scene);
  m_pressed = false;
  return true;
}

bool SheetViewTool::wantsKey(QKeyEvent* e) {
  if (m_tool == Tool::None) return false;
  const int k = e->key();
  const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
  return k == Qt::Key_Escape || k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Backspace ||  // bare digits are a value's, never a shortcut
         (plain && (k == Qt::Key_Tab || k == Qt::Key_Backtab || (k >= Qt::Key_0 && k <= Qt::Key_9) || k == Qt::Key_Period || k == Qt::Key_Comma || k == Qt::Key_Colon ||
                    k == Qt::Key_Minus || k == Qt::Key_Plus));
}

bool SheetViewTool::keyPress(QKeyEvent* e) {
  if (!wantsKey(e)) return false;
  if (inputKey(e)) return true;
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) finish();
  else if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Backspace) back();
  return true;
}
