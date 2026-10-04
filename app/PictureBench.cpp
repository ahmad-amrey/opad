// OPAD_BENCH_PICTURES=<prefix> (UI-71), on an empty document: a JPEG photo imported as a canvas keeps the file's bytes and
// is decoded on a worker before it is shown (the frame, <prefix>.canvas.png, shows it); inserted into a sketch the JPEG is
// kept as the file has it (it was re-encoded as PNG, 4.5 times larger) and, the sketch finished, it is an image canvas on
// the sketch's plane where it lay (UI-70), which the sketch's Trace image then takes; moving and fading a backdrop an older
// file keeps in its sketch appends an edit of the fields that changed, not the picture again.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

#include "AppDocument.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
#include "SketchEditor.hpp"
#include "Viewport.hpp"
#include "opad/canvas.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"

OPAD_BENCH(OPAD_BENCH_PICTURES, pictures) {
  const QString prefix = value;
  static int phase = 0;
  const QString photo = prefix + ".photo.jpg";
  auto fail = [](const QString& why) {
    trace::log("bench: pictures FAIL: " + why);
    QCoreApplication::exit(2);
    return true;
  };
  // Polls `ready` every 50 ms (15 s at most), then runs `next`.
  auto when = [&w, fail](std::function<bool()> ready, const QString& what, std::function<void()> next) {
    auto* timer = new QTimer(&w);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QObject::connect(timer, &QTimer::timeout, &w, [timer, clock, ready, what, next, fail] {
      if (!ready() && clock->elapsed() < 15000) return;
      timer->stop();
      timer->deleteLater();
      if (!ready()) return (void)fail("timed out waiting for " + what);
      next();
    });
    timer->start(50);
  };
  if (phase == 0) {
    QImage pixels(320, 200, QImage::Format_RGB32);
    pixels.fill(QColor(255, 0, 255));
    QPainter(&pixels).fillRect(0, 0, 80, 200, QColor(20, 20, 20));
    if (!pixels.save(photo, "JPG", 90)) return fail("the photo could not be written");
    phase = 1;
    w.beginLoad({});
    w.m_doc->startImport(photo, {}, {}, {}, false);  // runBench again
    return true;
  }
  if (phase > 1) return true;  // the canvases this bench makes are no loads of its own
  phase = 2;
  QFile file(photo);
  if (!file.open(QIODevice::ReadOnly)) return fail("the photo");
  const std::string bytes = file.readAll().toBase64().toStdString();
  std::string canvas;
  for (const auto& id : w.m_doc->scene.all_bodies())
    if (!w.m_doc->node(id)->raster.is_null()) canvas = id;
  if (canvas.empty()) return fail("no canvas");
  if (w.m_doc->node(canvas)->raster.value("href", "") != "data:image/jpeg;base64," + bytes) return fail("the canvas does not keep the JPEG's bytes");
  // The last part: a backdrop as an older build stored it in its sketch, moved and faded through the sketch's image tools.
  auto legacy = [=, &w] {
    opad::design::Sketch old;
    old.images.push_back({{"id", old.next_id()}, {"name", photo.toStdString()}, {"data", bytes}, {"position", {0, 0}}, {"width", 64.0}, {"height", 40.0}, {"angle", 0.0},
                          {"opacity", 0.5}});
    w.m_design->applyOps({opad::design::make_sketch_op("Older backdrop", {{"base", "xy"}, {"frame", opad::Frame().to_json()}}, old.to_json())}, MainWindow::tr("sketch"),
                         [=, &w](bool ok, const QString& error) {
      if (!ok || w.m_doc->scene.sketches.empty()) return (void)fail("sketch: " + error);
      const std::string sketch = w.m_doc->scene.sketches.back().id;
      SketchEditor* editor = w.m_design->sketch();
      const size_t before = w.m_doc->doc.serialize().size();
      w.m_design->editOp(sketch);
      if (!editor->active()) return (void)fail("the older sketch did not open");
      editor->setTool("image_edit");
      editor->setOption("imageId", QString::number(old.images[0]["id"].get<int>()));
      editor->setOption("imageX", "12 mm");
      editor->setOption("imageY", "5 mm");
      editor->setOption("imageWidth", "64 mm");
      editor->setOption("imageAngle", "15 deg");
      editor->setOption("imageOpacity", "0.6");
      editor->applyTool();
      when([editor] { return !editor->busy(); }, "the move", [=, &w] {
        const opad::json moved = editor->geometryDelta();
        trace::log(QString("bench: pictures: move delta %1").arg(QString::fromStdString(moved.dump())));
        if (moved.contains("images") || !moved.contains("image_fields") || moved["image_fields"][0].contains("data"))
          return (void)fail("moving the backdrop stores the picture again");
        w.m_design->finishSketch([=, &w] {
          const opad::Op& edit = w.m_doc->doc.ops.back().type == "edit" ? w.m_doc->doc.ops.back() : w.m_doc->doc.ops[w.m_doc->doc.ops.size() - 2];
          const size_t grown = w.m_doc->doc.serialize().size() - before;
          trace::log(QString("bench: pictures: the move added %1 bytes (the picture is %2)").arg(grown).arg(bytes.size()));
          const opad::json shown = w.m_doc->scene.sketch(sketch)->geometry["images"];
          if (edit.type != "edit" || !edit.data["set"]["geometry_delta"].contains("image_fields") || grown > 1024)
            return (void)fail("the edit op carries the picture");
          if (shown.size() != 1 || shown[0].value("data", "") != bytes || std::abs(shown[0]["position"][0].get<double>() - 12) > 1e-9 ||
              std::abs(shown[0].value("opacity", 0.0) - 0.6) > 1e-9)
            return (void)fail("the moved backdrop as replayed");
          // What the viewport compares on every scene sync: the picture's samples, not its bytes.
          const std::string stamp = opad::design::geometry_stamp(w.m_doc->scene.sketch(sketch)->geometry);
          trace::log(QString("bench: pictures: sketch stamp %1 bytes, picture %2").arg(stamp.size()).arg(bytes.size()));
          if (stamp.find(bytes.substr(bytes.size() / 2, 64)) != std::string::npos || stamp.size() * 2 > bytes.size())
            return (void)fail("the sketch's stamp carries the picture");
          trace::log("bench: pictures sketch backdrop keeps the JPEG, a move stores only its fields PASS");
          QCoreApplication::exit(0);
        });
      });
    });
  };
  when([&w, canvas] { return w.m_viewport->showsPicture(canvas); }, "the canvas's picture", [=, &w] {
    if (w.m_viewport->rastersDecoded() < 1) return (void)fail("the picture was not decoded on a worker");
    w.m_viewport->standardView("top");
    w.m_viewport->fitAll();
    QTimer::singleShot(600, &w, [=, &w] {
      const QImage frame = w.m_viewport->grabImage().convertToFormat(QImage::Format_RGB32);
      int magenta = 0;
      for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x)
          if (const QRgb c = frame.pixel(x, y); qRed(c) > 200 && qGreen(c) < 70 && qBlue(c) > 200) ++magenta;
      frame.save(prefix + ".canvas.png");
      trace::log(QString("bench: pictures: canvas shown, %1 pictures decoded on workers, %2 magenta pixels").arg(w.m_viewport->rastersDecoded()).arg(magenta));
      if (magenta < frame.width() * frame.height() / 50) return (void)fail("the canvas does not show its picture");
      trace::log("bench: pictures canvas keeps the JPEG, decoded on a worker, shown PASS");
      opad::Frame xy;
      w.m_design->applyOps({opad::design::make_sketch_op("Backdrop", {{"base", "xy"}, {"frame", xy.to_json()}}, opad::design::Sketch().to_json())}, MainWindow::tr("sketch"), [=, &w](bool ok, const QString& error) {
        if (!ok || w.m_doc->scene.sketches.empty()) return (void)fail("sketch: " + error);
        const std::string sketch = w.m_doc->scene.sketches.back().id;
        w.m_design->editOp(sketch);
        SketchEditor* editor = w.m_design->sketch();
        if (!editor->active()) return (void)fail("the sketch did not open");
        editor->setTool("image_insert");
        editor->setOption("imageFile", photo);
        editor->setOption("imageWidth", "64 mm");
        editor->placePrecise("0", "0", 0);
        editor->applyTool();
        when([editor] { return !editor->busy(); }, "the backdrop", [=, &w] {
          const opad::json images = editor->geometry().value("images", opad::json::array());
          if (images.size() != 1 || images[0].value("data", "") != bytes) return (void)fail("the backdrop is not the JPEG as the file has it");
          if (std::abs(images[0].value("height", 0.0) - 40) > 1e-9) return (void)fail("the backdrop's height");
          std::set<std::string> had;
          for (const auto& id : w.m_doc->scene.all_bodies()) had.insert(id);
          w.m_design->finishSketch([=, &w] {
            // Finished: the picture is an image canvas on the sketch's plane where it lay; the sketch keeps no record of it.
            std::string placed;
            for (const auto& id : w.m_doc->scene.all_bodies())
              if (!had.count(id) && opad::is_canvas(*w.m_doc->node(id))) placed = id;
            if (placed.empty() || !w.m_doc->scene.sketch(sketch)->geometry.value("images", opad::json::array()).empty())
              return (void)fail("the picture inserted into the sketch did not become a canvas");
            const opad::CanvasPlace p = opad::canvas_place(w.m_doc->scene, placed);
            const opad::Node& n = *w.m_doc->node(placed);
            trace::log(QString("bench: pictures: the sketch's picture is canvas %1: %2 x %3 mm at %4, %5").arg(QString::fromStdString(n.name)).arg(p.width).arg(p.height).arg(p.x).arg(p.y));
            if (n.raster.value("href", "") != "data:image/jpeg;base64," + bytes || std::abs(p.width - 64) > 1e-9 || std::abs(p.height - 40) > 1e-9 ||
                std::abs(p.x - 32) > 1e-9 || std::abs(p.y - 20) > 1e-9 || !p.on_plane || std::abs(n.opacity - 0.5) > 1e-9)
              return (void)fail("the canvas is not the picture where the sketch had it");
            trace::log("bench: pictures a picture inserted into a sketch is an image canvas on its plane once the sketch is finished PASS");
            // The sketch's Trace image takes the canvas (on its plane): the photo's dark band as curves where it is shown.
            w.m_design->editOp(sketch);
            if (!editor->active()) return (void)fail("the sketch did not open again");
            bool listed = false;
            for (const auto& [id, name] : editor->planeCanvases()) listed = listed || id == placed;
            if (!listed) return (void)fail("Trace image does not offer the canvas on the sketch's plane");
            editor->setTool("image_trace");
            editor->setOption("imageId", QString::fromStdString("canvas:" + placed));
            editor->setOption("threshold", "60");
            editor->setOption("noise", "2");
            editor->applyTool();
            when([editor] { return !editor->busy(); }, "the trace", [=, &w] {
              const opad::design::Sketch traced = opad::design::Sketch::from_json(editor->geometry());
              double left = 1e300, right = -1e300, top = -1e300;
              for (const auto& pt : traced.points) left = std::min(left, pt.x), right = std::max(right, pt.x), top = std::max(top, pt.y);
              trace::log(QString("bench: pictures: traced %1 curves from the canvas, x %2 to %3 mm, up to y %4 mm").arg(traced.entities.size()).arg(left).arg(right).arg(top));
              if (traced.entities.size() < 4 || std::abs(left) > 1 || std::abs(right - 16) > 1.5 || std::abs(top - 40) > 1)
                return (void)fail("the canvas's dark band is not traced where it is shown");
              trace::log("bench: pictures the sketch's Trace image takes an image canvas on its plane PASS");
              w.m_design->finishSketch([legacy] { legacy(); });
            });
          });
        });
      });
    });
  });
  return true;
}
