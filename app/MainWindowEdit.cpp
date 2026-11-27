// Edit: undo/redo, rename, hide/show, delete (tombstone) and restore, the timeline's menu and op targets.
#include "MainWindow.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStatusBar>

#include <algorithm>
#include <functional>
#include <set>

#include "AgentSentences.hpp"
#include "CommandHelp.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "SmartRules.hpp"

void MainWindow::buildEditActions() {
  addAction("edit.undo", tr("&Undo"), "rollLeft", QKeySequence::Undo, [this] {
    if (m_annotationEditor) return m_annotationEditor->undo();  // its strokes; the document waits for Save
    if (m_design->sketchActive()) return m_design->sketch()->undo();  // a sketch has its own history until it is finished
    if (areaHistory()) return (void)areaCommand("edit.undo");  // an area's mode (Edit point cloud's strokes)
    if (m_design->featureActive()) return (void)m_design->undoPick();  // a feature's panel: its last pick (UI-116)
    if (m_design->ownsSelection() || m_design->busy()) return;
    m_doc->undo();
  });
  QAction* redo = addAction("edit.redo", tr("&Redo"), "rollRight", QKeySequence::Redo, [this] {
    if (m_annotationEditor) return m_annotationEditor->redo();
    if (m_design->sketchActive()) return m_design->sketch()->redo();
    if (areaHistory()) return (void)areaCommand("edit.redo");
    if (m_design->ownsSelection() || m_design->busy()) return;
    m_doc->redo();
  });
  // Ctrl+Shift+Z as well (UI-09), as most apps take it, while the binding is the default one (Ctrl+Y here).
  if (const QKeySequence other("Ctrl+Shift+Z"); redo->shortcut() == QKeySequence(QKeySequence::Redo) && redo->shortcut() != other)
    redo->setShortcuts({redo->shortcut(), other});
  connect(m_doc, &AppDocument::undoChanged, this, &MainWindow::updateUndoActions);
  // Select all and Invert (UI-111): the sketch's curves while sketching, else the bodies on screen (visible, inside the
  // isolation); a tool or a feature input owns the picks meanwhile.
  addAction("edit.selectall", tr("Select all"), "", QKeySequence::SelectAll, [this] { selectShown(false); });
  addAction("edit.invert", tr("Invert selection"), "", QKeySequence("Ctrl+Shift+I"), [this] { selectShown(true); });
  addAction("edit.rename", tr("Rename"), "rename", QKeySequence("F2"), [this] {
    auto ids = currentNodeIds();
    // In the browser's row, expanded at once (the tree is hidden while it opens) and with the keyboard.
    if (ids.size() == 1) {
      m_browserOverlay->reveal(true);
      return m_browser->startRename(ids.front());
    }
    if (ids.empty() && m_selRows.size() == 1) {  // an area's row, when its folder renames
      m_browserOverlay->reveal(true);
      return m_browser->startRename(m_selRows.front());
    }
    if (ids.empty()) return;
    // Several at once: one name, numbered in selection order (TODO 10 B15).
    const QString base = renameBase(m_doc->nodeName(ids.front()));
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
    if (id.empty()) throw opad::UserHint("Select a delete or Remove step, or a tombstoned marker, on the timeline first.", true);
    restoreOp(id);
  });
  addAction("edit.selecttouched", tr("Select what it touches"), "isolate", QKeySequence("T"), [this] {
    if (!m_timeline->currentOp().empty()) selectOpTargets(m_timeline->currentOp());
  });
  // The last tool, feature, check or edit again (UI-100 / UI-111): Shift+Enter, the Edit menu, first in the context menus
  // as "Repeat Fillet".
  QAction* repeat = addAction("edit.repeat", tr("Repeat last command"), "repeat", QKeySequence("Shift+Return"), [this] {
    QAction* last = m_lastCommand.isEmpty() ? nullptr : action(m_lastCommand);
    if (!last || !last->isEnabled()) return statusBar()->showMessage(tr("Nothing to repeat yet: start a feature, a sketch tool or a measurement first."), 5000);
    last->trigger();
  });
  repeat->setProperty("shortcutHint", tr("Starts the last feature, sketch tool, measurement or note again."));
  repeat->setEnabled(false);
}

