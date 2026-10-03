// Bench of the i18n holes and small polish (UI-116): what the view, the chips, the timeline, the undo labels, the tool
// panel and the feature panel say, in the UI's language.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "CommandHelp.hpp"
#include "DesignPanels.hpp"
#include "I18n.hpp"
#include "Theme.hpp"
#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QSettings>
#include <QToolTip>

// OPAD_BENCH_POLISH=<prefix> (a box and the sketch "Plate", the help area's guided.opad): the standard views are named
// ("Top view", never "View: top"); the hover says "face 3" in the UI's language; the section chip is translated; the
// timeline's tooltip names an op's target in it; undoing a note's new tag says "edit note", not "pin measurement"; the
// Distance tool's panel does not repeat its header and the step it waits for under the steps (its guide plays, or the
// tool's one sentence); Ctrl+Z in a feature's panel takes back its last pick, one at a time, then says there is none,
// the document's history untouched; the value echo under an expression box has a row of its own and stays empty for a
// plain number; the hidden-object warning takes the theme's warning colour. Saved as <prefix>.feature.png.
OPAD_BENCH(OPAD_BENCH_POLISH, polish) {
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: polish: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  const bool arabic = i18n::current() == "ar";
  auto tr = [](const char* text) { return QCoreApplication::translate("MainWindow", text); };
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  const std::string box = w.m_doc->scene.all_bodies().empty() ? std::string() : w.m_doc->scene.all_bodies().front();

  add(600, [=, &w] {
    QStringList names;
    for (const char* id : {"view.top", "view.front", "view.right", "view.iso", "view.bottom", "view.back", "view.left"}) names << w.action(id)->text();
    check(names.first() == tr("Top view") && names.filter("View:").isEmpty() && names.filter("top").isEmpty() && (!arabic || names.first() != "Top view"),
          "the standard views are named (" + names.join(", ") + ")");
    w.action("select.faces")->trigger();  // sliced: the bodies take the face mode in the next turns
  });
  add(500, [=, &w] {
    int x = 0, y = 0;
    const bool found = !box.empty() && w.m_viewport->benchBodyPoint(box, x, y) && w.m_viewport->benchDetect(x, y);
    QPaintEvent paint(w.m_viewport->rect());  // the hover text is made where a frame is flushed; a hidden window paints none
    QApplication::sendEvent(w.m_viewport, &paint);
    const QString hover = w.m_statusHover->text(), face = i18n::t("face");
    check(found && hover.contains(QString::fromUtf8("› ") + face + " ") && (!arabic || !hover.contains("face")), "the hover names the face in the UI's language (" + hover + ")");
    w.action("select.bodies")->trigger();
    w.action("inspect.section")->trigger();
  });
  add(400, [=, &w] {
    const QString prefixWord = tr("Section %1 = %2").section('%', 0, 0);
    QString chip;
    for (QLabel* label : w.m_chips->findChildren<QLabel*>())
      if (label->isVisibleTo(w.m_chips) && label->text().startsWith(prefixWord)) chip = label->text();
    check(!chip.isEmpty() && (!arabic || !chip.startsWith("Section")), "the section chip is translated (" + chip + ")");
    w.action("inspect.section")->trigger();
    // An op with a target: the timeline's tooltip names it.
    w.m_doc->run("appearance", opad::json{{"targets", {box}}, {"color", {0.8, 0.3, 0.2}}});
  });
  add(400, [=, &w] {
    const QString want = QCoreApplication::translate("TimelineWidget", "Target: %1").arg(w.m_doc->nodeName(box).toHtmlEscaped());
    QString tip;
    for (int x = 138; x < w.m_timeline->width() - 72 && tip.isEmpty(); x += 26) {  // the markers' centres, 26 px apart
      QMouseEvent move(QEvent::MouseMove, QPointF(x, 22), w.m_timeline->mapToGlobal(QPointF(x, 22)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(w.m_timeline, &move);
      if (QToolTip::text().contains(want)) tip = QToolTip::text();
    }
    QToolTip::hideText();
    check(!tip.isEmpty() && !tip.contains(">target "), "the timeline's tooltip names the target in the UI's language (" + want + ")");
    // Undo labels: a pinned measurement, then a new tag on it.
    const auto pinned = w.m_doc->run("append", opad::json{{"op", {{"op", "measurement"}, {"kind", "distance"}, {"refs", {"point/0,0,0", "point/3,0,0"}},
                                                                   {"result", {{"value", 3}, {"unit", "mm"}}}}}});
    const QString pin = w.m_doc->undoLabel();
    w.restyleAnnotation(pinned["appended"][0].get<std::string>(), "warning");
    check(pin == QCoreApplication::translate("AppDocument", "pin measurement") && w.m_doc->undoLabel() == QCoreApplication::translate("AppDocument", "edit note"),
          "undo says pin measurement, then edit note for the new tag (" + pin + ", " + w.m_doc->undoLabel() + ")");
    // The Distance tool's panel.
    QSettings().setValue("ui/toolGuide", true);
    w.startTool("distance");
  });
  add(300, [=, &w] {
    const QStringList guided = w.m_toolSteps->summary();
    const QString waiting = w.toolSteps().value(0).label;
    check(guided.isEmpty(), "with its guide playing, no summary repeats the header and the waiting step (" + guided.join(" | ") + ")");
    QSettings().setValue("ui/toolGuide", false);
    w.refreshToolUi();
    const QStringList plain = w.m_toolSteps->summary();
    const CommandHelp* h = help::find("inspect.distance");
    check(h && plain.size() == 3 && plain[0].isEmpty() && plain[1] == h->summary && !plain[1].contains(waiting), "without it, the tool's one sentence (" + plain.join(" | ") + ")");
    QSettings().setValue("ui/toolGuide", true);
    w.cancelTool();
    // Ctrl+Z in a feature's panel: a fillet with two edges picked.
    w.m_design->startFeature("fillet");
  });
  add(400, [=, &w] {
    FeaturePanel* form = w.m_design->featurePanel();
    const QStringList history = w.m_doc->undoLabels();
    form->setPicks("edges", opad::json::array({opad::json{{"body", box}, {"kind", "edge"}, {"index", 0}}, opad::json{{"body", box}, {"kind", "edge"}, {"index", 1}}}));
    QAction* undo = w.action("edit.undo");
    check(w.m_design->featureActive() && undo->isEnabled() && undo->text() == QCoreApplication::translate("MainWindow", "&Undo last pick"), "Undo is the last pick's while the panel is open (" + undo->text() + ")");
    undo->trigger();
    const size_t one = form->picks("edges").size();
    undo->trigger();
    const size_t none = form->picks("edges").size();
    const bool third = w.m_design->undoPick();
    check(one == 1 && none == 0 && !third && w.m_doc->undoLabels() == history, QString("Ctrl+Z takes the picks back one at a time, then has none; the document's history is untouched (%1, %2)").arg(one).arg(none));
    check(w.m_promptText == QCoreApplication::translate("DesignController", "No pick to take back · Esc closes the feature"), "it says there is none (" + w.m_promptText + ")");
    w.m_design->escape();
    w.m_design->startFeature("extrude");
  });
  add(500, [=, &w] {
    FeaturePanel* form = w.m_design->featurePanel();
    bool rows = true, quiet = false, shown = false;
    for (ExprEdit* expr : form->findChildren<ExprEdit*>()) {
      if (!expr->isVisibleTo(form)) continue;
      auto* echo = expr->findChild<QLabel*>();
      rows = rows && echo && echo->geometry().top() >= expr->lineEdit()->geometry().bottom() && echo->height() >= 12;
      if (expr->lineEdit()->text() == "10 mm" || expr->lineEdit()->text() == "0 deg") quiet = quiet || (echo && echo->text().isEmpty());
    }
    ExprEdit* distance = nullptr;
    for (ExprEdit* expr : form->findChildren<ExprEdit*>())
      if (expr->isVisibleTo(form) && expr->lineEdit()->text().endsWith("mm")) { distance = expr; break; }
    if (distance) {
      distance->setText("6 mm * 2");
      auto* echo = distance->findChild<QLabel*>();
      shown = echo && echo->text().startsWith("= ") && echo->text().contains("12");
    }
    check(rows, "each value echo has its own row under its box");
    check(quiet && shown, "the echo stays empty for a plain number and shows an expression's value");
    form->setEditHidden(true);
    QLabel* warning = nullptr;
    for (QLabel* label : form->findChildren<QLabel*>())
      if (label->text() == QCoreApplication::translate("FeaturePanel", "The object being edited is hidden. Show it in the browser to see the result.")) warning = label;
    check(warning && warning->styleSheet().contains(theme::css(theme::current().warning)), "the hidden-object warning takes the theme's warning colour");
    form->setEditHidden(false);
    if (ToolPanel* panel = w.m_featurePanel) panel->grab().save(prefix + ".feature.png");
    w.m_design->escape();
  });
  add(0, [=] {
    trace::log(QString("bench: polish: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
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
