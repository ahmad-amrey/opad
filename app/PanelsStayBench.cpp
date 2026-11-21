// OPAD_BENCH_PANELS_STAY=1 on a document with a body (tools/bench_cases/sketch.py, case panels-stay). Keys and clicks are
// sent where the keyboard is, as typed ones arrive (Qt offers them to the window's shortcuts first).
//  (a) A panel that shows the document is never closed by using a tool (ToolPanel::persistent, MainWindow::openPanel): the
//      Section panel stays open beside Distance's panel and beside a feature's, without covering them; the feature's
//      panel replaces Distance's (a tool's panel replaces another tool's).
//  (b) In the Note editor Shift+Enter is never the window's Repeat (which closed the editor unsaved): in the text it is a
//      new line, elsewhere Save and another note at once.
//  (c) A tool's panel whose footer says Enter on its button (Canvas OK, KiCad Sync): Enter in the view presses it,
//      Shift+Enter presses it with Shift held; while a tool's panel is shown a plain Enter in the view is never Repeat.
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

#include "AnnotationEditor.hpp"
#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"

namespace {
void keyTo(QWidget* to, int key, Qt::KeyboardModifiers modifiers) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers);  // not spontaneous: Qt offers it to the shortcuts first
  QApplication::sendEvent(to, &event);
}
bool apart(const QWidget* a, const QWidget* b) { return !a->frameGeometry().adjusted(8, 8, -8, -8).intersects(b->frameGeometry().adjusted(8, 8, -8, -8)); }
}  // namespace

OPAD_BENCH(OPAD_BENCH_PANELS_STAY, panelsStay) {
  bool all = true;
  auto check = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: panels stay: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  MainWindow* window = &w;
  Viewport* view = w.m_viewport;
  AppDocument* doc = w.m_doc;
  const std::string body = doc->scene.all_bodies().empty() ? std::string() : doc->scene.all_bodies().front();
  check(!body.empty() && !doc->browse, "an editable document with a body");
  if (body.empty()) {
    QCoreApplication::exit(2);
    return true;
  }
  w.setWorkspace("design");
  QApplication::setActiveWindow(window);
  auto toView = [window, view] {
    QApplication::setActiveWindow(window);
    view->setFocus();
  };
  auto repeats = std::make_shared<int>(0);
  QObject::connect(w.action("edit.repeat"), &QAction::triggered, window, [repeats] { ++*repeats; });

  // (a)
  w.openPanel(w.m_sectionPanel);
  w.startTool("distance");
  check(w.m_sectionPanel->isVisible() && w.m_toolPanel->isVisible(), "Distance opens its panel and the Section panel stays open");
  check(apart(w.m_sectionPanel, w.m_toolPanel), "Distance's panel sits beside the Section panel, not over it");
  w.m_design->startFeature("fillet");
  check(w.m_featurePanel->isVisible() && !w.m_toolPanel->isVisible() && w.m_sectionPanel->isVisible(),
        "a feature's panel replaces Distance's, the Section panel stays open");
  check(apart(w.m_sectionPanel, w.m_featurePanel), "the feature's panel sits beside the Section panel, not over it");
  w.m_featurePanel->hide();  // its close button: the feature ends
  check(w.m_sectionPanel->isVisible(), "the Section panel is still open after the feature");
  w.m_sectionPanel->hide();

  // (b)
  const size_t notes = doc->scene.annotations.size();
  w.startAnnotation(false);
  AnnotationEditor* editor = w.m_annotationEditor;
  auto* text = w.m_annotationPanel->findChild<QPlainTextEdit*>("annotationText");
  check(editor && text, "Note opens its editor");
  if (editor && text) {
    opad::Ref r;
    r.body = body;
    r.kind = opad::Ref::Kind::Body;
    editor->adoptSelection({r});
    QApplication::setActiveWindow(w.m_annotationPanel);
    text->setFocus();
    text->setPlainText("first");
    text->moveCursor(QTextCursor::End);
    keyTo(text, Qt::Key_Return, Qt::ShiftModifier);
    check(w.m_annotationEditor == editor && text->toPlainText() == "first\n" && *repeats == 0, "Shift+Enter in the note's text is a new line, the editor stays");
    text->insertPlainText("second");
    toView();
    keyTo(view, Qt::Key_Return, Qt::ShiftModifier);
  }
  const QPointer<AnnotationEditor> saved(editor);
  QTimer::singleShot(200, window, [=, &w]() mutable {
    bool ok = true;
    auto check2 = [&ok](bool pass, const QString& what) {
      trace::log(QString("bench: panels stay: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
      ok = ok && pass;
    };
    check2(doc->scene.annotations.size() == notes + 1 && doc->scene.annotations.back().text == "first\nsecond", "Shift+Enter in the view saves the note");
    check2(w.m_annotationEditor && w.m_annotationEditor != saved.data() && !w.m_annotationEditor->drawingMode() && w.m_annotationPanel->isVisible(),
           "... and a new Note starts at once (Shift kept the tool)");
    if (w.m_annotationEditor) w.m_annotationEditor->cancel();
    const bool before = all && ok;

    // (c), once the cancelled editor is gone (deleteLater: until then a note counts as being written)
    QTimer::singleShot(100, window, [=, &w] {
    bool ok = before;
    auto check2 = [&ok](bool pass, const QString& what) {
      trace::log(QString("bench: panels stay: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
      ok = ok && pass;
    };
    check2(!w.m_annotationEditor, "the Note editor closed");
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    auto* footer = new PanelFooter(body);
    footer->setPrimary(PanelFooter::Primary::Close);
    footer->setShiftKeeps(true);
    layout->addWidget(footer);
    auto* panel = new ToolPanel("benchEnter", "canvas", &Tokens::sel, "Enter", body, 120, window);
    int accepted = 0, shifted = 0;
    QObject::connect(footer, &PanelFooter::accepted, panel, [&accepted, &shifted, footer] {
      ++accepted;
      if (footer->shiftAccepted()) ++shifted;
    });
    w.m_panels << panel;
    w.openPanel(panel);
    const int repeatsBefore = *repeats;
    QApplication::setActiveWindow(window);
    view->setFocus();
    keyTo(view, Qt::Key_Return, Qt::NoModifier);
    check2(accepted == 1 && shifted == 0 && *repeats == repeatsBefore, "Enter in the view presses the footer's OK (Enter), not Repeat");
    keyTo(view, Qt::Key_Return, Qt::ShiftModifier);
    check2(accepted == 2 && shifted == 1 && *repeats == repeatsBefore, "Shift+Enter in the view presses it with Shift held, not Repeat");
    footer->setPrimaryVisible(false);  // a tool's panel without such a button
    keyTo(view, Qt::Key_Return, Qt::NoModifier);
    check2(accepted == 2 && *repeats == repeatsBefore, "with a tool's panel shown, a plain Enter in the view is not Repeat");
    w.m_panels.removeAll(panel);
    panel->hide();
    panel->deleteLater();
    trace::log(QString("bench: panels stay: persistent panels, the note's Shift+Enter, a footer's Enter %1").arg(ok ? "PASS" : "FAIL"));
    QCoreApplication::exit(ok ? 0 : 2);
    });
  });
  return true;
}
