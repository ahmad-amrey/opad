#include "TimelineWidget.hpp"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

#include "I18n.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "opad/drawing/sheet.hpp"

namespace {
QString shortId(const std::string& id) { return QString::fromStdString(id.substr(0, 8)); }
constexpr int kStrip = 128;  // where the markers start
constexpr int kGap = 8;      // between markers
}  // namespace

// Ops the timeline draws. Visibility toggles, notes and pinned measurements are view state with their own
// UI (browser eye, Annotations tab, measurement card) and would bury the structural history; their
// tombstones go with them.
bool timelineShows(const opad::Document& doc, const opad::Op& op) {
  if (op.type == "annotation" || op.type == "measurement") return false;
  if (!opad::Document::known_type(op.type)) return false;  // a newer build's op: nothing to show or edit here
  if (opad::drawing::is_drawing_op(op.type)) return false;  // sheets and part properties: the Drawings folder, Properties
  // The design history shows sketches and features; edits and the results they regenerate are how those
  // changed, not steps of their own, and parameters live in their dialog.
  if (op.type == "edit" || op.type == "regen" || op.type == "param") return false;
  if (op.type == "appearance" && op.data.contains("visible")) return false;
  if (op.type == "delete") {
    const opad::Op* t = doc.find_op(op.data.value("target", ""));
    return !t || timelineShows(doc, *t);
  }
  return true;
}

// The design history alone (UI-99): what makes and places geometry; names, colours, views, sections and units are left out.
bool designStep(const opad::Document& doc, const opad::Op& op) {
  if (op.type == "delete") {
    const opad::Op* t = doc.find_op(op.data.value("target", ""));
    return !t || designStep(doc, *t);
  }
  return op.type == "import" || op.type == "sketch" || op.type == "feature" || op.type == "transform" || op.type == "reparent";
}

QString opTypeIcon(const std::string& type) {
  if (type == "import") return "import";
  if (type == "rename") return "rename";
  if (type == "transform") return "move";
  if (type == "appearance") return "eye";
  if (type == "reparent") return "reparent";
  if (type == "annotation") return "annotate";
  if (type == "measurement") return "distance";
  if (type == "section") return "section";
  if (type == "view") return "home";
  if (type == "delete") return "delete";
  if (type == "sketch") return "sketch";
  if (type == "feature") return "box";
  return "dot";
}

// ---------------------------------------------------------------- TimelineWidget
TimelineWidget::TimelineWidget(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
    layoutMarkers();
    update();
  });
  setMouseTracking(true);
  setFixedHeight(48);
  setFocusPolicy(Qt::StrongFocus);
  m_scroll = new QScrollBar(Qt::Horizontal, this);
  m_scroll->setLayoutDirection(Qt::LeftToRight);
  m_scroll->setAccessibleName(tr("Timeline"));
  m_scroll->setSingleStep(26);
  connect(m_scroll, &QScrollBar::valueChanged, this, [this] { setHover(-1); QToolTip::hideText(); update(); });
  setAttribute(Qt::WA_Hover);
  connect(doc, &AppDocument::changed, this, &TimelineWidget::rebuild);
  rebuild();
}

void TimelineWidget::rebuild() {
  const bool atEnd = m_scroll->value() == m_scroll->maximum();
  m_deleted = std::set<std::string>(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end());
  m_unresolved.clear();
  for (const auto& u : m_doc->scene.unresolved) m_unresolved.insert(u.op_id);
  if (!m_current.empty() && !m_doc->doc.find_op(m_current)) m_current.clear();
  m_shown.clear();
  for (size_t i = 0; i < m_doc->doc.ops.size(); ++i)
    if (const opad::Op& op = m_doc->doc.ops[i]; timelineShows(m_doc->doc, op) && (!m_designOnly || designStep(m_doc->doc, op)) && !(m_hideDimmed && m_dimmed.count(op.id) && op.id != m_editing))
      m_shown.push_back(i);
  setHover(-1);
  m_dragging = false;
  layoutMarkers();
  updateScrollRange();
  if (atEnd) m_scroll->setValue(m_scroll->maximum());
  update();
}

std::vector<std::string> TimelineWidget::shownOps() const {
  std::vector<std::string> ids;
  for (size_t i : m_shown) ids.push_back(m_doc->doc.ops[i].id);
  return ids;
}

