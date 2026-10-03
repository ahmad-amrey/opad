#include "ComparePanel.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "I18n.hpp"
#include "Icons.hpp"
#include "PanelFooter.hpp"
#include "Units.hpp"

namespace {
QString text(const opad::json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_string() ? QString::fromStdString(j[key].get<std::string>()) : QString(); }
const char* stateOf(ComparePanel::Category c) {
  static const char* const states[] = {"diffAdded", "diffRemoved", "diffModified", "diffMoved", nullptr};
  return states[c];
}
// The kind of change a row is drawn as: its mark and colour.
ComparePanel::Category categoryOf(const opad::json& c) {
  const std::string change = c.value("change", "");
  if (change == "added" || change == "reopened") return ComparePanel::Added;
  if (change == "removed" || change == "resolved") return ComparePanel::Removed;
  if (change == "moved" || change == "reparented") return ComparePanel::Moved;
  return ComparePanel::Modified;
}
// The list's groups: what changed, in the order a person reads a design.
struct Group {
  const char* title;
  std::vector<std::string> kinds;
};
const std::vector<Group>& groups() {
  static const std::vector<Group> list = {
      {QT_TRANSLATE_NOOP("ComparePanel", "Parameters"), {"units", "param"}},  {QT_TRANSLATE_NOOP("ComparePanel", "Sketches"), {"sketch"}},
      {QT_TRANSLATE_NOOP("ComparePanel", "Features"), {"feature"}},          {QT_TRANSLATE_NOOP("ComparePanel", "Bodies"), {"component", "body"}},
      {QT_TRANSLATE_NOOP("ComparePanel", "Notes"), {"annotation", "measurement"}}, {QT_TRANSLATE_NOOP("ComparePanel", "Assets"), {"asset"}},
      {QT_TRANSLATE_NOOP("ComparePanel", "Sections and views"), {"section", "view"}}};
  return list;
}
QString yesNo(const opad::json& v) { return v.is_boolean() ? (v.get<bool>() ? QObject::tr("yes") : QObject::tr("no")) : QString(); }
QString valueText(const opad::json& v) {
  if (v.is_string()) return QString::fromStdString(v.get<std::string>());
  if (v.is_boolean()) return yesNo(v);
  if (v.is_number()) return QString::number(v.get<double>(), 'g', 6);
  return v.is_null() ? QString() : QString::fromStdString(v.dump());
}
std::array<double, 3> vec(const opad::json& v) {
  return v.is_array() && v.size() == 3 ? std::array<double, 3>{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()} : std::array<double, 3>{0, 0, 0};
}
}  // namespace

