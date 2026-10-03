// The Plot bench (drawing2d, UI-88). Case in tools/bench_cases/drawing2d.py; what is plotted and where is tests/test_drawing2d.
#include <QAction>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>

#ifdef OPAD_HAVE_PRINT
#include <QPrinter>
#endif

#include <cmath>

#include "BenchRegistry.hpp"
#include "Drawing2DBench.hpp"
#include "GuidedTool.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "PlotDialog.hpp"

using namespace bench2d;

namespace {
int coloured(const QImage& image, int hue) {  // pixels clearly of one hue: 0 red, 1 green, 2 blue (a hue at least 80 above the others)
  int n = 0;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      const int v[3] = {c.red(), c.green(), c.blue()};
      if (v[hue] - std::max(v[(hue + 1) % 3], v[(hue + 2) % 3]) > 80) ++n;
    }
  return n;
}
int grey(const QImage& image, bool dark) {  // pixels without a hue; dark: darker than mid grey
  int n = 0;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      const bool hueless = std::max({c.red(), c.green(), c.blue()}) - std::min({c.red(), c.green(), c.blue()}) < 24;
      if (hueless && (!dark || c.lightness() < 128)) ++n;
    }
  return n;
}
// Ink in the bottom margin of a page image (the plot stamp's band).
int stampInk(const QImage& page, const plot::Settings& s) {
  const double k = page.width() / s.paperWidth;
  int n = 0;
  for (int y = int((s.paperHeight - s.margin) * k) + 1; y < page.height() - 1; ++y)
    for (int x = 0; x < page.width(); ++x) n += page.pixelColor(x, y).lightness() < 160;
  return n;
}
// How many pixels down a column are ink near paper (x, y) mm, at `k` pixels per mm.
int thickness(const QImage& image, double k, double x, double y) {
  int n = 0;
  for (int py = int(y * k) - 40; py <= int(y * k) + 40; ++py)
    if (py >= 0 && py < image.height() && image.pixelColor(int(x * k), py).lightness() < 160) ++n;
  return n;
}
}  // namespace

