#include "TimelineWidget.hpp"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>

#include "I18n.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

namespace {
QString shortId(const std::string& id) { return QString::fromStdString(id.substr(0, 8)); }
}  // namespace

// Ops the timeline draws. Visibility toggles, notes and pinned measurements are view state with their own
// UI (browser eye, Annotations tab, measurement card) and would bury the structural history; their
// tombstones go with them.
bool timelineShows(const opad::Document& doc, const opad::Op& op) {
  if (op.type == "annotation" || op.type == "measurement") return false;
  if (!opad::Document::known_type(op.type)) return false;  // a newer build's op (drawing sheets...): nothing to show or edit here
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
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
  setMouseTracking(true);
  setFixedHeight(48);
  setFocusPolicy(Qt::StrongFocus);
  m_scroll = new QScrollBar(Qt::Horizontal, this);
  m_scroll->setLayoutDirection(Qt::LeftToRight);
  m_scroll->setAccessibleName(tr("Timeline"));
  m_scroll->setSingleStep(26);
  connect(m_scroll, &QScrollBar::valueChanged, this, [this] { m_hover = -1; QToolTip::hideText(); update(); });
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
    if (const opad::Op& op = m_doc->doc.ops[i]; timelineShows(m_doc->doc, op) && !(m_hideDimmed && m_dimmed.count(op.id) && op.id != m_editing)) m_shown.push_back(i);
  m_hover = -1;
  updateScrollRange();
  if (atEnd) m_scroll->setValue(m_scroll->maximum());
  update();
}

bool TimelineWidget::isUnresolved(const std::string& opId) const { return m_unresolved.count(opId) > 0; }

void TimelineWidget::updateScrollRange() {
  const int available = std::max(0, width() - 200);
  m_scroll->setGeometry(128, 36, available, 12);
  m_scroll->setPageStep(available);
  m_scroll->setRange(0, std::max(0, int(m_shown.size()) * 26 + 4 - available));
  m_scroll->setVisible(m_scroll->maximum() > 0);
}

void TimelineWidget::ensureCurrentVisible() {
  for (size_t i = 0; i < m_shown.size(); ++i) if (m_doc->doc.ops[m_shown[i]].id == m_current) {
    const int x = int(i) * 26;
    if (x < m_scroll->value()) m_scroll->setValue(x);
    else if (x + 26 > m_scroll->value() + m_scroll->pageStep()) m_scroll->setValue(x + 26 - m_scroll->pageStep());
    break;
  }
}

void TimelineWidget::resizeEvent(QResizeEvent*) { updateScrollRange(); ensureCurrentVisible(); }
void TimelineWidget::wheelEvent(QWheelEvent* e) {
  const auto pixel = e->pixelDelta(), angle = e->angleDelta();
  const int delta = !pixel.isNull() ? (pixel.x() ? pixel.x() : pixel.y()) : (angle.x() ? angle.x() : angle.y()) * 78 / 120;
  m_scroll->setValue(m_scroll->value() - delta); e->accept();
}
void TimelineWidget::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Left) step(-1);
  else if (e->key() == Qt::Key_Right) step(1);
  else if (e->key() == Qt::Key_Home || e->key() == Qt::Key_End) {
    if (!m_shown.empty()) { setCurrentOp(m_doc->doc.ops[e->key() == Qt::Key_Home ? m_shown.front() : m_shown.back()].id); emit opClicked(m_current); }
  } else { QWidget::keyPressEvent(e); return; }
  e->accept();
}

QRect TimelineWidget::markerRect(int i) const { return QRect(128 + i * 26 - m_scroll->value(), 13, 18, 18); }

int TimelineWidget::indexAt(const QPoint& p) const {
  if (p.x() < 128 || p.x() >= width() - 72 || p.y() < 10 || p.y() >= 34) return -1;
  const int i = (p.x() - 128 + m_scroll->value() + 3) / 26;
  return i >= 0 && i < int(m_shown.size()) && markerRect(i).adjusted(-3,-3,3,3).contains(p) ? i : -1;
}

