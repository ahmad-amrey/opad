#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "SketchSteps.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>

using namespace opad::design;

// OPAD_BENCH_SKETCH_STEPS=<prefix> (TODO 11 UI-25): every tool the panel lists or a ribbon button starts says what it waits
// for, from one source: the status line is "<tool>: <the step that waits>", the prompt bar and the panel list the same
// steps; a done step shows what it took (a corner's place, the points of a chain, the line picked, a dimension's value) in
// place of "Ready"; the panel's step rows are never covered by the fields under them (the dimension's value field hid its
// last step).
void SketchEditor::benchSteps() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_STEPS");
  bool ok = true;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sketch steps: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  QWidget* window = m_viewport->window();
  auto* panel = window->findChild<SketchPanel*>();
  auto* stepsPanel = panel ? panel->findChild<ToolStepsPanel*>() : nullptr;
  PromptBar* prompt = nullptr;
  for (auto* bar : m_viewport->findChildren<PromptBar*>(Qt::FindDirectChildrenOnly))
    if (bar->objectName().isEmpty()) prompt = bar;
  if (!panel || !stepsPanel || !prompt) {
    check(false, "the sketch panel, its steps and the prompt bar exist");
    return QCoreApplication::exit(2);
  }
  QString status;
  const auto listening = connect(this, &SketchEditor::status, this, [&status](const QString& text) { status = text; });
  auto waiting = [](const QList<ToolStep>& steps) {
    int i = 0;
    while (i + 1 < steps.size() && !steps[i].picked.isEmpty()) ++i;
    return steps.value(i).label;
  };
  auto same = [&] {  // the prompt bar lists the panel's steps, the status line names the one that waits
    const auto steps = toolSteps();
    if (prompt->steps().size() != steps.size()) return false;
    for (int i = 0; i < steps.size(); ++i)
      if (prompt->steps()[i].label != steps[i].label || prompt->steps()[i].picked != steps[i].picked) return false;
    return status.contains(waiting(steps));
  };
  auto place = [&](double u, double v, Qt::KeyboardModifiers mods = Qt::AltModifier) {  // Alt: exactly there
    sketchMove(u, v, mods, false);
    sketchPress(u, v, mods);
    sketchRelease(u, v, mods);
  };
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {15, 10, 100}}, {"target", {15, 10, 0}}, {"up", {0, 1, 0}}, {"scale", 60}, {"projection", "orthographic"}, {"absolute", true}});

  // Every tool: the panel's registry and the ribbon's buttons.
  QStringList tools;
  for (const auto& t : SketchPanel::tools()) tools << t.id;
  for (QAction* a : window->findChildren<QAction*>())
    if (a->property("sketchTool").isValid()) tools << a->property("sketchTool").toString();
  tools.removeDuplicates();
  QStringList blank;
  for (const QString& id : tools) {
    status.clear();
    setTool(id);
    QCoreApplication::processEvents();  // the prompt bar follows the editor's signals
    const auto* entry = sketchsteps::find(id.toStdString());
    const auto steps = toolSteps();
    bool good = entry && m_tool == id && !steps.isEmpty() && status.startsWith(i18n::t(entry->name) + ": ") && same();
    for (const auto& s : steps) good = good && !s.label.trimmed().isEmpty() && s.picked != "Ready";
    if (!good) blank << id + " (" + status + ")";
    setTool("select");
  }
  check(blank.isEmpty() && tools.size() >= 70, QString("all %1 tools prompt for the step that waits, as the prompt bar and the panel list it%2").arg(tools.size()).arg(blank.isEmpty() ? QString() : ": not " + blank.join(", ")));

  // Done steps show what they took.
  setTool("rect");
  place(0, 0);
  QCoreApplication::processEvents();
  check(toolSteps()[0].picked == "0, 0" && toolSteps()[1].picked.isEmpty() && status == "Rectangle: Click the opposite corner" && same(), "a rectangle's first corner shows where it went; the status asks for the opposite one");
  prompt->grab().save(prefix + ".rect-prompt.png");
  place(30, 20);
  check(m_clicks.empty() && toolSteps()[0].picked.isEmpty() && status == "Rectangle: Click the first corner", "the rectangle is made; the tool asks for the next one");
  int bottom = 0;
  for (const auto& e : m_sk.entities)
    if (e.type == SkEntity::Type::Line && m_sk.point(e.p[0])->y == 0 && m_sk.point(e.p[1])->y == 0) bottom = e.id;
  setTool("line");
  place(0, -10);
  place(12.5, -10);
  QCoreApplication::processEvents();
  check(toolSteps()[0].picked == "0, -10" && toolSteps()[1].picked == "1 more point" && status.startsWith("Line: Click more points or press Enter to end") && same(), "a chain shows its start and how many points follow");
  closeTool();
  for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
  setTool("c:parallel");
  const Hit under = hitTest(15, 0);
  if (under.kind != Hit::Entity || under.id != bottom) trace::log(QString("bench: sketch steps: under (15, 0): kind %1 id %2, the bottom line %3, tool %4, busy %5").arg(int(under.kind)).arg(under.id).arg(bottom).arg(m_tool).arg(busy()));
  place(15, 0, Qt::NoModifier);
  QCoreApplication::processEvents();
  check(m_picked.size() == 1 && toolSteps()[0].picked == QString("Line %1").arg(bottom) && status == "Parallel: Pick the second line" && same(), QString("a constraint's first pick names the line picked (%1: %2)").arg(toolSteps()[0].picked, status));
  closeTool();

  // A dimension: the line picked, its value, then the value step waits; its rows stay clear of the value field.
  setTool("dimension");
  place(15, 0, Qt::NoModifier);
  place(15, -6, Qt::NoModifier);
  for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
  const auto steps = toolSteps();
  check(m_dimEditing && steps.size() == 3 && steps[0].picked == QString("Line %1").arg(bottom) && steps[1].picked == "30 mm" && status == "Dimension: Set the value and apply" && same(),
        QString("a dimension being placed shows its line and value; the status asks for the value (%1, %2: %3)").arg(steps.value(0).picked, steps.value(1).picked, status));
  auto* scroll = stepsPanel->findChild<QScrollArea*>();
  auto* field = panel->findChild<QLineEdit*>("sketchOption-expression");
  const int stepsBottom = stepsPanel->mapTo(panel, QPoint(0, stepsPanel->height())).y(), fieldTop = field ? field->mapTo(panel, QPoint(0, 0)).y() : -1;
  check(scroll && scroll->verticalScrollBar()->maximum() == 0 && field && fieldTop >= stepsBottom,
        QString("the step rows fit their space and end above the value field (%1 px of rows hidden, rows end at %2, the field starts at %3)").arg(scroll ? scroll->verticalScrollBar()->maximum() : -1).arg(stepsBottom).arg(fieldTop));
  panel->grab().save(prefix + ".dimension-panel.png");
  prompt->grab().save(prefix + ".dimension-prompt.png");
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  if (m_dimEdit) QApplication::sendEvent(m_dimEdit, &esc);
  closeTool();
  disconnect(listening);
  QCoreApplication::exit(ok ? 0 : 2);
}