// Icons only: 18 px squares 26 px apart. With names: as wide as the name needs, up to 120 px of it.
void TimelineWidget::layoutMarkers() {
  m_left.clear();
  m_width.clear();
  const QFontMetrics metrics(theme::ui(11));
  int x = 0;
  for (size_t i : m_shown) {
    int w = 18;
    if (m_names) w += 4 + std::min(120, metrics.horizontalAdvance(label(m_doc->doc.ops[i]))) + 6;
    m_left.push_back(x);
    m_width.push_back(w);
    x += w + kGap;
  }
  m_extent = x;
}

void TimelineWidget::setShowNames(bool on) {
  if (on == m_names) return;
  m_names = on;
  layoutMarkers();
  updateScrollRange();
  ensureCurrentVisible();
  update();
}

void TimelineWidget::setDesignOnly(bool on) {
  if (on == m_designOnly) return;
  m_designOnly = on;
  rebuild();
  ensureCurrentVisible();
}

bool TimelineWidget::isUnresolved(const std::string& opId) const { return m_unresolved.count(opId) > 0; }

void TimelineWidget::updateScrollRange() {
  const int available = std::max(0, width() - 200);
  m_scroll->setGeometry(kStrip, 36, available, 12);
  m_scroll->setPageStep(available);
  m_scroll->setRange(0, std::max(0, m_extent + 4 - available));
  m_scroll->setVisible(m_scroll->maximum() > 0);
}

void TimelineWidget::ensureVisible(size_t i) {
  if (i >= m_shown.size()) return;
  const int x = m_left[i], w = m_width[i] + kGap;
  if (x < m_scroll->value()) m_scroll->setValue(x);
  else if (x + w > m_scroll->value() + m_scroll->pageStep()) m_scroll->setValue(x + w - m_scroll->pageStep());
}

void TimelineWidget::ensureCurrentVisible() {
  for (size_t i = 0; i < m_shown.size(); ++i)
    if (m_doc->doc.ops[m_shown[i]].id == m_current) return ensureVisible(i);
}

void TimelineWidget::resizeEvent(QResizeEvent*) { updateScrollRange(); ensureCurrentVisible(); }
void TimelineWidget::wheelEvent(QWheelEvent* e) {
  const auto pixel = e->pixelDelta(), angle = e->angleDelta();
  const int delta = !pixel.isNull() ? (pixel.x() ? pixel.x() : pixel.y()) : (angle.x() ? angle.x() : angle.y()) * 78 / 120;
  m_scroll->setValue(m_scroll->value() - delta); e->accept();
}

// Ctrl+C on the timeline is its own (the marker's op id), whatever the window's shortcuts say.
bool TimelineWidget::event(QEvent* e) {
  if (e->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent*>(e)->matches(QKeySequence::Copy) && !m_current.empty()) {
    e->accept();
    return true;
  }
  return QWidget::event(e);
}

void TimelineWidget::keyPressEvent(QKeyEvent* e) {
  if (e->matches(QKeySequence::Copy)) {
    if (m_current.empty()) return QWidget::keyPressEvent(e);
    QGuiApplication::clipboard()->setText(QString::fromStdString(m_current));
  } else if (e->key() == Qt::Key_Left) step(-1);
  else if (e->key() == Qt::Key_Right) step(1);
  else if (e->key() == Qt::Key_Home || e->key() == Qt::Key_End) {
    if (!m_shown.empty()) { setCurrentOp(m_doc->doc.ops[e->key() == Qt::Key_Home ? m_shown.front() : m_shown.back()].id); emit opClicked(m_current); }
  } else { QWidget::keyPressEvent(e); return; }
  e->accept();
}

QRect TimelineWidget::markerRect(int i) const {
  if (i < 0 || i >= int(m_left.size())) return {};
  return QRect(kStrip + m_left[size_t(i)] - m_scroll->value(), 13, m_width[size_t(i)], 18);
}

QRect TimelineWidget::markerAt(const std::string& id) const {
  for (size_t i = 0; i < m_shown.size(); ++i)
    if (m_doc->doc.ops[m_shown[i]].id == id) return markerRect(int(i));
  return {};
}

int TimelineWidget::indexAt(const QPoint& p) const {
  if (p.x() < kStrip || p.x() >= width() - 72 || p.y() < 10 || p.y() >= 34 || m_left.empty()) return -1;
  const int x = p.x() - kStrip + m_scroll->value() + 3;
  const int i = int(std::upper_bound(m_left.begin(), m_left.end(), x) - m_left.begin()) - 1;
  return i >= 0 && markerRect(i).adjusted(-3, -3, 3, 3).contains(p) ? i : -1;
}

