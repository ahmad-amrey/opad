// Files: open, import, save, viewer mode (save to edit), recent files, load progress, drag and drop.
#include "MainWindow.hpp"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>

#include <memory>

#include "AssetsArea.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "KicadBoards.hpp"
#include "opad/drawing_io.hpp"

void MainWindow::buildFileActions() {
  addAction("file.new", tr("&New document"), "doc", QKeySequence::New, [this] { if (maybeSave()) m_doc->newDocument(); });
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
    if (m_doc->doc.path.empty()) action("file.saveas")->trigger();
    else m_doc->save();
  });
  addAction("file.saveas", tr("Save &As…"), "save", QKeySequence("Ctrl+Shift+S"), [this] {
    if (m_doc->browse) { saveViewerAs(); return; }
    QString p = QFileDialog::getSaveFileName(this, tr("Save document"), m_settings.value("ui/lastDir").toString(), tr("OPAD document (*.opad)"));
    if (p.isEmpty()) return;
    if (!p.endsWith(".opad", Qt::CaseInsensitive)) p += ".opad";
    m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
    m_doc->saveAs(p);
    addRecent(p);
  });
  addAction("file.export", tr("&Export…"), "export", QKeySequence("Ctrl+E"), [this] { exportDialog(); });
  addAction("file.screenshot", tr("Save screens&hot…"), "export", QKeySequence("Ctrl+Shift+P"), [this] { screenshot(); });
  addAction("file.close", tr("&Close document"), "close", QKeySequence("Ctrl+W"), [this] {
    if (!m_doc->hasDocument || m_doc->loading || !maybeSave()) return;
    m_viewport->clearSelection();
    m_doc->closeDocument();  // AppDocument::changed -> showDocument(false) -> the start screen
    statusBar()->showMessage(tr("Document closed"), 4000);
  });
  addAction("file.quit", tr("&Quit"), "", QKeySequence::Quit, [this] { close(); })->setMenuRole(QAction::QuitRole);
}

