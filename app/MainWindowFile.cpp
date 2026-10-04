// Files: open, import, save, viewer mode (save to edit), recent files, load progress, drag and drop.
#include "MainWindow.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <utility>

#include "AssetsArea.hpp"
#include "FileLocation.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "KicadBoards.hpp"
#include "Units.hpp"
#include "KeyGuard.hpp"
#include "Toast.hpp"
#include "opad/drawing_io.hpp"

void MainWindow::buildFileActions() {
  addAction("file.new", tr("&New document"), "doc", QKeySequence::New, [this] { if (maybeSave([this] { action("file.new")->trigger(); })) m_doc->newDocument(); });
  addAction("file.open", tr("&Open…"), "open", QKeySequence("Ctrl+O"), [this] {
    // openPath asks about unsaved changes once a file is chosen (asking here too asked twice after Discard).
    QString p = QFileDialog::getOpenFileName(this, tr("Open"), m_settings.value("ui/lastDir").toString(),
                                             tr("Design files (%1);;OPAD document (*.opad);;CAD models (*.step *.stp *.iges *.igs *.brep *.brp);;"
                                                "Meshes (*.stl *.3mf *.obj *.ply *.gltf *.glb *.wrl *.vrml);;2D drawings (*.dxf *.dwg *.svg)").arg(fileFilter(true)) +
                                                ";;" + tr("KiCad boards (*.kicad_pcb)"));
    if (!p.isEmpty()) openPath(p);
  });
  addAction("file.import", tr("&Import…"), "import", QKeySequence("Ctrl+I"), [this] {
    QString p = QFileDialog::getOpenFileName(this, tr("Import design"), m_settings.value("ui/lastDir").toString(), tr("Design files (%1)").arg(fileFilter(false)));
    if (!p.isEmpty()) importPath(p, -1);
  });
  addAction("file.importdoc", tr("Save as OPAD document…"), "save", QKeySequence("Ctrl+Shift+E"), [this] { if (m_doc->browse) saveViewerAs(); });
  addAction("file.save", tr("&Save"), "save", QKeySequence("Ctrl+S"), [this] {
    if (m_doc->browse) { saveViewerAs(); return; }  // viewer mode: saving makes it an OPAD document, which can be edited
    if (m_doc->readOnly) { saveReadOnlyCopy(); return; }  // the file stays as it is
    if (m_doc->doc.path.empty()) action("file.saveas")->trigger();
    else m_doc->save();
  });
  addAction("file.saveas", tr("Save &As…"), "save", QKeySequence("Ctrl+Shift+S"), [this] {
    if (m_doc->browse) { saveViewerAs(); return; }
    if (m_doc->readOnly) { saveReadOnlyCopy(); return; }
    QString p = QFileDialog::getSaveFileName(this, tr("Save document"), m_settings.value("ui/lastDir").toString(), tr("OPAD document (*.opad)"));
    if (p.isEmpty()) return;
    if (!p.endsWith(".opad", Qt::CaseInsensitive)) p += ".opad";
    m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
    m_doc->saveAs(p);
    addRecent(p);
  });
  CommandInfo saveTemplate;  // UI-113: a copy of this document among the start page's templates
  saveTemplate.id = "file.savetemplate";
  saveTemplate.label = tr("Save as template…");
  saveTemplate.icon = "template";
  saveTemplate.keywords = {"template", "starting point", "default document"};
  saveTemplate.enabledWhen = [](const CommandContext& c) { return c.document && !c.viewer; };
  addCommand(saveTemplate, [this] { saveAsTemplate(); });
  addAction("file.export", tr("&Export…"), "export", QKeySequence("Ctrl+E"), [this] { exportDialog(); });
  addAction("file.screenshot", tr("Save screens&hot…"), "export", QKeySequence("Ctrl+Shift+P"), [this] { screenshot(); });
  addAction("file.close", tr("&Close document"), "close", QKeySequence("Ctrl+W"), [this] {
    if (!m_doc->hasDocument || m_doc->loading || !maybeSave([this] { action("file.close")->trigger(); })) return;
    m_viewport->clearSelection();
    m_doc->closeDocument();  // AppDocument::changed -> showDocument(false) -> the start screen
    statusBar()->showMessage(tr("Document closed"), 4000);
  });
  addAction("file.quit", tr("&Quit"), "", QKeySequence::Quit, [this] { close(); })->setMenuRole(QAction::QuitRole);
}