void TimelineWidget::setHover(int i) {
  if (i == m_hover) return;
  m_hover = i;
  emit markerHovered(i >= 0 && size_t(i) < m_shown.size() ? m_doc->doc.ops[m_shown[size_t(i)]].id : std::string());
}

// Replay applies every op but tombstones, the tombstoned and the edits and regens folded into earlier ops (effective_ops).
std::string TimelineWidget::rollPoint(size_t opIndex) const {
  const auto& ops = m_doc->doc.ops;
  for (size_t i = opIndex; i < ops.size(); ++i)
    if (ops[i].type != "delete" && ops[i].type != "edit" && ops[i].type != "regen" && !m_deleted.count(ops[i].id)) return ops[i].id;
  return {};
}

size_t TimelineWidget::rollbackGap() const {
  const std::string& until = m_doc->rollback();
  if (until.empty()) return m_shown.size();
  const auto& ops = m_doc->doc.ops;
  size_t at = ops.size();
  for (size_t i = 0; i < ops.size(); ++i)
    if (ops[i].id == until) at = i;
  return size_t(std::lower_bound(m_shown.begin(), m_shown.end(), at) - m_shown.begin());
}

int TimelineWidget::gapX(size_t gap) const {
  if (m_shown.empty()) return kStrip;
  return gap < m_shown.size() ? markerRect(int(gap)).left() - kGap / 2 : markerRect(int(m_shown.size()) - 1).right() + 9;
}

QRect TimelineWidget::playhead() const {
  if (m_shown.empty()) return {};
  const int x = gapX(m_dragging ? m_dragGap : rollbackGap());
  return QRect(x - 4, 6, 9, 36);
}

std::string TimelineWidget::rollPointAfter(const std::string& id) const {
  const auto& ops = m_doc->doc.ops;
  for (size_t i = 0; i < ops.size(); ++i)
    if (ops[i].id == id) {
      // After it: before the next marker shown, so the hidden steps right after it (a visibility change) come along.
      const auto next = std::upper_bound(m_shown.begin(), m_shown.end(), i);
      return next == m_shown.end() ? std::string() : rollPoint(*next);
    }
  return {};
}

void TimelineWidget::setCurrentOp(const std::string& id) {
  m_current = id;
  ensureCurrentVisible();
  update();
}

void TimelineWidget::pulse(const std::string& id) {
  if (!m_pulseTimer) {
    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(50);
    connect(m_pulseTimer, &QTimer::timeout, this, [this] {
      if (++m_pulseTick >= 30) {  // 1.5 s
        m_pulseTimer->stop();
        m_pulse.clear();
      }
      update();
    });
  }
  m_pulse = m_doc->doc.find_op(id) ? id : std::string();
  m_pulseTick = 0;
  if (m_pulse.empty()) {
    m_pulseTimer->stop();
    return update();
  }
  for (size_t i = 0; i < m_shown.size(); ++i)  // into view, the current marker left as it is
    if (m_doc->doc.ops[m_shown[i]].id == id) {
      const int x = m_left[i];
      if (x < m_scroll->value() || x + m_width[i] + kGap > m_scroll->value() + m_scroll->pageStep()) m_scroll->setValue(x + m_width[i] / 2 - m_scroll->pageStep() / 2);
    }
  m_pulseTimer->start();
  update();
}

void TimelineWidget::setEditingOp(const std::string& id) {
  if (id == m_editing) return;
  m_editing = id;
  if (m_hideDimmed && !m_dimmed.empty()) rebuild();  // an edited op outside the active component is shown while edited
  if (!id.empty()) setCurrentOp(id);  // selected and scrolled into view
  else update();
}

void TimelineWidget::setDimmedOps(std::set<std::string> ops, bool hidden) {
  if (ops == m_dimmed && hidden == m_hideDimmed) return;
  const bool markers = hidden || m_hideDimmed;  // which markers there are changes
  m_dimmed = std::move(ops);
  m_hideDimmed = hidden;
  if (markers) rebuild();
  else update();
}

void TimelineWidget::setMarkedOps(std::map<std::string, QColor Tokens::*> marks, const QString& legend) {
  m_marks = std::move(marks);
  m_markLegend = legend.isEmpty() ? QStringLiteral("%1") : legend;
  update();
}