// ---------------------------------------------------------------- legend chip
LegendChip::LegendChip(const char* state, QWidget* parent) : QAbstractButton(parent), m_cue(state ? theme::cue(state) : nullptr), m_state(state) {
  setCheckable(true);
  setChecked(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::NoFocus);  // ] and [ stay with the window
  setObjectName("legendChip");
  const QString name = m_cue ? i18n::t(m_cue->label) : tr("Unchanged");
  setAccessibleName(name);
  setToolTip(tr("%1: click to hide or show them").arg(name));
  connect(this, &QAbstractButton::toggled, this, qOverload<>(&QWidget::update));
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void LegendChip::setCount(int n) {
  m_count = n;
  updateGeometry();
  update();
}

QSize LegendChip::sizeHint() const {
  const QFontMetrics fm(theme::ui(12));
  const QString name = m_cue ? i18n::t(m_cue->label) : tr("Unchanged");
  return QSize(16 + 6 + fm.horizontalAdvance(QString::number(m_count)) + 5 + fm.horizontalAdvance(name) + 6 + 16 + 14, 26);
}

void LegendChip::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setLayoutDirection(Qt::LeftToRight);  // the sides are placed by hand below; a right-to-left painter would flip them back
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  p.setPen(QPen(underMouse() ? t.fg3 : t.line, 1));
  p.setBrush(isChecked() ? t.bg3 : t.bg2);
  p.drawRoundedRect(r, 4, 4);
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  QColor colour = m_cue ? t.*m_cue->colour : t.ghost;
  colour.setAlpha(255);
  if (!isChecked()) colour.setAlphaF(0.35);
  const QRect swatch(rtl ? width() - 23 : 7, 5, 16, 16);
  p.setPen(Qt::NoPen);
  p.setBrush(colour);
  p.drawRoundedRect(swatch, 3, 3);
  p.setPen(m_cue ? t.bg : t.fg);  // the state's mark: never told by colour alone
  p.setFont(theme::ui(12, QFont::DemiBold));
  p.drawText(swatch, Qt::AlignCenter, m_cue ? QString::fromUtf8(m_cue->mark) : QStringLiteral("="));
  p.setFont(theme::ui(12));
  const QFontMetrics fm(p.font());
  const QString count = QString::number(m_count), name = m_cue ? i18n::t(m_cue->label) : tr("Unchanged");
  const int eye = 16;
  QRect body = rect().adjusted(29, 0, -(eye + 10), 0);
  if (rtl) body = rect().adjusted(eye + 10, 0, -29, 0);
  p.setPen(isChecked() ? t.fg : t.fg3);
  p.setFont(theme::ui(12, QFont::DemiBold));
  p.drawText(body, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), count);
  p.setFont(theme::ui(12));
  p.setPen(isChecked() ? t.fg2 : t.fg3);
  const int shift = QFontMetrics(theme::ui(12, QFont::DemiBold)).horizontalAdvance(count) + 5;
  p.drawText(body.adjusted(rtl ? 0 : shift, 0, rtl ? -shift : 0, 0), Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft),
             fm.elidedText(name, Qt::ElideRight, std::max(0, body.width() - shift)));
  p.drawPixmap(rtl ? 6 : width() - eye - 6, (height() - eye) / 2,
               icons::pixmap(isChecked() ? "eye" : "eye-off", isChecked() ? t.fg2 : t.fg3, eye, devicePixelRatioF()));
}

// ---------------------------------------------------------------- panel
QColor Tokens::* ComparePanel::colour(Category c) {
  switch (c) {
    case Added: return &Tokens::diffAdded;
    case Removed: return &Tokens::diffRemoved;
    case Modified: return &Tokens::diffModified;
    case Moved: return &Tokens::diffMoved;
    default: return &Tokens::ghost;
  }
}

