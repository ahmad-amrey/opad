// Edit: undo/redo, rename, hide/show, delete (tombstone) and restore, the timeline's menu and op targets.
#include "MainWindow.hpp"

#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>

#include <algorithm>
#include <functional>
#include <set>

#include "I18n.hpp"
#include "Icons.hpp"

void MainWindow::buildEditActions() {
  addAction("edit.undo", tr("&Undo"), "rollLeft", QKeySequence::Undo, [this] {
    if (m_annotationEditor) return m_annotationEditor->undo();  // its strokes; the document waits for Save
    if (m_design->sketchActive()) return m_design->sketch()->undo();  // a sketch has its own history until it is finished
    if (m_design->ownsSelection() || m_design->busy()) return;
    m_doc->undo();
  });
  addAction("edit.redo", tr("&Redo"), "rollRight", QKeySequence::Redo, [this] {
    if (m_annotationEditor) return m_annotationEditor->redo();
    if (m_design->sketchActive()) return m_design->sketch()->redo();
    if (m_design->ownsSelection() || m_design->busy()) return;
    m_doc->redo();
  });
  connect(m_doc, &AppDocument::undoChanged, this, &MainWindow::updateUndoActions);
  // Repeat (UI-111): the last tool, feature, check or edit run, from anywhere (key, menu, ribbon, palette).
  addAction("edit.repeat", tr("Repeat"), "regen", QKeySequence("Shift+Return"), [this] {
    if (QAction* a = action(m_lastCommand); a && a->isEnabled()) a->trigger();
  })->setEnabled(false);
  // Select all and Invert (UI-111): the sketch's curves while sketching, else the bodies on screen (visible, inside the
  // isolation); a tool or a feature input owns the picks meanwhile.
  addAction("edit.selectall", tr("Select all"), "", QKeySequence::SelectAll, [this] { selectShown(false); });
  addAction("edit.invert", tr("Invert selection"), "", QKeySequence("Ctrl+Shift+I"), [this] { selectShown(true); });
  addAction("edit.rename", tr("Rename"), "rename", QKeySequence("F2"), [this] {
    auto ids = currentNodeIds();
    if (ids.size() == 1) return m_browser->startRename(ids.front());
    if (ids.empty()) return;
    // Several at once: one name, numbered in selection order (TODO 10 B15).
    QString base = m_doc->nodeName(ids.front());
    static const QRegularExpression number(" \d+$");
    base.remove(number);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename %1 objects").arg(ids.size()), tr("Name ({n} is replaced by 1, 2, 3, ...):"), QLineEdit::Normal, base + " {n}", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    m_doc->run("rename", opad::json{{"targets", ids}, {"name", name.toStdString()}});
  });
  addAction("edit.hide", tr("Hide"), "hide", QKeySequence("V"), [this] {
    const auto ids = currentNodeIds();
    if (!ids.empty()) m_doc->run("appearance", opad::json{{"targets", ids}, {"visible", false}});
  });
  addAction("edit.showall", tr("Unhide all"), "eye", QKeySequence("Shift+V"), [this] {
    std::vector<std::string> hidden;
    for (const auto& [id, n] : m_doc->scene.nodes)
      if (!n.visible) hidden.push_back(id);
    if (!hidden.empty()) m_doc->run("appearance", opad::json{{"targets", hidden}, {"visible", true}});
  });
  addAction("edit.filter", tr("Filter objects"), "search", QKeySequence("Ctrl+F"), [this] { m_browserOverlay->reveal(); m_browser->focusFilter(); });
  addAction("edit.selectparent", tr("Select parent"), "chevronUp", QKeySequence("Ctrl+Up"), [this] { m_browser->selectParent(); });
  addAction("edit.delete", tr("Delete (tombstone)"), "delete", QKeySequence::Delete, [this] { deleteCurrent(); });
  addAction("edit.restore", tr("Restore"), "restore", QKeySequence("Shift+Del"), [this] {
    std::string id = m_timeline->currentOp();
    if (id.empty()) throw opad::UserHint("Select a tombstoned marker on the timeline first.", true);
    restoreOp(id);
  });
  addAction("edit.selecttouched", tr("Select what it touches"), "isolate", QKeySequence("T"), [this] {
    if (!m_timeline->currentOp().empty()) selectOpTargets(m_timeline->currentOp());
  });
}