void TimelineWidget::step(int delta) {
  const auto& ops = m_doc->doc.ops;
  if (m_shown.empty()) return;
  int i = -1;
  for (size_t k = 0; k < m_shown.size(); ++k) if (ops[m_shown[k]].id == m_current) i = static_cast<int>(k);
  const int n = static_cast<int>(m_shown.size());
  i = i < 0 ? (delta > 0 ? 0 : n - 1) : std::clamp(i + delta, 0, n - 1);
  setCurrentOp(ops[m_shown[static_cast<size_t>(i)]].id);
  emit opClicked(m_current);
}

QString TimelineWidget::describe(const opad::Op& op) const {
  const opad::json& d = op.data;
  QString target = d.contains("target") && d["target"].is_string() ? m_doc->nodeName(d["target"].get<std::string>()) : QString();
  if (op.type == "import") return tr("Import %1").arg(QString::fromStdString(d.value("source", "")));
  if (op.type == "rename") return tr("Rename → %1").arg(QString::fromStdString(d.value("name", "")));
  if (op.type == "annotation") {
    try { return tr("Note on %1").arg(m_doc->nodeName(opad::Ref::from_json(d["anchor"]).body)); } catch (...) { return tr("Note"); }
  }
  if (op.type == "measurement") return tr("%1 measurement").arg(QString::fromStdString(d.value("kind", "")));
  if (op.type == "section") return tr("Section %1").arg(QString::fromStdString(d.value("name", "")));
  if (op.type == "view") return tr("View %1").arg(QString::fromStdString(d.value("name", "")));
  if (op.type == "delete") {
    const opad::Op* t = m_doc->doc.find_op(d.value("target", ""));
    return tr("Delete %1").arg(t ? QString::fromStdString(t->type) : shortId(d.value("target", "")));
  }
  if (op.type == "sketch" || op.type == "feature") {
    // The name an edit may have changed; the scene has it unless the timeline is rolled back past this op.
    if (const opad::SketchItem* s = m_doc->scene.sketch(op.id)) return QString::fromStdString(s->name);
    if (const opad::Feature* f = m_doc->scene.feature(op.id))
      return QString::fromStdString(f->name) + (f->suppressed ? tr(" (suppressed)") : QString()) +
             (f->suppress_if.empty() ? QString() : tr(" — suppressed while %1").arg(QString::fromStdString(f->suppress_if))) +  // gap log #9
             (f->error.empty() ? QString() : QString::fromUtf8(" — ") + i18n::t(QString::fromStdString(f->error)));
    return QString::fromStdString(d.value("name", op.type));
  }
  if (op.type == "appearance") return tr("Appearance %1").arg(target);
  if (op.type == "transform") return tr("Transform %1").arg(target);
  if (op.type == "reparent") return tr("Reparent %1").arg(target);
  return QString::fromStdString(op.type);
}

QString TimelineWidget::label(const opad::Op& op) const {
  const opad::json& d = op.data;
  if (op.type == "sketch" || op.type == "feature") {
    if (const opad::SketchItem* s = m_doc->scene.sketch(op.id)) return QString::fromStdString(s->name);
    if (const opad::Feature* f = m_doc->scene.feature(op.id)) return QString::fromStdString(f->name);
    std::string name = d.value("name", op.type);  // rolled back past it, or tombstoned: its name as last edited
    for (const auto& o : m_doc->doc.ops)
      if (o.type == "edit" && o.data.value("target", "") == op.id && o.data.contains("set") && o.data["set"].contains("name") && o.data["set"]["name"].is_string())
        name = o.data["set"]["name"].get<std::string>();
    return QString::fromStdString(name);
  }
  if (op.type == "import") {
    const QString source = QString::fromStdString(d.value("source", ""));
    const qsizetype slash = std::max(source.lastIndexOf('/'), source.lastIndexOf('\\'));
    if (!source.isEmpty()) return source.mid(slash + 1);
    const opad::json nodes = d.value("nodes", opad::json::array());  // New component: its name
    return nodes.size() == 1 && nodes[0].is_object() ? QString::fromStdString(nodes[0].value("name", "")) : tr("Import");
  }
  if (op.type == "delete") {
    const opad::Op* t = m_doc->doc.find_op(d.value("target", ""));
    return t ? tr("Delete %1").arg(label(*t)) : describe(op);
  }
  return describe(op);
}