ComparePanel::ComparePanel(QWidget* parent) : QWidget(parent) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  auto* body = new QWidget(this);
  auto* b = new QVBoxLayout(body);
  b->setContentsMargins(12, 10, 12, 8);
  b->setSpacing(8);
  // A over B: the version compared with, then the one the view shows.
  auto* pickers = new QGridLayout();
  pickers->setHorizontalSpacing(6);
  pickers->setVerticalSpacing(4);
  auto side = [this](const QString& letter, const QString& tip) {
    auto* l = new QLabel(letter, this);
    l->setObjectName("secondary");
    l->setFont(theme::mono(12));
    l->setToolTip(tip);
    return l;
  };
  m_pickA = new QComboBox(this);
  m_pickB = new QComboBox(this);
  m_pickA->setToolTip(tr("A: the version compared with"));
  m_pickB->setToolTip(tr("B: the version the view shows"));
  for (QComboBox* c : {m_pickA, m_pickB}) {
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    c->setMinimumContentsLength(12);
  }
  m_swap = new QToolButton(this);
  m_swap->setIcon(icons::themed("swap", 16));
  m_swap->setAutoRaise(true);
  m_swap->setToolTip(tr("Swap A and B"));
  m_swap->setAccessibleName(tr("Swap A and B"));
  pickers->addWidget(side("A", m_pickA->toolTip()), 0, 0);
  pickers->addWidget(m_pickA, 0, 1);
  pickers->addWidget(side("B", m_pickB->toolTip()), 1, 0);
  pickers->addWidget(m_pickB, 1, 1);
  pickers->addWidget(m_swap, 0, 2, 2, 1);
  pickers->setColumnStretch(1, 1);
  b->addLayout(pickers);
  // How they are shown: B over A in one view, or A's view beside B's.
  auto* layouts = new QWidget(this);
  layouts->setObjectName("segmented");
  auto* sl = new QHBoxLayout(layouts);
  sl->setContentsMargins(1, 1, 1, 1);
  sl->setSpacing(0);
  for (QToolButton** button : {&m_overlay, &m_sideBySide}) {
    auto* s = *button = new QToolButton(layouts);
    s->setObjectName("segment");
    s->setCheckable(true);
    s->setAutoExclusive(true);
    s->setFixedHeight(26);
    s->setAutoRaise(true);
    s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sl->addWidget(s, 1);
  }
  m_overlay->setText(tr("Overlay"));
  m_overlay->setToolTip(tr("B in one view with A's ghosts over it"));
  m_overlay->setChecked(true);
  m_sideBySide->setText(tr("Side by side"));
  m_sideBySide->setToolTip(tr("A's view on the left, B's on the right, turning together"));
  b->addWidget(layouts);
  // How much of each: A alone at the left end, B alone at the right one, both in the middle.
  m_weight = new QWidget(this);
  auto* weight = new QHBoxLayout(m_weight);
  weight->setContentsMargins(0, 0, 0, 0);
  weight->setSpacing(6);
  m_slider = new QSlider(Qt::Horizontal, this);
  m_slider->setRange(0, 100);
  m_slider->setValue(50);
  m_slider->setPageStep(25);
  m_slider->setToolTip(tr("Emphasis: A alone at the A end, B alone at the B end, both in the middle"));
  m_slider->setAccessibleName(tr("Emphasis between A and B"));
  weight->addWidget(side("A", tr("Show A")));
  weight->addWidget(m_slider, 1);
  weight->addWidget(side("B", tr("Show B")));
  b->addWidget(m_weight);
  // The legend: one chip per kind of change, its count, and an eye.
  auto* legend = new QGridLayout();
  legend->setSpacing(4);
  for (int c = 0; c < Categories; ++c) {
    m_chips[c] = new LegendChip(stateOf(Category(c)), this);
    legend->addWidget(m_chips[c], c / 2, c % 2);  // two to a row: each keeps its whole name at the panel's narrowest
    connect(m_chips[c], &QAbstractButton::toggled, this, [this, c](bool on) { emit categoryToggled(c, on); });
  }
  b->addLayout(legend);
  m_summary = new QLabel(this);
  m_summary->setWordWrap(true);
  m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  b->addWidget(m_summary);
  m_status = new QLabel(this);
  m_status->setObjectName("secondary");
  m_status->setWordWrap(true);
  m_status->hide();
  b->addWidget(m_status);
  m_list = makeList(this);
  m_list->setObjectName("compareChanges");
  m_list->setFocusPolicy(Qt::NoFocus);  // ] and [ stay with the window
  b->addWidget(m_list, 1);
  m_details = makeDetails(this);
  m_details->setObjectName("compareDetails");
  b->addWidget(m_details);
  v->addWidget(body, 1);
  m_footer = new PanelFooter(this);
  QPushButton* previous = m_footer->addSecondary(tr("Previous"), QStringLiteral("["));
  QPushButton* next = m_footer->addSecondary(tr("Next"), QStringLiteral("]"));
  previous->setObjectName("comparePrevious");
  next->setObjectName("compareNext");
  m_footer->setCancel(tr("Done"));
  m_footer->setPrimaryVisible(false);
  m_footer->setKeysStayWithWindow(true);
  v->addWidget(m_footer);
  connect(previous, &QPushButton::clicked, this, [this] { emit stepRequested(-1); });
  connect(next, &QPushButton::clicked, this, [this] { emit stepRequested(1); });
  connect(m_footer, &PanelFooter::cancelled, this, &ComparePanel::doneRequested);
  connect(m_swap, &QToolButton::clicked, this, &ComparePanel::swapRequested);
  connect(m_slider, &QSlider::valueChanged, this, &ComparePanel::emphasisChanged);
  for (QToolButton* s : {m_overlay, m_sideBySide})
    connect(s, &QToolButton::clicked, this, [this] {
      setSideBySide(m_sideBySide->isChecked());
      emit layoutChosen(m_sideBySide->isChecked());
    });
  connect(m_pickA, &QComboBox::activated, this, [this] { picked(0); });
  connect(m_pickB, &QComboBox::activated, this, [this] { picked(1); });
  connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
    const int change = item ? item->data(0, Qt::UserRole).toInt() : -1;
    if (!item || change < 0 || change == m_current) return;
    setCurrent(change);
    emit changeActivated(change);
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
    m_swap->setIcon(icons::themed("swap", 16));
    if (!m_changes.empty()) {  // the rows' marks are drawn in the role colours
      const int keep = m_current;
      opad::json changes = m_changes;
      std::array<int, Categories> counts{};
      for (int c = 0; c < Categories; ++c) counts[c] = m_chips[c]->count();
      const QString summary = m_summary->text();
      setResult({{"changes", changes}}, counts);
      m_summary->setText(summary);
      setCurrent(keep);
    }
  });
  clear();
}