std::vector<std::string> MainWindow::shownBodies() const {
  std::set<std::string> isolated;
  for (const auto& id : m_viewport->isolatedNodes())
    for (const auto& b : m_doc->scene.bodies_under(id)) isolated.insert(b);
  std::vector<std::string> out;
  for (const auto& b : m_doc->scene.all_bodies()) {
    const auto path = m_doc->scene.path_to(b);
    const bool visible = std::all_of(path.begin(), path.end(), [this](const std::string& id) { const opad::Node* n = m_doc->node(id); return n && n->visible; });
    if (visible && (isolated.empty() || isolated.count(b))) out.push_back(b);
  }
  return out;
}

void MainWindow::selectShown(bool invert) {
  if (m_design->sketchActive()) return m_design->sketch()->selectAll(invert);
  if (!m_doc->hasDocument || m_design->ownsSelection() || !m_tool.id.isEmpty() || m_annotationEditor) return;
  std::set<std::string> selected;
  if (invert)
    for (const auto& id : currentNodeIds())
      for (const auto& b : m_doc->scene.bodies_under(id)) selected.insert(b);
  std::vector<std::string> ids;
  for (const auto& b : shownBodies())
    if (!selected.count(b)) ids.push_back(b);
  if (m_viewport->selectionFilter() != Viewport::SelFilter::Body) action("select.bodies")->trigger();
  m_browser->setSelectedIds(ids);
  onBrowserSelection(ids);
}

// What Repeat runs again: a tool, feature, check, note or edit, not a view change, a file command, a toggle of the
// window or a selection command.
bool MainWindow::repeatable(const QString& id) {
  static const QStringList never{"edit.undo", "edit.redo", "edit.repeat", "edit.selectall", "edit.invert", "edit.filter", "edit.selectparent", "edit.selecttouched",
                                 "inspect.clear", "inspect.pin", "inspect.flip", "sketch.finish", "sketch.cancel", "sketch.panel", "annotate.show", "annotate.resolve"};
  static const QStringList yes{"design.", "sketch.", "inspect.", "annotate.", "edit.", "select.geometry", "view.isolate", "view.saveview", "file.import", "file.export", "file.screenshot"};
  return !never.contains(id) && std::any_of(yes.begin(), yes.end(), [&id](const QString& p) { return id.startsWith(p); });
}

void MainWindow::noteCommand(const QString& id) {
  if (!repeatable(id) || !action(id)) return;
  m_lastCommand = id;
  QAction* repeat = action("edit.repeat");
  repeat->setText(tr("Repeat %1").arg(QString(action(id)->text()).remove('&').remove(QString::fromUtf8("…"))));
  repeat->setEnabled(true);
  shortcuts::updateTooltip(repeat);
}

