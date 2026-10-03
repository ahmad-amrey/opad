#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "CommandHelp.hpp"
#include "DesignController.hpp"
#include "GuidedTool.hpp"
#include "HelpClip.hpp"
#include "HelpReference.hpp"
#include "I18n.hpp"
#include "RichTip.hpp"
#include "SketchPanel.hpp"
#include "Theme.hpp"
#include "opad/design/feature.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QStatusBar>
#include <QToolButton>
#include <QToolTip>

// OPAD_BENCH_RICHTIP=<prefix>: every command has its help record (translated in a translated run); the rich card on
// real ribbon buttons through synthesised events: nothing before 450 ms, compact after, expanded after +1200 ms,
// theme change, press hides, disabled button with its reason, Shift and F1 at once, browse mode, Qt's tooltip held
// back, the clip slot, grace on leaving; then the View menu's entries: a card beside the menu after 450 ms, none on a
// submenu entry, browse mode, F1, a disabled entry's reason, gone with the menu. Cards saved as
// <prefix>.compact/.expanded/.expanded-light/.disabled/.clip/.menu/.menu-expanded.png.
OPAD_BENCH(OPAD_BENCH_RICHTIP, richtip) {
  const QString prefix = value;
  const bool translated = i18n::current() != "en";
  QStringList missing;
  int checked = 0;
  auto need = [&](const QString& id) {
    ++checked;
    const CommandHelp* h = help::find(id);
    if (!h || h->title.isEmpty() || h->summary.isEmpty() || h->details.isEmpty() || (translated && !h->translated)) missing << id;
  };
  for (QAction* a : w.m_actions) if (!a->objectName().isEmpty()) need(a->objectName());
  for (const auto& tool : SketchPanel::tools()) need("sketch." + QString(tool.id).replace(':', '.'));
  for (const auto& spec : opad::design::feature_specs()) need("design." + QString::fromStdString(spec.kind));
  missing.removeDuplicates();
  trace::log(QString("bench: richtip: help for %1 command ids (%2), %3 missing %4 %5")
                 .arg(checked).arg(help::language()).arg(missing.size()).arg(missing.join(' ')).arg(missing.isEmpty() ? "PASS" : "FAIL"));

  w.m_ribbon->setWorkspace(0);
  w.m_ribbon->setCurrentTab(0);  // Review > View: Fit, Home, Exit isolate (disabled: nothing is isolated)
  auto button = [&w](const char* id) -> QToolButton* {
    for (auto* b : w.m_ribbon->findChildren<QToolButton*>()) if (b->defaultAction() == w.action(id) && b->isVisibleTo(w.m_ribbon)) return b;
    return nullptr;
  };
  QToolButton *fit = button("view.fit"), *home = button("view.home"), *unisolate = button("view.unisolate");
  if (!fit || !home || !unisolate || unisolate->isEnabled()) {
    trace::log("bench: richtip: ribbon buttons not found FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  // Attached as the window was built: every ribbon tool of every group (menu buttons too; not a group's own title or
  // collapsed button), the selection filters, quick access, search, the settings button and the status bar's toggles,
  // each to its own command, which has help.
  QStringList unattached;
  int attached = 0;
  QList<QAbstractButton*> commandButtons{w.m_ribbon->searchField()};
  for (auto* g : w.m_ribbon->findChildren<RibbonGroup*>())
    for (QToolButton* b : g->buttons()) commandButtons << b;
  for (auto* b : w.m_ribbon->findChildren<QToolButton*>())
    if (QStringList{"segment", "segmentPrimary", "ribbonSettings", "ribbonQuick"}.contains(b->objectName())) commandButtons << b;
  for (QAbstractButton* b : commandButtons) {
    const QString id = RichTip::attachedId(b);
    auto* tool = qobject_cast<QToolButton*>(b);
    if (id.isEmpty() || !help::find(id) || (tool && tool->defaultAction() && tool->defaultAction()->objectName() != id)) unattached << (id.isEmpty() ? b->text() : id);
    else ++attached;
  }
  for (const char* id : {"view.extensions", "view.tracking", "view.gridSnap"}) {
    bool found = false;
    for (auto* b : w.statusBar()->findChildren<QToolButton*>()) found = found || (b->defaultAction() == w.action(id) && RichTip::attachedId(b) == id);
    if (found) ++attached; else unattached << id;
  }
  trace::log(QString("bench: richtip: %1 ribbon and status buttons show their command's card, %2 do not %3 %4").arg(attached).arg(unattached.size()).arg(unattached.join(' ')).arg(unattached.isEmpty() && attached > 60 ? "PASS" : "FAIL"));
  if (!unattached.isEmpty()) missing << unattached;
  RichTip* tip = RichTip::instance();
  auto move = [](QWidget* w) {
    const QPointF at(w->width() / 2.0, w->height() / 2.0);
    QMouseEvent e(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
  };
  auto moveTo = [](QMenu* menu, QAction* entry) {  // onto a menu's entry
    const QPointF at = QRectF(menu->actionGeometry(entry)).center();
    QMouseEvent e(QEvent::MouseMove, at, menu->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(menu, &e);
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
  add(1450, [=, &w] {
    check(tip->state() == State::Expanded && tip->height() > run->compactHeight, QString("expanded after +1200 ms (%1 -> %2 px)").arg(run->compactHeight).arg(tip->height()));
    check(save("expanded"), "expanded card saved");
    run->dark = tip->grab().toImage().pixelColor(RichTip::kMargin + 3, tip->height() / 2);
    w.action("view.dark")->setChecked(!w.action("view.dark")->isChecked());
  });
  add(200, [=, &w] {
    const QColor light = tip->grab().toImage().pixelColor(RichTip::kMargin + 3, tip->height() / 2);
    check(light != run->dark && tip->state() == State::Expanded, QString("theme change repaints the card (%1 -> %2)").arg(run->dark.name(), light.name()));
    check(save("expanded-light"), "light card saved");
    w.action("view.dark")->setChecked(!w.action("view.dark")->isChecked());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(4, 4), fit->mapToGlobal(QPointF(4, 4)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(fit, &press);
    check(tip->state() == State::Hidden && !tip->isVisible(), "a press hides the card");
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(4, 4), fit->mapToGlobal(QPointF(4, 4)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(fit, &release);
    move(fit);
  });
  add(700, [=, &w] {
    check(tip->state() == State::Hidden, "no card again on the pressed button");
    move(w.statusBar());
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
    auto* plain = new QToolButton(w.m_ribbon);  // a button that is no command keeps Qt's tooltip
    plain->setToolTip("Not a command");
    plain->setGeometry(0, 0, 20, 20);
    QHelpEvent other(QEvent::ToolTip, QPoint(4, 4), plain->mapToGlobal(QPoint(4, 4)));
    QApplication::sendEvent(plain, &other);
    check(held && QToolTip::text().contains("Not a command"), "Qt's tooltip held back on attached buttons only");
    delete plain;
    QToolTip::hideText();
    RichTip::setClipFactory([](const QString& clip, QWidget* parent) { auto* w = new QLabel(clip, parent); w->setAlignment(Qt::AlignCenter); return w; });
    tip->showFor(fit, State::Expanded);
    check(tip->clip() && tip->clip()->isVisible() && tip->clip()->size() == RichTip::kClip, "the expanded card holds the clip slot (288 x 162)");
    check(save("clip"), "clip slot card saved");
    tip->showFor(fit, State::Compact);
    check(!tip->clip(), "no clip in the compact card");
    RichTip::setClipFactory([](const QString& clip, QWidget* parent) -> QWidget* { return new ClipView(clip, parent); }, &clips::has);  // the help area's
    tip->hideTip();
    move(w.statusBar());
    move(fit);
    const bool f1 = key(fit, Qt::Key_F1, QEvent::ShortcutOverride);
    key(fit, Qt::Key_F1);
    check(f1 && tip->state() == State::Expanded, "F1 shows the expanded card at once");
    key(fit, Qt::Key_A);
    check(tip->state() == State::Hidden, "another key hides the card");
    if (translated) check(tip->layoutDirection() == (QApplication::isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight) && help::find("view.fit")->translated, "card in the UI language and direction");
    if (translated) check(RichTip::tr("Shift or F1 for more") != QLatin1String("Shift or F1 for more"), "the card's own strings translated (app/i18n/ar/help.json)");
    move(w.statusBar());
    move(home);
    key(home, Qt::Key_Shift);
    move(w.statusBar());
    check(tip->state() == State::Expanded, "leaving keeps the card for a moment");
  });
  add(450, [=] { check(tip->state() == State::Hidden, "the card hides 300 ms after leaving"); });
  add(400, [=, &w] {  // past the browse grace
    // Menus: the menu bar's View menu, as it drops down in the middle of the screen (kept off it by the bench).
    w.m_viewMenu->popup(QGuiApplication::primaryScreen()->availableGeometry().center() - QPoint(0, 200));
    moveTo(w.m_viewMenu, w.action("view.fit"));
  });
  add(250, [=] { check(tip->state() == State::Hidden, "menu: no card before 450 ms"); });
  add(350, [=, &w] {
    QMenu* menu = w.m_viewMenu;
    check(tip->state() == State::Compact && tip->target() == menu && tip->entry() == w.action("view.fit") && tip->commandId() == "view.fit", "menu: a command entry shows its card after 450 ms");
    const QRect frame(menu->mapToGlobal(QPoint(0, 0)), menu->size()), card = tip->geometry().adjusted(RichTip::kMargin, RichTip::kMargin, -RichTip::kMargin, -RichTip::kMargin);
    const bool rtl = tip->layoutDirection() == Qt::RightToLeft;
    const QRect entry(menu->mapToGlobal(menu->actionGeometry(w.action("view.fit")).topLeft()), menu->actionGeometry(w.action("view.fit")).size());
    check((rtl ? card.right() < frame.left() : card.left() > frame.right()) && card.top() <= entry.bottom() && card.bottom() >= entry.top(),
          QString("menu: the card beside the menu, level with its entry (%1 by the menu's %2 side)").arg(card.left()).arg(rtl ? "left" : "right"));
    check(save("menu"), "menu card saved");
    moveTo(menu, w.m_viewsMenu->menuAction());  // a submenu entry is no command
    check(tip->state() == State::Compact, "menu: the card stays for a moment off its entry");
  });
  add(450, [=, &w] {
    check(tip->state() == State::Hidden, "menu: an entry that is no command hides the card");
    moveTo(w.m_viewMenu, w.action("view.isolate"));  // within the grace: browse mode
    check(tip->state() == State::Compact && tip->commandId() == "view.isolate", "menu: the next command's card at once (browse mode)");
    const bool f1 = key(w.m_viewMenu, Qt::Key_F1, QEvent::ShortcutOverride);
    key(w.m_viewMenu, Qt::Key_F1);
    check(f1 && tip->state() == State::Expanded && tip->clip(), "menu: F1 expands it with the clip");
    check(save("menu-expanded"), "expanded menu card saved");
    moveTo(w.m_viewMenu, w.action("view.unisolate"));
    key(w.m_viewMenu, Qt::Key_Shift);
    check(tip->commandId() == "view.unisolate" && tip->showsRequirement(), "menu: a disabled entry says what it needs");
    w.m_viewMenu->hide();
    check(tip->state() == State::Hidden && !tip->isVisible(), "menu: closing the menu hides the card");
    trace::log(QString("bench: richtip: %1").arg(run->ok ? "PASS" : "FAIL: " + run->failed.join("; ")));
    QCoreApplication::exit(run->ok && missing.isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [&w, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [steps, next, check, i] {
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
OPAD_BENCH(OPAD_BENCH_CLIPS, clips) {
  const QString dir = value;
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

  // OPAD_BENCH_CLIPS_ONLY=<prefix,...>: sheets of those clips only (authoring).
  const QStringList only = qEnvironmentVariable("OPAD_BENCH_CLIPS_ONLY").split(',', Qt::SkipEmptyParts);
  QStringList sheetIds;
  for (const QString& id : clips::ids())
    if (only.isEmpty() || std::any_of(only.begin(), only.end(), [&](const QString& o) { return id.startsWith(o); })) sheetIds << id;
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
    for (const QString& id : sheetIds)
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
  add(250, [=, &w] {
    check(view->grab().toImage() == *shot, "the still frame does not change");
    shot->save(dir + "/player-still.png");
    QSettings().remove("ui/tipAnimate");
    view->close();
    // The expanded rich card plays the command's clip; a command without a clip gets no slot.
    auto* button = new QToolButton(&w);
    button->setDefaultAction(w.action("design.extrude"));
    button->setGeometry(40, 40, 40, 40);
    RichTip::setActionLookup([&w](const QString& id) { return w.action(id); });
    RichTip::attach(button, "design.extrude");
    RichTip* tip = RichTip::instance();
    tip->showFor(button, RichTip::State::Expanded);
    auto* played = qobject_cast<ClipView*>(tip->clip());
    check(played && played->clip() == "design.extrude" && played->isVisible() && played->playing(), "the expanded card plays the command's clip");
    tip->grab().save(dir + "/richtip-clip.png");
    auto* other = new QToolButton(&w);
    other->setDefaultAction(w.action("file.quit"));
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
  *next = [&w, steps, next](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [steps, next, i] {
      (*steps)[i].fn();
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}

// OPAD_BENCH_GUIDE=<prefix> (a document with a box and a sketch): the guide slot of the tool panels (UI-107). The
// Distance tool's panel plays inspect.distance at its first step and moves on with the first pick; a command run more
// than ToolGuide::kUses times starts folded and unfolding it is remembered; a new extrude plays design.extrude at its
// profile pick and moves on to the distance once a profile is picked; the sketch panel plays the line tool's clip and
// follows its clicks; ui/toolGuide off removes the slot. Panels saved as <prefix>.tool/.tool-picked/.feature/.sketch.png.
OPAD_BENCH(OPAD_BENCH_GUIDE, guide) {
  const QString prefix = value;
  QSettings().setValue("ui/tipAnimate", true);
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: guide: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  auto range = [](ToolGuide* g) { return g ? QString("%1-%2").arg(g->view()->range().first).arg(g->view()->range().second) : QString("none"); };
  auto plays = [](ToolGuide* g, const QString& id) { return g && g->shown() && g->command() == id && g->view()->isVisible() && g->view()->playing(); };
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  const std::string sketch = w.m_doc->scene.sketches.empty() ? std::string() : w.m_doc->scene.sketches.front().id;
  check(!sketch.empty() && !w.m_doc->scene.all_bodies().empty(), "the document has a body and a sketch");
  auto unfoldedHeight = std::make_shared<int>(0);
  add(1500, [=, &w] {
    w.startTool("distance");
    ToolGuide* g = w.m_toolSteps->guide();
    check(plays(g, "inspect.distance"), "the Distance panel plays its clip");
    check(g && g->view()->range() == qMakePair(0, 0), "at the first pick's step (" + range(g) + ")");
    *unfoldedHeight = w.m_toolPanel->height();
    w.m_toolPanel->grab().save(prefix + ".tool.png");
  });
  add(300, [=, &w] {  // a face of the box picked, as the viewport reports a click (a hidden window paints no frame to click in)
    opad::Ref face;
    face.body = w.m_doc->scene.all_bodies().front();
    face.kind = opad::Ref::Kind::Face;
    face.index = 1;
    w.toolPicksChanged({face}, false);
  });
  add(300, [=, &w] {
    ToolGuide* g = w.m_toolSteps->guide();
    check(w.m_toolPicks.size() == 1 && g && g->view()->range() == qMakePair(1, 1), QString("a pick moves it to the second pick's step (%1 picks, %2)").arg(w.m_toolPicks.size()).arg(range(g)));
    w.m_toolPanel->grab().save(prefix + ".tool-picked.png");
    w.cancelTool();
    for (int i = 1; i < ToolGuide::kUses; ++i) { w.startTool("distance"); w.cancelTool(); }
    w.startTool("distance");
    check(g && g->shown() && !g->expanded() && !g->view()->isVisible(), QString("folded after %1 runs").arg(ToolGuide::kUses));
  });
  add(300, [=, &w] {
    ToolGuide* g = w.m_toolSteps->guide();
    check(w.m_toolPanel->height() < *unfoldedHeight, QString("the folded panel is shorter (%1 < %2 px)").arg(w.m_toolPanel->height()).arg(*unfoldedHeight));
    if (auto* head = g ? g->findChild<QToolButton*>("guideHead") : nullptr) head->click();
    check(g && g->expanded() && g->view()->isVisible(), "the header unfolds it");
    w.cancelTool();
    w.startTool("distance");
    check(g && g->expanded(), "unfolded by hand stays unfolded");
    w.cancelTool();
    w.m_design->startFeature("extrude");
    ToolGuide* f = w.m_design->featurePanel()->guide();
    check(plays(f, "design.extrude") && f->view()->range() == qMakePair(0, 0), "a new extrude plays its clip at the profile pick (" + range(f) + ")");
    w.m_design->featurePanel()->setPicks("profiles", opad::json::array({opad::json{{"sketch", sketch}, {"at", {30.0, 5.0}}}}));
    check(f && f->view()->range() == qMakePair(1, 2), "a picked profile moves it on to the distance (" + range(f) + ")");
  });
  add(600, [=, &w] {
    w.m_design->featurePanel()->grab().save(prefix + ".feature.png");
    w.m_design->escape();
    w.m_design->editOp(sketch);
  });
  add(900, [=, &w] {
    w.m_design->sketch()->setTool("line");
    auto panels = w.findChildren<SketchPanel*>();
    ToolGuide* s = panels.isEmpty() ? nullptr : panels.front()->findChild<ToolGuide*>();
    check(w.m_design->sketchActive() && plays(s, "sketch.line") && s->view()->range() == qMakePair(0, 0), "the sketch panel plays the line tool at its first point (" + range(s) + ")");
    w.m_design->sketch()->placePrecise("0", "0", 0);
    check(s && s->view()->range() == qMakePair(1, 2), "a placed point moves it on (" + range(s) + ")");
  });
  add(400, [=, &w] {
    auto panels = w.findChildren<SketchPanel*>();
    if (!panels.isEmpty()) panels.front()->grab().save(prefix + ".sketch.png");
    QSettings().setValue("ui/toolGuide", false);
    w.m_design->sketch()->setTool("rect");
    ToolGuide* s = panels.isEmpty() ? nullptr : panels.front()->findChild<ToolGuide*>();
    check(s && !s->shown(), "ui/toolGuide off: no slot");
    QSettings().remove("ui/toolGuide");
    if (i18n::current() != "en") check(s && s->findChild<QToolButton*>("guideHead")->text() != "Guide", "the header in the UI language");
    trace::log(QString("bench: guide: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [&w, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [steps, next, check, i] {
      try { (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}

// OPAD_BENCH_REFERENCE=<prefix> (a document with a box): Help > Command reference and the command palette's preview
// (UI-107). F1 opens the reference listing every command by area; with the Distance tool running it opens at Distance,
// whose clip plays and whose steps loop one by one; the search finds commands by their keywords; a command not
// available now says what it needs. The palette shows the current command's card and clip beside its list and finds
// commands by keyword; its group column names the area. Saved as <prefix>.reference/.reference-search/.palette.png.
OPAD_BENCH(OPAD_BENCH_REFERENCE, reference) {
  const QString prefix = value;
  QSettings().setValue("ui/tipAnimate", true);
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: reference: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  int listed = 0;
  for (const CommandHelp& h : help::all()) listed += !h.id.section('.', -1).startsWith("more");
  add(1000, [=, &w] {
    w.action("help.reference")->trigger();
    auto* reference = w.findChild<CommandReference*>();
    check(reference && reference->isVisible() && reference->shown().size() == listed && !reference->current().isEmpty(),
          QString("F1 opens the reference with every command (%1 of %2)").arg(reference ? reference->shown().size() : 0).arg(listed));
    if (reference) reference->hide();
    w.startTool("distance");
    w.action("help.reference")->trigger();
    check(reference && reference->isVisible() && reference->current() == "inspect.distance", "F1 while measuring opens it at Distance (" + (reference ? reference->current() : QString()) + ")");
    ClipView* clip = reference ? reference->preview()->clip() : nullptr;
    check(clip && clip->isVisible() && clip->playing() && clip->clip() == "inspect.distance", "its clip plays");
    QListWidget* list = reference ? reference->preview()->steps() : nullptr;
    check(list && list->count() == clips::steps("inspect.distance").size() + 1, "its steps are listed after All steps");
    if (list) list->setCurrentRow(2);
    check(clip && clip->range() == qMakePair(1, 1), "a click on a step loops it");
  });
  add(400, [=, &w] {
    auto* reference = w.findChild<CommandReference*>();
    reference->grab().save(prefix + ".reference.png");
    w.cancelTool();
    reference->setFilter("push pull");
    const QStringList found = reference->shown();
    bool all = !found.isEmpty();
    for (const QString& id : found) all = all && help::matches(*help::find(id), "push pull");
    check(found.contains("design.offset_face") && all, QString("the search finds Press pull by its keywords (%1)").arg(found.join(' ')));
    reference->setFilter("fillet");
    check(reference->shown().contains("design.fillet") && reference->shown().contains("sketch.fillet") && reference->preview()->command() == reference->current(), "both fillets found, the first one shown");
    reference->grab().save(prefix + ".reference-search.png");
    reference->open("view.unisolate");
    check(reference->current() == "view.unisolate" && reference->shown().size() == listed && reference->preview()->showsRequirement(), "a command not available now says what it needs");
    if (i18n::current() != "en") check(reference->layoutDirection() == Qt::RightToLeft && reference->windowTitle() != "Command reference", "the reference in the UI language and direction");
    reference->close();
    check(help::group("design.extrude") == opGroup(w.action("design.extrude")) && opGroup(w.action("sketch.line")) != opGroup(w.action("help.about")), "the palette groups commands by area");
    auto* palette = new CommandPalette(w.m_actions, &w);
    palette->setAttribute(Qt::WA_DeleteOnClose);
    palette->show();
    auto* edit = palette->findChild<QLineEdit*>("paletteInput");
    auto* preview = palette->findChild<CommandPreview*>();
    auto* items = palette->findChild<QListWidget*>("paletteList");
    edit->setText("extrude");
    check(preview && preview->command() == "design.extrude" && preview->clip()->isVisible() && preview->clip()->playing(), "the palette previews the current command with its clip (" + (preview ? preview->command() : QString()) + ")");
    edit->setText("push pull");
    QStringList ids;
    for (int i = 0; i < items->count(); ++i) ids << static_cast<QAction*>(items->item(i)->data(Qt::UserRole).value<void*>())->objectName();
    check(ids.contains("design.offset_face"), "the palette finds commands by keyword (" + ids.join(' ') + ")");
    edit->setText("fit");
  });
  add(400, [=, &w] {
    auto* palette = w.findChild<CommandPalette*>();
    if (palette) {
      palette->grab().save(prefix + ".palette.png");
      palette->close();
    }
    trace::log(QString("bench: reference: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [&w, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [steps, next, check, i] {
      try { (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}
