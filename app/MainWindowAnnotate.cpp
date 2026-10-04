// Annotate: notes and hand drawings (AnnotationEditor), resolving and restyling them.
#include "MainWindow.hpp"

#include <QStatusBar>

#include <utility>

#include "I18n.hpp"

void MainWindow::buildAnnotateActions() {
  addAction("annotate.add", tr("Note"), "annotate", QKeySequence("N"), [this] { startAnnotation(false); }, true);
  addAction("annotate.draw", tr("Hand drawing"), "pen", QKeySequence("Shift+N"), [this] { startAnnotation(true); }, true);
  addAction("annotate.resolve", tr("Resolve note"), "check", QKeySequence("Ctrl+Return"), [this] { resolveCurrentAnnotation(); });
  QAction* notes = addAction("annotate.show", tr("Show notes"), "annotate", QKeySequence(), [this] {}, true);
  notes->setChecked(m_settings.value("ui/notes", true).toBool());  // m_noteCards reads the same key once the viewport exists
  connect(notes, &QAction::toggled, this, [this](bool on) { if (m_noteCards) m_noteCards->setShown(on); });
}

// Note and Hand drawing: an editor that asks for its target like a guided tool (AnnotationEditor.hpp). A single
// selected body, face, edge or vertex is taken as the target straight away (right-click > Note works on it).
void MainWindow::startAnnotation(bool drawing) {
  if (m_annotationEditor) {
    const bool same = m_annotationEditor->drawingMode() == drawing;
    m_annotationEditor->cancel();
    if (same) return syncAnnotationActions();  // the same command again closes it
  }
  if (!m_doc->hasDocument || m_doc->browse || m_design->ownsSelection() || m_design->busy() || m_design->sketchActive())
    return syncAnnotationActions();
  clearAnnotationCardTarget();  // the editor asks for its own target
  const auto selected = m_viewport->selection();
  cancelTool();
  action("annotate.show")->setChecked(true);  // the note being written shows among the others
  m_annotationEditor = new AnnotationEditor(m_doc, m_viewport, m_annotationPanel, this, drawing);
  connect(m_design, &DesignController::stateChanged, m_annotationEditor, &AnnotationEditor::cancel);
  connect(m_annotationEditor, &QObject::destroyed, this, &MainWindow::syncAnnotationActions);
  m_annotationEditor->adoptSelection(selected);
  syncAnnotationActions();
}

void MainWindow::syncAnnotationActions() {
  const bool open = m_annotationEditor && m_annotationPanel->isVisible();
  action("annotate.add")->setChecked(open && !m_annotationEditor->drawingMode());
  action("annotate.draw")->setChecked(open && m_annotationEditor->drawingMode());
}

void MainWindow::resolveCurrentAnnotation() {
  std::string id = m_annotations->currentOpId();
  if (id.empty()) id = m_timeline->currentOp();
  const opad::Op* op = id.empty() ? nullptr : m_doc->doc.find_op(id);
  if (!op || (op->type != "annotation" && op->type != "measurement")) throw opad::UserHint("Select a note in the Annotations panel or on the timeline first.");
  deleteOp(id);
}

void MainWindow::restyleAnnotation(const std::string& opId, const std::string& style) {
  const opad::Op* op = m_doc->doc.find_op(opId);
  if (!op || (op->type != "annotation" && op->type != "measurement")) return;
  guarded([&] { m_doc->run("append", opad::json{{"op", opad::json{{"op", "edit"}, {"target", opId}, {"set", {{"style", style}}}}}}); });
}

// The Annotations panel's cards (TODO 11 help audit P9.4): a click lights up what the note or pinned measurement is
// anchored to the way the note editor showed it while the note was written: a face tinted and ringed by a dashed
// outline, an edge drawn thick, a vertex or a point ringed, a body tinted inside its dashed box. Nothing is selected
// (the selection stays as it was, and a whole body in the selection blue would hide the face); Resolve acts on the card.
void MainWindow::showAnnotationCardTarget(const opad::Ref& anchor) {
  if (m_annotationEditor) return;  // a note being written keeps its own target in the view
  clearAnnotationCardTarget();
  const bool gone = anchor.kind != opad::Ref::Kind::Point && !m_doc->scene.node(anchor.body);
  m_cardTarget = !gone && m_viewport->showAnnotationTarget(anchor);
  if (!m_cardTarget) statusBar()->showMessage(gone ? tr("What this note was pinned to no longer exists.") : tr("What this note is pinned to is not shown in the view (hidden or isolated away)."), 6000);
}

void MainWindow::clearAnnotationCardTarget() {
  if (!std::exchange(m_cardTarget, false) || m_annotationEditor) return;  // the editor's target is its own
  m_viewport->clearAnnotationTarget();
}
