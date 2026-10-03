#include "MainWindow.hpp"
#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "I18n.hpp"
#include "RichTip.hpp"
#include "SketchPanel.hpp"
#include "Theme.hpp"
#include "opad/design/feature.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QSettings>
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

// OPAD_BENCH_CLIPS=<dir>: the animated help clips (UI-107). app/help/clips.json loads without a problem and has every
// clip the help promises; each clip moves (distinct frames over its timeline), renders within the frame budget, and is
// saved as a contact sheet of 5 moments, <dir>/<id>.dark.png and .light.png (.ar.png in a right-to-left run, whose
// captions must all be translated), plus <dir>/all.<variant>.png with every clip's still frame (budget at 1.5x, best
// of 3 renders: a median under 6 ms; frames over one 30 fps tick are listed). Then the player: it
// animates only while visible, holds the still frame with reduced motion, loops one step, and plays in the expanded
// rich card of a command that has a clip.
bool MainWindow::benchClips() {
  const QString dir = qEnvironmentVariable("OPAD_BENCH_CLIPS");
  if (dir.isEmpty()) return false;
  QDir().mkpath(dir);
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: clips: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  const bool rtl = QApplication::isRightToLeft();
  const QStringList problems = clips::problems();
  check(problems.isEmpty(), QString("clips.json loads cleanly (%1 clips) %2").arg(clips::ids().size()).arg(problems.join("; ")));
  // The commands the help promises a clip for (UI-107): the most used ones, plus the workflows other tracks add.
  const QStringList required{"sketch.line", "sketch.rect", "sketch.circle", "sketch.arc3", "sketch.slot", "sketch.polygon", "sketch.offset",
                             "sketch.trim", "sketch.fillet", "sketch.dimension", "sketch.c.horizontal", "sketch.c.coincident", "sketch.c.perpendicular",
                             "design.extrude", "design.revolve", "design.fillet", "design.chamfer", "design.shell", "design.hole", "design.pattern_rect",
                             "design.pattern_circ", "design.mirror", "inspect.section", "inspect.distance", "inspect.angle", "inspect.radius",
                             "assembly.explode", "component.activate", "select.smart", "vcs.compare", "vcs.commit", "insert.canvas", "drawing.baseView"};
  QStringList missing;
  for (const QString& id : required) if (!clips::has(id)) missing << id;
  check(missing.isEmpty(), QString("%1 required clips present %2").arg(required.size()).arg(missing.join(' ')));
  QStringList untranslated;
  if (rtl)
    for (const QString& id : clips::ids())
      for (const QString& text : clips::texts(id)) if (i18n::t(text) == text) untranslated << id + ": " + text;
  if (rtl) check(untranslated.isEmpty(), "every caption and label translated " + untranslated.join(" | "));

  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  // Contact sheets: 5 moments per clip, light and dark (Arabic: dark, mirrored chrome); one clip per event-loop turn.
  const QSize cell(288, 162);
  const qreal dpr = 2;
  const int gap = 8, labelH = 16;
  struct Sheets { QStringList still, slow; QList<double> times; };
  auto sheets = std::make_shared<Sheets>();
  const QStringList variants = rtl ? QStringList{"ar"} : QStringList{"dark", "light"};
  for (const QString& variant : variants) {
    auto options = [variant, rtl](const Tokens& tokens) {
      clips::Options o;
      o.tokens = &tokens;
      o.rtl = rtl;
      return o;
    };
    for (const QString& id : clips::ids())
      add(0, [=] {
        const Tokens tokens = theme::tokens(variant != "light");
        const clips::Options o = options(tokens);
        const QList<double> moments{0.12, 0.32, 0.52, 0.72, 1.0};
        QImage sheet(QSize(5 * cell.width() + 6 * gap, cell.height() + 2 * gap + labelH) * dpr, QImage::Format_ARGB32_Premultiplied);
        sheet.setDevicePixelRatio(dpr);
        sheet.fill(tokens.bg);
        QPainter p(&sheet);
        QList<QImage> shots;
        for (int i = 0; i < moments.size(); ++i) {
          const double t = clips::duration(id) * moments[i];
          double ms = 1e9;  // the best of 3: other processes on the machine do not count against the clip
          for (int k = 0; k < 3; ++k) {
            QElapsedTimer clock;
            clock.start();
            const QImage f = clips::frame(id, t, cell, 1.5, o);
            ms = std::min(ms, clock.nsecsElapsed() / 1e6);
            if (!k) shots << f;
          }
          sheets->times << ms;
          if (ms > 33) sheets->slow << QString("%1@%2s %3ms").arg(id).arg(t, 0, 'f', 1).arg(ms, 0, 'f', 1);
          const QRectF at(gap + i * (cell.width() + gap), gap, cell.width(), cell.height());
          clips::paint(p, at, id, t, o);
          p.setPen(tokens.fg2);
          p.setFont(theme::ui(11));
          p.drawText(QRectF(at.left(), at.bottom() + 2, at.width(), labelH), Qt::AlignCenter, QString("%1  %2 s").arg(id).arg(t, 0, 'f', 2));
        }
        p.end();
        sheet.save(dir + "/" + id + "." + variant + ".png");
        int distinct = 1;
        for (int i = 1; i < shots.size(); ++i) if (shots[i] != shots[i - 1]) ++distinct;
        if (distinct < 3) sheets->still << id;
      });
    // Every clip's still frame (reduced motion: the clicks numbered, every step in the caption bar).
    add(0, [=] {
      const Tokens tokens = theme::tokens(variant != "light");
      clips::Options o = options(tokens);
      o.still = true;
      const QStringList ids = clips::ids();
      const int columns = 5, rows = int((ids.size() + columns - 1) / columns);
      QImage all(QSize(columns * (cell.width() + gap) + gap, rows * (cell.height() + gap + labelH) + gap) * dpr, QImage::Format_ARGB32_Premultiplied);
      all.setDevicePixelRatio(dpr);
      all.fill(tokens.bg);
      QPainter p(&all);
      for (int i = 0; i < ids.size(); ++i) {
        const QRectF at(gap + (i % columns) * (cell.width() + gap), gap + (i / columns) * (cell.height() + gap + labelH), cell.width(), cell.height());
        clips::paint(p, at, ids[i], clips::stillTime(ids[i]), o);
        p.setPen(tokens.fg2);
        p.setFont(theme::ui(11));
        p.drawText(QRectF(at.left(), at.bottom() + 1, at.width(), labelH), Qt::AlignCenter, ids[i]);
      }
      p.end();
      all.save(dir + "/all." + variant + ".png");
    });
  }
  add(0, [=] {
    Sheets& s = *sheets;
    check(s.still.isEmpty(), "every clip moves (3+ distinct moments) " + s.still.join(' '));
    // The median, not the worst: other processes on a busy machine (parallel builds) stall single frames.
    std::sort(s.times.begin(), s.times.end());
    const double median = s.times.isEmpty() ? 0 : s.times[s.times.size() / 2], p95 = s.times.isEmpty() ? 0 : s.times[s.times.size() * 95 / 100];
    check(!s.times.isEmpty() && median < 6, QString("frames render within budget: %1 frames, median %2 ms, 95th %3 ms").arg(s.times.size()).arg(median, 0, 'f', 2).arg(p95, 0, 'f', 2));
    if (!s.slow.isEmpty()) trace::log("bench: clips: frames over 33 ms (machine load or a heavy clip): " + s.slow.join(' '));
  });

  // The player: a timer only while visible; the still frame and no timer with reduced motion; one step looped.
  const QString clip = clips::has("design.extrude") ? "design.extrude" : clips::ids().value(0);
  auto* view = new ClipView(clip);
  view->setAttribute(Qt::WA_DeleteOnClose);
  view->resize(cell);
  auto shot = std::make_shared<QImage>();
  add(0, [=] {
    check(!view->playing(), "a hidden player does not run");
    view->show();
    check(view->playing(), "a visible player runs");
  });
  add(400, [=] { *shot = view->grab().toImage(); });
  add(300, [=] {
    check(view->grab().toImage() != *shot && view->time() > 0.4, QString("the player animates (t = %1 s)").arg(view->time(), 0, 'f', 2));
    view->setStep(1);
    const auto s = clips::steps(clip);
    check(s.size() > 1 && view->time() >= s[1].from - 1e-6 && view->time() <= s[1].to + 1e-6, "one step's segment loops in place");
    view->hide();
    check(!view->playing(), "hiding stops the timer");
    QSettings().setValue("ui/tipAnimate", false);
    view->setStep(-1);
    view->show();
    *shot = view->grab().toImage();
    check(!view->playing() && view->still() && std::abs(view->time() - clips::stillTime(clip)) < 1e-6, "reduced motion: the still frame, no timer");
  });
  add(250, [=, this] {
    check(view->grab().toImage() == *shot, "the still frame does not change");
    shot->save(dir + "/player-still.png");
    QSettings().remove("ui/tipAnimate");
    view->close();
    // The expanded rich card plays the command's clip; a command without a clip gets no slot.
    auto* button = new QToolButton(this);
    button->setDefaultAction(action("design.extrude"));
    button->setGeometry(40, 40, 40, 40);
    RichTip::setActionLookup([this](const QString& id) { return action(id); });
    RichTip::attach(button, "design.extrude");
    RichTip* tip = RichTip::instance();
    tip->showFor(button, RichTip::State::Expanded);
    auto* played = qobject_cast<ClipView*>(tip->clip());
    check(played && played->clip() == "design.extrude" && played->isVisible() && played->playing(), "the expanded card plays the command's clip");
    tip->grab().save(dir + "/richtip-clip.png");
    auto* other = new QToolButton(this);
    other->setDefaultAction(action("file.quit"));
    RichTip::attach(other, "file.quit");
    tip->showFor(other, RichTip::State::Expanded);
    check(!clips::has("file.quit") && !tip->clip(), "no clip slot for a command without a clip");
    tip->hideTip();
  });
  add(0, [=] {
    trace::log(QString("bench: clips: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [this, steps, next](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, this, [steps, next, i] {
      (*steps)[i].fn();
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}