void ComparePanel::picked(int side) {
  QComboBox* box = side == 0 ? m_pickA : m_pickB;
  const int index = box->currentData().toInt();
  if (index == -2) {  // Other file…
    emit otherFileRequested(side);
    return;
  }
  emit versionsChosen(m_pickA->currentData().toInt(), m_pickB->currentData().toInt());
}

void ComparePanel::setVersions(const std::vector<CompareVersion>& versions, int a, int b) {
  m_versions = int(versions.size());
  for (QComboBox* box : {m_pickA, m_pickB}) {
    QSignalBlocker block(box);
    box->clear();
    if (box == m_pickA && a < 0) box->addItem(tr("Choose a version…"), -1);
    for (size_t i = 0; i < versions.size(); ++i) {
      box->addItem(versions[i].label, int(i));
      box->setItemData(box->count() - 1, versions[i].detail.isEmpty() ? versions[i].label : versions[i].detail, Qt::ToolTipRole);
    }
    box->insertSeparator(box->count());
    box->addItem(tr("Other file…"), -2);
    const int want = box == m_pickA ? a : b;
    box->setCurrentIndex(std::max(0, box->findData(want)));
    box->setToolTip(box->currentData(Qt::ToolTipRole).toString());
  }
  m_swap->setEnabled(a >= 0 && b >= 0);
}

void ComparePanel::setBusy(const QString& status) {
  m_status->setText(status);
  m_status->setVisible(!status.isEmpty());
}

void ComparePanel::setFailed(const QString& error) {
  clear();
  m_status->setText(error);
  m_status->show();
}

void ComparePanel::clear() {
  m_changes = opad::json::array();
  m_order.clear();
  m_rows.clear();
  m_current = -1;
  m_list->clear();
  m_details->hide();
  m_summary->clear();
  m_status->hide();
  for (LegendChip* chip : m_chips) chip->setCount(0);
}

QString ComparePanel::status() const { return m_status->isVisible() ? m_status->text() : QString(); }

void ComparePanel::setSideBySide(bool on) {
  (on ? m_sideBySide : m_overlay)->setChecked(true);
  m_weight->setVisible(!on);  // each view shows its version whole: nothing to weigh
}

bool ComparePanel::sideBySide() const { return m_sideBySide->isChecked(); }
bool ComparePanel::shown(Category c) const { return m_chips[c]->isChecked(); }
int ComparePanel::emphasis() const { return m_slider->value(); }

QTreeWidget* ComparePanel::makeList(QWidget* parent) {
  auto* list = new QTreeWidget(parent);
  list->setColumnCount(2);
  list->setHeaderHidden(true);
  list->setRootIsDecorated(false);
  list->setIndentation(10);
  list->setUniformRowHeights(true);
  list->header()->setStretchLastSection(true);
  list->header()->setSectionResizeMode(0, QHeaderView::Fixed);
  list->header()->resizeSection(0, 26);
  list->setAccessibleName(tr("Changes"));
  return list;
}

QTableWidget* ComparePanel::makeDetails(QWidget* parent) {
  auto* table = new QTableWidget(parent);
  table->setColumnCount(3);
  table->setHorizontalHeaderLabels({tr("What"), tr("A (before)"), tr("B (after)")});
  table->verticalHeader()->hide();
  table->horizontalHeader()->setStretchLastSection(true);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionMode(QAbstractItemView::NoSelection);
  table->setFocusPolicy(Qt::NoFocus);
  table->setWordWrap(true);
  table->setAccessibleName(tr("What the change changed"));
  table->hide();
  return table;
}