void MainWindow::importPath(const QString& p, int mode) {
  m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
  auto ids = currentNodeIds();
  QString parent;
  if (ids.size() == 1 && m_doc->node(ids[0]) && m_doc->node(ids[0])->kind == opad::Node::Kind::Component && !m_doc->node(ids[0])->linked &&
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
  beginLoad([this, p] { addRecent(p); m_viewport->fitWhenReady(); });
  m_doc->startImport(p, parent, {}, {}, link);
}

bool MainWindow::isEditAction(const QString& id) {
  if (id.startsWith("sketch.")) return true;
  // Design tools change the model; how it looks (colour, opacity, lock) is a view setting while viewing.
  if (id.startsWith("design.")) return id != "design.colour" && id != "design.opacity" && id != "design.lock";
  static const QStringList edits = {"edit.rename", "edit.delete", "edit.restore", "annotate.add", "annotate.draw", "annotate.resolve",
                                    "inspect.pin", "view.saveview", "file.import"};
  return edits.contains(id);
}

QString MainWindow::fileFilter(bool withOpad) {
  QStringList patterns;
  if (withOpad) patterns << "*.opad";
  for (const auto& ext : opad::importable_extensions()) patterns << "*" + QString::fromStdString(ext);
  return patterns.join(' ');
}

// Viewer mode: everything that edits says so and offers to save the file as an OPAD document, which can be edited;
// `resume` (the command that asked) runs again once that is done.
bool MainWindow::requireEditable(std::function<void()> resume) {
  if (!m_doc->browse) return true;
  QMessageBox box(this);
  box.setIcon(QMessageBox::Information);
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
  m_doc->startEditable(m_jobs, [this, savePath, then](bool ok, const QString& error) {
    if (!ok) {
      statusBar()->clearMessage();
      QMessageBox::warning(this, tr("OPAD"), i18n::t(error));
      return;
    }
    updateViewerCard();
    if (savePath.isEmpty()) {
      statusBar()->showMessage(tr("Editable copy: save it to keep your changes"), 8000);
      if (then) then();
      return;
    }
    guarded([&] {
      m_doc->saveAsync(m_jobs, savePath, true, [this, savePath, then](bool saved, const QString& why) {
        if (!saved) { QMessageBox::warning(this, tr("OPAD"), i18n::t(why)); return; }
        addRecent(savePath);
        m_viewPath = QFileInfo(savePath).absoluteFilePath();
        statusBar()->showMessage(tr("Saved %1; it can be edited now").arg(QDir::toNativeSeparators(savePath)), 8000);
        if (then) then();
      });
    });
  });
}

void MainWindow::updateViewerCard() {
  if (!m_chips) return;
  m_chips->setViewer(m_doc->browse ? QFileInfo(m_doc->viewing).fileName() : QString());
  positionOverlays();
}

// ---------------------------------------------------------------- export (F14/F15)

void MainWindow::screenshot() {
  QString out = QFileDialog::getSaveFileName(this, tr("Save screenshot"), m_settings.value("ui/lastDir").toString(), tr("PNG image (*.png)"));
  if (out.isEmpty()) return;
  if (!out.endsWith(".png", Qt::CaseInsensitive)) out += ".png";
  QImage img = m_viewport->grabImage();
  if (img.isNull() || !img.save(out)) throw opad::Error("Screenshot failed");
  statusBar()->showMessage(tr("Saved %1").arg(out), 5000);
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

void MainWindow::rebuildRecentMenu() {
  if (!m_recentMenu) return;
  m_recentMenu->clear();
  for (const QString& p : recent()) {
    QAction* a = m_recentMenu->addAction(icons::themed("recent", 16), p);
    connect(a, &QAction::triggered, this, [this, p] { openPath(p); });
  }
  if (m_recentMenu->isEmpty()) m_recentMenu->addAction(tr("No recent files"))->setEnabled(false);
}

// ---------------------------------------------------------------- lifecycle
void MainWindow::openPath(const QString& path) {
  // A load already running is dropped (the next file wins, as when stepping through a folder); the document on screen
  // has not changed since it was asked about.
  if (!m_doc->loading && !maybeSave()) return;
  if (m_doc->loading && m_loadJob) m_loadJob->cancel();
  if (m_doc->loading) m_doc->cancelLoad();
  m_settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
  beginLoad([this, path] { m_viewPath=QFileInfo(path).absoluteFilePath(); addRecent(path); m_viewport->fitWhenReady(); updateViewerCard(); });
  // A drawing is picked by its edges (see loadFinished): set before its bodies are displayed, so each is activated once.
  if (const QString suffix = QFileInfo(path).suffix().toLower(); suffix == "dxf" || suffix == "dwg" || suffix == "svg")
    m_viewport->setSelectionFilter(Viewport::SelFilter::Edge);
  m_doc->startOpen(path);
}

// ---------------------------------------------------------------- load progress
// One job spans the document worker (reading/translating/building) and the viewport's tessellation.
void MainWindow::beginLoad(std::function<void()> after) {
  if (m_loadJob) m_loadJob->cancel();
  m_afterLoad = std::move(after);
  m_loadDocDone = false;
  m_meshTotal = m_meshRemaining = 0;
  m_viewport->resetMeshing();
  m_loadJob = m_jobs->begin(tr("Loading…"), true);
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
    if (!ok) {
      if (err.contains("cancel", Qt::CaseInsensitive)) statusBar()->showMessage(tr("Load cancelled"), 4000);
      else QMessageBox::warning(this, tr("OPAD"), err);
    }
    if(ok) {
      m_doc->storeViewerCache(m_jobs);  // a slow viewer read, now meshed: the next open of the file skips it
      if(!m_benchSelect) QTimer::singleShot(0, this, [this] { offerKicadModels(); offerAssetTrust(); });  // library models, linked files (benches call them)
      if(!m_doc->path().isEmpty()) m_viewPath=QFileInfo(m_doc->path()).absoluteFilePath();
      if((!m_benchSelect || qEnvironmentVariableIsSet("OPAD_BENCH_NAVIGATION")) && !m_viewPath.isEmpty() && m_settings.value("view/lastPath").toString()==m_viewPath) {
        try {
          const auto camera=opad::json::parse(m_settings.value("view/lastCamera").toString().toStdString());
          action("view.ortho")->setChecked(camera.value("projection","")=="orthographic");
          m_viewport->setCameraJson(camera);
        } catch(const std::exception&) { /* Ignore stale settings from another version. */ }
      }
    }
    if (int skipped = m_viewport->skippedCount()) statusBar()->showMessage(tr("%1 bodies were not tessellated (cancelled); reopen the file to show them").arg(skipped), 8000);
    if (m_benchSelect && !ok && !m_doc->hasDocument) {  // nothing to run the benches on: say so instead of walking an empty scene
      trace::log("bench: load failed: " + err);
      QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(3); });
    } else if (m_benchSelect) QTimer::singleShot(300, this, &MainWindow::runBench);
  });
}

QString MainWindow::meshPhase() const {
  return tr("Tessellating and displaying bodies (%1 of %2)").arg(m_meshTotal - m_meshRemaining).arg(m_meshTotal);
}

void MainWindow::setLoadPhase(const QString& phase, int pct) {
  if (!m_loadJob) return;
  if (trace::enabled()) trace::log(QStringLiteral("load phase: %1 (%2%)").arg(phase).arg(pct));
  m_loadJob->setPhase(phase, pct);
  m_loadJob->setOverall(overallPercent(phase, pct));
}

// Maps a phase name + within-phase percent to an overall 0-100 across reading -> building -> tessellating.
int MainWindow::overallPercent(const QString& phase, int pct) const {
  int base = 65, span = 35;  // tessellating + displaying (last phase) by default
  if (phase.contains("Reading") || phase.contains("Opening")) { base = 0; span = 10; }
  else if (phase.contains("Translating")) { base = 10; span = 30; }
  else if (phase.contains("Building")) { base = 40; span = 15; }
  else if (phase.contains("Preparing")) { base = 55; span = 10; }
  const int within = pct < 0 ? 0 : pct;
  return base + within * span / 100;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
  for (const QUrl& u : e->mimeData()->urls()) {
    QString p = u.toLocalFile();
    QString ext = QFileInfo(p).suffix().toLower();
    if (ext == "opad" || fileFilter(false).split(' ').contains("*." + ext)) { openPath(p); return; }
  }
}

void MainWindow::saveLastView() {
  if((m_benchSelect && !qEnvironmentVariableIsSet("OPAD_BENCH_NAVIGATION")) || m_viewPath.isEmpty() || (m_design && m_design->sketchActive())) return;
  const auto camera=m_viewport->cameraJson(); if(camera.empty()) return;
  m_settings.setValue("view/lastPath",m_viewPath);
  m_settings.setValue("view/lastCamera",QString::fromStdString(camera.dump()));
}