// The name a numbered rename starts from: "Bolt 3" -> "Bolt" (the number was lost to an unknown escape, \d, until UI-116).
QString MainWindow::renameBase(QString name) {
  static const QRegularExpression number(R"( \d+$)");
  return name.remove(number);
}

std::vector<std::string> MainWindow::shownBodies() const {
  std::set<std::string> isolated;
  for (const auto& id : m_viewport->isolatedNodes())
    for (const auto& b : m_doc->scene.bodies_under(id)) isolated.insert(b);
  std::vector<std::string> out;
  const std::function<void(const std::string&)> walk = [&](const std::string& id) {  // one pass down the tree
    const opad::Node* n = m_doc->node(id);
    if (!n || !n->visible) return;
    if (n->kind == opad::Node::Kind::Body && (isolated.empty() || isolated.count(id))) out.push_back(id);
    for (const auto& child : n->children) walk(child);
  };
  for (const auto& root : m_doc->scene.roots) walk(root);
  return out;
}

void MainWindow::selectShown(bool invert) {
  trace::Scope scope(invert ? "MainWindow: invert selection" : "MainWindow: select all");
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

void MainWindow::noteCommand(const QString& id) {
  if (!repeatable(id) || !action(id)) return;
  m_lastCommand = id;
  QAction* repeat = action("edit.repeat");
  repeat->setText(tr("Repeat %1").arg(QString(action(id)->text()).remove('&').remove(QString::fromUtf8("…"))));
  repeat->setEnabled(true);
  shortcuts::updateTooltip(repeat);
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
  const auto keyed = keys::menuText;  // the menu's key column: the command's key now, if it has one
  if (const opad::Op* menuOp = m_doc->doc.find_op(opId)) {
    const bool deleted = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), opId) != m_doc->scene.deleted_ops.end();
    // Deleting a delete record brings back what it deleted, so on a delete marker that entry is offered as what it does;
    // a record tombstoned by another (an older document) can be applied again.
    const bool deleteMarker = menuOp->type == "delete";
    entry(deleteMarker ? "restore" : "delete", keyed(deleteMarker ? tr("Restore what it deleted") : tr("Delete"), "edit.delete"), "timelineDelete", [this, opId] { if (requireEditable()) deleteOp(opId); });
    entry(deleteMarker ? "delete" : "restore", keyed(deleteMarker ? tr("Delete it again") : tr("Restore"), "edit.restore"), "timelineRestore", [this, opId] { if (requireEditable()) restoreOp(opId); })->setEnabled(deleted);
    const bool designOp = (menuOp->type == "feature" || menuOp->type == "sketch") && !deleted;
    const opad::Feature* feat = m_doc->scene.feature(opId);
    if (designOp) entry("rename", menuOp->type == "sketch" ? tr("Edit sketch") : tr("Edit feature"), "timelineEdit", [this, opId] { if (requireEditable()) m_design->editOp(opId); });
    if (designOp && feat) entry(feat->suppressed ? "eye" : "hide", feat->suppressed ? tr("Unsuppress") : tr("Suppress"), "timelineSuppress", [this, opId, on = !feat->suppressed] { if (requireEditable()) m_design->setSuppressed(opId, on); });
    if (designOp && menuOp->type == "sketch") entry("export", tr("Export sketch"), "timelineExport", [this, opId] { exportDialog({opId}); });
    if (designOp) menu.addSeparator();
    entry("isolate", keyed(tr("Select what it touches"), "edit.selecttouched"), "timelineTouched", [this, opId] { selectOpTargets(opId); });
    // The model as it was right after this step (UI-99): the playhead goes after its marker.
    if (!deleted && !m_doc->browse)
      entry("rollBack", tr("Roll back to here"), "timelineRollBack", [this, opId] { emit m_timeline->rollbackRequested(m_timeline->rollPointAfter(opId)); })
          ->setEnabled(!m_timeline->rollPointAfter(opId).empty() || m_doc->rolledBack());
    menu.addSeparator();
    entry("commit", keyed(tr("Copy op id"), "edit.copy"), "timelineCopy", [opId] { QApplication::clipboard()->setText(QString::fromStdString(opId)); });
    forEachArea([&](AreaController* area) { area->timelineMenu(opId, menu); });  // a read-only document asks for a copy first above
    menu.addSeparator();
  }
  for (const char* id : {"timeline.rollForward", "timeline.names", "timeline.designOnly", "timeline.historyList"})
    if (QAction* a = action(id); a && (std::string(id) != "timeline.rollForward" || m_doc->rolledBack())) menu.addAction(a);
}