void MainWindow::timelineMenu(const std::string& requestedId, const QPoint& globalPos) {
  const std::string opId=requestedId;
  const auto generation=m_doc->generation;
  bool deleted = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), opId) != m_doc->scene.deleted_ops.end();
  QMenu menu(this);
  menu.setMinimumWidth(232);  // not fixed: that cut the shortcuts off ("Shift+" for Shift+Del)
  const opad::Op* menuOp = m_doc->doc.find_op(opId);
  // Tombstoning a delete op brings back what it deleted (docs/format.md), so on a delete marker that entry is offered
  // as what it does; once undone, the delete can be applied again.
  const bool deleteMarker = menuOp && menuOp->type == "delete";
  QAction* del = menu.addAction(icons::themed(deleteMarker ? "restore" : "delete", 16), deleteMarker ? tr("Restore what it deleted\tDel") : tr("Delete (tombstone)\tDel"));
  del->setEnabled(!deleted);
  QAction* restore = menu.addAction(icons::themed(deleteMarker ? "delete" : "restore", 16), deleteMarker ? tr("Delete it again\tShift+Del") : tr("Restore\tShift+Del"));
  restore->setEnabled(deleted);
  const bool designOp = menuOp && (menuOp->type == "feature" || menuOp->type == "sketch") && !deleted;
  const opad::Feature* feat = m_doc->scene.feature(opId);
  const bool suppressed=feat && feat->suppressed;
  QAction* editOp = designOp ? menu.addAction(icons::themed("rename", 16), menuOp->type == "sketch" ? tr("Edit sketch") : tr("Edit feature")) : nullptr;
  QAction* suppress = designOp && feat ? menu.addAction(icons::themed(feat->suppressed ? "eye" : "hide", 16), feat->suppressed ? tr("Unsuppress") : tr("Suppress")) : nullptr;
  QAction* exportSketch=designOp && menuOp->type=="sketch" ? menu.addAction(icons::themed("export",16),tr("Export sketch")) : nullptr;
  if (designOp) menu.addSeparator();
  QAction* sel = menu.addAction(icons::themed("isolate", 16), tr("Select what it touches\tT"));
  menu.addSeparator();
  QAction* copy = menu.addAction(icons::themed("commit", 16), tr("Copy op id\tCtrl+C"));
  QAction* log = menu.addAction(icons::themed("git", 16), tr("Show in git log"));
  QAction* chosen = menu.exec(globalPos);
  if (!chosen || generation!=m_doc->generation) return;
  if (chosen == exportSketch) exportDialog({opId});
  else if (chosen == editOp) m_design->editOp(opId);
  else if (chosen == suppress) m_design->setSuppressed(opId, !suppressed);
  else if (chosen == del) deleteOp(opId);
  else if (chosen == restore) restoreOp(opId);
  else if (chosen == sel) selectOpTargets(opId);
  else if (chosen == copy) QApplication::clipboard()->setText(QString::fromStdString(opId));
  else if (chosen == log) showOpGitLog(opId,m_doc->path());
}

void MainWindow::updateUndoActions() {
  QAction* u = action("edit.undo");
  QAction* r = action("edit.redo");
  if (!u || !r) return;
  if(m_design && m_design->sketchActive()) {
    const auto* sketch=m_design->sketch();
    u->setEnabled(sketch->canUndo());r->setEnabled(sketch->canRedo());
    u->setText(tr("&Undo"));r->setText(tr("&Redo"));
  } else {
    u->setEnabled(m_doc->hasDocument && m_doc->canUndo());
    r->setEnabled(m_doc->hasDocument && m_doc->canRedo());
    u->setText(m_doc->canUndo() ? tr("&Undo %1").arg(m_doc->undoLabel()) : tr("&Undo"));
    r->setText(m_doc->canRedo() ? tr("&Redo %1").arg(m_doc->redoLabel()) : tr("&Redo"));
  }
  shortcuts::updateTooltip(u);  // the quick-access buttons say what they undo
  shortcuts::updateTooltip(r);
}

// Undo ▾ / Redo ▾ in the tab row: the document's steps, the next one first; a click on one takes it and every step before
// it at once. A sketch or a drawing note keeps its own history: then the list is Undo (Redo) itself.
QMenu* MainWindow::historyMenu(bool undo) {
  auto* menu = new QMenu(this);
  menu->setObjectName(undo ? "undoSteps" : "redoSteps");
  connect(menu, &QMenu::aboutToShow, this, [this, menu, undo] {
    menu->clear();
    const bool document = m_doc->hasDocument && !m_annotationEditor && !m_design->sketchActive() && !m_design->ownsSelection() && !m_design->busy();
    const QStringList steps = document ? (undo ? m_doc->undoLabels() : m_doc->redoLabels()) : QStringList();
    if (steps.isEmpty()) {
      menu->addAction(action(undo ? "edit.undo" : "edit.redo"));
      return;
    }
    for (int i = 0; i < std::min<int>(steps.size(), 20); ++i)
      menu->addAction((undo ? tr("&Undo %1") : tr("&Redo %1")).arg(steps[i]), this, [this, undo, n = i + 1] { undo ? m_doc->undo(n) : m_doc->redo(n); });
  });
  return menu;
}