void TimelineWidget::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setLayoutDirection(Qt::LeftToRight);  // the strip's geometry is fixed; time runs left to right in every language
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), t.bg2);
  p.setPen(QPen(t.line, 1));
  p.drawLine(0, 0, width(), 0);
  const auto& ops = m_doc->doc.ops;
  p.setFont(theme::ui(13, QFont::Medium));
  p.setPen(t.fg2);
  p.drawText(QRect(12, 6, 100, 16), Qt::AlignVCenter | Qt::AlignLeft, tr("Timeline"));
  p.setFont(theme::mono(11));
  p.setPen(m_doc->rolledBack() ? t.candidate : t.fg3);
  size_t tomb = 0;
  for (size_t i : m_shown) tomb += m_deleted.count(ops[i].id);
  QString count = m_doc->rolledBack() ? tr("rolled back") : tr("%1 ops").arg(m_shown.size());
  if (tomb > 0 && !m_doc->rolledBack()) count += tr(" · %1 tomb").arg(tomb);
  p.drawText(QRect(12, 24, 100, 16), Qt::AlignVCenter | Qt::AlignLeft, count);
  p.setPen(QPen(t.line, 1));
  p.drawLine(112, 8, 112, 40);
  p.drawLine(width() - 72, 8, width() - 72, 40);
  if (m_shown.empty() || !m_doc->hasDocument) {
    p.setFont(theme::ui(12));
    p.setPen(t.fg3);
    p.drawText(QRect(kStrip, 0, width() - 200, height()), Qt::AlignVCenter | Qt::AlignLeft,
               m_designOnly && !ops.empty() ? tr("No design steps yet. Show every step from the timeline's menu.") : tr("One marker per operation. Import a file to start the log."));
  }
  const qreal dpr = devicePixelRatioF();
  p.save();
  p.setClipRect(QRect(kStrip - 3, 4, std::max(0, width() - 197), 31));
  // Rolled back or being edited (a feature or sketch): what comes after is not part of the shown state, or will be
  // regenerated from the edit.
  const std::string& from = !m_editing.empty() ? m_editing : m_doc->rollback();
  size_t fromIndex = ops.size();
  for (size_t q = 0; !from.empty() && q < ops.size(); ++q)
    if (ops[q].id == from) fromIndex = q;
  const int available = std::max(0, width() - 200);
  const size_t first = size_t(std::max<ptrdiff_t>(0, std::upper_bound(m_left.begin(), m_left.end(), m_scroll->value()) - m_left.begin() - 1));
  const size_t end = size_t(std::upper_bound(m_left.begin(), m_left.end(), m_scroll->value() + available + 4) - m_left.begin());
  if (m_names) p.setFont(theme::ui(11));
  for (size_t k = first; k < end; ++k) {
    const size_t i = m_shown[k];
    QRect r = markerRect(static_cast<int>(k));
    const bool deleted = m_deleted.count(ops[i].id) > 0, unresolved = isUnresolved(ops[i].id);
    const bool current = ops[i].id == m_current, hovered = static_cast<int>(k) == m_hover;
    p.setOpacity(m_dimmed.count(ops[i].id) && !current && !hovered ? 0.35 : 1.0);  // outside the active component
    QColor fill = t.bg4, iconColor = t.fg;
    if (ops[i].type == "annotation") { fill = t.amber; iconColor = QColor("#1e1f22"); }
    if (current) { fill = t.sel; iconColor = t.onsel; }
    p.setPen(Qt::NoPen);
    if (deleted) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(t.fg2, 1.5, Qt::DashLine));
      iconColor = t.fg2;
    } else {
      p.setBrush(fill);
    }
    p.drawRoundedRect(r, 2, 2);
    if (unresolved && !deleted) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(t.red, 1.5));
      p.drawRoundedRect(r, 2, 2);
    }
    if (hovered || current) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(current ? t.sel : t.hov, 1.5));
      p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 3, 3);
    }
    const bool beyond = i > fromIndex || (i == fromIndex && from != m_editing);
    if (ops[i].id == m_pulse) {  // pointed at: a ring that swells and fades three times
      const double phase = std::fmod(m_pulseTick / 10.0, 1.0);
      QColor ring = t.candidate;
      ring.setAlphaF(1.0 - 0.7 * phase);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(ring, 2.5));
      const int grow = 2 + int(std::round(3 * phase));
      p.drawRoundedRect(r.adjusted(-grow, -grow, grow, grow), 3 + grow / 2, 3 + grow / 2);
    }
    if (ops[i].id == m_editing) {  // the edited op: a thick ring, "changes apply from here"
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(t.sel, 2.5));
      p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), 4, 4);
    }
    const opad::Feature* feat = ops[i].type == "feature" ? m_doc->scene.feature(ops[i].id) : nullptr;
    if (beyond || (feat && feat->suppressed)) iconColor = t.fg3;
    p.drawPixmap(r.left() + 3, r.top() + 3, icons::pixmap(iconFor(ops[i]), iconColor, 12, dpr));
    if (const auto mark = m_marks.find(ops[i].id); mark != m_marks.end()) p.fillRect(QRect(r.left(), r.bottom() + 3, r.width(), 3), t.*mark->second);
    if (m_names) {
      p.setPen(iconColor);
      const QRect text = r.adjusted(20, 0, -6, 0);
      p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(label(ops[i]), Qt::ElideRight, text.width()));
    }
    // A reference was taken by its nearest match after the body changed (TODO 10 B7): worth a look.
    if (feat && !deleted && !feat->result.value("rehinted", opad::json::array()).empty()) {
      p.setPen(QPen(t.bg2, 1));
      p.setBrush(t.amber);
      p.drawEllipse(QPointF(r.right() - 1, r.top() + 1), 3.5, 3.5);
    }
  }
  p.setOpacity(1.0);
  if (!m_shown.empty()) {  // the roll-back marker: where the shown state ends
    const int x = gapX(m_dragging ? m_dragGap : rollbackGap());
    const QColor c = m_dragging || m_doc->rolledBack() ? t.candidate : t.sel;
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRect(x - 1, 8, 2, 32);
    QPainterPath tri;
    tri.moveTo(x - 4, 7);
    tri.lineTo(x + 4, 7);
    tri.lineTo(x, 12);
    tri.closeSubpath();
    p.drawPath(tri);
  }
  p.restore();
  m_prevBtn = QRect(width() - 60, 12, 24, 24);
  m_nextBtn = QRect(width() - 32, 12, 24, 24);
  for (const QRect& b : {m_prevBtn, m_nextBtn}) {
    if (b.contains(mapFromGlobal(QCursor::pos()))) { p.setPen(Qt::NoPen); p.setBrush(t.bg3); p.drawRoundedRect(b, 3, 3); }
  }
  p.setFont(theme::ui(14));
  p.setPen(m_shown.empty() ? t.fg3 : t.fg2);
  p.drawText(m_prevBtn, Qt::AlignCenter, QString::fromUtf8("‹"));
  p.drawText(m_nextBtn, Qt::AlignCenter, QString::fromUtf8("›"));
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
  if (m_dragging) {  // the playhead follows to the nearest gap between markers
    size_t best = m_dragGap;
    int distance = INT_MAX;
    for (size_t g = 0; g <= m_shown.size(); ++g)
      if (const int d = std::abs(gapX(g) - e->pos().x()); d < distance) distance = d, best = g;
    if (best != m_dragGap) {
      m_dragGap = best;
      update();
    }
    return;
  }
  const bool onPlayhead = playhead().contains(e->pos()) && indexAt(e->pos()) < 0;
  setCursor(onPlayhead ? Qt::SizeHorCursor : Qt::ArrowCursor);
  int i = indexAt(e->pos());
  if (i != m_hover) { setHover(i); update(); }
  if (onPlayhead) {
    QToolTip::showText(e->globalPosition().toPoint() + QPoint(0, 8),
                       m_doc->rolledBack() ? tr("Rolled back: the steps after this marker are not shown. Drag it to the end to roll forward.")
                                           : tr("Roll-back marker: drag it to see the model as it was at an earlier step."), this);
  } else if (i >= 0) {
    const Tokens& t = theme::current();
    const opad::Op& op = m_doc->doc.ops[m_shown[static_cast<size_t>(i)]];
    QColor sw = op.type == "annotation" ? t.amber : m_deleted.count(op.id) ? t.fg2 : t.bg4;
    QString target;
    if (op.data.contains("target") && op.data["target"].is_string()) target = m_doc->nodeName(op.data["target"].get<std::string>());
    QString html = QString("<div style='width:248px'><table cellspacing='0' cellpadding='0'><tr><td style='background:%1;width:14px;height:14px;'>&nbsp;&nbsp;&nbsp;</td><td>&nbsp;<b>%2</b>&nbsp;&nbsp;<span style='color:%3;font-family:%4;font-size:11px'>%5</span></td></tr></table>"
                           "<div style='color:%6'>%7 · %8</div>%9<div style='color:%3;font-size:11px'>%10</div></div>")
                       .arg(sw.name(), describe(op).toHtmlEscaped(), t.fg3.name(), theme::mono().family(), shortId(op.id), t.fg2.name(),
                            QString::fromStdString(op.data.value("by", "")).toHtmlEscaped(), i18n::localTime(op.data.value("ts", "")),
                            target.isEmpty() ? QString() : QString("<div>target %1</div>").arg(target.toHtmlEscaped()),
                            [&] {
                              const opad::Feature* f = op.type == "feature" ? m_doc->scene.feature(op.id) : nullptr;
                              return f && !f->result.value("rehinted", opad::json::array()).empty() ? QString("<div style='color:%1'>%2</div>").arg(t.amber.name(), tr("a reference was re-picked by its nearest match after its body changed; check it")) : QString();
                            }() + (m_dimmed.count(op.id) ? QString("<div style='color:%1'>%2</div>").arg(t.fg3.name(), tr("does not touch the active component")) : QString()) +
                            (op.id == m_editing ? tr("being edited · the change applies from here in the history")
                            : m_deleted.count(op.id) ? (op.type == "delete" ? tr("undone · right-click to delete it again") : tr("tombstoned · right-click to restore"))
                            : isUnresolved(op.id) ? tr("unresolved · kept, never hidden")
                            : op.type == "delete" ? tr("right-click to restore what it deleted") : tr("Right-click for actions")) +
                            [&] {
                              const auto mark = m_marks.find(op.id);
                              if (mark == m_marks.end()) return QString();
                              for (const theme::Cue& c : theme::cues())
                                if (c.colour == mark->second)
                                  return QString("<div style='color:%1'>%2 %3</div>").arg((t.*c.colour).name(), QString::fromUtf8(c.mark), m_markLegend.arg(i18n::t(c.label)).toHtmlEscaped());
                              return QString();
                            }());
    QToolTip::showText(e->globalPosition().toPoint() + QPoint(0, 8), html, this);
  } else {
    QToolTip::hideText();
  }
  update();
}