void ComparePanel::setResult(const opad::json& diff, const std::array<int, Categories>& counts) {
  m_changes = diff.value("changes", opad::json::array());
  m_current = -1;
  {
    QSignalBlocker block(m_list);
    listChanges(m_list, m_changes, m_rows, m_order);
  }
  for (int c = 0; c < Categories; ++c) m_chips[c]->setCount(counts[c]);
  const std::string relation = diff.value("relation", "");
  const auto ops = diff.value("op_count", opad::json::object());
  const int na = ops.value("a", 0), nb = ops.value("b", 0), common = diff.value("common_ops", 0);
  QString how;
  if (relation == "same") how = tr("Same history.");
  else if (relation == "descendant") how = tr("B continues A: %n new op(s).", nullptr, nb - na);
  else if (relation == "ancestor") how = tr("B is an earlier A: %n op(s) fewer.", nullptr, na - nb);
  else if (relation == "diverged") how = tr("Diverged after %n common op(s).", nullptr, common);
  else if (relation == "unrelated") how = tr("Another document.");
  if (diff.contains("summary")) m_summary->setText(how + (m_changes.empty() ? " " + tr("No changes.") : " " + text(diff, "summary") + "."));
  m_status->hide();
  m_details->hide();
  emit contentResized();
}

void ComparePanel::listChanges(QTreeWidget* list, const opad::json& changes, std::vector<QTreeWidgetItem*>& rows, std::vector<int>& order) {
  const Tokens& t = theme::current();
  order.clear();
  rows.assign(changes.size(), nullptr);
  list->clear();
  for (const Group& g : groups()) {
    std::vector<int> members;
    for (size_t i = 0; i < changes.size(); ++i)
      if (std::find(g.kinds.begin(), g.kinds.end(), changes[i].value("kind", "")) != g.kinds.end()) members.push_back(int(i));
    if (members.empty()) continue;
    auto* head = new QTreeWidgetItem(list);
    head->setFirstColumnSpanned(true);
    head->setText(0, tr("%1 (%2)").arg(i18n::t(g.title)).arg(members.size()));
    head->setFont(0, theme::ui(11, QFont::DemiBold));
    head->setForeground(0, t.fg3);
    head->setFlags(Qt::ItemIsEnabled);
    head->setData(0, Qt::UserRole, -1);
    for (int i : members) {
      const opad::json& c = changes[size_t(i)];
      const Category cat = categoryOf(c);
      const theme::Cue* cue = theme::cue(stateOf(cat));
      auto* row = new QTreeWidgetItem(list);
      row->setText(0, QString::fromUtf8(cue->mark));
      row->setTextAlignment(0, Qt::AlignCenter);
      row->setForeground(0, t.*cue->colour);
      row->setFont(0, theme::ui(13, QFont::Bold));
      row->setToolTip(0, i18n::t(cue->label));
      const QString name = c.value("kind", "") == "annotation" ? "“" + text(c, "text").left(60) + "”" : text(c, "name");
      QString what;
      const std::string change = c.value("change", ""), kind = c.value("kind", "");
      if (change == "added") what = tr("added");
      else if (change == "removed") what = tr("removed");
      else if (change == "renamed") what = tr("renamed from %1").arg(text(c, "before"));
      else if (change == "moved") what = tr("moved");
      else if (change == "reparented") what = tr("moved into %1").arg(text(c, "after").isEmpty() ? tr("the root") : text(c, "after"));
      else if (change == "geometry") what = tr("geometry changed");
      else if (change == "appearance") what = tr("appearance changed");
      else if (change == "regenerated") what = tr("regenerated");
      else if (change == "suppressed") what = tr("suppressed");
      else if (change == "unsuppressed") what = tr("unsuppressed");
      else if (change == "failed") what = tr("fails");
      else if (change == "fixed") what = tr("no longer fails");
      else if (change == "resolved") what = tr("resolved");
      else if (change == "reopened") what = tr("reopened");
      else if (change == "commented") what = tr("replied to");
      else if (change == "synced") what = tr("synced");
      else if (change == "edited" && kind == "feature") {
        QStringList labels;
        for (const auto& d : c.value("details", opad::json::array())) labels << i18n::t(text(d, "label"));
        what = labels.join(", ");
      } else if (change == "edited" && (kind == "param" || kind == "units")) what = text(c, "before") + QString::fromUtf8(" → ") + text(c, "after");
      else what = tr("changed");
      row->setText(1, kind == "units" ? tr("Units: %1").arg(what) : name.isEmpty() ? what : tr("%1 · %2").arg(name, what));
      row->setToolTip(1, row->text(1));
      row->setData(0, Qt::UserRole, i);
      rows[size_t(i)] = row;
      order.push_back(i);
    }
  }
}