void MainWindow::importPath(const QString& p, int mode) {
  m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
  auto ids = currentNodeIds();
  QString parent;  // the active component's (AppDocument::startImport) unless the root is active and one is selected
  if (m_doc->activeComponent().empty() && ids.size() == 1 && m_doc->node(ids[0]) && m_doc->node(ids[0])->kind == opad::Node::Kind::Component && !m_doc->node(ids[0])->linked &&
      QMessageBox::question(this, tr("Import"), tr("Import under the selected component “%1”?").arg(m_doc->nodeName(ids[0]))) == QMessageBox::Yes)
    parent = QString::fromStdString(ids[0]);
  const QString suffix = QFileInfo(p).suffix().toLower();
  const bool drawing = suffix == "dxf" || suffix == "svg" || suffix == "dwg";
  if (suffix == "kicad_pcb" && KicadDialog(this, true).exec() != QDialog::Accepted) return;
  bool link = mode == 1 || (suffix == "kicad_pcb" && KicadDialog::linked());  // KiCad's export: linked to its board
  if (!link && mode < 0) {
    const assets::Mode chosen = assets::askImport(this, p);
    if (chosen == assets::Mode::Cancel) return;
    link = chosen == assets::Mode::Link;
  }
  if (drawing) return importDrawing(p, parent, link);
  beginLoad([this, p] { addRecent(p); m_viewport->fitWhenReady(); }, tr("Importing %1").arg(QFileInfo(p).fileName()), tr("Imported %1 · %2 bodies").arg(QFileInfo(p).fileName()));
  m_doc->startImport(p, parent, {}, {}, link);
}

bool MainWindow::isEditAction(const QString& id) {
  if (id.startsWith("sketch.")) return true;
  // Design tools change the model; how it looks (colour, opacity, lock) is a view setting while viewing.
  if (id.startsWith("design.")) return id != "design.colour" && id != "design.opacity" && id != "design.lock";
  static const QStringList edits = {"edit.rename", "edit.delete", "edit.restore", "edit.cut", "edit.paste", "edit.pastelinked", "annotate.add", "annotate.draw", "annotate.resolve",
                                    "inspect.pin", "view.saveview", "view.setHome", "view.resetHome", "file.import"};
  return edits.contains(id);
}

QString MainWindow::fileFilter(bool withOpad) {
  QStringList patterns;
  if (withOpad) patterns << "*.opad";
  for (const auto& ext : opad::importable_extensions()) patterns << "*" + QString::fromStdString(ext);
  return patterns.join(' ');
}

