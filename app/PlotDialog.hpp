#pragma once
// Plot for 2D drawings (UI-88, area drawing2d): the plot area (the drawings' extents, what the view shows, or a window
// picked in the view), the paper and its orientation, fit or 1:N, monochrome and lineweights, a live preview, and the
// output, a vector PDF or a printer (Qt6::PrintSupport). What is plotted and where is Plot2D.hpp; here it is painted with
// QPainter, the same paint for the preview, the PDF and the printer. Collecting, previewing and writing run on workers.
#include <QDialog>
#include <QImage>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QTransform>

#include <AIS_Shape.hxx>

#include <functional>
#include <memory>
#include <vector>

#include "Plot2D.hpp"

class AreaServices;
class Job;
class PanelFooter;
class PromptBar;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPainter;
class QPrinter;
class QPushButton;
class QRadioButton;

// The sheet as painter paths per style, built once on a worker (QPainterPath is reentrant).
struct PlotPicture {
  plot::Sheet sheet;
  std::vector<QPainterPath> strokes;               // per style: every line of it
  std::vector<std::vector<QPainterPath>> fills;    // per style: each fill (its rings even-odd)
  std::vector<std::vector<QPointF>> dots;          // per style
  struct Raster {
    QImage image;      // decoded, fitted to its frame as the view shows it
    QTransform place;  // its pixels to the plot plane
  };
  std::vector<Raster> images;
  static std::shared_ptr<const PlotPicture> build(plot::Sheet sheet);
};
// Paints the plot on paper of `unitsPerMm` device units per millimetre, the paper's top left at the painter's origin, and
// the stamp along the bottom margin. Any thread: the preview's image, a PDF and a printer get the same picture.
void paintPlot(QPainter& painter, const PlotPicture& picture, const plot::Settings& settings, const plot::Placement& placement, double unitsPerMm);

// The page with the plot on it, as the worker rendered it.
class PlotPreview : public QWidget {
  Q_OBJECT
 public:
  explicit PlotPreview(QWidget* parent = nullptr);
  void setImage(const QImage& image);
  const QImage& image() const { return m_image; }
  QSize pageSize(double paperWidth, double paperHeight) const;  // the image size that fits the widget, device pixels
 protected:
  void paintEvent(QPaintEvent*) override;
 private:
  QImage m_image;
};

class PlotDialog : public QDialog {
  Q_OBJECT
 public:
  PlotDialog(AreaServices& services, QWidget* parent);
  ~PlotDialog() override;
  void start();  // gathers what is plotted (a worker) and shows the dialog over the window
  // For the benches (and the bench-free checks): what the dialog has now.
  plot::Settings settings() const;
  plot::Placement placement() const;
  std::shared_ptr<const PlotPicture> picture() const { return m_picture; }
  bool previewReady() const { return m_previewStamp == m_stamp && m_picture; }
  QImage previewImage() const;
  bool picking() const { return m_picking; }
  QPointF rubberCorner() const { return m_rubberCorner; }  // the window pick's moving corner as last drawn (plane u, v)
  // Output without the file or print dialog: a PDF at `path`, or the printer as set up. plotted() reports the outcome.
  void plotToPdf(const QString& path);
  void plotToPrinter(std::shared_ptr<QPrinter> printer);
  // A printer set to the dialog's paper, made on a worker (Windows asks the spooler for the default printer, seconds on
  // some machines), handed to `then` on the UI thread; null without printing support.
  void withPrinter(std::function<void(std::shared_ptr<QPrinter>)> then);
 signals:
  void previewUpdated();
  void plotted(bool ok, const QString& error);
 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
 private:
  void collect();
  void refresh();   // the settings changed: placement, info, a new preview (debounced)
  void render();
  void plot();      // Plot…: the file or print dialog, then the output
  void pickWindow();
  void endPick(bool done);
  void pickCorner(const QPointF& widgetPos);
  void showRubber(const QPointF& widgetPos);
  bool planeAt(const QPointF& widgetPos, double& u, double& v);
  bool cornerAt(const QPointF& widgetPos, double& u, double& v);  // the object snap there (its marker shows), else planeAt
  plot::Area displayArea();
  void saveSettings() const;

  AreaServices& m_services;
  std::shared_ptr<const PlotPicture> m_picture;
  Job *m_collectJob = nullptr, *m_previewJob = nullptr, *m_outputJob = nullptr, *m_printerJob = nullptr;
  int m_stamp = 0, m_previewStamp = -1;
  QTimer m_refreshTimer;
  plot::Area m_window;
  bool m_hasWindow = false;
  unsigned long long m_generation = ~0ull;  // the document the window was picked in
  // the window pick
  bool m_picking = false;
  int m_corners = 0;
  double m_cornerU = 0, m_cornerV = 0;
  QPointer<PromptBar> m_prompt;
  Handle(AIS_Shape) m_rubber;
  QPointF m_rubberCorner;
  int m_snapBefore = 0;  // the view's Viewport::SnapPicks before the pick
  // widgets
  QComboBox *m_paper, *m_orientation;
  QDoubleSpinBox *m_margin, *m_scale;
  QRadioButton *m_extents, *m_display, *m_windowRegion, *m_pdf, *m_printer;
  QPushButton* m_pick;
  QCheckBox *m_fit, *m_monochrome, *m_lineweights, *m_stampBox;
  QLabel *m_info, *m_warning, *m_scaleShown;
  PlotPreview* m_preview;
  PanelFooter* m_footer;
};
