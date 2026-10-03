// Linked files (opad/assets.hpp) in the window: the question after a load before reading files outside the document's project
// (AssetsArea.cpp asks it), and the bench that opens, trusts, saves, syncs and links them.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <filesystem>
#include <functional>

#include "AppDocument.hpp"
#include "AssetsArea.hpp"
#include "Jobs.hpp"
#include "Viewport.hpp"
#include "opad/assets.hpp"

// The question about the linked files outside the document's project after a load (assets::askTrust, AssetsArea.cpp).
bool MainWindow::offerAssetTrust() {
  return assets::askTrust(this, m_doc, m_jobs, [this](const QString& error) { statusBar()->showMessage(error, 6000); });
}

// OPAD_BENCH_ASSETS=<png>: tools/gui_benches.py writes a document beside parts/part.step (changed since it was linked) that
// also links ../outside/other.step, and parts/third.step. Opened: the part's bodies come from its file (state "changed", a toast
// offers to sync it), the outside file is not read (its bodies missing, its badge offers to read it) and the question about it
// is asked; read once trusted, its bodies are displayed; saved, the file holds no body of either; a sync planned on a worker
// commits as one edit of the import that keeps the node and changes its body, and undoes and redoes; third.step imported
// linked shows its bodies, none stored; a picture linked last is shown on its canvas (its colour in the frame) while the op
// keeps none of its bytes.
OPAD_BENCH(OPAD_BENCH_ASSETS, assets) {
  const QString shot = value;
  static int phase = 0;
  auto fail = [](const QString& why) {
    trace::log("bench: assets FAIL: " + why);
    QCoreApplication::exit(2);
    return true;
  };
  auto state = [&w](const std::string& name) {
    for (const auto& s : w.m_doc->assetStates)
      if (s.value("name", "") == name) return s.value("state", std::string());
    return std::string("none");
  };
  auto linked = [&w](const std::string& import, bool& missing) {
    int n = 0;
    missing = false;
    for (const auto& id : w.m_doc->scene.all_bodies())
      if (const opad::Node* node = w.m_doc->node(id); node->linked && node->source_op == import) ++n, missing = missing || node->body_missing;
    return n;
  };
  auto import_of = [&w](const std::string& source) {
    for (const auto& o : w.m_doc->doc.ops)
      if (o.type == "import" && o.data.value("source", "") == source) return o.id;
    return std::string();
  };
  // Waits until `count` bodies are displayed (15 s at most): meshing ran past a fixed 1.5 s when the machine was busy.
  auto displayed = [&w](int count, std::function<void(bool)> then) {
    auto* timer = new QTimer(&w);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QObject::connect(timer, &QTimer::timeout, &w, [&w, timer, clock, count, then] {
      const bool ok = w.m_viewport->displayedCount() == count;
      if (!ok && clock->elapsed() < 15000) return;
      timer->stop();
      timer->deleteLater();
      QTimer::singleShot(300, &w, [then, ok] { then(ok); });  // the display job's last slice settled
    });
    timer->start(100);
  };
  const std::string part = import_of("part.step"), other = import_of("other.step");
  trace::log(QString("bench: assets: phase %1: part.step %2, other.step %3, %4 bodies displayed")
                 .arg(phase).arg(QString::fromStdString(state("part.step")), QString::fromStdString(state("other.step"))).arg(w.m_viewport->displayedCount()));
  if (phase == 0) {
    ++phase;
    bool partMissing = true, otherMissing = false;
    if (part.empty() || other.empty() || linked(part, partMissing) != 1 || linked(other, otherMissing) != 1) return fail("two linked imports of one body each");
    if (state("part.step") != "changed" || state("other.step") != "untrusted") return fail("states");
    if (partMissing || !otherMissing) return fail("the part read, the outside file not");
    if (w.m_doc->isDirty() || w.m_viewport->displayedCount() != 1) return fail("opened dirty, or not one body displayed");
    {  // The asset UI (UI-68): the load's toast offers to sync the changed file; the outside file's badge offers to read it.
      AssetsArea* area = nullptr;
      for (AreaController* a : w.m_areas)
        if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
      const QList<Toast*> toasts = w.m_toasts->toasts();
      const bool toast = std::any_of(toasts.begin(), toasts.end(), [](Toast* t) {
        return t->text() == "part.step changed since the last sync" && t->actionButton() && t->actionButton()->text() == "Sync";
      });
      std::string body;
      for (const auto& id : w.m_doc->scene.all_bodies())
        if (w.m_doc->node(id)->source_op == other) body = id;
      browser::Decoration d;
      if (area) area->decorate({body, "body", {}, w.m_doc->node(body)}, d);
      if (!toast || d.badges.isEmpty() || d.badges[0].text != "not read" || !d.badges[0].clicked || !d.italic) return fail("the changed file's toast, the outside file's badge");
    }
    if (!w.offerAssetTrust()) return fail("no question about the outside file");  // dismissed: nothing read
    if (linked(other, otherMissing) != 1 || !otherMissing) return fail("read without the user's answer");
    w.m_doc->loadAssets(w.m_jobs, true, [=, &w](bool ok, const QString& error) {
      bool missing = true;
      linked(other, missing);
      if (!ok || missing || state("other.step") != "ok") return (void)fail("trusted file not read: " + error);
      displayed(2, [=, &w](bool shown) {
        if (!shown) return (void)fail(QString("the trusted file's body is not displayed (%1)").arg(w.m_viewport->displayedCount()));
        w.m_doc->save();
        const std::string text = opad::read_text_file(w.m_doc->doc.path);
        if (text.find("#body ") != std::string::npos || w.m_doc->isDirty()) return (void)fail("linked bodies were saved into the document");
        // Sync as the asset UI will: planned on a worker, committed here, one undo step.
        auto plan = std::make_shared<opad::design::Plan>();
        auto snapshot = std::make_shared<opad::Document>(w.m_doc->doc);
        w.m_jobs->async(MainWindow::tr("Syncing linked file"), [snapshot, plan, part](Progress) { *plan = opad::plan_asset_sync(*snapshot, part, AppDocument::assetOptions()); },
                        [=, &w](bool ok, const QString& error) {
                          if (!ok) return (void)fail("sync: " + error);
                          std::string body;
                          for (const auto& id : w.m_doc->scene.all_bodies())
                            if (w.m_doc->node(id)->source_op == part) body = id;
                          const std::string before = w.m_doc->node(body)->body_key;
                          const size_t ops = w.m_doc->doc.ops.size();
                          w.m_doc->commitPlan(std::move(*plan), MainWindow::tr("Sync linked file"));
                          const opad::Node* now = w.m_doc->node(body);
                          if (w.m_doc->doc.ops.size() != ops + 1 || w.m_doc->doc.ops.back().type != "edit" || !now || now->body_key == before || now->body_missing)
                            return (void)fail("sync is not one edit keeping the node with a new body");
                          w.m_doc->undo();
                          if (w.m_doc->node(body)->body_key != before) return (void)fail("undo");
                          w.m_doc->redo();
                          if (w.m_doc->node(body)->body_key == before) return (void)fail("redo");
                          trace::log("bench: assets opened (changed part read, outside file asked about and read once trusted, displayed), saved without "
                                     "their bodies, synced as one edit with undo PASS");
                          w.beginLoad({});
                          w.m_doc->startImport(QFileInfo(w.m_doc->path()).absolutePath() + "/parts/third.step", {}, {}, {}, true);  // runBench again
                        });
      });
    });
    return true;
  }
  if (phase == 2) {
    const std::string picture = import_of("picture.png");
    bool gone = true;
    if (picture.empty() || linked(picture, gone) != 1 || gone || state("picture.png") != "ok") return fail("the linked picture");
    const opad::Op* op = w.m_doc->doc.find_op(picture);
    std::string canvas;
    for (const auto& id : w.m_doc->scene.all_bodies())
      if (w.m_doc->node(id)->source_op == picture) canvas = id;
    if (op->data["asset"].value("kind", "") != "image" || op->data["nodes"][0]["raster"].contains("href") || !w.m_doc->node(canvas)->raster.contains("href"))
      return fail("the picture's bytes belong with its file, shown, never in the op");
    QTimer::singleShot(1500, &w, [=, &w] {
      w.m_viewport->standardView("top");
      w.m_viewport->fitAll();
      QTimer::singleShot(800, &w, [=, &w] {
        const QImage frame = w.m_viewport->grabImage().convertToFormat(QImage::Format_RGB32);
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
  for (const auto& id : w.m_doc->scene.all_bodies())
    if (w.m_doc->node(id)->source_op == third && !w.m_doc->doc.body(w.m_doc->node(id)->body_key)->external) return fail("its body is stored");
  displayed(3, [=, &w](bool shown) {
    if (!shown) return (void)fail(QString("the linked import is not displayed (%1)").arg(w.m_viewport->displayedCount()));
    if (w.m_doc->doc.serialize().find("#body ") != std::string::npos) return (void)fail("the linked import is stored");
    w.m_viewport->standardView("iso");
    w.m_viewport->fitAll();
    QTimer::singleShot(800, &w, [=, &w] {
      if (!w.m_viewport->grabImage().save(shot)) return (void)fail("frame");
      trace::log("bench: assets import linked, displayed, not stored PASS");
      // A picture, linked: one colour, so the frame shows whether it is on its canvas.
      const QString picture = QFileInfo(w.m_doc->path()).absolutePath() + "/parts/picture.png";
      QImage pixels(64, 32, QImage::Format_RGB32);
      pixels.fill(QColor(255, 0, 255));
      if (!pixels.save(picture)) return (void)fail("picture");
      phase = 2;
      w.beginLoad({});
      w.m_doc->startImport(picture, {}, {}, {}, true);  // runBench again
    });
  });
  return true;
}
