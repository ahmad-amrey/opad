// The timeline's commands, the roll-back marker and its chip (TimelineArea.hpp, TODO 11 UI-99).
#include "TimelineArea.hpp"

#include <QAction>
#include <QMenu>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidgetItemIterator>

#include <algorithm>

#include "AppDocument.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "ShortcutEditor.hpp"
#include "TimelineWidget.hpp"
#include "Viewport.hpp"
#include "ViewportChips.hpp"

OPAD_ICON_TABLE(timeline,
  {"historyFilter", R"(<path d="M4 5h16l-6 7.5V18l-4 2v-7.5z"/>)"},
  {"history", R"(<path d="M4 6h9M4 12h6M4 18h6"/><circle cx="17" cy="15" r="4"/><path d="M17 13v2l1.5 1"/>)"},
  {"rollBack", R"(<path d="M20 12H9M13 8l-4 4 4 4"/><path d="M5 4v16"/>)"},
  {"rollForward", R"(<path d="M4 12h11M11 8l4 4-4 4"/><path d="M19 4v16"/>)"});

void TimelineArea::buildActions() {
  CommandInfo names;
  names.id = "timeline.names";
  names.label = tr("Names on the timeline");
  names.icon = "rename";
  names.checkable = true;
  names.keywords = {tr("timeline"), tr("history list"), tr("feature names")};
  m_names = services().addCommand(names, [this] {
    QSettings().setValue("timeline/names", m_names->isChecked());
    if (TimelineWidget* t = services().timeline()) t->setShowNames(m_names->isChecked());
  });
  m_names->setChecked(QSettings().value("timeline/names", false).toBool());
  m_names->setProperty("shortcutHint", tr("Each marker on the timeline carries its step's name, not only its icon."));
  CommandInfo design;
  design.id = "timeline.designOnly";
  design.label = tr("Design history only");
  design.icon = "historyFilter";
  design.checkable = true;
  design.keywords = {tr("timeline"), tr("filter"), tr("hide renames and colours")};
  m_designOnly = services().addCommand(design, [this] {
    QSettings().setValue("timeline/designOnly", m_designOnly->isChecked());
    if (TimelineWidget* t = services().timeline()) t->setDesignOnly(m_designOnly->isChecked());
    if (m_list->isChecked()) services().browser()->rebuild();
  });
  m_designOnly->setChecked(QSettings().value("timeline/designOnly", false).toBool());
  m_designOnly->setProperty("shortcutHint", tr("The timeline shows what makes and places geometry (imports, sketches, features, moves) and leaves out renames, colours, views and sections."));
  CommandInfo forward;
  forward.id = "timeline.rollForward";
  forward.label = tr("Roll forward to the end");
  forward.icon = "rollForward";
  forward.keywords = {tr("timeline"), tr("roll back"), tr("history marker")};
  m_forward = services().addCommand(forward, [this] { rollTo({}); });
  m_forward->setProperty("shortcutHint", tr("The model with every step again, after the timeline's marker was dragged back."));
  m_forward->setEnabled(false);
  CommandInfo list;
  list.id = "timeline.historyList";
  list.label = tr("History list in the browser");
  list.icon = "history";
  list.checkable = true;
  list.keywords = {tr("timeline"), tr("feature list"), tr("history tree")};
  m_list = services().addCommand(list, [this] {
    QSettings().setValue("timeline/historyList", m_list->isChecked());
    BrowserPanel* b = services().browser();
    if (!b) return;
    b->rebuild();
    for (QTreeWidgetItemIterator it(b->tree()); *it; ++it)  // turned on: open
      if ((*it)->data(0, Qt::UserRole).toString() == "folder" && (*it)->data(0, browser::kFolderRole).toString() == "history") (*it)->setExpanded(true);
  });
  m_list->setChecked(QSettings().value("timeline/historyList", false).toBool());
  m_list->setProperty("shortcutHint", tr("The browser lists the timeline's steps top to bottom in a History folder, with the roll-back marker among them."));
  for (QAction* a : {m_names, m_designOnly, m_forward, m_list}) shortcuts::updateTooltip(a);
}

void TimelineArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  QMenu* view = menus.value("view");
  if (!view) return;
  view->addSeparator();
  view->addActions({m_names, m_designOnly, m_list, m_forward});
}

void TimelineArea::ready() {
  TimelineWidget* t = services().timeline();
  t->setShowNames(m_names->isChecked());
  t->setDesignOnly(m_designOnly->isChecked());
  connect(t, &TimelineWidget::rollbackRequested, this, [this](const std::string& op) { services().guarded([&] { rollTo(op); }); });
  // Rolled back: a chip over the view says so, a click rolls forward.
  m_chip = new QToolButton;
  m_chip->setObjectName("chipAction");
  m_chip->setFocusPolicy(Qt::NoFocus);
  m_chip->setCursor(Qt::PointingHandCursor);
  m_chip->setToolTip(tr("The model is shown as it was before this step of the timeline; a change made now is added at the end and rolls forward"));
  connect(m_chip, &QToolButton::clicked, this, [this] { rollTo({}); });
  services().chips()->addChip(m_chip);
  m_chip->hide();
  browser::Folder history;
  history.id = "history";
  history.title = tr("History");
  history.icon = "history";
  history.items = [this] { return historyRows(); };
  history.contextMenu = [this](const std::string& id, QMenu& menu) { rowMenu(id, menu); };
  history.activated = [this](const std::string& id) { rowActivated(id); };
  services().browser()->addFolder(history);
  services().browser()->addDecorator([this](const browser::Row& row, browser::Decoration& out) { decorate(row, out); });
  refresh();
}

std::string TimelineArea::rowOp(const std::string& id) { return id.rfind("history:", 0) == 0 && id != kRollRow ? id.substr(8) : std::string(); }

// One row per marker (the timeline's filter too), the roll-back marker's where the model stops; none while the list is off.
std::vector<browser::Item> TimelineArea::historyRows() {
  m_beyond.clear();
  m_tombstoned.clear();
  m_suppressed.clear();
  m_failing.clear();
  std::vector<browser::Item> rows;
  const AppDocument* doc = services().document();
  const TimelineWidget* t = services().timeline();
  if (!m_list || !m_list->isChecked() || !doc->hasDocument || !t) return rows;
  const auto& deleted = doc->scene.deleted_ops;
  const std::string& until = doc->rollback();
  bool beyond = false;
  for (const auto& op : doc->doc.ops) {
    if (!until.empty() && op.id == until) {
      beyond = true;
      if (doc->rolledBack()) rows.push_back({kRollRow, tr("Rolled back here"), "rollBack", tr("The model is shown as it was before the steps below it; double-click to roll forward"), {}});
    }
    if (!TimelineWidget::shows(doc->doc, op, t->designOnly())) continue;
    const std::string id = "history:" + op.id;
    if (beyond) m_beyond.insert(id);
    if (std::find(deleted.begin(), deleted.end(), op.id) != deleted.end()) m_tombstoned.insert(id);
    if (const opad::Feature* f = op.type == "feature" ? doc->scene.feature(op.id) : nullptr) {
      if (f->suppressed) m_suppressed.insert(id);
      if (!f->error.empty()) m_failing[id] = i18n::t(QString::fromStdString(f->error));
    }
    rows.push_back({id, t->label(op), t->icon(op), t->describe(op), {}});
  }
  return rows;
}

void TimelineArea::decorate(const browser::Row& row, browser::Decoration& out) const {
  if (row.kind != "provided" || row.folder != "history") return;
  if (row.id == kRollRow) {
    out.bold = true;
    return;
  }
  if (m_beyond.count(row.id)) {
    out.dim = true;
    out.tooltip = tr("After the roll-back marker: not in the model shown");
  }
  if (m_tombstoned.count(row.id)) out.italic = out.dim = true;
  if (m_suppressed.count(row.id)) out.italic = true;
  if (const auto it = m_failing.find(row.id); it != m_failing.end()) {
    browser::Badge failing;
    failing.text = tr("fails");
    failing.color = &Tokens::red;
    failing.fill = nullptr;
    failing.tooltip = it->second;
    out.badges << failing;
  }
}

