// The timeline's commands, the roll-back marker and its chip (TimelineArea.hpp, TODO 11 UI-99).
#include "TimelineArea.hpp"

#include <QAction>
#include <QMenu>
#include <QSettings>
#include <QToolButton>

#include <algorithm>

#include "AppDocument.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "ShortcutEditor.hpp"
#include "TimelineWidget.hpp"
#include "Viewport.hpp"
#include "ViewportChips.hpp"

OPAD_ICON_TABLE(timeline,
  {"historyFilter", R"(<path d="M4 5h16l-6 7.5V18l-4 2v-7.5z"/>)"},
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
  for (QAction* a : {m_names, m_designOnly, m_forward}) shortcuts::updateTooltip(a);
}

void TimelineArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  QMenu* view = menus.value("view");
  if (!view) return;
  view->addSeparator();
  view->addActions({m_names, m_designOnly, m_forward});
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
  refresh();
}

void TimelineArea::documentChanged(bool) { refresh(); }

bool TimelineArea::rollTo(const std::string& op) {
  AppDocument* doc = services().document();
  DesignController* design = services().design();
  TimelineWidget* t = services().timeline();
  if (!doc->hasDocument || doc->browse) return false;
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