// OPAD_BENCH_PLOT=<prefix> on plot.dxf: a 200 x 100 frame (colour 7), a red 0.70 mm line, a blue fill and a green line on a
// layer left out of plots. The Plot command (File menu, Export ribbon) opens the dialog: the extents fit A4 landscape, the
// preview shows red and blue and no green (three bodies plotted); monochrome leaves no colour; the red line comes out
// thicker than the frame with lineweights and as thin without; the plot stamp is text along the bottom margin (file,
// paper, scale) and nothing when off; Enter goes to Plot…, never to Pick…; 1:2 is half size, 2:1 does not fit and says so;
// the display area is what the view shows; a window picked in the view (its prompt, the dialog away meanwhile, object
// snap's marker shown and the dashed window from the snapped point) is plotted; a printer (here printing to a PDF file)
// and a PDF come out as one vector page; the file opened again plots its extents. <prefix>.dialog.png, .preview.png,
// .stamp.png (monochrome with the stamp), .pdf, .printer.pdf.
// OPAD_BENCH_PLOT_IMAGE=<prefix> on picture.svg (gui_benches' plot-image): a 200 x 100 frame with a red picture embedded
// at (20, 10), 60 x 40. The plot carries the image (one of two bodies): the preview shows it red where it lies on the
// page, grey in monochrome, and the PDF holds it as an image. <prefix>.preview.png, .pdf.
OPAD_BENCH(OPAD_BENCH_PLOT_IMAGE, plotImage) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: plot-image: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  AppDocument* doc = w.m_doc;
  auto settled = [&w, v, doc] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id);
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() >= expected && expected > 1 && !v->looksPending();
  };
  pollUntil(&w, settled, 60000, [&w, require, all, value](bool shown) {
    QAction* plotAction = w.action("drawing2d.plot");
    require(shown && plotAction && plotAction->isEnabled(), "the drawing and its picture are shown and Plot is enabled");
    if (!shown || !plotAction) return QCoreApplication::exit(2);
    auto dialog = [&w] { return w.findChild<PlotDialog*>("plotDialog"); };
    auto ready = [dialog] { return dialog() && dialog()->isVisible() && dialog()->previewReady(); };
    auto script = std::make_shared<Script>();
    script->add("open", [plotAction] { plotAction->trigger(); }, ready);
    script->add("image", [dialog, require, value] {
      PlotDialog* d = dialog();
      const auto picture = d->picture();
      const QImage preview = d->previewImage();
      const plot::Placement p = d->placement();
      // The picture's middle, 50 from the frame's left and 30 down from its top (the extents), on the page.
      const double k = preview.width() / d->settings().paperWidth;
      const double cx = (p.x + 50 * p.scale) * k, cy = (p.y + 30 * p.scale) * k;
      const QColor middle = preview.pixelColor(int(cx), int(cy));
      require(picture->sheet.images.size() == 1 && picture->images.size() == 1 && picture->sheet.bodies == 2 && coloured(preview, 0) > 200 && middle.red() > 200 && middle.green() < 60,
              QString("the picture is plotted where it lies, red (%1 red pixels, its middle %2)").arg(coloured(preview, 0)).arg(middle.name()));
      preview.save(value + ".preview.png");
      static_cast<QCheckBox*>(d->findChild<QWidget*>("plotMonochrome"))->setChecked(true);
    }, ready);
    script->add("grey", [dialog, require, value] {
      const QImage preview = dialog()->previewImage();
      require(coloured(preview, 0) == 0 && grey(preview, true) > 200, "monochrome: the picture in grey");
      static_cast<QCheckBox*>(dialog()->findChild<QWidget*>("plotMonochrome"))->setChecked(false);
    }, ready);
    script->add("to PDF", [dialog, value] { dialog()->plotToPdf(value + ".pdf"); }, [dialog] { return !dialog()->isVisible(); });
    script->add("the PDF", [require, value] {
      QFile f(value + ".pdf");
      const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
      require(bytes.startsWith("%PDF") && bytes.contains("/Subtype /Image"), QString("the PDF holds the picture as an image (%1 bytes)").arg(bytes.size()));
    });
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}

OPAD_BENCH(OPAD_BENCH_PLOT, plot) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: plot: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  AppDocument* doc = w.m_doc;
  auto settled = [&w, v, doc] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id);
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() >= expected && expected > 0 && !v->looksPending();
  };
  pollUntil(&w, settled, 60000, [&w, v, doc, require, all, value, settled](bool shown) {
    QAction* plotAction = w.action("drawing2d.plot");
    require(shown && plotAction && plotAction->isEnabled(), "the drawing is shown and Plot is enabled");
    if (!shown || !plotAction) return QCoreApplication::exit(2);
    auto dialog = [&w] { return w.findChild<PlotDialog*>("plotDialog"); };
    auto ready = [dialog] { return dialog() && dialog()->isVisible() && dialog()->previewReady(); };
    auto child = [dialog](const char* name) { return dialog()->findChild<QWidget*>(name); };
    auto script = std::make_shared<Script>();
    script->add("open", [&w, plotAction, require] {
      require(w.m_commands.find("drawing2d.plot")->menuPath == "file" && w.m_commands.inWorkspace("review").contains("drawing2d.plot"), "Plot is in the File menu and on the Export ribbon");
      w.m_viewport->fitAll();
      plotAction->trigger();
    }, ready);
    script->add("extents", [dialog, require, value] {
      PlotDialog* d = dialog();
      const auto picture = d->picture();
      const plot::Placement p = d->placement();
      const plot::Settings s = d->settings();
      require(picture->sheet.bodies == 3 && s.region == plot::Region::Extents && s.fit && p.valid() && !p.clipped && std::abs(p.area.width() - 200) < 0.5 &&
                  std::abs(p.area.height() - 100) < 0.5 && s.paperWidth > s.paperHeight,
              QString("the frame, the red line and the fill are plotted (%1 bodies), their extents fit A4 landscape at %2")
                  .arg(picture->sheet.bodies).arg(QString::fromStdString(plot::scaleText(p.scale))));
      const QImage preview = d->previewImage();
      require(!preview.isNull() && coloured(preview, 0) > 20 && coloured(preview, 2) > 100 && coloured(preview, 1) == 0,
              QString("the preview shows the red line and the blue fill, not the unplotted green line (%1 red, %2 blue, %3 green)")
                  .arg(coloured(preview, 0)).arg(coloured(preview, 2)).arg(coloured(preview, 1)));
      preview.save(value + ".preview.png");
      d->grab().save(value + ".dialog.png");
      require(stampInk(preview, d->settings()) == 0, "no plot stamp by default: the bottom margin is blank");
      static_cast<QCheckBox*>(d->findChild<QWidget*>("plotMonochrome"))->setChecked(true);
      static_cast<QCheckBox*>(d->findChild<QWidget*>("plotStamp"))->setChecked(true);
    }, ready);
    script->add("monochrome", [dialog, require, value] {
      const QImage preview = dialog()->previewImage();
      preview.save(value + ".stamp.png");
      require(coloured(preview, 0) == 0 && coloured(preview, 2) == 0 && grey(preview, true) > 100, "monochrome: every colour black");
      PlotDialog* d = dialog();
      const QString stamp = QString::fromStdString(d->settings().stamp);
      require(stampInk(preview, d->settings()) > 20 && stamp.contains("plot.dxf") && stamp.contains("ISO A4") && stamp.contains(QString::fromStdString(plot::scaleText(d->placement().scale))),
              QString("the plot stamp is printed along the bottom margin (%1 dark pixels): %2").arg(stampInk(preview, d->settings())).arg(stamp));
      static_cast<QCheckBox*>(d->findChild<QWidget*>("plotStamp"))->setChecked(false);
      // The same paint at 10 pixels per mm: the 0.70 mm line against the frame's default 0.25 mm, then without lineweights.
      auto render = [d](bool lineweights) {
        plot::Settings s = d->settings();
        s.lineweights = lineweights;
        QImage image(int(s.paperWidth * 10), int(s.paperHeight * 10), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        paintPlot(painter, *d->picture(), s, plot::place(d->picture()->sheet, s), 10);
        return image;
      };
      const plot::Placement p = d->placement();
      const double x = p.x + (100 - p.area.x0) * p.scale, red = p.y + (p.area.y1 - 50) * p.scale, frame = p.y + (p.area.y1 - 0) * p.scale;
      const QImage on = render(true), off = render(false);
      const int redOn = thickness(on, 10, x, red), frameOn = thickness(on, 10, x, frame), redOff = thickness(off, 10, x, red), frameOff = thickness(off, 10, x, frame);
      require(redOn >= 2 * frameOn && frameOn > 0 && std::abs(redOff - frameOff) <= 1,
              QString("lineweights: the 0.70 mm line %1 px against the frame's %2 px; without them %3 and %4 px").arg(redOn).arg(frameOn).arg(redOff).arg(frameOff));
      static_cast<QCheckBox*>(d->findChild<QWidget*>("plotFit"))->setChecked(false);
      static_cast<QDoubleSpinBox*>(d->findChild<QWidget*>("plotScale"))->setValue(2);
    }, ready);
    script->add("1:2", [dialog, require] {
      PlotDialog* d = dialog();
      require(d->settings().stamp.empty() && stampInk(d->previewImage(), d->settings()) == 0, "the stamp off again: the margin is blank");
      // Enter in the dialog goes to the footer's Plot… (the default button), never to Pick…: with Plot… disabled (here by
      // hand, so that no file dialog opens; a field's Enter would enable it again) it does nothing.
      auto* footer = d->findChild<PanelFooter*>();
      auto* pick = static_cast<QPushButton*>(d->findChild<QWidget*>("plotPick"));
      require(footer && footer->primary()->isDefault() && !pick->autoDefault() && !pick->isDefault(), "Plot… is the dialog's default button, Pick… is not");
      footer->setPrimaryEnabled(false);
      QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QCoreApplication::sendEvent(d->findChild<QWidget*>("plotMonochrome"), &enter);
      require(!d->picking() && d->isVisible(), "Enter does not start a window pick");
      const plot::Placement p = d->placement();
      const auto* info = d->findChild<QLabel*>("plotInfo");
      require(std::abs(p.scale - 0.5) < 1e-12 && !p.clipped && info->text().contains("1:2") && !d->findChild<QLabel*>("plotWarning")->isVisible(), "1:2 draws the frame 100 mm wide: " + info->text());
      static_cast<QDoubleSpinBox*>(d->findChild<QWidget*>("plotScale"))->setValue(0.5);
    }, ready);
    script->add("2:1", [dialog, require] {
      PlotDialog* d = dialog();
      require(d->placement().clipped && d->findChild<QLabel*>("plotWarning")->isVisible(), "2:1 does not fit A4 and the dialog says so");
      static_cast<QCheckBox*>(d->findChild<QWidget*>("plotFit"))->setChecked(true);
      static_cast<QRadioButton*>(d->findChild<QWidget*>("plotDisplay"))->setChecked(true);
    }, ready);
    script->add("display", [v, dialog, require] {
      PlotDialog* d = dialog();
      const plot::Placement p = d->placement();
      const opad::Frame plane = d->picture()->sheet.plane;
      double u0, v0, u1, v1;
      v->planePoint(QPointF(0, v->height()), plane, u0, v0);
      v->planePoint(QPointF(v->width(), 0), plane, u1, v1);
      require(d->settings().region == plot::Region::Display && std::abs(p.area.x0 - u0) < 1e-6 && std::abs(p.area.x1 - u1) < 1e-6 && std::abs(p.area.y0 - v0) < 1e-6,
              QString("the display area is what the view shows: %1..%2").arg(p.area.x0).arg(p.area.x1));
      static_cast<QPushButton*>(d->findChild<QWidget*>("plotPick"))->click();
    }, [&w, v, dialog] { return dialog()->picking() && !dialog()->isVisible() && w.findChild<PromptBar*>("plotPrompt") && v->snapIndexesReady(); });
    script->add("window", [v, dialog, require] {
      require(v->snapPicks() == Viewport::SnapPicks::Always, "Pick… hides the dialog and asks for the window's corners, object snap on");
      const opad::Frame plane = dialog()->picture()->sheet.plane;
      auto at = [v, plane](double u, double w, double off = 3) { return QPointF(v->widgetPoint(plane.to_world(u, w))) + QPointF(off, -off); };
      auto click = [v](const QPointF& at) {
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
          QMouseEvent e(type, at, v->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
          QCoreApplication::sendEvent(v, &e);
        }
      };
      click(at(0, 0));  // beside the frame's corner: its end
      opad::Vec3 snapped;
      QString kind;
      require(v->benchSnap(at(200, 100)) && v->shownSnap(snapped, &kind) && kind == "endpoint", "near the frame's far corner the snap's marker shows during the pick");
      QMouseEvent move(QEvent::MouseMove, at(200, 100), v->mapToGlobal(at(200, 100)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QCoreApplication::sendEvent(v, &move);
      const QPointF corner = dialog()->rubberCorner();
      require(std::abs(corner.x() - 200) < 1e-6 && std::abs(corner.y() - 100) < 1e-6, QString("the dashed window follows the snapped point: (%1, %2)").arg(corner.x()).arg(corner.y()));
      click(at(110, 90, 0));  // nothing to snap to there
    }, [dialog, ready] { return !dialog()->picking() && ready(); });
    script->add("windowed", [&w, v, dialog, require, value] {
      PlotDialog* d = dialog();
      const plot::Placement p = d->placement();
      const double tolerance = 2 * v->pixelSize();
      require(d->settings().region == plot::Region::Window && !w.findChild<PromptBar*>("plotPrompt") && std::abs(p.area.x0) < 1e-6 && std::abs(p.area.y0) < 1e-6 &&
                  std::abs(p.area.x1 - 110) < tolerance && std::abs(p.area.y1 - 90) < tolerance && v->snapPicks() == Viewport::SnapPicks::None,
              QString("the window picked in the view is plotted, its first corner snapped to the frame's: (%1, %2) to (%3, %4)").arg(p.area.x0).arg(p.area.y0).arg(p.area.x1).arg(p.area.y1));
#ifdef OPAD_HAVE_PRINT
      d->withPrinter([d, value, require](std::shared_ptr<QPrinter> printer) {  // made on a worker, as Plot… does
        require(bool(printer), "a printer is set up off the UI thread");
        if (!printer) return;
        printer->setOutputFormat(QPrinter::PdfFormat);  // no paper wasted: the printer's path to a file
        printer->setOutputFileName(value + ".printer.pdf");
        d->plotToPrinter(printer);
      });
#else
      require(false, "this build has no printing support");
#endif
    }, [dialog] { return !dialog()->isVisible(); });
    auto pdf = [](const QString& path) {
      QFile f(path);
      return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    script->add("printed", [plotAction, require, pdf, value] {
      const QByteArray bytes = pdf(value + ".printer.pdf");
      require(bytes.startsWith("%PDF") && bytes.size() > 500, QString("the printer path wrote its page (%1 bytes)").arg(bytes.size()));
      plotAction->trigger();
    }, ready);
    script->add("to PDF", [dialog, value] { dialog()->plotToPdf(value + ".pdf"); }, [dialog] { return !dialog()->isVisible(); });
    script->add("the PDF", [require, pdf, value] {
      const QByteArray bytes = pdf(value + ".pdf");
      require(bytes.startsWith("%PDF") && bytes.contains("/MediaBox [0 0 842") && !bytes.contains("/Subtype /Image") && bytes.size() > 500,
              QString("one vector page, A4 landscape (%1 bytes)").arg(bytes.size()));
    });
    // Another document (the same file opened again): its plot starts from its extents, not the window picked before.
    auto generation = std::make_shared<unsigned long long>(0);
    script->add("another document", [&w, doc, generation] {
      *generation = doc->generation;
      w.m_benchSelect = false;  // the load does not start this bench again
      w.openPath(doc->browse ? doc->viewing : doc->path());
    }, [doc, generation, settled] { return doc->generation != *generation && !doc->loading && settled(); });
    script->add("plot it", [&w, plotAction] {
      w.m_benchSelect = true;
      plotAction->trigger();
    }, ready);
    script->add("its extents", [dialog, require] {
      PlotDialog* d = dialog();
      require(d->settings().region == plot::Region::Extents && static_cast<QRadioButton*>(d->findChild<QWidget*>("plotExtents"))->isChecked(),
              "another document's plot starts from its extents, not from the window picked in the last one");
      d->reject();
    }, [dialog] { return !dialog()->isVisible(); });
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}
