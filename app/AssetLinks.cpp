// Linked files (opad/assets.hpp) in the window: the question before reading files outside the document's project, and the
// bench that opens, trusts, saves, syncs and links them.
#include "MainWindow.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <filesystem>
#include <functional>

#include "AppDocument.hpp"
#include "Jobs.hpp"
#include "Viewport.hpp"
#include "opad/assets.hpp"

// A document from elsewhere must not make OPAD open files of its choosing (a network share hands over the user's
// credentials): the linked files outside its project are read only once the user says so, here or for good (settings).
bool MainWindow::offerAssetTrust() {
  QStringList files, folders;
  for (const auto& s : m_doc->assetStates) {
    if (s.value("state", "") != "untrusted") continue;
    const QString file = QString::fromStdString(s.value("file", s.value("path", std::string())));
    files << QDir::toNativeSeparators(file);
    if (const QString folder = QFileInfo(file).absolutePath(); !folders.contains(folder)) folders << folder;
  }
  if (files.isEmpty()) return false;
  QMessageBox box(QMessageBox::Question, tr("Linked files"),
                  tr("This document links files outside its project folder:\n\n%1\n\nRead them?").arg(files.mid(0, 6).join('\n') + (files.size() > 6 ? "\n…" : "")),
                  QMessageBox::NoButton, this);
  auto* once = box.addButton(tr("Read them"), QMessageBox::AcceptRole);
  auto* always = box.addButton(folders.size() == 1 ? tr("Always trust this folder") : tr("Always trust these folders"), QMessageBox::AcceptRole);
  box.addButton(tr("Not now"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == always) {
    QStringList trusted = m_settings.value("assets/trusted").toStringList();
    for (const QString& f : folders)
      if (!trusted.contains(f)) trusted << f;
    m_settings.setValue("assets/trusted", trusted);
  } else if (box.clickedButton() != once) {
    return true;
  }
  m_doc->loadAssets(m_jobs, box.clickedButton() == once, [this](bool ok, const QString& error) {
    if (!ok) statusBar()->showMessage(error, 6000);
  });
  return true;
}

// OPAD_BENCH_ASSETS=<png>: tools/gui_benches.py writes a document beside parts/part.step (changed since it was linked) that
// also links ../outside/other.step, and parts/third.step. Opened: the part's bodies come from its file (state "changed"),
// the outside file is not read (its bodies missing) and the question about it is asked; read once trusted, its bodies are
// displayed; saved, the file holds no body of either; a sync planned on a worker commits as one edit of the import that
// keeps the node and changes its body, and undoes and redoes; third.step imported linked shows its bodies, none stored; a
// picture linked last is shown on its canvas (its colour in the frame) while the op keeps none of its bytes.
bool MainWindow::benchAssets() {
  const QString shot = qEnvironmentVariable("OPAD_BENCH_ASSETS");
  if (shot.isEmpty()) return false;
  static int phase = 0;
  auto fail = [](const QString& why) {
    trace::log("bench: assets FAIL: " + why);
    QCoreApplication::exit(2);
    return true;
  };
  auto state = [this](const std::string& name) {
    for (const auto& s : m_doc->assetStates)
      if (s.value("name", "") == name) return s.value("state", std::string());
    return std::string("none");
  };
  auto linked = [this](const std::string& import, bool& missing) {
    int n = 0;
    missing = false;
    for (const auto& id : m_doc->scene.all_bodies())
      if (const opad::Node* node = m_doc->node(id); node->linked && node->source_op == import) ++n, missing = missing || node->body_missing;
    return n;
  };
  auto import_of = [this](const std::string& source) {
    for (const auto& o : m_doc->doc.ops)
      if (o.type == "import" && o.data.value("source", "") == source) return o.id;
    return std::string();
  };
  const std::string part = import_of("part.step"), other = import_of("other.step");
  trace::log(QString("bench: assets: phase %1: part.step %2, other.step %3, %4 bodies displayed")
                 .arg(phase).arg(QString::fromStdString(state("part.step")), QString::fromStdString(state("other.step"))).arg(m_viewport->displayedCount()));
  if (phase == 0) {
    ++phase;
    bool partMissing = true, otherMissing = false;
    if (part.empty() || other.empty() || linked(part, partMissing) != 1 || linked(other, otherMissing) != 1) return fail("two linked imports of one body each");
    if (state("part.step") != "changed" || state("other.step") != "untrusted") return fail("states");
    if (partMissing || !otherMissing) return fail("the part read, the outside file not");
    if (m_doc->isDirty() || m_viewport->displayedCount() != 1) return fail("opened dirty, or not one body displayed");
    if (!offerAssetTrust()) return fail("no question about the outside file");  // dismissed: nothing read
    if (linked(other, otherMissing) != 1 || !otherMissing) return fail("read without the user's answer");
    m_doc->loadAssets(m_jobs, true, [=, this](bool ok, const QString& error) {
      bool missing = true;
      linked(other, missing);
      if (!ok || missing || state("other.step") != "ok") return (void)fail("trusted file not read: " + error);
      QTimer::singleShot(1500, this, [=, this] {
        if (m_viewport->displayedCount() != 2) return (void)fail(QString("the trusted file's body is not displayed (%1)").arg(m_viewport->displayedCount()));
        m_doc->save();
        const std::string text = opad::read_text_file(m_doc->doc.path);
        if (text.find("#body ") != std::string::npos || m_doc->isDirty()) return (void)fail("linked bodies were saved into the document");
        // Sync as the asset UI will: planned on a worker, committed here, one undo step.
        auto plan = std::make_shared<opad::design::Plan>();
        auto snapshot = std::make_shared<opad::Document>(m_doc->doc);
        m_jobs->async(tr("Syncing linked file"), [snapshot, plan, part](Progress) { *plan = opad::plan_asset_sync(*snapshot, part, AppDocument::assetOptions()); },
                      [=, this](bool ok, const QString& error) {
                        if (!ok) return (void)fail("sync: " + error);
                        std::string body;
                        for (const auto& id : m_doc->scene.all_bodies())
                          if (m_doc->node(id)->source_op == part) body = id;
                        const std::string before = m_doc->node(body)->body_key;
                        const size_t ops = m_doc->doc.ops.size();
                        m_doc->commitPlan(std::move(*plan), tr("Sync linked file"));
                        const opad::Node* now = m_doc->node(body);
                        if (m_doc->doc.ops.size() != ops + 1 || m_doc->doc.ops.back().type != "edit" || !now || now->body_key == before || now->body_missing)
                          return (void)fail("sync is not one edit keeping the node with a new body");
                        m_doc->undo();
                        if (m_doc->node(body)->body_key != before) return (void)fail("undo");
                        m_doc->redo();
                        if (m_doc->node(body)->body_key == before) return (void)fail("redo");
                        trace::log("bench: assets opened (changed part read, outside file asked about and read once trusted, displayed), saved without "
                                   "their bodies, synced as one edit with undo PASS");
                        beginLoad({});
                        m_doc->startImport(QFileInfo(m_doc->path()).absolutePath() + "/parts/third.step", {}, {}, {}, true);  // runBench again
                      });
      });
    });
    return true;
  }
  if (phase == 2) {
    const std::string picture = import_of("picture.png");
    bool gone = true;
    if (picture.empty() || linked(picture, gone) != 1 || gone || state("picture.png") != "ok") return fail("the linked picture");
    const opad::Op* op = m_doc->doc.find_op(picture);
    std::string canvas;
    for (const auto& id : m_doc->scene.all_bodies())
      if (m_doc->node(id)->source_op == picture) canvas = id;
    if (op->data["asset"].value("kind", "") != "image" || op->data["nodes"][0]["raster"].contains("href") || !m_doc->node(canvas)->raster.contains("href"))
      return fail("the picture's bytes belong with its file, shown, never in the op");
    QTimer::singleShot(1500, this, [=, this] {
      m_viewport->standardView("top");
      m_viewport->fitAll();
      QTimer::singleShot(800, this, [=, this] {
        const QImage frame = m_viewport->grabImage().convertToFormat(QImage::Format_RGB32);
        int magenta = 0;
        for (int y = 0; y < frame.height(); ++y)
          for (int x = 0; x < frame.width(); ++x)
            if (const QRgb c = frame.pixel(x, y); qRed(c) > 200 && qGreen(c) < 60 && qBlue(c) > 200) ++magenta;
        trace::log(QString("bench: assets: picture pixels %1 of %2").arg(magenta).arg(frame.width() * frame.height()));
        if (magenta < frame.width() * frame.height() / 50) return (void)fail("the picture is not shown on its canvas");
        frame.save(QString(shot).replace(".png", ".picture.png"));
        trace::log("bench: assets picture linked, shown on its canvas, its bytes not in the document PASS");
        QCoreApplication::exit(0);
      });
    });
    return true;
  }
  const std::string third = import_of("third.step");
  bool missing = true;
  if (third.empty() || linked(third, missing) != 1 || missing || state("third.step") != "ok") return fail("the linked import");
  for (const auto& id : m_doc->scene.all_bodies())
    if (m_doc->node(id)->source_op == third && !m_doc->doc.body(m_doc->node(id)->body_key)->external) return fail("its body is stored");
  QTimer::singleShot(1500, this, [=, this] {
    if (m_viewport->displayedCount() != 3) return (void)fail(QString("the linked import is not displayed (%1)").arg(m_viewport->displayedCount()));
    if (m_doc->doc.serialize().find("#body ") != std::string::npos) return (void)fail("the linked import is stored");
    m_viewport->standardView("iso");
    m_viewport->fitAll();
    QTimer::singleShot(800, this, [=, this] {
      if (!m_viewport->grabImage().save(shot)) return (void)fail("frame");
      trace::log("bench: assets import linked, displayed, not stored PASS");
      // A picture, linked: one colour, so the frame shows whether it is on its canvas.
      const QString picture = QFileInfo(m_doc->path()).absolutePath() + "/parts/picture.png";
      QImage pixels(64, 32, QImage::Format_RGB32);
      pixels.fill(QColor(255, 0, 255));
      if (!pixels.save(picture)) return (void)fail("picture");
      phase = 2;
      beginLoad({});
      m_doc->startImport(picture, {}, {}, {}, true);  // runBench again
    });
  });
  return true;
}
