// Edit: undo/redo, rename, hide/show, delete (tombstone) and restore, the timeline's menu and op targets.
#include "MainWindow.hpp"

#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStatusBar>

#include <algorithm>
#include <functional>
#include <set>

#include "I18n.hpp"
#include "Icons.hpp"
#include "SmartRules.hpp"

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
  addAction("edit.selectparent", tr("Select parent"), "chevronUp", QKeySequence("Ctrl+Up"), [this] {
    if (!areaCommand("edit.selectparent")) m_browser->selectParent();  // picked faces grow to their feature (SmartSelect)
  });
  addAction("edit.delete", tr("Delete"), "delete", QKeySequence::Delete, [this] { deleteCurrent(); });
  addAction("edit.restore", tr("Restore"), "restore", QKeySequence("Shift+Del"), [this] {
    std::string id = m_timeline->currentOp();
    if (id.empty()) throw opad::Error("Select a tombstoned marker on the timeline first.");
    restoreOp(id);
  });
  addAction("edit.selecttouched", tr("Select what it touches"), "isolate", QKeySequence("T"), [this] {
    if (!m_timeline->currentOp().empty()) selectOpTargets(m_timeline->currentOp());
  });
  // The last tool again (UI-100): first in the context menus as "Repeat Fillet"; no key of its own unless one is set.
  addAction("edit.repeat", tr("Repeat last command"), "repeat", QKeySequence(), [this] {
    QAction* last = m_lastCommand.isEmpty() ? nullptr : action(m_lastCommand);
    if (!last || !last->isEnabled()) return statusBar()->showMessage(tr("Nothing to repeat yet: start a feature, a sketch tool or a measurement first."), 5000);
    last->trigger();
  })->setProperty("shortcutHint", tr("Starts the last feature, sketch tool, measurement or note again."));
}

void MainWindow::timelineMenu(const std::string& requestedId, const QPoint& globalPos) {
  QMenu menu(this);
  buildTimelineMenu(menu, requestedId);
  menu.exec(globalPos);
}

// A marker's actions, then the roll-back marker and how the timeline shows; on no marker (empty id) only the latter.
void MainWindow::buildTimelineMenu(QMenu& menu, const std::string& requestedId) {
  const std::string opId=requestedId;
  const auto generation=m_doc->generation;
  menu.setMinimumWidth(232);  // not fixed: that cut the shortcuts off ("Shift+" for Shift+Del)
  // Each entry acts only on the document it was offered for (a load may replace it while the menu is open).
  auto entry = [&](const QString& icon, const QString& text, const char* name, std::function<void()> fn) {
    QAction* a = icon.isEmpty() ? menu.addAction(text) : menu.addAction(icons::themed(icon, 16), text);
    a->setObjectName(name);
    connect(a, &QAction::triggered, this, [this, generation, fn] { if (generation == m_doc->generation) guarded(fn); });
    return a;
  };
  if (const opad::Op* menuOp = m_doc->doc.find_op(opId)) {
    const bool deleted = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), opId) != m_doc->scene.deleted_ops.end();
    // Tombstoning a delete op brings back what it deleted (docs/format.md), so on a delete marker that entry is offered
    // as what it does; once undone, the delete can be applied again.
    const bool deleteMarker = menuOp->type == "delete";
    entry(deleteMarker ? "restore" : "delete", deleteMarker ? tr("Restore what it deleted\tDel") : tr("Delete (tombstone)\tDel"), "timelineDelete", [this, opId] { deleteOp(opId); })->setEnabled(!deleted);
    entry(deleteMarker ? "delete" : "restore", deleteMarker ? tr("Delete it again\tShift+Del") : tr("Restore\tShift+Del"), "timelineRestore", [this, opId] { restoreOp(opId); })->setEnabled(deleted);
    const bool designOp = (menuOp->type == "feature" || menuOp->type == "sketch") && !deleted;
    const opad::Feature* feat = m_doc->scene.feature(opId);
    if (designOp) entry("rename", menuOp->type == "sketch" ? tr("Edit sketch") : tr("Edit feature"), "timelineEdit", [this, opId] { m_design->editOp(opId); });
    if (designOp && feat) entry(feat->suppressed ? "eye" : "hide", feat->suppressed ? tr("Unsuppress") : tr("Suppress"), "timelineSuppress", [this, opId, on = !feat->suppressed] { m_design->setSuppressed(opId, on); });
    if (designOp && menuOp->type == "sketch") entry("export", tr("Export sketch"), "timelineExport", [this, opId] { exportDialog({opId}); });
    if (designOp) menu.addSeparator();
    entry("isolate", tr("Select what it touches\tT"), "timelineTouched", [this, opId] { selectOpTargets(opId); });
    // The model as it was right after this step (UI-99): the playhead goes after its marker.
    if (!deleted && !m_doc->browse)
      entry("rollBack", tr("Roll back to here"), "timelineRollBack", [this, opId] { emit m_timeline->rollbackRequested(m_timeline->rollPointAfter(opId)); })
          ->setEnabled(!m_timeline->rollPointAfter(opId).empty() || m_doc->rolledBack());
    menu.addSeparator();
    entry("commit", tr("Copy op id\tCtrl+C"), "timelineCopy", [opId] { QApplication::clipboard()->setText(QString::fromStdString(opId)); });
    entry("git", tr("Show in git log"), "timelineLog", [this, opId] { showOpGitLog(opId, m_doc->path()); });
    menu.addSeparator();
  }
  for (const char* id : {"timeline.rollForward", "timeline.names", "timeline.designOnly", "timeline.historyList"})
    if (QAction* a = action(id); a && (std::string(id) != "timeline.rollForward" || m_doc->rolledBack())) menu.addAction(a);
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
  if (m_doc->snapshotBusy()) return m_doc->afterCapture([this, opId] { guarded([&] { deleteOp(opId); }); });  // a copy being taken
  // A feature or a sketch: what depends on it is asked about first, the result previewed (smart selection, UI-96).
  const opad::Op* op = m_doc->doc.find_op(opId);
  const bool live = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), opId) == m_doc->scene.deleted_ops.end();
  if (op && live && (op->type == "feature" || op->type == "sketch") && areaCommand("timeline.delete", opId)) return m_timeline->setCurrentOp(opId);
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
  throw opad::Error("That operation is not tombstoned.");
}