bool MainWindow::areaHistory(bool* canUndo, bool* canRedo) const {
  if (!m_areasReady) return false;
  bool u = false, r = false;
  for (AreaController* area : m_areas)
    if (area->history(u, r)) {
      if (canUndo) *canUndo = u;
      if (canRedo) *canRedo = r;
      return true;
    }
  return false;
}

void MainWindow::updateUndoActions() {
  QAction* u = action("edit.undo");
  QAction* r = action("edit.redo");
  if (!u || !r) return;
  bool areaUndo = false, areaRedo = false;
  if (areaHistory(&areaUndo, &areaRedo)) {  // an area's mode keeps its own history (Edit point cloud)
    u->setEnabled(areaUndo);
    r->setEnabled(areaRedo);
    u->setText(tr("&Undo"));
    r->setText(tr("&Redo"));
  } else if(m_design && m_design->sketchActive()) {
    const auto* sketch=m_design->sketch();
    u->setEnabled(sketch->canUndo());r->setEnabled(sketch->canRedo());
    u->setText(tr("&Undo"));r->setText(tr("&Redo"));
  } else if (m_design && m_design->featureActive()) {  // a feature's panel: Undo takes back its last pick (UI-116)
    u->setEnabled(true);
    r->setEnabled(false);
    u->setText(tr("&Undo last pick"));
    r->setText(tr("&Redo"));
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
    const bool document = m_doc->hasDocument && !m_annotationEditor && !m_design->sketchActive() && !m_design->ownsSelection() && !m_design->busy() && !areaHistory();
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

// Del on a marker deletes its op for good (no tombstone): the op, and the edits and deletes aimed at it, leave the log;
// what later ops built on it fails or resolves as missing, marked on the timeline (the designer's to mend). A feature or
// a sketch asks first when later features use it, with the result previewed (smart selection, UI-96). One undo step.
void MainWindow::deleteOp(const std::string& requestedId) {
  const std::string opId=requestedId; // Rebuilding cards can destroy the signal sender during this operation.
  if (m_doc->snapshotBusy()) return m_doc->afterCapture([this, opId] { guarded([&] { deleteOp(opId); }); });  // a copy being taken
  const opad::Op* op = m_doc->doc.find_op(opId);
  if (!op) throw opad::UserHint("That operation is no longer in the document.");
  const bool live = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), opId) == m_doc->scene.deleted_ops.end();
  if (live && (op->type == "feature" || op->type == "sketch")) {
    if (m_doc->rolledBack())  // as Del on the marker: the question reads the last state, so rolled forward first
      if (QAction* forward = action("timeline.rollForward")) forward->trigger();
    if (areaCommand("timeline.delete", opId)) return m_timeline->setCurrentOp(opId);
  }
  removeOps({opId}, tr("delete"));
}

// Restore on a step that took things out (TODO 11 help audit P9.2: a delete record, or the Remove step Del adds in a
// design) deletes that step, which brings back what it removed; on an op a delete record tombstoned (an older document,
// an agent's delete), that record goes. One undo step either way.
void MainWindow::restoreOp(const std::string& requestedId) {
  const std::string opId=requestedId;
  const opad::Op* step = m_doc->doc.find_op(opId);
  if (!step) throw opad::UserHint("That operation is no longer in the document.");
  const bool live = !m_doc->doc.is_deleted(opId);
  const opad::Feature* removal = live && step->type == "feature" ? m_doc->scene.feature(opId) : nullptr;
  if (live && (step->type == "delete" || (removal && removal->kind == "remove"))) return removeOps({opId}, tr("restore"));
  std::vector<std::string> records;
  for (const auto& op : m_doc->doc.ops)
    if (op.type == "delete" && op.data.value("target", "") == opId && !m_doc->doc.is_deleted(op.id)) records.push_back(op.id);
  if (records.empty()) throw opad::UserHint("That operation is not deleted.");
  removeOps(records, tr("restore"));
}

