#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QPushButton>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_LADDER=<prefix> (TODO 11 UI-20): Undo point, Done and the Esc ladder through key events and the tool
// panel's buttons. A polyline: Backspace takes the last segment back and the drawing goes on from the point before (the
// panel's Undo point too), Enter ends the chain and keeps the tool (the chain is one undo step), Backspace and Enter with
// nothing in progress neither delete nor leave; Esc ends a chain and a second Esc closes the tool, alike from the view,
// from a field of the tool panel and through the main window; keys no panel button takes are the sketch's, a field keeps
// its own; Close tool keeps a chain's segments; a spline, a rectangle and a dimension take their last point or pick back
// without leaving the tool. At each step the prompt bar and the panel say what the keys do.
void SketchEditor::benchLadder() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_LADDER");
  bool ok = true;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sketch ladder: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  QWidget* window = m_viewport->window();
  auto* panel = window->findChild<SketchPanel*>();
  auto* undoButton = window->findChild<QPushButton*>("sketchUndoPoint");
  auto* closeButton = window->findChild<QPushButton*>("sketchCloseTool");
  QPushButton* doneButton = nullptr;
  for (auto* b : window->findChildren<QPushButton*>())
    if (b->property("sketchApply").toBool()) doneButton = b;
  PromptBar* prompt = nullptr;
  for (auto* bar : m_viewport->findChildren<PromptBar*>(Qt::FindDirectChildrenOnly))
    if (bar->objectName().isEmpty()) prompt = bar;
  auto* clear = window->findChild<QAction*>("inspect.clear");
  auto* field = window->findChild<QLineEdit*>("sketchPreciseU");  // a text field of the panel that stays
  if (!panel || !undoButton || !closeButton || !doneButton || !prompt || !clear || !field) {
    check(false, "the panel's buttons, the prompt bar and the main window's Esc exist");
    return QCoreApplication::exit(2);
  }
  // A key pressed with that widget focused: Qt sends a press that is not spontaneous as a shortcut override to the focus
  // first (the sketch takes its keys there), then through the shortcut map, then as the press.
  auto key = [](QWidget* to, int k) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
    QApplication::sendEvent(to, &press);
  };
  auto place = [&](double u, double v, Qt::KeyboardModifiers mods = Qt::AltModifier) {  // Alt: exactly there
    sketchMove(u, v, mods, false);
    sketchPress(u, v, mods);
    sketchRelease(u, v, mods);
  };
  auto lines = [&] {
    int n = 0;
    for (const auto& e : m_sk.entities) n += e.type == SkEntity::Type::Line;
    return n;
  };
  auto at = [&](int id, double u, double v) { const SkPoint* p = m_sk.point(id); return p && std::abs(p->x - u) < 1e-6 && std::abs(p->y - v) < 1e-6; };
  auto says = [&](const QString& hints) { return prompt->hints() == hints && keyHints() == hints; };
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {15, 20, 100}}, {"target", {15, 20, 0}}, {"up", {0, 1, 0}}, {"scale", 60}, {"projection", "orthographic"}, {"absolute", true}});

  // A polyline from the view.
  setTool("line");
  check(says("Esc close tool") && undoButton->isVisibleTo(panel) && !undoButton->isEnabled() && closeButton->isVisibleTo(panel) && doneButton->text() == "Done   Enter" && !doneButton->isEnabled(),
        "the line tool waits: Esc closes it, nothing to take back or finish");
  const size_t undo = m_undo.size();
  place(0, 0);
  place(30, 0);
  place(30, 20);
  place(10, 30);
  check(m_chain.size() == 4 && lines() == 3 && m_undo.size() == undo + 4, "four points: three segments, one undo step each while drawing");
  check(says(QString::fromUtf8("⌫ undo point · Enter/Esc end chain")) && undoButton->isEnabled() && undoButton->text() == QString::fromUtf8("Undo point   ⌫") && doneButton->isEnabled(),
        "the prompt and the panel offer Undo point and Done");
  sketchMove(0, 40, Qt::AltModifier, false);
  m_viewport->grabImage().save(prefix + ".chain.png");
  const int third = m_chain[2];
  key(m_viewport, Qt::Key_Backspace);
  check(m_tool == "line" && m_chain.size() == 3 && m_chain.back() == third && lines() == 2 && m_undo.size() == undo + 3, "Backspace takes the last segment back and keeps drawing");
  sketchMove(0, 40, Qt::AltModifier, false);
  m_viewport->grabImage().save(prefix + ".undo.png");
  prompt->grab().save(prefix + ".prompt.png");
  panel->grab().save(prefix + ".panel.png");
  place(0, 40);
  const SkEntity segment = m_sk.entities.back();
  check(m_chain.size() == 4 && lines() == 3 && segment.type == SkEntity::Type::Line && segment.p[0] == third && at(segment.p[1], 0, 40), "the next click draws on from the point before");
  undoButton->click();
  check(m_tool == "line" && m_chain.size() == 3 && lines() == 2, "the panel's Undo point does the same");
  key(m_viewport, Qt::Key_Return);
  check(m_tool == "line" && m_chain.empty() && lines() == 2 && m_undo.size() == undo + 1 && says("Esc close tool") && !doneButton->isEnabled(),
        "Enter ends the chain and keeps its segments and the tool; the chain is one undo step");
  m_sel = {m_sk.entities.back().id};  // Backspace used to delete a selection whatever the tool
  key(m_viewport, Qt::Key_Return);
  key(m_viewport, Qt::Key_Backspace);
  check(m_tool == "line" && lines() == 2 && m_undo.size() == undo + 1, "Enter and Backspace with nothing in progress neither leave the tool nor delete");
  m_sel.clear();

  // Esc: the chain, then the tool; from the view, from a field of the tool panel, through the main window.
  place(50, 0);
  place(70, 0);
  place(70, 15);
  key(m_viewport, Qt::Key_Escape);
  check(m_tool == "line" && m_chain.empty() && lines() == 4, "Esc ends the chain and keeps its segments and the tool");
  key(m_viewport, Qt::Key_Escape);
  check(m_tool == "select", "a second Esc closes the tool");
  setTool("line");
  place(90, 0);
  place(110, 0);
  key(field, Qt::Key_Escape);
  check(m_tool == "line" && m_chain.empty() && lines() == 5, "Esc in a field of the tool panel ends the chain");
  key(field, Qt::Key_Escape);
  check(m_tool == "select", "a second Esc there closes the tool");
  setTool("line");
  place(130, 0);
  place(150, 0);
  clear->trigger();
  check(m_tool == "line" && m_chain.empty() && lines() == 6, "the main window's Esc ends the chain");
  clear->trigger();
  check(m_tool == "select", "and then closes the tool");

  // The panel: a text field keeps Backspace, a button passes Backspace and Enter on; Close tool keeps the segments.
  setTool("line");
  place(0, -20);
  place(20, -20);
  place(40, -20);
  const QString text = field->text();
  field->setText("12");
  key(field, Qt::Key_Backspace);
  check(field->text() == "1" && m_chain.size() == 3, "Backspace in a panel field edits the field");
  field->setText(text);
  key(doneButton, Qt::Key_Backspace);
  check(m_tool == "line" && m_chain.size() == 2 && lines() == 7, "Backspace on a panel button takes the last point back");
  key(doneButton, Qt::Key_Return);
  check(m_tool == "line" && m_chain.empty() && lines() == 7, "Enter on a panel button ends the chain");
  place(60, -20);
  place(80, -20);
  closeButton->click();
  check(m_tool == "select" && m_chain.empty() && lines() == 8, "Close tool ends the chain, keeps its segment and leaves the tool");

  // A spline: its points go back one by one, Enter makes it from the rest and keeps the tool.
  setTool("spline");
  const size_t points = m_sk.points.size(), entities = m_sk.entities.size();
  place(3.3, 63.7, Qt::NoModifier);  // Alt on a spline's first point inserts a node into a finished spline
  place(20, 75);
  place(40, 60);
  place(60, 75);
  check(m_chain.size() == 4 && m_sk.points.size() == points + 4 && says(QString::fromUtf8("⌫ undo point · Enter/Esc finish spline")), "a spline's points, and Enter or Esc finishes it");
  key(m_viewport, Qt::Key_Backspace);
  check(m_tool == "spline" && m_chain.size() == 3 && m_sk.points.size() == points + 3, "Backspace takes the spline's last point back");
  key(m_viewport, Qt::Key_Return);
  check(m_tool == "spline" && m_chain.empty() && m_sk.entities.size() == entities + 1 && m_sk.entities.back().type == SkEntity::Type::Spline && m_sk.entities.back().p.size() == 7,
        "Enter makes the spline through the three points left and keeps the tool");
  key(m_viewport, Qt::Key_Escape);
  check(m_tool == "select", "Esc then closes the spline tool");

  // A rectangle: its corner goes back, the tool stays.
  setTool("rect");
  const size_t shapes = m_sk.entities.size();
  place(0, 100);
  check(m_clicks.size() == 1 && says(QString::fromUtf8("⌫ undo point · Esc cancel shape")), "a rectangle's first corner: Backspace or Esc takes it back");
  key(m_viewport, Qt::Key_Backspace);
  check(m_tool == "rect" && m_clicks.empty() && m_sk.entities.size() == shapes && says("Esc close tool"), "Backspace takes the corner back, the tool stays");
  key(m_viewport, Qt::Key_Backspace);
  check(m_tool == "rect", "Backspace with nothing to take back stays in the tool");
  place(0, 100);
  place(20, 120);
  check(m_sk.entities.size() == shapes + 4, "the rectangle is drawn after that");
  key(m_viewport, Qt::Key_Escape);
  check(m_tool == "select", "Esc closes the rectangle tool");

  // A dimension: its pick goes back.
  setTool("dimension");
  place(60, 0, Qt::NoModifier);  // the line from (50, 0) to (70, 0)
  check(m_picked.size() == 1 && m_placingDim && undoButton->text() == QString::fromUtf8("Undo pick   ⌫") && says(QString::fromUtf8("⌫ undo pick · Esc clear picks")), "a dimension's pick: Undo pick");
  key(m_viewport, Qt::Key_Backspace);
  check(m_tool == "dimension" && m_picked.empty() && !m_placingDim && says("Esc close tool"), "Backspace takes the pick back, the tool stays");
  key(m_viewport, Qt::Key_Escape);
  check(m_tool == "select" && !undoButton->isVisibleTo(panel) && !closeButton->isVisibleTo(panel), "Esc closes the dimension tool; the select tool has no tool to close");

  // The select tool: Esc clears the selection, then the prompt says how to finish the sketch.
  m_sel = {m_sk.entities.back().id};
  rebuild();
  emit changed();
  check(says(QString::fromUtf8("Del/⌫ delete · Esc clear selection")), "with a selection the prompt says what Backspace and Esc do");
  key(m_viewport, Qt::Key_Escape);
  check(m_sel.empty() && m_tool == "select" && keyHints().isEmpty() && prompt->hints().endsWith("finish sketch"), "Esc clears the selection; then the prompt says how to finish the sketch");
  QCoreApplication::exit(ok ? 0 : 2);
}
