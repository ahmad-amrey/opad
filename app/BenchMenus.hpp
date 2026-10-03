#pragma once
// Menus in benches: the object names of a menu's entries, an entry by name, and a menu that pops up (exec or popup) found
// and handed over. Benches keep their windows off the screen (BenchQuiet), where a popup is no active popup.
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>

namespace benchmenus {
inline QStringList names(const QMenu* menu) {
  QStringList out;
  for (QAction* a : menu->actions())
    if (!a->objectName().isEmpty()) out << a->objectName();
  return out;
}
inline QAction* named(const QMenu* menu, const QString& name) {
  for (QAction* a : menu->actions())
    if (a->objectName() == name) return a;
  return nullptr;
}
inline QList<QMenu*> shownMenus() {
  QList<QMenu*> out;
  for (QWidget* top : QApplication::topLevelWidgets())
    if (auto* m = qobject_cast<QMenu*>(top); m && m->isVisible()) out << m;
  return out;
}
// Runs `inspect` on the menu `open` pops up (exec or popup), then closes it. Benches keep it off the screen, where it is
// no active popup: it is the menu shown that was not before.
inline void withPopup(const std::function<void()>& open, const std::function<void(QMenu*)>& inspect) {
  auto seen = std::make_shared<bool>(false);
  auto* poll = new QTimer;
  poll->setInterval(20);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, seen, inspect, before = shownMenus()] {
    QMenu* menu = nullptr;
    for (QMenu* m : shownMenus())
      if (!before.contains(m)) menu = m;
    if (!menu) return;
    *seen = true;
    poll->stop();
    poll->deleteLater();
    inspect(menu);
    if (menu->isVisible()) menu->close();
  });
  poll->start();
  open();
  QElapsedTimer clock;  // a popup() returns at once: wait for the poll
  clock.start();
  while (!*seen && clock.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
  if (!*seen) {
    poll->stop();
    poll->deleteLater();
    throw std::runtime_error("no menu popped up");
  }
}
}  // namespace benchmenus