QString TimelineWidget::iconFor(const opad::Op& op) const {
  if (op.type == "feature")
    if (const auto* spec = opad::design::feature_spec(op.data.value("kind", ""))) return QString::fromStdString(spec->icon);
  return opTypeIcon(op.type);
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
  const int i = indexAt(e->pos());
  if (i < 0) return;
  const opad::Op& op = m_doc->doc.ops[m_shown[static_cast<size_t>(i)]];
  if ((op.type == "feature" || op.type == "sketch") && !m_deleted.count(op.id)) emit opActivated(op.id);
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
  if (m_prevBtn.contains(e->pos())) return step(-1);
  if (m_nextBtn.contains(e->pos())) return step(+1);
  int i = indexAt(e->pos());
  if (i < 0 && e->button() == Qt::LeftButton && playhead().contains(e->pos()) && m_editing.empty()) {
    m_dragging = true;
    m_dragGap = rollbackGap();
    QToolTip::hideText();
    return update();
  }
  if (i < 0) {
    if (e->button() == Qt::RightButton && e->pos().x() >= kStrip && e->pos().x() < width() - 72) emit contextRequested(std::string(), e->globalPosition().toPoint());
    return;
  }
  const std::string id = m_doc->doc.ops[m_shown[static_cast<size_t>(i)]].id;
  m_current = id;
  ensureCurrentVisible();
  update();
  if (e->button() == Qt::RightButton) emit contextRequested(id, e->globalPosition().toPoint());
  else emit opClicked(id);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
  if (!m_dragging || e->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(e);
  m_dragging = false;
  const size_t gap = m_dragGap;
  update();
  if (gap != rollbackGap() || (gap == m_shown.size()) != m_doc->rollback().empty())
    emit rollbackRequested(gap < m_shown.size() ? rollPoint(m_shown[gap]) : std::string());
}

void TimelineWidget::leaveEvent(QEvent*) {
  setHover(-1);
  update();
}