// Viewer mode: everything that edits says so and offers to save the file as an OPAD document, which can be edited;
// `resume` (the command that asked) runs again once that is done. A read-only .opad offers a copy the same way.
bool MainWindow::requireEditable(std::function<void()> resume) {
  if (!m_doc->viewOnly()) return true;
  QMessageBox box(this);
  box.setIcon(QMessageBox::Information);
  if (m_doc->readOnly) {
    box.setWindowTitle(tr("Read-only"));
    box.setText(tr("Save a copy to edit"));
    box.setInformativeText(tr("“%1” is open read-only. Save a copy to edit it; this file stays as it is.").arg(QFileInfo(m_doc->path()).fileName()));
    QPushButton* save = box.addButton(tr("Save a copy…"), QMessageBox::AcceptRole);
    QPushButton* copy = box.addButton(tr("Edit unsaved copy"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(save);
    box.exec();
    if (box.clickedButton() == save) saveReadOnlyCopy(std::move(resume));
    else if (box.clickedButton() == copy) {
      m_doc->detach();
      statusBar()->showMessage(tr("Editable copy: save it to keep your changes"), 8000);
      if (resume && !m_doc->readOnly) resume();
    }
    return false;
  }
  box.setWindowTitle(tr("Viewer mode"));
  box.setText(tr("Save first to edit"));
  box.setInformativeText(tr("“%1” is open in viewer mode, read-only. Save it as an OPAD document to edit it; the file you opened stays as it is.")
                             .arg(QFileInfo(m_doc->viewing).fileName()));
  QPushButton* save = box.addButton(tr("Save as OPAD…"), QMessageBox::AcceptRole);
  QPushButton* copy = box.addButton(tr("Edit unsaved copy"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(save);
  box.exec();
  if (box.clickedButton() == save) saveViewerAs(std::move(resume));
  else if (box.clickedButton() == copy) makeEditable({}, std::move(resume));
  return false;
}

void MainWindow::saveViewerAs(std::function<void()> then) {
  if (!m_doc->browse) return;
  const QFileInfo source(m_doc->viewing);
  QString path = QFileDialog::getSaveFileName(this, tr("Save as OPAD document"), source.absolutePath() + "/" + source.completeBaseName() + ".opad",
                                              tr("OPAD document (*.opad)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(".opad", Qt::CaseInsensitive)) path += ".opad";
  m_settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
  makeEditable(path, std::move(then));
}

// Viewer -> editable in place (the shapes on screen are kept, see AppDocument::startEditable), then written to `savePath`
// on a worker if one was chosen; `then` runs when the document can be edited.
void MainWindow::makeEditable(const QString& savePath, std::function<void()> then) {
  statusBar()->showMessage(tr("Preparing %1 for editing…").arg(QFileInfo(m_doc->viewing).fileName()));
  // A command resumed afterwards (Rename) gets the one-key presses typed while the copy is made, the window's shortcuts
  // none of them (UI-09); KeyGuard drops them when it opens no editor.
  QPointer<KeyGuard> guard = then ? m_keyGuard : nullptr;
  if (guard) guard->hold();
  auto finish = [then, guard](bool resume) {
    const std::function<void()> command = resume ? then : nullptr;
    if (guard) guard->release(command);
    else if (command) command();
  };
  m_doc->startEditable(m_jobs, [this, savePath, finish](bool ok, const QString& error) {
    if (!ok) {
      finish(false);
      statusBar()->clearMessage();
      QMessageBox::warning(this, tr("OPAD"), i18n::t(error));
      return;
    }
    updateViewerCard();
    if (savePath.isEmpty()) {
      statusBar()->clearMessage();
      m_toasts->toast(tr("Editable copy: save it to keep your changes"), tr("Save"), [this] { action("file.save")->trigger(); }, 8000);
      finish(true);
      return;
    }
    bool saving = false;
    guarded([&] {
      m_doc->saveAsync(m_jobs, savePath, true, [this, savePath, finish](bool saved, const QString& why) {
        if (!saved) { finish(false); QMessageBox::warning(this, tr("OPAD"), i18n::t(why)); return; }
        addRecent(savePath);
        m_viewPath = QFileInfo(savePath).absoluteFilePath();
        statusBar()->clearMessage();
        resultToast(tr("Saved %1; it can be edited now").arg(QFileInfo(savePath).fileName()), QFileInfo(savePath).absolutePath());
        finish(true);
      });
      saving = true;
    });
    if (!saving) finish(false);
  });
}

// Read-only .opad: a copy goes where the user says (among their documents for a version kept in the temporary folder),
// written on a worker; this session edits the copy from then on and the file it opened stays as it was.
void MainWindow::saveReadOnlyCopy(std::function<void()> then) {
  if (!m_doc->readOnly) return;
  const QFileInfo source(m_doc->path());
  const bool scratch = source.absoluteFilePath().startsWith(QDir::temp().absolutePath(), Qt::CaseInsensitive);
  const QString folder = scratch ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : source.absolutePath();
  const QString name = scratch ? source.completeBaseName() : tr("%1 copy").arg(source.completeBaseName());
  QString path = QFileDialog::getSaveFileName(this, tr("Save a copy"), folder + "/" + name + ".opad", tr("OPAD document (*.opad)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(".opad", Qt::CaseInsensitive)) path += ".opad";
  m_settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
  saveCopy(path, std::move(then));
}

void MainWindow::saveCopy(const QString& path, std::function<void()> then) {
  guarded([&] {
    m_doc->saveAsync(m_jobs, path, true, [this, path, then](bool saved, const QString& why) {
      if (!saved) { QMessageBox::warning(this, tr("OPAD"), i18n::t(why)); return; }
      addRecent(path);
      m_viewPath = QFileInfo(path).absoluteFilePath();
      if (!m_doc->readOnly) units::setSessionUnit({});  // the copy shows its own unit, as an editable document does
      updateViewerCard();
      statusBar()->showMessage(tr("Saved a copy as %1; it can be edited").arg(QDir::toNativeSeparators(path)), 8000);
      if (then && !m_doc->readOnly) then();
    });
  });
}

void MainWindow::updateViewerCard() {
  if (!m_chips) return;
  const QString shown = m_doc->browse ? m_doc->viewing : m_doc->readOnly ? m_doc->path() : QString();
  m_chips->setViewer(QFileInfo(shown).fileName(), m_doc->readOnly);
  positionOverlays();
}

// ---------------------------------------------------------------- export (F14/F15)

void MainWindow::screenshot() {
  QString out = QFileDialog::getSaveFileName(this, tr("Save screenshot"), m_settings.value("ui/lastDir").toString(), tr("PNG image (*.png)"));
  if (out.isEmpty()) return;
  if (!out.endsWith(".png", Qt::CaseInsensitive)) out += ".png";
  QImage img = m_viewport->grabImage();
  if (img.isNull() || !img.save(out)) throw opad::Error("Screenshot failed");
  resultToast(tr("Saved %1").arg(QFileInfo(out).fileName()), QFileInfo(out).absolutePath());
}

// ---------------------------------------------------------------- recent files
QStringList MainWindow::recent() const { return m_settings.value("ui/recent").toStringList(); }

void MainWindow::addRecent(const QString& path) {
  QStringList list = recent();
  list.removeAll(path);
  list.prepend(path);
  while (list.size() > 8) list.removeLast();
  m_settings.setValue("ui/recent", list);
  m_empty->setRecent(list);
  rebuildRecentMenu();
}

void MainWindow::removeRecent(const QString& path) {
  QStringList list = recent();
  list.removeAll(path);
  m_settings.setValue("ui/recent", list);
  m_empty->setRecent(list);
  rebuildRecentMenu();
}

QMenu* MainWindow::recentMenu(const QString& path, QWidget* parent) {
  return location::recentMenu(path, parent, [this](const QString& text) { tell(text); }, [this, path] { openPath(path); },
                              [this, path] { removeRecent(path); });
}

void MainWindow::tell(const QString& text) {
  if (m_stack->currentWidget() == m_empty) statusBar()->showMessage(text, 6000);  // the start page: no view to show a toast over
  else m_toasts->toast(text);
}

void MainWindow::rebuildRecentMenu() {
  if (!m_recentMenu) return;
  m_recentMenu->clear();
  for (const QString& p : recent()) {
    QAction* a = m_recentMenu->addAction(icons::themed("recent", 16), p);
    a->setData(p);  // a right click: its own menu (Open file location, Copy path, Remove from list)
    connect(a, &QAction::triggered, this, [this, p] { openPath(p); });
  }
  if (m_recentMenu->isEmpty()) m_recentMenu->addAction(tr("No recent files"))->setEnabled(false);
}

// ---------------------------------------------------------------- templates (UI-113)
// A built-in template is a new document set up a little; a template file opens on the worker like any .opad and then
// becomes an untitled copy with an identity of its own (the template is never written to, nor listed as recent).
void MainWindow::newFromTemplate(const QString& id) {
  if (m_doc->loading || !maybeSave([this, id] { newFromTemplate(id); })) return;
  if (id.startsWith("builtin:")) {
    m_doc->newDocument();
    if (id == "builtin:in") setDocumentUnit("in");
    if (id == "builtin:sketch") {
      setWorkspace("design");
      action("design.sketch")->trigger();  // asks for the plane
    }
    return;
  }
  const QString name = QFileInfo(id).completeBaseName();
  beginLoad([this] { m_doc->detachCopy(); m_viewport->fitWhenReady(); }, tr("New from %1").arg(name), tr("New from %1 · %2 bodies").arg(name));
  m_doc->startOpen(id);
}

void MainWindow::saveAsTemplate() {
  bool ok = false;
  const QString suggested = m_doc->path().isEmpty() ? tr("My template") : QFileInfo(m_doc->path()).completeBaseName();
  QString name = QInputDialog::getText(this, tr("Save as template"), tr("Template name:"), QLineEdit::Normal, suggested, &ok).trimmed();
  for (const QChar c : QString("<>:\"/\\|?*")) name.replace(c, '_');
  if (!ok || name.isEmpty()) return;
  const QString path = templates::folder() + "/" + name + ".opad";
  if (QFileInfo::exists(path) && QMessageBox::question(this, tr("Save as template"), tr("Replace the template “%1”?").arg(name)) != QMessageBox::Yes) return;
  saveTemplate(path);
}

// A copy of the document (taken on a worker) written with an identity of its own, so documents made from it are not this one.
void MainWindow::saveTemplate(const QString& path) {
  const QString name = QFileInfo(path).completeBaseName(), folder = QFileInfo(path).absolutePath();
  const bool started = m_doc->captureSnapshot(m_jobs, [this, path, name, folder](std::shared_ptr<opad::Document> copy, const QString& error) {
    if (!copy) return failedToast(error.isEmpty() ? tr("The template was not saved.") : i18n::t(error));
    m_jobs->async(tr("Saving the template %1").arg(name), [copy, path, folder](Progress) {
      QDir().mkpath(folder);
      copy->header.uuid = opad::new_uuid();
      copy->header.created = opad::now_iso8601();
      copy->save_as(std::filesystem::path(path.toStdU16String()));
    }, [this, name, folder](bool ok, const QString& error) {
      if (!ok) return failedToast(i18n::t(error));
      resultToast(tr("Saved as the template %1; New from template offers it").arg(name), folder);
    });
  });
  if (!started) hint(tr("The document is busy; try again in a moment."), false);
}

void MainWindow::rebuildTemplateMenu() {
  m_templateMenu->clear();
  for (const templates::Entry& e : templates::list())
    m_templateMenu->addAction(icons::themed(e.icon, 16), e.title, this, [this, id = e.id] { guarded([&] { newFromTemplate(id); }); });
  m_templateMenu->addSeparator();
  m_templateMenu->addAction(icons::themed("folder", 16), tr("Open templates folder"), this, [] {
    const QString dir = templates::folder();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
  });
}

// ---------------------------------------------------------------- lifecycle
void MainWindow::openPath(const QString& path, bool readOnly) {
  // A load already running is dropped (the next file wins, as when stepping through a folder); the document on screen
  // has not changed since it was asked about.
  if (!m_doc->loading && !maybeSave([this, path] { openPath(path); })) return;
  if (m_doc->loading && m_loadJob) m_loadJob->cancel();
  if (m_doc->loading) m_doc->cancelLoad();
  m_settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
  beginLoad([this, path] { m_viewPath=QFileInfo(path).absoluteFilePath(); addRecent(path); m_viewport->fitWhenReady(); updateViewerCard(); },
            tr("Opening %1").arg(QFileInfo(path).fileName()), tr("Opened %1 · %2 bodies").arg(QFileInfo(path).fileName()));
  // A drawing is picked by its edges (see loadFinished): set before its bodies are displayed, so each is activated once.
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix == "dxf" || suffix == "dwg" || suffix == "svg") {
    if (m_viewport->selectionFilter() != Viewport::SelFilter::Edge) m_autoEdges = true;
    m_viewport->setSelectionFilter(Viewport::SelFilter::Edge);
  } else if (m_autoEdges) {  // the next file that is not a drawing picks bodies again, unless the filter was chosen meanwhile
    m_autoEdges = false;
    m_viewport->setSelectionFilter(Viewport::SelFilter::Body);
  }
  m_doc->startOpen(path, readOnly);
}

// ---------------------------------------------------------------- load progress
// One job spans the document worker (reading/translating/building) and the viewport's tessellation.
void MainWindow::beginLoad(std::function<void()> after, const QString& title, const QString& done) {
  if (m_loadJob) m_loadJob->cancel();
  m_afterLoad = std::move(after);
  m_loadDone = done;
  m_loadDocDone = false;
  m_meshTotal = m_meshRemaining = 0;
  m_viewport->resetMeshing();
  m_loadJob = m_jobs->begin(title.isEmpty() ? tr("Loading…") : title, true);
  m_loadShade->setStatus(m_loadJob->title(), QString(), -1);
  m_viewport->setStreamJob(m_loadJob);  // the display pump is its child: one Cancel stops reading, meshing and showing
  setLoading(true);
  // OPAD_BENCH_LOADSHOT=<prefix>: the status bar every 2 s while the load runs (<prefix>-<n>.png).
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_LOADSHOT"); !shot.isEmpty()) {
    auto* timer = new QTimer(this);
    auto count = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, this, [this, shot, timer, count] {
      statusBar()->grab().save(QString("%1-%2.png").arg(shot).arg(++*count));
      if (!m_loadJob || *count >= 30) { timer->stop(); timer->deleteLater(); }
    });
    timer->start(2000);
  }
  connect(m_loadJob, &Job::cancelRequested, this, [this] {
    m_doc->cancelLoad();
    m_viewport->cancelMeshing();
  });
  connect(m_loadJob, &Job::finished, this, [this](bool ok, const QString& err) {
    m_loadJob = nullptr;
    m_afterLoad = nullptr;
    setLoading(false);
    const QPointer<QAction> deferred = std::exchange(m_afterStream, nullptr);
    if (!ok) {
      if (err.contains("cancel", Qt::CaseInsensitive)) resultToast(tr("Load cancelled"));
      else QMessageBox::warning(this, tr("OPAD"), i18n::message(err));
    }
    if(ok) {
      m_doc->storeViewerCache(m_jobs);  // a slow viewer read, now meshed: the next open of the file skips it
      if(!m_benchSelect) QTimer::singleShot(0, this, [this] { offerKicadModels(); if(trustAfterLoad()) offerAssetTrust(); });  // library models, linked files (benches call them)
      if (deferred) QTimer::singleShot(0, deferred, &QAction::trigger);  // the edit asked for while the bodies streamed in
    }
    if (int skipped = m_viewport->skippedCount()) m_toasts->toast(tr("%1 bodies were not tessellated (cancelled); reopen the file to show them").arg(skipped), QString(), {}, 8000);
    if (m_benchSelect && !ok && !m_doc->hasDocument) {  // nothing to run the benches on: say so instead of walking an empty scene
      trace::log("bench: load failed: " + err);
      QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(3); });
    } else if (m_benchSelect) QTimer::singleShot(300, this, &MainWindow::runBench);
  });
}

QString MainWindow::meshPhase() const {
  return tr("Tessellating and displaying bodies (%1 of %2)").arg(m_meshTotal - m_meshRemaining).arg(m_meshTotal);
}

// The document's own phases come with their place in the whole load (AppDocument::loadProgress); the display of the bodies
// fills the rest, from AppDocument::displayStart (UI-40).
void MainWindow::setLoadPhase(const QString& phase, int pct, int overall) {
  if (!m_loadJob) return;
  if (overall < 0) overall = m_doc->displayStart() + std::max(pct, 0) * (100 - m_doc->displayStart()) / 100;
  if (trace::enabled()) trace::log(QStringLiteral("load phase: %1 (%2%, overall %3%)").arg(phase).arg(pct).arg(overall));
  m_loadJob->setPhase(phase, pct);
  m_loadJob->setOverall(overall);
  m_loadShade->setStatus(m_loadJob->title(), phase, pct);
}

void MainWindow::restoreLastView() {
  if (!m_doc->path().isEmpty()) m_viewPath = QFileInfo(m_doc->path()).absoluteFilePath();
  if ((m_benchSelect && !qEnvironmentVariableIsSet("OPAD_BENCH_NAVIGATION")) || m_viewPath.isEmpty() || m_settings.value("view/lastPath").toString() != m_viewPath) return;
  try {
    const auto camera = opad::json::parse(m_settings.value("view/lastCamera").toString().toStdString());
    action("view.ortho")->setChecked(camera.value("projection", "") == "orthographic");
    m_viewport->setCameraJson(camera);
  } catch (const std::exception&) { /* Ignore stale settings from another version. */ }
}

void MainWindow::deferEdit(QAction* a) {
  if (a->isCheckable()) { QSignalBlocker block(a); a->setChecked(!a->isChecked()); }
  m_afterStream = a;  // the last one asked for
  m_toasts->toast(tr("Still loading: “%1” runs once every body is shown").arg(a->text().remove('&')));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
  for (const QUrl& u : e->mimeData()->urls()) {
    QString p = u.toLocalFile();
    QString ext = QFileInfo(p).suffix().toLower();
    if (QAction* canvas = action("canvas.insert"); canvas && m_doc->hasDocument && !m_doc->browse && QStringList{"png", "jpg", "jpeg", "bmp", "gif", "webp"}.contains(ext)) {
      canvas->setProperty("file", p);  // a picture dropped onto a document: an image canvas in it (UI-70)
      canvas->trigger();
      return;
    }
    if (ext == "opad" || fileFilter(false).split(' ').contains("*." + ext)) { openPath(p); return; }
  }
}

void MainWindow::saveLastView() {
  if((m_benchSelect && !qEnvironmentVariableIsSet("OPAD_BENCH_NAVIGATION")) || m_viewPath.isEmpty() || (m_design && m_design->sketchActive())) return;
  const auto camera=m_viewport->cameraJson(); if(camera.empty()) return;
  m_settings.setValue("view/lastPath",m_viewPath);
  m_settings.setValue("view/lastCamera",QString::fromStdString(camera.dump()));
}