void MainWindow::deleteOp(const std::string& requestedId) {
  const std::string opId=requestedId; // Rebuilding cards can destroy the signal sender during this operation.
  // With a design history a tombstone changes what later features produce: planned on a worker.
  if (!m_doc->scene.features.empty() || !m_doc->scene.sketches.empty()) {
    m_design->applyOps({opad::json{{"op", "delete"}, {"target", opId}}}, tr("delete"));
    return m_timeline->setCurrentOp(opId);
  }
  opad::json r = m_doc->run("delete", opad::json{{"target", opId}});
  if (r.contains("id")) m_timeline->setCurrentOp(opId);
}

void MainWindow::restoreOp(const std::string& requestedId) {
  const std::string opId=requestedId;
  // Restoring = tombstoning the delete op that targets it.
  for (const auto& op : m_doc->doc.ops)
    if (op.type == "delete" && op.data.value("target", "") == opId && !m_doc->doc.is_deleted(op.id)) {
      if (!m_doc->scene.features.empty() || !m_doc->scene.sketches.empty() || m_doc->doc.find_op(opId)->type == "feature" || m_doc->doc.find_op(opId)->type == "sketch")
        m_design->applyOps({opad::json{{"op", "delete"}, {"target", op.id}}}, tr("restore"));
      else
        m_doc->run("delete", opad::json{{"target", op.id}});
      m_timeline->setCurrentOp(opId);
      return;
    }
  throw opad::UserHint("That operation is not tombstoned.");
}

void MainWindow::deleteCurrent() {
  std::string id = m_timeline->currentOp();
  if (!id.empty() && m_timeline->hasFocus()) return deleteOp(id);
  std::set<std::string> ops;
  const auto selected = currentNodeIds();
  for (const auto& nid : selected) if (const opad::Node* n = m_doc->node(nid)) ops.insert(n->source_op);
  if (ops.empty()) {  // the timeline's marker only when nothing is selected (an area's row or a sketch is not that marker)
    if (id.empty() || !selected.empty() || !m_selRows.empty()) throw opad::UserHint("Select objects, or a marker on the timeline, to tombstone.");
    return deleteOp(id);
  }
  if (QMessageBox::question(this, tr("Delete"), tr("Tombstone %1 import operation(s)? History is kept; Shift+Del on the timeline restores.").arg(ops.size())) != QMessageBox::Yes) return;
  for (const auto& op : ops) deleteOp(op);
}

void MainWindow::selectOpTargets(const std::string& opId) {
  const opad::Op* op = m_doc->doc.find_op(opId);
  if (!op) return;
  m_timeline->setCurrentOp(opId);
  std::vector<std::string> ids;
  const opad::json& d = op->data;
  if (d.contains("target") && d["target"].is_string() && m_doc->node(d["target"])) ids.push_back(d["target"]);
  if (d.contains("anchor")) { try { ids.push_back(opad::Ref::from_json(d["anchor"]).body); } catch (...) {} }
  if (d.contains("refs")) for (const auto& r : d["refs"]) { try { ids.push_back(opad::Ref::from_json(r).body); } catch (...) {} }
  if (const opad::Feature* f = m_doc->scene.feature(opId))
    for (const auto& b : f->result.value("bodies", opad::json::array())) ids.push_back(b.value("id", ""));
  if (op->type == "import") {
    std::function<void(const opad::json&)> walk = [&](const opad::json& nodes) {
      for (const auto& n : nodes) { ids.push_back(n.value("id", "")); if (n.contains("children")) walk(n["children"]); }
    };
    walk(d.value("nodes", opad::json::array()));
  }
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) { return !m_doc->node(id); }), ids.end());
  m_browser->setSelectedIds(ids);
  onBrowserSelection(ids);
  if (!m_propsPanel->isVisible()) return;  // pinned open: show the operation itself
  m_propsPanel->setContext(QString::fromStdString(opId.substr(0, 8)));
  m_props->setSubject({{}, opId});
  m_props->showEntity(m_timeline->describe(*op), QString::fromUtf8("%1 · %2").arg(QString::fromStdString(d.value("by", "")), i18n::localTime(d.value("ts", ""))),
                      QString::fromStdString(opId.substr(0, 8)), d);
}
