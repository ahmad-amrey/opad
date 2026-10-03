#include "MainWindow.hpp"
#include "CommandHelp.hpp"
#include "I18n.hpp"
#include "RichTip.hpp"
#include "SketchPanel.hpp"
#include "opad/design/feature.hpp"
#include <QApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStatusBar>
#include <QToolButton>
#include <QToolTip>

// OPAD_BENCH_RICHTIP=<prefix>: every command has its help record (translated in a translated run); the rich card on
// real ribbon buttons through synthesised events: nothing before 450 ms, compact after, expanded after +1200 ms,
// theme change, press hides, disabled button with its reason, Shift and F1 at once, browse mode, Qt's tooltip held
// back, the clip slot, grace on leaving. Cards saved as <prefix>.compact/.expanded/.expanded-light/.disabled/.clip.png.
bool MainWindow::benchRichTip() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_RICHTIP");
  if (prefix.isEmpty()) return false;
  const bool translated = i18n::current() != "en";
  QStringList missing;
  int checked = 0;
  auto need = [&](const QString& id) {
    ++checked;
    const CommandHelp* h = help::find(id);
    if (!h || h->title.isEmpty() || h->summary.isEmpty() || h->details.isEmpty() || (translated && !h->translated)) missing << id;
  };
  for (QAction* a : m_actions) if (!a->objectName().isEmpty()) need(a->objectName());
  for (const auto& tool : SketchPanel::tools()) need("sketch." + QString(tool.id).replace(':', '.'));
  for (const auto& spec : opad::design::feature_specs()) need("design." + QString::fromStdString(spec.kind));
  missing.removeDuplicates();
  trace::log(QString("bench: richtip: help for %1 command ids (%2), %3 missing %4 %5")
                 .arg(checked).arg(help::language()).arg(missing.size()).arg(missing.join(' ')).arg(missing.isEmpty() ? "PASS" : "FAIL"));

  m_ribbon->setWorkspace(0);
  m_ribbon->setCurrentTab(0);  // Review > View: Fit, Home, Exit isolate (disabled: nothing is isolated)
  auto button = [this](const char* id) -> QToolButton* {
    for (auto* b : m_ribbon->findChildren<QToolButton*>()) if (b->defaultAction() == action(id) && b->isVisibleTo(m_ribbon)) return b;
    return nullptr;
  };
  QToolButton *fit = button("view.fit"), *home = button("view.home"), *unisolate = button("view.unisolate"), *ortho = button("view.ortho");
  if (!fit || !home || !unisolate || !ortho || unisolate->isEnabled()) {
    trace::log("bench: richtip: ribbon buttons not found FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  RichTip::setActionLookup([this](const QString& id) { return action(id); });
  for (auto* b : {fit, home, unisolate}) RichTip::attach(b, b->defaultAction()->objectName());
  RichTip* tip = RichTip::instance();
  auto move = [](QWidget* w) {
    const QPointF at(w->width() / 2.0, w->height() / 2.0);
    QMouseEvent e(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
  };
  auto key = [](QWidget* w, int k, QEvent::Type type = QEvent::KeyPress) {
    QKeyEvent e(type, k, k == Qt::Key_Shift ? Qt::ShiftModifier : Qt::NoModifier);
    QApplication::sendEvent(w, &e);
    return e.isAccepted();
  };
  using State = RichTip::State;
  struct Run { int compactHeight = 0; QColor dark; bool ok = true; QStringList failed; };
  auto run = std::make_shared<Run>();
  auto check = [run](bool ok, const QString& what) {
    trace::log(QString("bench: richtip: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) { run->ok = false; run->failed << what; }
  };
  auto save = [tip, prefix](const QString& name) { return tip->grab().save(prefix + "." + name + ".png"); };
  // Steps run one after the other, each after its delay (ms) from the previous one.
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  add(0, [=] { move(fit); });
  add(250, [=] { check(tip->state() == State::Hidden, "no card before 450 ms"); });
  add(400, [=] {
    check(tip->state() == State::Compact && tip->isVisible() && tip->commandId() == "view.fit" && tip->target() == fit, "compact card after 450 ms");
    check(tip->width() <= 372 && tip->width() >= 200, QString("card width %1 within 200-372").arg(tip->width()));
    check(tip->geometry().top() >= fit->mapToGlobal(QPoint(0, fit->height())).y() - RichTip::kMargin, "card below its button");
    run->compactHeight = tip->height();
    check(save("compact"), "compact card saved");
  });
  add(1450, [=, this] {
    check(tip->state() == State::Expanded && tip->height() > run->compactHeight, QString("expanded after +1200 ms (%1 -> %2 px)").arg(run->compactHeight).arg(tip->height()));
    check(save("expanded"), "expanded card saved");
    run->dark = tip->grab().toImage().pixelColor(RichTip::kMargin + 3, tip->height() / 2);
    action("view.dark")->setChecked(!action("view.dark")->isChecked());
  });
  add(200, [=, this] {
    const QColor light = tip->grab().toImage().pixelColor(RichTip::kMargin + 3, tip->height() / 2);
    check(light != run->dark && tip->state() == State::Expanded, QString("theme change repaints the card (%1 -> %2)").arg(run->dark.name(), light.name()));
    check(save("expanded-light"), "light card saved");
    action("view.dark")->setChecked(!action("view.dark")->isChecked());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(4, 4), fit->mapToGlobal(QPointF(4, 4)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(fit, &press);
    check(tip->state() == State::Hidden && !tip->isVisible(), "a press hides the card");
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(4, 4), fit->mapToGlobal(QPointF(4, 4)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(fit, &release);
    move(fit);
  });
  add(700, [=, this] {
    check(tip->state() == State::Hidden, "no card again on the pressed button");
    move(statusBar());
    move(unisolate);
    key(unisolate, Qt::Key_Shift);
    check(tip->state() == State::Expanded && tip->commandId() == "view.unisolate", "Shift shows the expanded card at once");
    check(tip->showsRequirement(), "a disabled command says what it needs");
    check(save("disabled"), "disabled card saved");
    move(home);
    check(tip->state() == State::Compact && tip->commandId() == "view.home", "browse mode: the next button's card at once");
    QToolTip::hideText();
    QHelpEvent help(QEvent::ToolTip, QPoint(4, 4), fit->mapToGlobal(QPoint(4, 4)));
    QApplication::sendEvent(fit, &help);
    const bool held = !QToolTip::text().contains(fit->defaultAction()->toolTip());
    QHelpEvent other(QEvent::ToolTip, QPoint(4, 4), ortho->mapToGlobal(QPoint(4, 4)));
    QApplication::sendEvent(ortho, &other);
    check(held && QToolTip::text().contains(ortho->defaultAction()->toolTip()), "Qt's tooltip held back on attached buttons only");
    QToolTip::hideText();
    RichTip::setClipFactory([](const QString& clip, QWidget* parent) { auto* w = new QLabel(clip, parent); w->setAlignment(Qt::AlignCenter); return w; });
    tip->showFor(fit, State::Expanded);
    check(tip->clip() && tip->clip()->isVisible() && tip->clip()->size() == RichTip::kClip, "the expanded card holds the clip slot (288 x 162)");
    check(save("clip"), "clip slot card saved");
    tip->showFor(fit, State::Compact);
    check(!tip->clip(), "no clip in the compact card");
    RichTip::setClipFactory(nullptr);
    tip->hideTip();
    move(statusBar());
    move(fit);
    const bool f1 = key(fit, Qt::Key_F1, QEvent::ShortcutOverride);
    key(fit, Qt::Key_F1);
    check(f1 && tip->state() == State::Expanded, "F1 shows the expanded card at once");
    key(fit, Qt::Key_A);
    check(tip->state() == State::Hidden, "another key hides the card");
    if (translated) check(tip->layoutDirection() == (QApplication::isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight) && help::find("view.fit")->translated, "card in the UI language and direction");
    if (translated) check(RichTip::tr("Shift or F1 for more") != QLatin1String("Shift or F1 for more"), "the card's own strings translated (app/i18n/ar/help.json)");
    move(statusBar());
    move(home);
    key(home, Qt::Key_Shift);
    move(statusBar());
    check(tip->state() == State::Expanded, "leaving keeps the card for a moment");
  });
  add(450, [=, this] {
    check(tip->state() == State::Hidden, "the card hides 300 ms after leaving");
    trace::log(QString("bench: richtip: %1").arg(run->ok ? "PASS" : "FAIL: " + run->failed.join("; ")));
    QCoreApplication::exit(run->ok && missing.isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [this, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, this, [steps, next, check, i] {
      try { (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}
