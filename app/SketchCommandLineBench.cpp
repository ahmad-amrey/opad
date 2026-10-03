#include "CommandLine.hpp"
#include "GuidedTool.hpp"
#include "Jobs.hpp"
#include "SketchEditor.hpp"
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_COMMANDLINE=<prefix> (TODO 11 UI-133): drafting by the keyboard through the command line, key events
// sent to its input. The strip shows at the bottom left while the sketch is open, its prompt the prompt bar's step; REC
// (the match named as it is typed), 0,0, @40,30 make a 40 x 30 rectangle held by its typed sizes; L, 50,0, @30<90 (a
// vertical held by its length), @20,0, CLOSE make a closed triangle; Up and Down recall the entries and the line being
// typed; an unknown word is said in red and changes nothing; C, 100,15, 20 make a circle of diameter 20; a bare value for a
// first point is refused, a value that does not evaluate is said and the chain waits; F then 3 sets the fillet's radius;
// Enter on an empty line with no tool repeats the last command; the keyboard stays in the line; its close button hides it,
// the command (Space) brings it back with the keyboard. <prefix>.png: the strip, <prefix>.view.png: the window.
void SketchEditor::benchCommandLine() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_COMMANDLINE");
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: command line: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  QWidget* window = m_viewport->window();
  auto* line = m_viewport->findChild<CommandLine*>();
  auto* show = window->findChild<QAction*>("sketch.commandLine");
  PromptBar* bar = nullptr;
  for (auto* b : m_viewport->findChildren<PromptBar*>(Qt::FindDirectChildrenOnly))
    if (b->objectName().isEmpty()) bar = b;
  if (!line || !show || !bar) {
    check(false, "the command line, its command and the prompt bar exist");
    return QCoreApplication::exit(2);
  }
  QLineEdit* edit = line->edit();
  auto* match = line->findChild<QLabel*>("commandMatch");
  window->activateWindow();
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {60, 15, 100}}, {"target", {60, 15, 0}}, {"up", {0, 1, 0}}, {"scale", 110}, {"projection", "orthographic"}, {"absolute", true}});
  check(line->isVisible() && line->geometry().left() == CommandLine::kMargin && line->geometry().bottom() == m_viewport->height() - CommandLine::kMargin - 1,
        "the strip shows at the bottom left of the view while the sketch is open");
  check(show->shortcut() == QKeySequence(Qt::Key_Space) && line->prompt() == tr("Command:"), "Space is its key; with no tool the prompt asks for a command");
  show->trigger();
  check(window->focusWidget() == edit, "its command gives it the keyboard");

  auto key = [](QChar c) {
    if (c.isLetter()) return Qt::Key_A + (c.toUpper().unicode() - 'A');
    if (c.isDigit()) return Qt::Key_0 + c.digitValue();
    switch (c.unicode()) {
      case '@': return int(Qt::Key_At);
      case '<': return int(Qt::Key_Less);
      case ',': return int(Qt::Key_Comma);
      case '.': return int(Qt::Key_Period);
      case '-': return int(Qt::Key_Minus);
      case ' ': return int(Qt::Key_Space);
      default: return int(Qt::Key_unknown);
    }
  };
  auto send = [edit, window](int k, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(window->focusWidget() ? window->focusWidget() : edit, &press);
  };
  auto type = [&](const QString& text) {
    for (const QChar c : text) send(key(c), QString(c));
  };
  auto enter = [&](const QString& text) {
    type(text);
    send(Qt::Key_Return);
  };
  auto waiting = [bar] {
    const auto& steps = bar->steps();
    int i = 0;
    while (i + 1 < steps.size() && !steps[i].picked.isEmpty()) ++i;
    return steps.value(i).label;
  };
  auto equal = [](double a, double b) { return std::abs(a - b) < 1e-6; };
  auto pointAt = [&](double u, double v) {
    for (const auto& p : m_sk.points)
      if (equal(p.x, u) && equal(p.y, v)) return p.id;
    return 0;
  };
  auto count = [&](SkEntity::Type type) { return std::count_if(m_sk.entities.begin(), m_sk.entities.end(), [type](const SkEntity& e) { return e.type == type; }); };
  auto dimension = [&](SkConstraint::Type type, double value) {
    return std::any_of(m_sk.constraints.begin(), m_sk.constraints.end(), [&](const SkConstraint& c) { return c.type == type && equal(c.value, value); });
  };
  auto logged = [line](const QString& text) { return line->lines().join('\n').contains(text); };

  type("REC");
  check(match && match->text().contains(tr("Rectangle")), "typing REC names what it runs: " + (match ? match->text() : QString()));
  send(Qt::Key_Return);
  check(m_tool == "rect" && logged("REC"), "REC Enter starts the rectangle and is echoed above the line");
  check(line->prompt() == prompt() + ":" && prompt().contains(waiting()), "the prompt is the prompt bar's step: " + line->prompt());
  enter("0,0");
  check(m_clicks.size() == 1 && equal(m_clicks[0].u, 0) && equal(m_clicks[0].v, 0), "0,0 places the first corner at the origin");
  check(line->prompt().contains(waiting()), "the prompt follows to the next step: " + line->prompt());
  enter("@40,30");
  check(count(SkEntity::Type::Line) == 4 && pointAt(40, 30) && pointAt(0, 30) && m_clicks.empty(), "@40,30 makes the opposite corner 40 right and 30 up");
  check(dimension(SkConstraint::Type::Distance, 40) && dimension(SkConstraint::Type::Distance, 30), "the typed sizes hold the rectangle as dimensions");
  check(window->focusWidget() == edit, "the keyboard stays in the line after an entry");

  enter("L");
  enter("50,0");
  enter("@30<90");
  check(m_tool == "line" && m_chain.size() == 2 && pointAt(50, 30), "L, 50,0 and @30<90 draw a vertical line 30 long");
  check(dimension(SkConstraint::Type::Distance, 30) &&
            std::count_if(m_sk.constraints.begin(), m_sk.constraints.end(), [](const SkConstraint& c) { return c.type == SkConstraint::Type::Vertical; }) >= 3,
        "the typed length and angle hold it (a distance, vertical)");
  enter("@20,0");
  check(m_chain.size() == 3 && pointAt(70, 30), "@20,0 goes on 20 to the right");
  enter("close");
  check(m_chain.empty() && count(SkEntity::Type::Line) == 7 && m_tool == "line", "CLOSE ends the polyline on its first point: a triangle");

  send(Qt::Key_Up);
  check(edit->text() == "close", "Up recalls the last entry");
  send(Qt::Key_Up);
  check(edit->text() == "@20,0", "Up again the one before");
  send(Qt::Key_Down);
  send(Qt::Key_Down);
  check(edit->text().isEmpty(), "Down goes back to the line being typed");
  type("12");
  send(Qt::Key_Escape);
  check(edit->text().isEmpty() && m_tool == "line", "Esc clears what is typed and leaves the tool alone");

  const auto before = m_sk.entities.size();
  enter("foo");
  check(logged(tr("Unknown command: %1").arg("foo")) && m_sk.entities.size() == before && m_tool == "line", "an unknown word is said and changes nothing");
  enter("25");
  check(logged(tr("A point needs X and Y: type x,y (or @dx,dy, @length<angle)")) && m_chain.empty(), "a bare value for a first point is refused");
  enter("0,-20");
  enter("@abc<30");
  check(m_chain.size() == 1 && !m_input->typed() && line->lines().size() >= 2 && m_tool == "line", "a value that does not evaluate is said; the chain waits: " + line->lines().back());
  send(Qt::Key_Escape);
  check(m_chain.empty() && m_tool == "line", "Esc on an empty line is the sketch's: it ends the chain");

  enter("C");
  enter("100,15");
  enter("20");
  const bool circle = count(SkEntity::Type::Circle) == 1 && std::any_of(m_sk.entities.begin(), m_sk.entities.end(), [&](const SkEntity& e) {
    const SkPoint* c = e.type == SkEntity::Type::Circle ? m_sk.point(e.p[0]) : nullptr;
    return c && equal(c->x, 100) && equal(c->y, 15) && equal(e.r, 10);
  });
  check(circle && dimension(SkConstraint::Type::Diameter, 20), "C, 100,15 and 20 make a circle of diameter 20 there");

  enter("F");
  enter("3");
  check(m_tool == "fillet" && option("radius") == "3", "F then 3 sets the fillet's radius");
  send(Qt::Key_Escape);
  check(m_tool == "select" && line->prompt() == tr("Command:"), "Esc closes the tool");
  send(Qt::Key_Return);
  check(m_tool == "fillet", "Enter on an empty line runs the last command again");
  enter("TR");
  check(m_tool == "trim", "TR is trim");
  enter("u");
  check(m_tool == "select" && count(SkEntity::Type::Circle) == 0, "U undoes the circle");

  line->grab().save(prefix + ".png");
  window->grab().save(prefix + ".view.png");
  line->findChild<QToolButton*>("commandClose")->click();
  check(!line->isVisible() && !QSettings().value("sketch/commandLine", true).toBool(), "its close button hides it and remembers that");
  show->trigger();
  check(line->isVisible() && window->focusWidget() == edit && QSettings().value("sketch/commandLine", true).toBool(),
        QString("its command shows it again with the keyboard (shown %1, keyboard %2, active %3)")
            .arg(line->isVisible()).arg(window->focusWidget() ? window->focusWidget()->objectName() : QString("none")).arg(window->isActiveWindow()));
  QTimer::singleShot(0, this, [ok] { QCoreApplication::exit(*ok ? 0 : 2); });
}
