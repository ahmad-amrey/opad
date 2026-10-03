// OPAD_BENCH_PICTURES=<prefix> (UI-71), on an empty document: a JPEG photo imported as a canvas keeps the file's bytes and
// is decoded on a worker before it is shown (the frame, <prefix>.canvas.png, shows it); inserted as a sketch backdrop the
// JPEG is stored as the file has it (it was re-encoded as PNG, 4.5 times larger); moving and fading the backdrop afterwards
// appends an edit of the fields that changed, not the picture again.
#include "MainWindow.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <cmath>
#include <functional>
#include <memory>

#include "AppDocument.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
#include "SketchEditor.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"

bool MainWindow::benchPictures() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_PICTURES");
  if (prefix.isEmpty()) return false;
  static int phase = 0;
  const QString photo = prefix + ".photo.jpg";
  auto fail = [](const QString& why) {
    trace::log("bench: pictures FAIL: " + why);
    QCoreApplication::exit(2);
    return true;
  };
  // Polls `ready` every 50 ms (15 s at most), then runs `next`.
  auto when = [this, fail](std::function<bool()> ready, const QString& what, std::function<void()> next) {
    auto* timer = new QTimer(this);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    connect(timer, &QTimer::timeout, this, [timer, clock, ready, what, next, fail] {
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
    beginLoad({});
    m_doc->startImport(photo, {}, {}, {}, false);  // runBench again
    return true;
  }
  QFile file(photo);
  if (!file.open(QIODevice::ReadOnly)) return fail("the photo");
  const std::string bytes = file.readAll().toBase64().toStdString();
  std::string canvas;
  for (const auto& id : m_doc->scene.all_bodies())
    if (!m_doc->node(id)->raster.is_null()) canvas = id;
  if (canvas.empty()) return fail("no canvas");
  if (m_doc->node(canvas)->raster.value("href", "") != "data:image/jpeg;base64," + bytes) return fail("the canvas does not keep the JPEG's bytes");
  when([this, canvas] { return m_viewport->showsPicture(canvas); }, "the canvas's picture", [=, this] {
    if (m_viewport->rastersDecoded() < 1) return (void)fail("the picture was not decoded on a worker");
    m_viewport->standardView("top");
    m_viewport->fitAll();
    QTimer::singleShot(600, this, [=, this] {
      const QImage frame = m_viewport->grabImage().convertToFormat(QImage::Format_RGB32);
      int magenta = 0;
      for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x)
          if (const QRgb c = frame.pixel(x, y); qRed(c) > 200 && qGreen(c) < 70 && qBlue(c) > 200) ++magenta;
      frame.save(prefix + ".canvas.png");
      trace::log(QString("bench: pictures: canvas shown, %1 pictures decoded on workers, %2 magenta pixels").arg(m_viewport->rastersDecoded()).arg(magenta));
      if (magenta < frame.width() * frame.height() / 50) return (void)fail("the canvas does not show its picture");
      trace::log("bench: pictures canvas keeps the JPEG, decoded on a worker, shown PASS");
      opad::Frame xy;
      m_design->applyOps({opad::design::make_sketch_op("Backdrop", {{"base", "xy"}, {"frame", xy.to_json()}}, opad::design::Sketch().to_json())}, tr("sketch"), [=, this](bool ok, const QString& error) {
        if (!ok || m_doc->scene.sketches.empty()) return (void)fail("sketch: " + error);
        const std::string sketch = m_doc->scene.sketches.back().id;
        m_design->editOp(sketch);
        SketchEditor* editor = m_design->sketch();
        if (!editor->active()) return (void)fail("the sketch did not open");
        editor->setTool("image_insert");
        editor->setOption("imageFile", photo);
        editor->setOption("imageWidth", "64 mm");
        editor->placePrecise("0", "0", 0);
        editor->applyTool();
        when([editor] { return !editor->busy(); }, "the backdrop", [=, this] {
          const opad::json images = editor->geometry().value("images", opad::json::array());
          if (images.size() != 1 || images[0].value("data", "") != bytes) return (void)fail("the backdrop is not the JPEG as the file has it");
          if (std::abs(images[0].value("height", 0.0) - 40) > 1e-9) return (void)fail("the backdrop's height");
          m_design->finishSketch([=, this] {
            const size_t before = m_doc->doc.serialize().size();
            m_design->editOp(sketch);
            if (!editor->active()) return (void)fail("the sketch did not open again");
            editor->setTool("image_edit");
            editor->setOption("imageX", "12 mm");
            editor->setOption("imageY", "5 mm");
            editor->setOption("imageWidth", "64 mm");
            editor->setOption("imageAngle", "15 deg");
            editor->setOption("imageOpacity", "0.6");
            editor->applyTool();
            when([editor] { return !editor->busy(); }, "the move", [=, this] {
              const opad::json moved = editor->geometryDelta();
              trace::log(QString("bench: pictures: move delta %1").arg(QString::fromStdString(moved.dump())));
              if (moved.contains("images") || !moved.contains("image_fields") || moved["image_fields"][0].contains("data"))
                return (void)fail("moving the backdrop stores the picture again");
              m_design->finishSketch([=, this] {
                const opad::Op& edit = m_doc->doc.ops.back().type == "edit" ? m_doc->doc.ops.back() : m_doc->doc.ops[m_doc->doc.ops.size() - 2];
                const size_t grown = m_doc->doc.serialize().size() - before;
                trace::log(QString("bench: pictures: the move added %1 bytes (the picture is %2)").arg(grown).arg(bytes.size()));
                const opad::json shown = m_doc->scene.sketch(sketch)->geometry["images"];
                if (edit.type != "edit" || !edit.data["set"]["geometry_delta"].contains("image_fields") || grown > 1024)
                  return (void)fail("the edit op carries the picture");
                if (shown.size() != 1 || shown[0].value("data", "") != bytes || std::abs(shown[0]["position"][0].get<double>() - 12) > 1e-9 ||
                    std::abs(shown[0].value("opacity", 0.0) - 0.6) > 1e-9)
                  return (void)fail("the moved backdrop as replayed");
                // What the viewport compares on every scene sync: the picture's samples, not its bytes.
                const std::string stamp = opad::design::geometry_stamp(m_doc->scene.sketch(sketch)->geometry);
                trace::log(QString("bench: pictures: sketch stamp %1 bytes, picture %2").arg(stamp.size()).arg(bytes.size()));
                if (stamp.find(bytes.substr(bytes.size() / 2, 64)) != std::string::npos || stamp.size() * 2 > bytes.size())
                  return (void)fail("the sketch's stamp carries the picture");
                trace::log("bench: pictures sketch backdrop keeps the JPEG, a move stores only its fields PASS");
                QCoreApplication::exit(0);
              });
            });
          });
        });
      });
    });
  });
  return true;
}