void ComparePanel::setCurrent(int change) {
  if (change < 0 || size_t(change) >= m_rows.size() || !m_rows[size_t(change)]) return;
  m_current = change;
  {
    QSignalBlocker block(m_list);
    m_list->setCurrentItem(m_rows[size_t(change)]);
  }
  m_list->scrollToItem(m_rows[size_t(change)]);
  fillDetails(m_details, m_changes[size_t(change)]);
}

void ComparePanel::fillDetails(QTableWidget* table, const opad::json& c) {
  std::vector<std::array<QString, 3>> rows;
  auto add = [&](const QString& what, const QString& before, const QString& after) { rows.push_back({what, before, after}); };
  const std::string change = c.value("change", ""), kind = c.value("kind", "");
  const QString none = QString::fromUtf8("—");
  if (change == "added" || change == "removed") {
    const bool added = change == "added";
    const QString state = added ? tr("added") : tr("removed");
    add(tr("State"), added ? none : tr("present"), added ? state : none);
    if (c.contains("parent")) add(tr("Under"), added ? none : text(c, "parent"), added ? text(c, "parent") : none);
    if (kind == "component") add(tr("Bodies"), added ? none : QString::number(c.value("bodies", 0)), added ? QString::number(c.value("bodies", 0)) : none);
    if (kind == "feature" && (c.contains("label") || c.contains("feature"))) add(tr("Kind"), QString(), i18n::t(c.contains("label") ? text(c, "label") : text(c, "feature")));
    if (kind == "param") add(tr("Value"), added ? none : text(c, "before"), added ? text(c, "after") : none);
    if (kind == "annotation") {
      add(tr("Text"), added ? none : text(c, "text"), added ? text(c, "text") : none);
      if (c.contains("by")) add(tr("By"), QString(), text(c, "by"));
    }
  } else if (change == "moved") {
    add(tr("Moved by"), QString(), units::vector(units::Kind::Length, vec(c.value("translation", opad::json()))));
    if (c.contains("axis"))
      add(tr("Turned"), QString(), tr("%1 about %2").arg(units::format(units::Kind::Angle, c.value("rotation_deg", 0.0)), [&] {
            const auto a = vec(c["axis"]);
            return QStringLiteral("(%1, %2, %3)").arg(a[0], 0, 'g', 4).arg(a[1], 0, 'g', 4).arg(a[2], 0, 'g', 4);
          }()));
  } else if (change == "geometry") {
    add(tr("Body"), text(c, "key_before").left(12), text(c, "key_after").left(12));
    if (c.contains("metrics"))
      for (const char* key : {"volume", "area"})
        if (c["metrics"]["before"].contains(key) && c["metrics"]["after"].contains(key)) {
          const auto k = std::string(key) == "volume" ? units::Kind::Volume : units::Kind::Area;
          add(std::string(key) == "volume" ? tr("Volume") : tr("Area"), units::format(k, c["metrics"]["before"][key].get<double>()),
              units::format(k, c["metrics"]["after"][key].get<double>()));
        }
  } else if (change == "appearance") {
    const opad::json f = c.value("fields", opad::json::object());
    auto colour = [](const opad::json& v) { return v.is_string() ? QString::fromStdString(v.get<std::string>()) : tr("default"); };
    if (f.contains("color")) add(tr("Colour"), colour(f["color"]["before"]), colour(f["color"]["after"]));
    if (f.contains("opacity")) add(tr("Opacity"), valueText(f["opacity"]["before"]), valueText(f["opacity"]["after"]));
    if (f.contains("visible")) add(tr("Shown"), yesNo(f["visible"]["before"]), yesNo(f["visible"]["after"]));
    if (f.contains("locked")) add(tr("Locked"), yesNo(f["locked"]["before"]), yesNo(f["locked"]["after"]));
  } else if (kind == "feature" && change == "edited") {
    for (const auto& d : c.value("details", opad::json::array())) add(i18n::t(text(d, "label")), text(d, "before"), text(d, "after"));
  } else if (kind == "sketch" && change == "edited") {
    for (const char* key : {"entities", "constraints", "images", "patterns"})
      if (c.contains(key)) {
        const opad::json& n = c[key];
        QStringList parts;
        if (n.contains("added")) parts << "+" + QString::number(n["added"].get<int>());
        if (n.contains("removed")) parts << QString::fromUtf8("−") + QString::number(n["removed"].get<int>());
        if (n.contains("changed")) parts << "~" + QString::number(n["changed"].get<int>());
        const QString label = std::string(key) == "entities" ? tr("Entities") : std::string(key) == "constraints" ? tr("Constraints")
                            : std::string(key) == "images" ? tr("Images") : tr("Patterns");
        add(label, QString(), parts.join(' '));
      }
    if (c.contains("points") && c["points"].contains("changed")) add(tr("Points moved"), QString(), QString::number(c["points"]["changed"].get<int>()));
    for (const auto& d : c.value("dimensions", opad::json::array()))
      add(tr("Dimension %1 (%2)").arg(d.value("id", 0)).arg(i18n::t(text(d, "type"))), text(d, "before"), text(d, "after"));
    if (c.value("plane", false)) add(tr("Plane"), QString(), tr("changed"));
    if (c.contains("dof")) add(tr("Degrees of freedom"), valueText(c["dof"]["before"]), valueText(c["dof"]["after"]));
    if (c.contains("error")) add(tr("Error"), QString(), text(c, "error").isEmpty() ? tr("none") : i18n::t(text(c, "error")));
  } else if (change == "failed") {
    add(tr("Error"), tr("none"), i18n::t(text(c, "error")));
  } else if (change == "suppressed" || change == "unsuppressed") {
    add(tr("Suppressed"), change == "suppressed" ? tr("no") : tr("yes"), change == "suppressed" ? tr("yes") : tr("no"));
  } else if (change == "commented") {
    for (const auto& r : c.value("comments", opad::json::array())) add(tr("Reply by %1").arg(text(r, "by")), QString(), text(r, "text"));
  } else if (c.contains("before") || c.contains("after")) {
    const QString what = change == "renamed" ? tr("Name") : change == "reparented" ? tr("Under") : kind == "units" ? tr("Units")
                       : kind == "param" ? tr("Value") : change == "restyled" ? tr("Style") : change == "reanchored" ? tr("Anchor")
                       : change == "storage" ? tr("Storage") : change == "synced" ? tr("Version") : kind == "annotation" ? tr("Text") : tr("Value");
    auto side = [&](const char* key) {
      const QString v = text(c, key);
      return v.isEmpty() && change == "reparented" ? tr("the root") : v;
    };
    add(what, side("before"), side("after"));
  }
  table->setRowCount(int(rows.size()));
  for (size_t i = 0; i < rows.size(); ++i)
    for (int col = 0; col < 3; ++col) {
      auto* item = new QTableWidgetItem(rows[i][size_t(col)]);
      item->setToolTip(rows[i][size_t(col)]);
      table->setItem(int(i), col, item);
    }
  table->resizeRowsToContents();
  int height = table->horizontalHeader()->sizeHint().height() + 2 * table->frameWidth() + 2;  // its rows, up to six: the list keeps the rest
  for (int i = 0; i < std::min(int(rows.size()), 6); ++i) height += table->rowHeight(i);
  table->setFixedHeight(height);
  table->setVisible(!rows.empty());
}

QSize ComparePanel::preferredSize(int width) const { return QSize(width, 660); }