void TimelineWidget::setCurrentOp(const std::string& id) {
  m_current = id;
  ensureCurrentVisible();
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

std::vector<std::string> TimelineWidget::shownOps() const {
  std::vector<std::string> ids;
  for (const size_t i : m_shown) ids.push_back(m_doc->doc.ops[i].id);
  return ids;
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
  p.setPen(t.fg3);
  size_t tomb = 0;
  for (size_t i : m_shown) tomb += m_deleted.count(ops[i].id);
  QString count = tr("%1 ops").arg(m_shown.size());
  if (tomb > 0) count += tr(" · %1 tomb").arg(tomb);
  p.drawText(QRect(12, 24, 100, 16), Qt::AlignVCenter | Qt::AlignLeft, count);
  p.setPen(QPen(t.line, 1));
  p.drawLine(112, 8, 112, 40);
  p.drawLine(width() - 72, 8, width() - 72, 40);
  if (m_shown.empty() || !m_doc->hasDocument) {
    p.setFont(theme::ui(12));
    p.setPen(t.fg3);
    p.drawText(QRect(128, 0, width() - 200, height()), Qt::AlignVCenter | Qt::AlignLeft, tr("One marker per operation. Import a file to start the log."));
  }
  const qreal dpr = devicePixelRatioF();
  p.save();
  p.setClipRect(QRect(125, 4, std::max(0, width() - 197), 31));
  const size_t first = size_t(m_scroll->value() / 26);
  const size_t end = std::min(m_shown.size(), first + size_t(std::max(0, width() - 200) / 26 + 2));
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
    // Rolled back or being edited (a feature or sketch): what comes after is not part of the shown state, or will be
    // regenerated from the edit.
    const std::string& from = !m_editing.empty() ? m_editing : m_doc->rollback();
    const bool beyond = !from.empty() && [&] {
      for (size_t q = 0; q < ops.size(); ++q)
        if (ops[q].id == from) return i > q || (i == q && from != m_editing);
      return false;
    }();
    if (ops[i].id == m_editing) {  // the edited op: a thick ring, "changes apply from here"
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(t.sel, 2.5));
      p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), 4, 4);
    }
    const opad::Feature* feat = ops[i].type == "feature" ? m_doc->scene.feature(ops[i].id) : nullptr;
    if (beyond || (feat && feat->suppressed)) iconColor = t.fg3;
    p.drawPixmap(r.left() + 3, r.top() + 3, icons::pixmap(iconFor(ops[i]), iconColor, 12, dpr));
    // A reference was taken by its nearest match after the body changed (TODO 10 B7): worth a look.
    if (feat && !deleted && !feat->result.value("rehinted", opad::json::array()).empty()) {
      p.setPen(QPen(t.bg2, 1));
      p.setBrush(t.amber);
      p.drawEllipse(QPointF(r.right() - 1, r.top() + 1), 3.5, 3.5);
    }
  }
  p.setOpacity(1.0);
  if (!m_shown.empty()) {
    int x = markerRect(int(m_shown.size()) - 1).right() + 9;
    p.setPen(Qt::NoPen);
    p.setBrush(t.sel);
    p.drawRect(x - 1, 8, 2, 32);
    QPainterPath tri;
    tri.moveTo(x - 3, 8);
    tri.lineTo(x + 3, 8);
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
  int i = indexAt(e->pos());
  if (i != m_hover) { m_hover = i; update(); }
  if (i >= 0) {
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
                            : op.type == "delete" ? tr("right-click to restore what it deleted") : tr("Right-click for actions")));
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
  if (i < 0) return;
  const std::string id = m_doc->doc.ops[m_shown[static_cast<size_t>(i)]].id;
  m_current = id;
  ensureCurrentVisible();
  update();
  if (e->button() == Qt::RightButton) emit contextRequested(id, e->globalPosition().toPoint());
  else emit opClicked(id);
}

void TimelineWidget::leaveEvent(QEvent*) {
  m_hover = -1;
  update();
}
