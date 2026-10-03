#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "DimensionHandle.hpp"
#include "ShortcutEditor.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QElapsedTimer>
#include <QApplication>
#include <QKeyEvent>
#include <QPushButton>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_KEYS=<prefix> (TODO 11 UI-16): typed values through key events, sent where the keyboard is (the box a
// digit started takes it). Offset: a distance typed (a keypad digit first) before anything is picked waits, Enter keeps it,
// the picked chain's preview and box use it, a keypad digit typed into the box by the arrow replaces it, Enter applies it,
// the value is still there after Apply, and a value typed while the tool panel has the keyboard reaches the tool, Esc puts
// the old value back, the next Esc closes the tool. A polyline typed: X, Tab, Y, Shift+Tab and Tab, Enter; length, Tab,
// angle, Enter; "20,0" (a comma moves on); keypad digits and the keypad's Enter. A click takes the typed values. Move: X,
// Tab, Y, Enter at once. Tab never takes the keyboard off the view, and 5, 6 and 7 never switch the display style in a
// sketch (their keys are let go there).
void SketchEditor::benchKeys() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_KEYS");
  QWidget* window = m_viewport->window();
  auto* panel = window->findChild<SketchPanel*>();
  auto* wire = window->findChild<QAction*>("view.wire");
  auto* edges = window->findChild<QAction*>("view.edges");
  auto* shaded = window->findChild<QAction*>("view.shaded");
  auto* handleBox = m_dimensionHandle->findChild<QLineEdit*>();
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch keys: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  if (!panel || !wire || !edges || !shaded || !handleBox) {
    check(false, "the tool panel, the display style actions and the offset's box exist");
    return QCoreApplication::exit(2);
  }
  const QAction* style = shaded->isChecked() ? shaded : edges->isChecked() ? edges : wire;
  auto sameStyle = [=] { return style->isChecked(); };
  // The keyboard: the widget with the focus (a box takes it when a digit starts it), else the view.
  auto keyboard = [this]() -> QWidget* { QWidget* w = QApplication::focusWidget(); return w ? w : m_viewport; };
  auto send = [keyboard](int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);  // not spontaneous: Qt sends it as a shortcut override first
    QApplication::sendEvent(keyboard(), &press);
  };
  auto type = [send](const QString& chars, bool keypad = false) {
    for (const QChar c : chars) {
      const int key = c.isDigit() ? Qt::Key_0 + c.digitValue() : c == '.' ? Qt::Key_Period : c == ',' ? Qt::Key_Comma : c == '-' ? Qt::Key_Minus : Qt::Key_unknown;
      send(key, keypad ? Qt::KeypadModifier : Qt::NoModifier, QString(c));
    }
  };
  auto box = [this](int i) { return m_input->box(i) ? m_input->box(i)->text() : QStringLiteral("<none>"); };
  auto at = [this](int id, double u, double v) { const SkPoint* p = m_sk.point(id); return p && std::abs(p->x - u) < 1e-6 && std::abs(p->y - v) < 1e-6; };
  auto extent = [](const Sketch& sk, bool maximum) {
    double out = maximum ? -1e300 : 1e300;
    for (const auto& p : sk.points) out = maximum ? std::max(out, p.x) : std::min(out, p.x);
    return out;
  };
  QApplication::setActiveWindow(window);
  m_viewport->setFocus();
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {50, 15, 100}}, {"target", {50, 15, 0}}, {"up", {0, 1, 0}}, {"scale", 90}, {"projection", "orthographic"}, {"absolute", true}});
  setTool("rect");
  placePrecise("0", "0", 0);
  placePrecise("40", "30", 0);
  placePrecise("60", "0", 0);
  placePrecise("100", "30", 0);
  setTool("select");
  check(m_sk.entities.size() == 8, "two rectangles drawn");
  check(wire->shortcut().isEmpty() && shortcuts::binding(wire) == QKeySequence("7") && shaded->isEnabled(),
        "the display styles keep their keys outside the sketch only and stay in the menus");
  send(Qt::Key_6, Qt::NoModifier, "6");
  check(sameStyle() && m_tool == "select", "6 with the select tool does not switch the display style");

  // Offset: a distance typed before anything is picked waits for the curves.
  setTool("offset");
  sketchMove(20, 45, Qt::NoModifier, false);
  check(m_input->isVisible() && m_input->count() == 1 && m_input->key(0) == "distance" && m_input->box(0)->placeholderText() == "5 mm" && !m_dimensionHandle->isVisible(),
        "the offset shows its distance box beside the pointer before anything is picked");
  const QPoint pointer = m_viewport->widgetPoint(m_frame.to_world(20, 45));
  check(!m_input->geometry().contains(pointer) && m_input->geometry().adjusted(-40, -40, 40, 40).contains(pointer), "the box sits beside the pointer, never under it");
  type("1", true);
  check(QApplication::focusWidget() == m_input->box(0), "a digit typed over the view starts the box, which takes the keyboard");
  type("2");
  check(option("distance") == "12" && box(0) == "12" && m_input->typed() && m_tool == "offset" && sameStyle(),
        "a keypad digit and a top-row digit type 12 (no display style, no filter change)");
  check(keyHints().contains("Enter use typed values") && keyHints().contains("Esc drop typed values"), "the prompt says what Enter and Esc do with the typed value");
  m_input->grab().save(prefix + ".offset-input.png");
  send(Qt::Key_Return);
  check(option("distance") == "12" && !m_input->typed() && m_input->box(0)->placeholderText() == "12" && m_tool == "offset" && m_sel.empty(),
        "Enter with nothing picked keeps 12 waiting");
  sketchPress(20, 0, Qt::NoModifier);
  check(m_sel.size() == 4 && m_dimensionHandle->isVisible() && handleBox->text() == "12" && !m_input->isVisible(), "the picked chain gets the box by the arrow, showing 12");

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
      case 0: {
        check(m_toolPreview && std::abs(extent(*m_toolPreview, false) + 12) < 1e-6, "the preview offsets by the 12 typed before the pick");
        type("8", true);
        check(handleBox->text() == "8" && option("distance") == "8" && sameStyle(), "a keypad 8 typed with the curves picked replaces the value in the box by the arrow");
        break;
      }
      case 1: {
        check(m_toolPreview && std::abs(extent(*m_toolPreview, false) + 8) < 1e-6, "the preview follows to 8");
        m_dimensionHandle->grab().save(prefix + ".handle.png");
        send(Qt::Key_Return);
        check(m_sk.entities.size() == 16 && std::abs(extent(m_sk, false) + 8) < 1e-6 && m_sel.empty(), "Enter applies the offset by 8");
        check(m_tool == "offset" && !m_dimensionHandle->isVisible() && m_input->isVisible() && m_input->box(0)->placeholderText() == "8", "after Apply the distance box is back and still says 8");
        m_viewport->setFocus();
        type("3");
        check(option("distance") == "3" && m_input->typed(), "3 typed after Apply waits for the next curves");
        sketchPress(80, 0, Qt::NoModifier);
        check(m_sel.size() == 4 && handleBox->text() == "3", "the next chain takes 3");
        break;
      }
      case 2: {
        check(m_toolPreview && std::abs(extent(*m_toolPreview, true) - 103) < 1e-6, "its preview offsets by 3");
        send(Qt::Key_Return);
        check(m_sk.entities.size() == 24 && std::abs(extent(m_sk, true) - 103) < 1e-6, "Enter applies it");
        // The tool panel has the keyboard (a click on one of its buttons): digits still reach the tool.
        QCoreApplication::processEvents();  // Apply rebuilt the panel's fields: Qt shows the new ones a turn later
        QPushButton* preview = nullptr;
        for (auto* b : panel->findChildren<QPushButton*>())
          if (b->text() == "Preview" && b->isVisible()) preview = b;
        check(preview && panel->window() != window && panel->window()->isVisible(), "the tool panel, a window of its own, shows the offset's Preview button");
        if (!preview) break;
        QApplication::setActiveWindow(panel->window());
        preview->setFocus(Qt::MouseFocusReason);
        check(QApplication::focusWidget() == preview, "a button of the tool panel has the keyboard");
        type("4");
        type("5");
        check(option("distance") == "45" && box(0) == "45" && sameStyle() && m_tool == "offset", "45 typed with the tool panel focused reaches the offset");
        send(Qt::Key_Escape);
        check(option("distance") == "3" && !m_input->typed() && m_tool == "offset", "Esc drops 45 and puts 3 back");
        send(Qt::Key_Escape);
        check(m_tool == "select", "the next Esc closes the tool");
        QApplication::setActiveWindow(window);
        m_viewport->setFocus();

        // A polyline from the keyboard: X Tab Y, Enter; length Tab angle, Enter; a comma moves on; the keypad.
        setTool("line");
        QElapsedTimer moving;
        moving.start();
        for (int i = 0; i < 200; ++i) sketchMove(5 + 0.37 * i, 60 - 0.21 * i, Qt::NoModifier, false);  // the boxes follow the pointer
        const double perMove = moving.nsecsElapsed() / 200e6;
        moving.restart();
        for (int i = 0; i < 200; ++i) {
          m_cursor.u = 5 + 0.37 * i;
          m_cursor.v = 60 - 0.21 * i;
          updateInput();
        }
        const double boxes = moving.nsecsElapsed() / 200e6;
        sketchMove(5, 60, Qt::NoModifier, false);
        check(boxes < 1.5, QString("the boxes follow the pointer in %1 ms of a %2 ms move").arg(boxes, 0, 'f', 3).arg(perMove, 0, 'f', 3));
        check(m_input->count() == 2 && m_input->key(0) == "x" && m_input->key(1) == "y" && m_input->box(0)->placeholderText().startsWith("5"),
              "the line's first point has X and Y boxes, the pointer's X shown: " + m_input->box(0)->placeholderText());
        type("10");
        send(Qt::Key_Tab);
        check(m_input->current() == 1 && QApplication::focusWidget() == m_input->box(1) && box(0) == "10", "Tab goes from X to Y and keeps 10");
        type("50");
        m_input->grab().save(prefix + ".point-input.png");
        send(Qt::Key_Backtab, Qt::ShiftModifier);
        check(m_input->current() == 0 && QApplication::focusWidget() == m_input->box(0), "Shift+Tab goes back to X");
        send(Qt::Key_Tab);
        check(m_input->current() == 1 && box(1) == "50", "Tab again to Y, 50 kept");
        send(Qt::Key_Return);
        check(m_chain.size() == 1 && at(m_chain[0], 10, 50) && m_tool == "line", "Enter starts the line at (10, 50)");
        check(m_input->count() == 2 && m_input->key(0) == "length" && m_input->key(1) == "angle", "then the boxes are length and angle");
        type("30");
        send(Qt::Key_Tab);
        type("90");
        send(Qt::Key_Return);
        check(m_chain.size() == 2 && at(m_chain[1], 10, 80), "30 at 90 degrees goes to (10, 80)");
        type("20,0");
        check(box(0) == "20" && box(1) == "0" && m_input->current() == 1, "a comma moves on from length to angle");
        send(Qt::Key_Return);
        check(m_chain.size() == 3 && at(m_chain[2], 30, 80), "20 at 0 degrees goes to (30, 80)");
        type("15", true);
        send(Qt::Key_Tab);
        type("0", true);
        send(Qt::Key_Enter, Qt::KeypadModifier);
        check(m_chain.size() == 4 && at(m_chain[3], 45, 80), "keypad digits and the keypad's Enter go to (45, 80)");
        send(Qt::Key_Escape);
        check(m_chain.empty() && m_tool == "line", "Esc ends the chain");
        int lines = 0;
        for (const auto& e : m_sk.entities) lines += e.type == SkEntity::Type::Line;
        check(lines == 8 + 8 + 3, "three segments were drawn");

        // A click takes the typed values (the pointer gives what is left empty).
        setTool("rect");
        sketchMove(150, 10, Qt::NoModifier, false);
        type("200");
        sketchPress(150, 10, Qt::NoModifier);
        check(m_clicks.size() == 1 && std::abs(m_clicks[0].u - 200) < 1e-9 && std::abs(m_clicks[0].v - 10) < 1e-9, "a click with X typed puts the corner at X 200, the pointer's Y");
        send(Qt::Key_Escape);
        send(Qt::Key_Escape);
        check(m_tool == "select", "Esc cancels the corner, then closes the tool");

        // Tab with the select tool keeps the keyboard on the view; tools without boxes drop digits.
        m_viewport->setFocus();
        send(Qt::Key_Tab);
        check(QApplication::focusWidget() == m_viewport, "Tab does not take the keyboard off the view");
        setTool("trim");
        type("5");
        check(sameStyle() && m_tool == "trim", "5 with the trim tool goes nowhere");
        setTool("select");
        type("7");
        check(sameStyle(), "7 with the select tool does not switch to wireframe");

        // Two option boxes: Move's X and Y offsets, Tab between them, Enter right after typing moves the line.
        setTool("line");
        placePrecise("200", "200", 0);
        placePrecise("220", "200", 0);
        send(Qt::Key_Escape);
        const int moved = m_sk.entities.back().id;
        setTool("move");
        sketchMove(210, 200, Qt::NoModifier, false);
        sketchPress(210, 200, Qt::NoModifier);
        check(m_sel == std::vector<int>{moved} && m_input->count() == 2 && m_input->key(0) == "dx" && m_input->key(1) == "dy", "Move with a line picked has X and Y offset boxes");
        type("5");
        send(Qt::Key_Tab);
        type("2");
        check(option("dx") == "5" && option("dy") == "2" && m_input->current() == 1, "5, Tab, 2 set both offsets");
        send(Qt::Key_Return);
        break;
      }
      case 3: {
        const SkEntity* line = nullptr;
        for (const auto& e : m_sk.entities)
          if (e.type == SkEntity::Type::Line && e.p.size() == 2 && at(e.p[0], 205, 202) && at(e.p[1], 225, 202)) line = &e;
        check(line && !m_input->typed() && m_tool == "move", "Enter right after typing moves the line by (5, 2)");
        end();
        QCoreApplication::processEvents();
        check(wire->shortcut() == QKeySequence("7") && !wire->property("heldShortcut").isValid(), "the display styles have their keys again once the sketch is closed");
        timer->stop();
        QCoreApplication::exit(*ok ? 0 : 2);
        break;
      }
    }
  });
  timer->start();
}
