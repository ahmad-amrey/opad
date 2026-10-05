// The workspace list under the switcher chip: one line per workspace (icon, name, key, the active one checked), no
// description in the list; a row's description and tools show as its hint after a moment of hovering. Case in
// tools/bench_cases/core.py.
#include <QApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFrame>
#include <QLabel>
#include <QToolTip>

#include <functional>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Ribbon.hpp"

namespace {
void wait(int ms) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}
}  // namespace

// OPAD_BENCH_WORKSPACE_MENU=<prefix>: opens the list from the chip, checks its rows, hovers the first one, saves
// <prefix>.menu.png.
OPAD_BENCH(OPAD_BENCH_WORKSPACE_MENU, workspaceMenu) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: workspace menu: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  WorkspaceChip* chip = w.m_ribbon->findChild<WorkspaceChip*>();
  if (chip) chip->click();
  QCoreApplication::processEvents();
  QFrame* menu = nullptr;
  for (QWidget* top : QApplication::topLevelWidgets())
    if (top->objectName() == "wsMenu" && top->isVisible()) menu = qobject_cast<QFrame*>(top);
  require(chip && menu, "the chip opens the workspace list");
  if (!menu) {
    QCoreApplication::exit(2);
    return true;
  }
  const QList<QFrame*> rows = menu->findChildren<QFrame*>("wsRow");
  int wordy = 0, tallest = 0;
  QStringList names;
  for (QFrame* row : rows) {
    tallest = std::max(tallest, row->sizeHint().height());
    for (QLabel* l : row->findChildren<QLabel*>()) wordy += l->wordWrap();
    names << row->accessibleName();
  }
  require(rows.size() >= 2 && wordy == 0 && tallest <= 36 && !rows[0]->toolTip().size(),
          QString("one line per workspace, no description in the list: %1 rows (%2), tallest %3 px, %4 wrapped labels")
              .arg(rows.size()).arg(names.join(", ")).arg(tallest).arg(wordy));
  if (!rows.isEmpty()) {
    QFrame* row = rows[0];
    const QString description = row->accessibleDescription();
    QEnterEvent enter(QPointF(10, 10), QPointF(10, 10), row->mapToGlobal(QPointF(10, 10)));
    QApplication::sendEvent(row, &enter);
    wait(120);
    const bool early = QToolTip::isVisible();
    wait(400);
    const bool shown = QToolTip::isVisible() && !description.isEmpty() && QToolTip::text().startsWith(description);
    require(!early && shown, QString("hovered, the hint waits (none at 120 ms) then says what the workspace is for: \"%1\"")
                                 .arg(QToolTip::text().left(80)));
    menu->grab().save(value + ".menu.png");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(row, &leave);
    wait(500);  // a tooltip fades out
    require(!QToolTip::isVisible(), "leaving the row takes the hint away");
  }
  menu->close();
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