void TimelineArea::rowMenu(const std::string& id, QMenu& menu) {
  if (id == kRollRow) return menu.addAction(m_forward), void();
  services().timelineMenu(menu, rowOp(id));  // the folder's own row: the timeline's entries
}

void TimelineArea::rowActivated(const std::string& id) {
  if (id == kRollRow) return services().guarded([this] { rollTo({}); });
  const std::string op = rowOp(id);
  const opad::Op* o = services().document()->doc.find_op(op);
  if (!o || (o->type != "feature" && o->type != "sketch") || m_tombstoned.count(id)) return;
  if (m_beyond.count(id)) return services().showMessage(tr("That step comes after the roll-back marker: roll forward to edit it."), 6000);
  emit services().timeline()->opActivated(op);  // as on its marker
}

// A row selected alone points at its step: the marker current and pulsing, what it made in amber (as under the pointer).
void TimelineArea::selectionChanged(const SelectionContext& selection) {
  const std::string op = selection.ids.size() == 1 ? rowOp(selection.ids.front()) : std::string();
  const std::string was = std::exchange(m_pointed, op);
  TimelineWidget* t = services().timeline();
  if (op == was || !t) return;
  if (op.empty()) return emit t->markerHovered({});
  t->setCurrentOp(op);
  t->pulse(op);
  emit t->markerHovered(op);
}

// Del on a row: as Del on its marker (the marker's own Delete entry).
bool TimelineArea::command(const QString& id, const SelectionContext& selection) {
  if (id != "edit.delete" || selection.ids.size() != 1 || rowOp(selection.ids.front()).empty()) return false;
  QMenu menu;
  services().timelineMenu(menu, rowOp(selection.ids.front()));
  QAction* del = nullptr;
  for (QAction* a : menu.actions())
    if (a->objectName() == "timelineDelete") del = a;
  if (!del || !del->isEnabled()) throw opad::Error("That step is deleted already: Shift+Del on its marker restores it.");
  del->trigger();
  return true;
}

void TimelineArea::documentChanged(bool) { refresh(); }

bool TimelineArea::rollTo(const std::string& op) {
  AppDocument* doc = services().document();
  DesignController* design = services().design();
  TimelineWidget* t = services().timeline();
  if (!doc->hasDocument || doc->browse) return false;
  if (doc->snapshotBusy()) {  // a copy of the document is being taken: the model follows the playhead once it is done
    doc->afterCapture([this, op] { services().guarded([&] { rollTo(op); }); });
    return true;
  }
  if (doc->loading || doc->designBusy || design->sketchActive() || design->featureActive() || design->pickingPlane()) {
    t->update();  // the playhead goes back where the model stops
    services().showMessage(tr("Finish or cancel what is open first: the roll-back marker stays where the edit put it."), 6000);
    return false;
  }
  if (op == doc->rollback() && doc->rolledBack() == !op.empty()) return true;
  // Picked faces, edges and points are those of the bodies as they are now: another state has other ones.
  const auto picks = services().selection().refs;
  if (std::any_of(picks.begin(), picks.end(), [](const opad::Ref& r) { return r.kind != opad::Ref::Kind::Body; })) services().select({});
  trace::log(QString("timeline: rolled %1").arg(op.empty() ? QString("forward") : "back before " + QString::fromStdString(op)));
  doc->rollBackTo(op);
  return true;
}

void TimelineArea::refresh() {
  const AppDocument* doc = services().document();
  const bool back = doc->rolledBack();
  if (m_forward) m_forward->setEnabled(back);
  if (!m_chip) return;
  if (back) {
    const opad::Op* op = doc->doc.find_op(doc->rollback());
    m_chip->setText(tr("Rolled back before %1 · Roll forward").arg(op ? services().timeline()->label(*op) : QString()));
  }
  if (m_chip->isVisibleTo(m_chip->parentWidget()) != back) m_chip->setVisible(back);
}

OPAD_AREA(TimelineArea)