void MainWindow::deleteCurrent() {
  std::string id = m_timeline->currentOp();
  if (!id.empty() && m_timeline->hasFocus()) return deleteOp(id);
  // Picked faces and edges go with what made them: smart selection deletes a feature's whole face set and offers the
  // rest (UI-04). Never the body's source op.
  if (areaCommand("edit.delete")) return;
  if (std::any_of(m_selRefs.begin(), m_selRefs.end(), [](const opad::Ref& r) { return r.kind != opad::Ref::Kind::Body; }))
    throw opad::Error("Faces and edges are deleted through the feature that made them: select it with Ctrl+Up, or use Remove faces.");
  const auto selected = currentNodeIds();
  if (selected.empty()) {  // the timeline's marker only when nothing is selected (an area's row is not that marker)
    if (id.empty() || !m_selRows.empty()) throw opad::Error("Select objects, or a marker on the timeline, to delete.");
    return deleteOp(id);
  }
  deleteNodes(selected);
}

void MainWindow::deleteNodes(const std::vector<std::string>& ids) {
  const smart::Deletion d = smart::routeDelete(m_doc->scene, ids);
  if (d.empty()) throw opad::Error("Nothing selected can be deleted.");
  QString what = ids.size() == 1 ? m_doc->nodeName(ids.front()) : tr("%1 objects").arg(ids.size());
  if (const opad::SketchItem* s = ids.size() == 1 ? m_doc->scene.sketch(ids.front()) : nullptr) what = QString::fromStdString(s->name);
  const QString text = d.remove.empty() ? tr("Deleted %1").arg(what) : tr("Removed %1: a Remove step on the timeline keeps its history").arg(what);
  m_design->applyOps(smart::deletionOps(d, m_doc->scene), tr("delete"), [this, text](bool ok, const QString& error) {
    if (!ok) return guarded([&] { throw opad::Error(error.toStdString()); });
    undoToast(text);
  });
}

void MainWindow::undoToast(const QString& text) {
  const auto depth = m_doc->undoLabels().size();
  const QString step = m_doc->undoLabel();
  const auto generation = m_doc->generation;
  m_toasts->toast(text, tr("Undo"), [this, depth, step, generation] {
    if (m_doc->generation == generation && m_doc->canUndo() && m_doc->undoLabels().size() == depth && m_doc->undoLabel() == step) return m_doc->undo();
    statusBar()->showMessage(tr("Other changes came after it: undo those first (Ctrl+Z)."), 6000);
  }, 8000);
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
