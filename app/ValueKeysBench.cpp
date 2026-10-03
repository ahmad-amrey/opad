// Tool panels and the keyboard (TODO 11 UI-05), typed values outside the sketch (UI-122). Cases in
// tools/bench_cases/sketch.py; keys and clicks are Qt events sent where the keyboard is, as a user's would arrive.
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QSlider>
#include <QTimer>
#include <QToolButton>

#include <cmath>
#include <memory>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"

namespace {
struct Checks {
  QString bench;
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: %1: %2 %3").arg(bench, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
};

// Polls `done` every 20 ms until it holds or `ms` have passed, then calls `then` with the outcome.
void waitFor(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(20);
}

// A left click in the middle of `target`, press and release, through the application (its filters see it); the press gives
// it the keyboard by its focus policy, as Qt does for a real (spontaneous) one.
void click(QWidget* target) {
  const QPointF local = QRectF(target->rect()).center(), global = target->mapToGlobal(local);
  if (target->focusPolicy() & Qt::ClickFocus) target->setFocus(Qt::MouseFocusReason);
  QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(target, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(target, &release);
}

// A key pressed where the keyboard is (not spontaneous: Qt offers it to the shortcuts first, as it does a typed key).
void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {}) {
  QWidget* to = QApplication::focusWidget();
  if (!to) to = QApplication::activeWindow();
  if (!to) return;
  QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
  QApplication::sendEvent(to, &event);
}

// A panel has the keyboard, as after a click into one of its text fields: the active window, `focus` focused in it; the
// main window's own focus is elsewhere (on `away`), so giving it back is seen.
void panelHasKeyboard(QWidget* panel, QWidget* focus, QWidget* away) {
  QApplication::setActiveWindow(panel);
  focus->setFocus(Qt::MouseFocusReason);
  away->setFocus(Qt::OtherFocusReason);
}
}  // namespace

// OPAD_BENCH_PANEL_FOCUS=1 on a document with a body (UI-05): the Section panel has the keyboard (a text field of it was
// typed into). A click on its Y button gives the window and the keyboard back to the view, and so does one on its slider;
// a click into its offset field keeps the keyboard there. A click activates a panel only on a text field or a list (not on a
// button, the slider or the header). Esc typed while a button of the panel has the keyboard is the panel's: it closes (it
// was ambiguous with the window's Esc, and nothing happened).
OPAD_BENCH(OPAD_BENCH_PANEL_FOCUS, panelFocus) {
  auto check = std::make_shared<Checks>(Checks{"panel focus"});
  auto finish = [check] { QCoreApplication::exit(check->all ? 0 : 2); };
  ToolPanel* panel = w.m_sectionPanel;
  QWidget* home = w.m_viewport;
  QWidget* away = w.m_timeline;
  w.action("inspect.section")->setChecked(true);
  QToolButton* y = nullptr;
  for (auto* b : w.m_section->findChildren<QToolButton*>())
    if (b->text() == "Y") y = b;
  auto* slider = w.m_section->findChild<QSlider*>();
  auto* field = w.m_section->findChild<QLineEdit*>();
  auto* list = w.m_section->findChild<QListWidget*>();
  (*check)(panel->isVisible() && y && slider && field && list, "the Section panel is open with its axis buttons, slider, offset field and list");
  if (!y || !slider || !field || !list) {
    finish();
    return true;
  }
  auto at = [](QWidget* widget) { return widget->mapToGlobal(widget->rect().center()); };
  (*check)(panel->takesKeyboardAt(at(field)) && panel->takesKeyboardAt(at(list)) && !panel->takesKeyboardAt(at(y)) && !panel->takesKeyboardAt(at(slider)) &&
               !panel->takesKeyboardAt(panel->mapToGlobal(QPoint(ToolPanel::kMargin + 60, ToolPanel::kMargin + 16))),
           "a click activates the panel on its text field and its list only (not a button, the slider or the header)");

  panelHasKeyboard(panel, field, away);
  (*check)(QApplication::activeWindow() == panel && QApplication::focusWidget() == field && w.focusWidget() == away, "the panel has the keyboard, in its offset field");
  click(y);
  waitFor(&w, [&w, home] { return w.focusWidget() == home; }, 1000, [&w, check, finish, panel, home, away, y, slider, field](bool back) {
    (*check)(back && std::abs(w.m_section->normal()[1]) > 0.9,
             QString("a click on the Y button sets the axis and gives the keyboard back to the view (active: %1)").arg(QApplication::activeWindow() == &w ? "the window" : "the panel, not activated in a hidden run"));
    panelHasKeyboard(panel, y, away);
    click(field);
    QTimer::singleShot(100, &w, [&w, check, finish, panel, home, away, y, slider, field] {
      (*check)(w.focusWidget() == away && QApplication::focusWidget() == field,
               QString("a click into the offset field keeps the keyboard in the panel (window focus %1, focus %2, active %3)")
                   .arg(w.focusWidget() ? w.focusWidget()->metaObject()->className() : "none", QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none",
                        QApplication::activeWindow() ? QApplication::activeWindow()->metaObject()->className() : "none"));
      panelHasKeyboard(panel, field, away);
      click(slider);
      waitFor(&w, [&w, home] { return w.focusWidget() == home; }, 1000, [&w, check, finish, panel, away, y](bool back) {
        (*check)(back, "a click on the slider gives the keyboard back to the view");
        panelHasKeyboard(panel, y, away);
        press(Qt::Key_Escape);
        (*check)(!panel->isVisible(), "Esc typed while a button of the panel has the keyboard closes the panel");
        QApplication::setActiveWindow(&w);
        w.action("inspect.section")->setChecked(false);
        finish();
      });
    });
  });
  return true;
}