void MainWindow::removeOps(const std::vector<std::string>& opIds, const QString& label, const QString& said) {
  if (opIds.empty()) return;
  m_design->removeOps(opIds, label, [this, said](bool ok, const QString& error) {
    if (!ok) return guarded([&] { throw opad::Error(error.toStdString()); });
    if (!said.isEmpty()) undoToast(said);
  });
}

void MainWindow::deleteCurrent() {
  std::string id = m_timeline->currentOp();
  if (!id.empty() && m_timeline->hasFocus()) return deleteOp(id);
  if (!m_selRows.empty() && m_browser->removeRows(m_selRows)) return;  // an area's rows (the Drawings folder's): its folder deletes them
  // Picked faces and edges go with what made them: smart selection deletes a feature's whole face set and offers the
  // rest (UI-04). Never the body's source op.
  if (areaCommand("edit.delete")) return;
  if (std::any_of(m_selRefs.begin(), m_selRefs.end(), [](const opad::Ref& r) { return r.kind != opad::Ref::Kind::Body; }))
    throw opad::Error("Faces and edges are deleted through the feature that made them: select it with Select parent ({key:edit.selectparent}), or use Remove faces.");
  const auto selected = currentNodeIds();
  if (selected.empty()) {  // the timeline's marker only when nothing is selected (an area's row or a pick is not that marker)
    if (id.empty() || !m_selRows.empty() || !m_selRefs.empty()) throw opad::UserHint("Select objects, or a marker on the timeline, to delete.");
    return deleteOp(id);
  }
  deleteNodes(selected);
}

void MainWindow::deleteNodes(const std::vector<std::string>& ids) {
  // A sketch alone as from its marker: what uses it named first, the result previewed (UI-96).
  if (ids.size() == 1 && m_doc->scene.sketch(ids.front())) return deleteOp(ids.front());
  const smart::Deletion d = smart::routeDelete(m_doc->scene, ids);
  if (d.empty()) throw opad::Error("Nothing selected can be deleted.");
  const QString what = ids.size() == 1 ? m_doc->nodeName(ids.front()) : tr("%1 objects").arg(ids.size());
  const QString text = d.remove.empty() ? tr("Deleted %1").arg(what) : tr("Removed %1: a Remove step on the timeline keeps its history").arg(what);
  // What made them goes for good (an import, a sketch); bodies of a design history leave through a Remove step.
  m_design->removeOps(d.tombstone, tr("delete"), [this, text](bool ok, const QString& error) {
    if (!ok) return guarded([&] { throw opad::Error(error.toStdString()); });
    undoToast(text);
  }, smart::deletionOps({{}, d.remove}, m_doc->scene));
}

Toast* MainWindow::undoToast(const QString& text, int ms) {
  const auto depth = m_doc->undoLabels().size();
  const QString step = m_doc->undoLabel();
  const auto generation = m_doc->generation;
  return m_toasts->toast(text, tr("Undo"), [this, depth, step, generation] {
    if (m_doc->generation == generation && m_doc->canUndo() && m_doc->undoLabels().size() == depth && m_doc->undoLabel() == step) return m_doc->undo();
    statusBar()->showMessage(help::expand(tr("Other changes came after it: undo those first ({key:edit.undo}).")), 6000);
  }, ms);
}

// Several ops deleted for good as one step (UI-02): one removal, planned on a worker with a design history.
void MainWindow::deleteOps(const std::vector<std::string>& opIds) {
  if (opIds.size() == 1) return deleteOp(opIds.front());
  removeOps(opIds, tr("delete"));
}

// Hide others (UI-02): one step, and the fewest nodes (a subtree with nothing kept is hidden as a whole), where it used to
// be one command and one undo step per body (1,294 on the Engine, about 100 s).
void MainWindow::hideOthers(const std::vector<std::string>& keep) {
  if (keep.empty()) throw opad::UserHint("Select the objects to keep shown first.", true);
  const auto hide = m_doc->scene.others_to_hide(keep);
  if (hide.empty()) return;
  m_doc->batch(tr("hide others"), [&] { m_doc->run("appearance", opad::json{{"targets", hide}, {"visible", false}}); });
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
  // Who and when: "Claude (AI) · 14:02" for an AI assistant's step (its name saved as ai_client), a person's as saved.
  m_props->showEntity(m_timeline->describe(*op), agentsay::authorLine(d, QDateTime::currentDateTime()),
                      QString::fromStdString(opId.substr(0, 8)), d);
}
