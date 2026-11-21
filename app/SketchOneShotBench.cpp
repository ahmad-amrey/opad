// OPAD_BENCH_SKETCH_ONESHOT=<prefix> (tools/bench_cases/sketch.py, case sketch-oneshot): a tool normally used once closes
// after its Apply, Shift keeps it (sketchkeys::afterApply). A rectangle moved 5 mm at a time: Enter applies Move and closes
// it, its panel with it; Shift+Enter applies and keeps Move; Shift+click on Apply keeps it; a plain click on Apply closes
// it again, and the Constraints page the user had opened stays (the panel is not closed by the tool). The prompt and the
// panel say that Shift keeps the tool. Rigid, a constraint tool, stays after Enter. Keys go to the view as typed ones do
// (Qt offers them to the shortcuts first, so a Shift+Enter the sketch did not take would be Repeat last command).
#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Jobs.hpp"
#include "KeyText.hpp"
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <algorithm>
#include <memory>

using namespace opad::design;

void SketchEditor::benchOneShot() {
  QWidget* window = m_viewport->window();
  auto* panel = window->findChild<SketchPanel*>();
  auto* constraintsPage = window->findChild<QAction*>("sketch.constraints");
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch one-shot: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  QPushButton* apply = nullptr;
  QLabel* hint = nullptr;
  if (panel) {
    for (auto* b : panel->findChildren<QPushButton*>())
      if (b->property("sketchApply").toBool()) apply = b;
    for (auto* l : panel->findChildren<QLabel*>())
      if (l->property("sketchKeepHint").toBool()) hint = l;
  }
  if (!panel || !apply || !hint || !constraintsPage) {
    check(false, "the sketch panel, its Apply button and hint, and the Constraints page command exist");
    return QCoreApplication::exit(2);
  }
  auto send = [this](int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QApplication::setActiveWindow(m_viewport->window());
    m_viewport->setFocus();
    QKeyEvent press(QEvent::KeyPress, key, mods);  // not spontaneous: Qt sends it as a shortcut override first
    QApplication::sendEvent(m_viewport, &press);
  };
  auto clickApply = [apply](Qt::KeyboardModifiers mods) {
    const QPointF local = QRectF(apply->rect()).center(), global = apply->mapToGlobal(local);
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, mods);
    QApplication::sendEvent(apply, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, mods);
    QApplication::sendEvent(apply, &release);
  };
  auto panelShown = [panel] { return panel->window()->isVisible(); };
  auto left = [this] {  // the rectangle's left side (its lines' points: the sketch has others, the origin's)
    double out = 1e300;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Line)
        for (int id : e.p)
          if (const SkPoint* p = m_sk.point(id)) out = std::min(out, p->x);
    return out;
  };
  std::vector<int> lines;
  QApplication::setActiveWindow(window);
  m_viewport->setFocus();
  m_viewport->setGridSnap(false);
  setTool("rect");
  placePrecise("0", "0", 0);
  placePrecise("40", "30", 0);
  setTool("select");
  for (const auto& e : m_sk.entities)
    if (e.type == SkEntity::Type::Line) lines.push_back(e.id);
  check(lines.size() == 4, "a rectangle drawn: left " + QString::number(left()));
  auto move = [this, lines] {  // the rectangle picked, Move 5 mm along X
    m_sel = lines;
    setTool("move");
    m_options["dx"] = "5 mm";
    m_options["dy"] = "0 mm";
    rebuild();
    emit changed();
  };
  move();
  check(m_tool == "move" && panelShown(), "Move opens with its panel");
  check(sketchkeys::keepsOnShiftEnter(keyState()) && keyHints().contains(keys::fixedText("shiftEnter")), "the prompt says Shift+Enter applies and keeps the tool: " + keyHints());
  check(hint->isVisible() && hint->text().contains(keys::fixedText("shiftEnter")), "the panel says Shift keeps the tool open: " + hint->text());
  send(Qt::Key_Return);

  auto phase = std::make_shared<int>(0), ticks = std::make_shared<int>(0);
  auto* timer = new QTimer(this);
  timer->setInterval(50);
  connect(timer, &QTimer::timeout, this, [=] {
    if (++*ticks > 400) {
      check(false, QString("phase %1 in time").arg(*phase));
      timer->stop();
      return QCoreApplication::exit(2);
    }
    if (m_editJob || m_toolPreviewTimer.isActive()) return;
    switch ((*phase)++) {
      case 0:
        check(std::abs(left() - 5) < 1e-6, "Enter moves the rectangle 5 mm: left " + QString::number(left()));
        check(m_tool == "select" && !panelShown(), "Enter closes Move and its panel");
        move();
        send(Qt::Key_Return, Qt::ShiftModifier);
        break;
      case 1:
        check(std::abs(left() - 10) < 1e-6, "Shift+Enter moves it again: left " + QString::number(left()));
        check(m_tool == "move" && panelShown() && m_sel.empty(), "Shift+Enter keeps Move open for the next curves");
        m_sel = lines;
        rebuild();
        emit changed();
        clickApply(Qt::ShiftModifier);
        break;
      case 2:
        check(std::abs(left() - 15) < 1e-6 && m_tool == "move" && panelShown(), "Shift+click on Apply applies and keeps Move open: left " + QString::number(left()));
        setTool("select");
        constraintsPage->trigger();  // the user opens the Constraints page, then a tool
        check(panelShown() && panel->userPage() == 2, "the Constraints page is open");
        move();
        clickApply(Qt::NoModifier);
        break;
      case 3:
        check(std::abs(left() - 20) < 1e-6 && m_tool == "select", "a plain click on Apply applies and closes Move: left " + QString::number(left()));
        check(panelShown() && panel->userPage() == 2, "the Constraints page the user opened stays open");
        // Rigid is a constraint tool: pick, hold, pick again.
        setTool("c:rigid");
        m_picked = lines;
        send(Qt::Key_Return);
        check(m_tool == "c:rigid" && std::any_of(m_sk.constraints.begin(), m_sk.constraints.end(), [](const SkConstraint& c) { return c.type == SkConstraint::Type::Rigid; }),
              "Rigid holds the curves on Enter and stays for the next ones");
        check(!sketchkeys::closesAfterApply("c:rigid") && !hint->isVisible(), "a tool that stays shows no keep hint");
        setTool("select");
        timer->stop();
        trace::log(QString("bench: sketch one-shot: Enter and Apply close a one-shot tool, Shift keeps it %1").arg(*ok ? "PASS" : "FAIL"));
        QCoreApplication::exit(*ok ? 0 : 2);
        break;
    }
  });
  timer->start();
}
