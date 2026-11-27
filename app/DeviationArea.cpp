// Deviation (Review > Inspect > Check): how far a measured body A (a 3D scan, a printed part, a revision) lies from a
// reference B, shown as a colour map on A with statistics, a histogram, a probe and pins at the worst spots. The numbers come
// from opad::deviation (core/src/deviation.cpp) on a worker; the map is a vertex paint over the arrays A is drawn with
// (VertexPaint.hpp, Viewport::setVertexPaints), coloured on a worker and refined with the body. The Deviation layer of
// looks keeps A plain under its map and fades B behind it. Best fit (ICP) is a preview: A is drawn moved
// (setPreviewMotion) and measured where it was moved to; Accept writes the motion as A's placement (one transform step),
// Discard puts it back. Nothing of the map is stored in the document.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <BRepBndLib.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <BRep_Tool.hxx>
#include <OSD_Parallel.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "BodyLook.hpp"
#include "BodyShape.hpp"
#include "Commands.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/deviation.hpp"
#include "opad/point_cloud.hpp"
#include "opad/geometry.hpp"

using opad::json;
namespace dev = opad::deviation;

namespace deviation_map {
// The diverging scale: green inside the tolerance band, then yellow to red outside B (material added), cyan to blue
// inside it (material missing), saturated past the range; grey where nothing was found within reach, a muted violet where
// the sign is undefined (an open border of B).
struct Scale {
  double tolerance = 0.1, range = 1;
  QColor color(double d, bool defined = true) const {
    if (!std::isfinite(d)) return QColor::fromRgbF(0.55f, 0.56f, 0.58f);
    const double a = std::abs(d);
    if (a <= tolerance) return QColor::fromRgbF(0.30f, 0.74f, 0.38f);
    if (!defined) return QColor::fromRgbF(0.62f, 0.55f, 0.74f);
    const double t = std::clamp((a - tolerance) / std::max(range - tolerance, 1e-9), 0.0, 1.0);
    auto mix = [](const QColor& x, const QColor& y, double k) {
      return QColor::fromRgbF(float(x.redF() + (y.redF() - x.redF()) * k), float(x.greenF() + (y.greenF() - x.greenF()) * k), float(x.blueF() + (y.blueF() - x.blueF()) * k));
    };
    const QColor out0 = QColor::fromRgbF(0.74f, 0.84f, 0.26f), out1 = QColor::fromRgbF(0.98f, 0.72f, 0.16f), out2 = QColor::fromRgbF(0.86f, 0.14f, 0.13f);
    const QColor in0 = QColor::fromRgbF(0.26f, 0.80f, 0.76f), in1 = QColor::fromRgbF(0.16f, 0.53f, 0.95f), in2 = QColor::fromRgbF(0.24f, 0.16f, 0.74f);
    if (d > 0) return t < 0.5 ? mix(out0, out1, t * 2) : mix(out1, out2, t * 2 - 1);
    return t < 0.5 ? mix(in0, in1, t * 2) : mix(in1, in2, t * 2 - 1);
  }
};

// The signed distance of every vertex of a drawn array to B, kept per array for the study (a recolour does not measure
// again). The array is held so its address is never reused by another while it is cached.
struct Cache {
  struct Entry {
    Handle(Graphic3d_ArrayOfTriangles) array;
    std::vector<float> value;
    std::vector<uint8_t> undefined;
  };
  std::mutex mu;
  std::map<const void*, std::shared_ptr<const Entry>> entries;
};

class Paint : public VertexPaint {
 public:
  Paint(std::shared_ptr<const dev::Surface> to, const gp_Trsf& motion, double reach, Scale scale, std::shared_ptr<Cache> cache)
      : m_to(std::move(to)), m_motion(motion), m_reach(reach > 0 ? reach : std::numeric_limits<double>::infinity()), m_scale(scale), m_cache(std::move(cache)) {}

  std::shared_ptr<BodyPrs> paint(const BodyPrs& src, const gp_Trsf& placed, const std::function<bool()>& cancelled) const override {
    const Handle(Graphic3d_ArrayOfTriangles)& tris = src.triangles;
    if (tris.IsNull() || !m_to) return nullptr;
    const int nv = tris->VertexNumber();
    std::shared_ptr<const Cache::Entry> entry;
    {
      std::lock_guard<std::mutex> lock(m_cache->mu);
      if (const auto it = m_cache->entries.find(tris.get()); it != m_cache->entries.end() && int(it->second->value.size()) == nv) entry = it->second;
    }
    if (!entry) {
      auto fresh = std::make_shared<Cache::Entry>();
      fresh->array = tris;
      fresh->value.assign(size_t(nv), std::numeric_limits<float>::quiet_NaN());
      fresh->undefined.assign(size_t(nv), 0);
      const gp_Trsf world = m_motion * placed;
      constexpr int kChunk = 4096;
      const int chunks = (nv + kChunk - 1) / kChunk;
      for (int start = 0; start < chunks; start += 64) {  // a cancel stops it between batches
        if (cancelled && cancelled()) return nullptr;
        OSD_Parallel::For(start, std::min(chunks, start + 64), [&](int c) {
          for (int i = c * kChunk; i < std::min(nv, (c + 1) * kChunk); ++i) {
            const dev::Surface::Hit hit = m_to->nearest(tris->Vertice(i + 1).Transformed(world), m_reach);
            if (!hit.found()) continue;
            fresh->value[size_t(i)] = float(hit.distance);
            fresh->undefined[size_t(i)] = hit.sign_defined ? 0 : 1;
          }
        });
      }
      std::lock_guard<std::mutex> lock(m_cache->mu);
      m_cache->entries[tris.get()] = entry = fresh;
    }
    const bool normals = tris->HasVertexNormals();
    Handle(Graphic3d_ArrayOfTriangles) coloured = new Graphic3d_ArrayOfTriangles(
        nv, tris->EdgeNumber(), Graphic3d_ArrayFlags_VertexColor | (normals ? Graphic3d_ArrayFlags_VertexNormal : Graphic3d_ArrayFlags_None));
    for (int i = 1; i <= nv; ++i) {
      if (normals) coloured->AddVertex(tris->Vertice(i), tris->VertexNormal(i));
      else coloured->AddVertex(tris->Vertice(i));
      const QColor c = m_scale.color(entry->value[size_t(i - 1)], !entry->undefined[size_t(i - 1)]);
      coloured->SetVertexColor(i, Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB));
    }
    for (int e = 1; e <= tris->EdgeNumber(); ++e) coloured->AddEdge(tris->Edge(e));
    auto out = std::make_shared<BodyPrs>();
    out->triangles = coloured;
    out->boundaries = src.boundaries;
    out->box = src.box;
    out->deflection = src.deflection;
    out->closed = src.closed;
    out->faceTriangles = src.faceTriangles;
    out->edgeLines = src.edgeLines;
    return out;
  }

 private:
  std::shared_ptr<const dev::Surface> m_to;
  gp_Trsf m_motion;
  double m_reach;
  Scale m_scale;
  std::shared_ptr<Cache> m_cache;
};

// The histogram: one bar per bin coloured by the scale, the tolerance band shaded, the range's ends and zero labelled.
class Histogram : public QWidget {
 public:
  explicit Histogram(QWidget* parent) : QWidget(parent) { setMinimumHeight(104); }
  void set(const json& h, const Scale& scale) {
    m_h = h;
    m_scale = scale;
    update();
  }
  QSize sizeHint() const override { return {260, 104}; }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const Tokens& t = theme::current();
    const QRectF area(4, 4, width() - 8, height() - 30);
    p.fillRect(rect(), t.bg2);
    if (!m_h.is_object() || !m_h.contains("counts")) return;
    const auto counts = m_h["counts"].get<std::vector<size_t>>();
    if (counts.empty()) return;
    const double from = m_h.value("from", -1.0), to = m_h.value("to", 1.0), bin = m_h.value("bin_mm", (to - from) / double(counts.size()));
    const size_t most = std::max<size_t>(1, *std::max_element(counts.begin(), counts.end()));
    auto x = [&](double v) { return area.left() + (v - from) / (to - from) * area.width(); };
    // The tolerance band behind the bars.
    p.fillRect(QRectF(QPointF(x(-m_scale.tolerance), area.top()), QPointF(x(m_scale.tolerance), area.bottom())), QColor(76, 190, 97, 40));
    for (size_t i = 0; i < counts.size(); ++i) {
      const double v0 = from + double(i) * bin, h = area.height() * std::sqrt(double(counts[i]) / double(most));  // sqrt: the tails show
      if (counts[i] == 0) continue;
      p.fillRect(QRectF(x(v0) + 0.5, area.bottom() - std::max(1.0, h), std::max(1.0, x(v0 + bin) - x(v0) - 1), std::max(1.0, h)), m_scale.color(v0 + bin / 2));
    }
    // The scale under it: the same colours as the map.
    const QRectF bar(area.left(), area.bottom() + 3, area.width(), 6);
    for (int k = 0; k < int(bar.width()); ++k) p.fillRect(QRectF(bar.left() + k, bar.top(), 1.5, bar.height()), m_scale.color(from + (to - from) * (k + 0.5) / bar.width()));
    p.setPen(t.fg3);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    const QRectF labels(area.left(), bar.bottom() + 1, area.width(), 14);
    p.setLayoutDirection(Qt::LeftToRight);  // a scale reads left to right in every language
    p.drawText(labels, Qt::AlignLeft | Qt::AlignVCenter, units::compact(units::Kind::Length, from));
    p.drawText(labels, Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("0"));
    p.drawText(labels, Qt::AlignRight | Qt::AlignVCenter, "+" + units::compact(units::Kind::Length, to));
    const size_t below = m_h.value("below", size_t(0)), above = m_h.value("above", size_t(0));
    if (below) p.drawText(area.adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("‹ %1").arg(below));
    if (above) p.drawText(area.adjusted(0, 0, -2, 0), Qt::AlignRight | Qt::AlignTop, QStringLiteral("%1 ›").arg(above));
  }

 private:
  json m_h;
  Scale m_scale;
};
}  // namespace deviation_map

using deviation_map::Scale;

// Point clouds (opad/point_cloud.hpp: a mesh body with nodes and no triangles): there are no triangles to paint, so the cloud
// display colours its own points (Viewport::setCloudColours): one colour per point in ordinal order, 0xAARRGGBB, alpha 0
// where there is no value (the cloud's own colour shows); null clears it.
namespace cloud_map {
using Colours = std::shared_ptr<const std::vector<uint32_t>>;
inline void set(Viewport* view, const std::string& node, const Colours& colours) { view->setCloudColours(node, colours); }
inline bool isCloud(const TopoDS_Shape& shape) { return opad::cloud::is_cloud(shape); }
}  // namespace cloud_map

class Deviation : public AreaController {
 public:
  explicit Deviation(AreaServices& s) : AreaController(s) { s_instance = this; }
  ~Deviation() override {
    if (s_instance == this) s_instance = nullptr;
  }
  static Deviation* instance() { return s_instance; }

  void buildActions() override {
    CommandInfo info{"inspect.deviation", tr("Deviation"), "deviation"};
    info.keywords = {"deviation", "scan", "compare surfaces", "colour map", "color map", "reverse engineering", "print accuracy", "fit", "icp", "best fit", "tolerance"};
    info.enabledWhen = [](const CommandContext& c) { return c.document && !c.sketching; };
    m_action = services().addCommand(info, [this] { start(); });
  }

  void ready() override {
    buildPanel();
    services().viewport()->installEventFilter(this);
    m_probeTimer.setSingleShot(true);
    m_probeTimer.setInterval(40);
    connect(&m_probeTimer, &QTimer::timeout, this, [this] { probe(m_probeAt, false); });
    connect(services().viewport(), &Viewport::vertexPaintsDrawn, this, [this] {
      if (m_panel->isVisible() && m_study) setRunning(QString());
    });
  }

  void selectionChanged(const SelectionContext& selection) override {
    if (!m_panel || !m_panel->isVisible() || m_waiting == 0) return;
    for (const auto& id : selection.ids) {
      if (!services().document()->scene.node(id) || (m_waiting == 1 ? id == m_b : id == m_a)) continue;
      (m_waiting == 1 ? m_a : m_b) = id;
      m_waiting = m_a.empty() ? 1 : m_b.empty() ? 2 : 0;
      refreshPicks();
      if (m_waiting == 0) measure();
      return;
    }
  }

  void documentChanged(bool replaced) override {
    if (!m_panel) return;
    if (replaced) return close();
    const opad::Scene& scene = services().document()->scene;
    if ((!m_a.empty() && !scene.node(m_a)) || (!m_b.empty() && !scene.node(m_b))) {
      if (!scene.node(m_a)) m_a.clear();
      if (!scene.node(m_b)) m_b.clear();
      clearMap();
      m_waiting = m_a.empty() ? 1 : m_b.empty() ? 2 : 0;
      refreshPicks();
      return;
    }
    // A or B moved or changed (an edit, an accepted fit, undo): the map is of where they were, measure again.
    if (m_study && m_panel->isVisible() && !m_fitPreview && signature() != m_signature) measure();
  }

  bool maybeClose() override {
    close();
    return true;
  }

  // ---- benches (DeviationBench below)
  void startWith(const std::string& a, const std::string& b) {
    start();
    m_a = a, m_b = b, m_waiting = 0;
    refreshPicks();
    measure();
  }
  bool busy() const { return m_job != nullptr; }
  const json& report() const { return m_report; }
  std::vector<std::string> measuredBodies() const { return m_study ? m_study->from_bodies : std::vector<std::string>(); }
  void setTolerance(double mm) { m_tolerance->setValue(units::toDisplay(units::Kind::Length, mm)); }
  // The value under a widget point of the view (NaN: not on A), as the probe reads it; `pin` keeps it as a label.
  double probe(const QPointF& at, bool pin) {
    Viewport* view = services().viewport();
    if (!m_study || !m_panel->isVisible() || m_waiting != 0) return showProbe({}), std::nan("");
    const std::string body = view->drawnAt(at);
    const bool onB = mapOnReference();  // the map on B: B's points against A
    const auto& mapped = onB ? m_study->to_bodies : m_study->from_bodies;
    const auto& against = onB ? m_study->from : m_study->to;
    opad::Vec3 w{0, 0, 0};
    if (!against || std::find(mapped.begin(), mapped.end(), body) == mapped.end() || !view->pointUnder(at, w)) return showProbe({}), std::nan("");
    // A previewed fit draws A moved: the point is where A was measured. Without one A stands where it was measured too.
    const dev::Surface::Hit hit = against->nearest(gp_Pnt(w[0], w[1], w[2]), maxDistance() > 0 ? maxDistance() : std::numeric_limits<double>::infinity());
    if (!hit.found()) return showProbe(tr("Probe: nothing within reach")), std::nan("");
    const QString value = signedText(hit.distance) + (hit.sign_defined ? QString() : tr(" (sign undefined)"));
    showProbe(tr("Probe: %1").arg(value));
    if (pin) {
      m_probes.push_back({w, value, scale().color(hit.distance, hit.sign_defined)});
      showPins();
    }
    return hit.distance;
  }
  ToolPanel* panel() const { return m_panel; }
  void bestFit() { measure(true); }
  bool fitPreviewed() const { return m_fitPreview; }
  void acceptFitNow() { acceptFit(); }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (object != services().viewport() || !m_panel || !m_panel->isVisible() || !m_study || m_waiting != 0) return AreaController::eventFilter(object, event);
    const auto* e = static_cast<const QMouseEvent*>(event);
    switch (event->type()) {
      case QEvent::MouseMove:
        if (e->buttons() == Qt::NoButton) {
          m_probeAt = e->position();
          m_probeTimer.start();  // once the mouse rests a moment: one ray and one BVH query
        }
        break;
      case QEvent::MouseButtonPress:
        if (e->button() == Qt::LeftButton) m_pressAt = e->position();
        break;
      case QEvent::MouseButtonRelease:
        if (e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && (e->position() - m_pressAt).manhattanLength() < 4) probe(e->position(), true);
        break;
      case QEvent::Leave:
        m_probeTimer.stop();
        showProbe({});
        break;
      default:
        break;
    }
    return false;
  }

 private:
  struct ProbePin {
    opad::Vec3 at;
    QString text;
    QColor color;
  };

  // ---------------------------------------------------------------- panel
  void buildPanel() {
    auto* body = new QWidget();
    auto* outer = new QVBoxLayout(body);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* scroll = new QScrollArea(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* inner = new QWidget();
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);
    auto* layout = new QVBoxLayout(inner);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);
    auto pickRow = [&](const QString& title, QLabel*& name, QPushButton*& button, int which) {
      auto* row = new QHBoxLayout();
      auto* label = new QLabel(title);
      label->setMinimumWidth(80);
      name = new QLabel();
      name->setTextInteractionFlags(Qt::NoTextInteraction);
      button = new QPushButton(tr("Pick"));
      button->setCheckable(true);
      connect(button, &QPushButton::clicked, this, [this, which] {
        m_waiting = which;
        refreshPicks();
      });
      row->addWidget(label);
      row->addWidget(name, 1);
      row->addWidget(button);
      layout->addLayout(row);
    };
    pickRow(tr("Measured"), m_aName, m_aPick, 1);
    pickRow(tr("Reference"), m_bName, m_bPick, 2);
    auto* options = new QGridLayout();
    options->setHorizontalSpacing(8);
    auto lengthBox = [&](double mm, const QString& zero) {
      auto* box = new QDoubleSpinBox();
      box->setDecimals(units::current().length == "in" ? 4 : 3);
      box->setRange(0, units::toDisplay(units::Kind::Length, 10000));
      box->setSingleStep(units::toDisplay(units::Kind::Length, 0.05));
      box->setSuffix(" " + units::symbol(units::Kind::Length));
      box->setValue(units::toDisplay(units::Kind::Length, mm));
      if (!zero.isEmpty()) box->setSpecialValueText(zero);
      box->setKeyboardTracking(false);
      return box;
    };
    m_tolerance = lengthBox(0.1, QString());
    m_range = lengthBox(0, tr("Auto"));
    m_reach = lengthBox(0, tr("Off"));
    options->addWidget(new QLabel(tr("Tolerance ±")), 0, 0);
    options->addWidget(m_tolerance, 0, 1);
    options->addWidget(new QLabel(tr("Colour range ±")), 1, 0);
    options->addWidget(m_range, 1, 1);
    options->addWidget(new QLabel(tr("Ignore beyond")), 2, 0);
    options->addWidget(m_reach, 2, 1);
    layout->addLayout(options);
    m_both = new QCheckBox(tr("Both ways"));
    m_both->setToolTip(tr("Also measure the reference against the measured body: where the scan has no surface (holes, unscanned sides)"));
    m_regionBox = new QCheckBox(tr("Only the faces picked on the measured body"));
    m_showReference = new QCheckBox(tr("Show the reference behind the map"));
    m_showReference->setChecked(true);
    m_mapOn = new QComboBox();
    m_mapOn->addItems({tr("Map on the measured body"), tr("Map on the reference")});
    m_mapOn->setVisible(false);
    for (QWidget* w : {static_cast<QWidget*>(m_both), static_cast<QWidget*>(m_regionBox), static_cast<QWidget*>(m_showReference), static_cast<QWidget*>(m_mapOn)}) layout->addWidget(w);
    connect(m_tolerance, &QDoubleSpinBox::valueChanged, this, [this] { resummarize(); });
    connect(m_range, &QDoubleSpinBox::valueChanged, this, [this] { resummarize(); });
    connect(m_showReference, &QCheckBox::toggled, this, [this] { applyLooks(); });
    connect(m_mapOn, &QComboBox::currentIndexChanged, this, [this] {
      applyMap();
      applyLooks();
    });
    connect(m_regionBox, &QCheckBox::toggled, this, [this](bool on) {
      m_region.clear();
      if (on) {
        for (const auto& r : services().selection().refs)
          if (r.kind == opad::Ref::Kind::Face && underA(r.body)) m_region.push_back(r.str());
        if (m_region.empty()) {
          services().showMessage(tr("Pick faces of the measured body first (the Face filter), then tick this."), 6000);
          QSignalBlocker block(m_regionBox);
          m_regionBox->setChecked(false);
        } else m_regionBox->setText(tr("Only the %1 faces picked on the measured body").arg(m_region.size()));
      } else m_regionBox->setText(tr("Only the faces picked on the measured body"));
    });
    // Best fit: a preview to accept or discard.
    auto* fitRow = new QHBoxLayout();
    m_fit = new QPushButton(tr("Best fit"));
    m_fit->setToolTip(tr("Fit the measured body onto the reference first (ICP, point to plane), as a preview to accept or discard"));
    m_accept = new QPushButton(tr("Accept fit"));
    m_discard = new QPushButton(tr("Discard fit"));
    m_accept->setVisible(false);
    m_discard->setVisible(false);
    fitRow->addWidget(m_fit);
    fitRow->addStretch(1);
    fitRow->addWidget(m_accept);
    fitRow->addWidget(m_discard);
    layout->addLayout(fitRow);
    m_fitText = new QLabel();
    m_fitText->setWordWrap(true);
    m_fitText->setVisible(false);
    layout->addWidget(m_fitText);
    connect(m_fit, &QPushButton::clicked, this, [this] { measure(true); });
    connect(m_accept, &QPushButton::clicked, this, [this] { acceptFit(); });
    connect(m_discard, &QPushButton::clicked, this, [this] { discardFit(); });
    // Results.
    m_state = new QLabel();
    m_state->setWordWrap(true);
    layout->addWidget(m_state);
    m_histogram = new deviation_map::Histogram(body);
    layout->addWidget(m_histogram);
    m_stats = new QTreeWidget();
    m_stats->setColumnCount(2);
    m_stats->setHeaderHidden(true);
    m_stats->setRootIsDecorated(false);
    m_stats->setSelectionMode(QAbstractItemView::NoSelection);
    m_stats->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_stats->header()->setStretchLastSection(true);
    m_stats->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_stats);
    auto* worstTitle = new QLabel(tr("Worst spots (click to show)"));
    layout->addWidget(worstTitle);
    m_worst = new QTreeWidget();
    m_worst->setColumnCount(3);
    m_worst->setHeaderLabels({tr("Spot"), tr("Deviation"), tr("Points")});
    m_worst->setRootIsDecorated(false);
    m_worst->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_worst->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_worst, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) { showSpot(item->data(0, Qt::UserRole).toInt()); });
    layout->addWidget(m_worst);
    m_probe = new QLabel(tr("Probe: rest the mouse on the measured body; click to pin the value"));
    m_probe->setWordWrap(true);
    layout->addWidget(m_probe);
    layout->addStretch(1);
    m_footer = new PanelFooter(body);
    m_footer->setPrimary(tr("Measure"));
    m_footer->setCancel(tr("Close"));
    m_clearPins = m_footer->addSecondary(tr("Clear pins"));
    connect(m_clearPins, &QPushButton::clicked, this, [this] {
      m_probes.clear();
      showPins();
    });
    connect(m_footer, &PanelFooter::accepted, this, [this] { measure(); });
    connect(m_footer, &PanelFooter::cancelled, this, [this] { m_panel->hide(); });
    outer->addWidget(m_footer);
    m_panel = new ToolPanel("deviation", "deviation", &Tokens::sel, tr("Deviation"), body, 640, services().window());
    m_panel->setObjectName("deviationPanel");
    m_panel->setHelpId("inspect.deviation");
    m_panel->setDefaultWidth(360);
    m_panel->setEscapeHandler([this] {
      if (m_waiting != 0 && (!m_a.empty() || !m_b.empty())) {  // a pick waits: stop waiting first
        m_waiting = 0;
        refreshPicks();
        if (m_a.empty() || m_b.empty()) m_panel->hide();
        return;
      }
      m_panel->hide();
    });
    connect(m_panel, &ToolPanel::visibilityChanged, this, [this](bool on) {
      if (!on) clearMap();
    });
    services().addPanel(m_panel);
    refreshPicks();
  }

  // ---------------------------------------------------------------- flow
  void start() {
    AppDocument* doc = services().document();
    if (!doc->hasDocument) return;
    if (!m_panel->isVisible()) {
      m_a.clear(), m_b.clear();
      // A selection made first: its first node is the measured body, the second the reference.
      for (const auto& id : services().selection().ids) {
        if (!doc->scene.node(id) || doc->scene.bodies_under(id).empty()) continue;
        if (m_a.empty()) m_a = id;
        else if (m_b.empty() && id != m_a) m_b = id;
      }
      m_waiting = m_a.empty() ? 1 : m_b.empty() ? 2 : 0;
    }
    services().openPanel(m_panel);
    refreshPicks();
    if (m_waiting == 0 && !m_study && !m_job) measure();
  }

  void close() {
    if (m_panel && m_panel->isVisible()) m_panel->hide();
    else clearMap();
  }

  bool underA(const std::string& body) const {
    const opad::Scene& scene = services().document()->scene;
    const auto bodies = m_a.empty() ? std::vector<std::string>() : scene.bodies_under(m_a);
    return std::find(bodies.begin(), bodies.end(), body) != bodies.end();
  }

  void refreshPicks() {
    if (!m_panel) return;
    AppDocument* doc = services().document();
    auto name = [doc](const std::string& id) { return id.empty() ? QString() : doc->nodeName(id); };
    m_aName->setText(m_waiting == 1 ? tr("Pick the scan or part to measure…") : name(m_a));
    m_bName->setText(m_waiting == 2 ? tr("Pick the reference (the CAD)…") : name(m_b));
    m_aPick->setChecked(m_waiting == 1);
    m_bPick->setChecked(m_waiting == 2);
    m_footer->setPrimaryEnabled(!m_a.empty() && !m_b.empty() && m_a != m_b);
    m_panel->setContext(m_a.empty() || m_b.empty() ? QString() : tr("%1 against %2").arg(name(m_a), name(m_b)));
  }

  double length(const QDoubleSpinBox* box) const { return units::fromDisplay(units::Kind::Length, box->value()); }
  double tolerance() const { return length(m_tolerance); }
  double maxDistance() const { return length(m_reach); }
  Scale scale() const {
    Scale s;
    s.tolerance = tolerance();
    const double range = length(m_range);
    s.range = range > 0 ? range : std::max(m_report.value("histogram", json::object()).value("to", 1.0), 2 * s.tolerance);
    return s;
  }
  QString signedText(double mm) const { return (mm > 0 ? "+" : "") + units::compact(units::Kind::Length, mm); }
  bool mapOnReference() const { return m_study && m_study->from && m_mapOn->isVisible() && m_mapOn->currentIndex() == 1; }
  // What the study depends on in the document: every body of A and B, its shape and where it stands.
  std::string signature() const {
    const opad::Scene& scene = services().document()->scene;
    std::string out;
    for (const auto& side : {m_a, m_b})
      for (const auto& body : scene.bodies_under(side)) {
        const opad::Node* n = scene.node(body);
        out += body + ":" + (n ? n->body_key : std::string()) + ":" + scene.world(body).to_json().dump() + ";";
      }
    return out;
  }

  void setRunning(const QString& text) { m_state->setText(text.isEmpty() ? summaryLine() : text); }

  // Measures A against B on a worker (with `fit`: the best fit first, previewed).
  void measure(bool fit = false) {
    AppDocument* doc = services().document();
    if (m_a.empty() || m_b.empty()) return;
    if (m_a == m_b) return services().showMessage(tr("Pick two different bodies."), 5000);
    const opad::Scene& scene = doc->scene;
    const auto a = scene.bodies_under(m_a), b = scene.bodies_under(m_b);
    for (const auto& x : a)
      if (std::find(b.begin(), b.end(), x) != b.end()) return services().showMessage(tr("The measured body is part of the reference: pick another one."), 6000);
    if (Job* old = std::exchange(m_job, nullptr)) old->cancel();
    if (!fit) dropFitPreview();
    json args = {{"from", m_a}, {"to", m_b}, {"tolerance", tolerance()}, {"both_ways", m_both->isChecked()}, {"align", fit}};
    if (maxDistance() > 0) args["max_distance"] = maxDistance();
    if (length(m_range) > 0) args["range"] = length(m_range);
    if (m_regionBox->isChecked() && !m_region.empty()) args["region"] = m_region;
    std::vector<std::string> nodes = a;
    nodes.insert(nodes.end(), b.begin(), b.end());
    auto document = doc->shapesOf(nodes);
    auto sceneCopy = std::make_shared<opad::Scene>(scene);
    auto out = std::make_shared<dev::Study>();
    const std::string sig = signature();  // where A and B stand as measured (an edit while measuring measures again)
    m_state->setText(fit ? tr("Fitting and measuring…") : tr("Measuring…"));
    QElapsedTimer clock;
    clock.start();
    m_job = services().jobs()->async(fit ? tr("Best fit and deviation") : tr("Measuring the deviation"), [document, sceneCopy, args, out](Progress p) {
      *out = dev::run(*document, *sceneCopy, args, [p] { return p.cancelled(); }, [p](const std::string& phase, int percent) { p.setPhase(phaseText(phase), percent); });
    }, [this, out, fit, clock, sig](bool ok, const QString& error) {
      m_job = nullptr;
      if (!ok) {
        m_state->setText(error == "cancelled" || error.contains("cancelled") ? tr("Cancelled.") : i18n::t(error));
        return;
      }
      if (trace::enabled()) trace::log(QStringLiteral("deviation: measured in %1 ms").arg(clock.elapsed()));
      if (signature() != sig) return measure(fit);  // edited while it measured: this map is of where they were
      m_study = out;
      m_signature = sig;
      m_report = out->report;
      m_cache = std::make_shared<deviation_map::Cache>();
      m_mapOn->setVisible(m_report.contains("reverse"));
      if (fit) previewFit();
      else m_fitText->setVisible(false);
      showReport();
      applyMap();
      applyLooks();
    });
  }

  static QString phaseText(const std::string& phase) {
    if (phase == "Meshing the reference") return tr("Meshing the reference");
    if (phase == "Indexing the reference") return tr("Indexing the reference");
    if (phase == "Sampling") return tr("Sampling the measured body");
    if (phase == "Best fit") return tr("Best fit");
    if (phase == "Measuring") return tr("Measuring");
    if (phase == "Measuring back") return tr("Measuring back");
    return {};
  }

  // Tolerance or range changed: the stats, histogram and spots again from the values (a worker), and the map recoloured.
  void resummarize() {
    if (!m_study || m_job) return;
    auto study = m_study;
    const double tol = tolerance(), range = length(m_range);
    auto out = std::make_shared<json>();
    m_summaryJob = services().jobs()->async(tr("Deviation statistics"), [study, tol, range, out](Progress) { *out = dev::summary(*study, tol, 40, range); },
      [this, study, out](bool ok, const QString&) {
        m_summaryJob = nullptr;
        if (!ok || study != m_study) return;
        for (auto& [k, v] : out->items()) m_report[k] = v;
        m_report["tolerance_mm"] = tolerance();
        showReport();
        applyMap();
      });
  }

  // ---------------------------------------------------------------- the map, looks, pins
  void applyMap() {
    Viewport* view = services().viewport();
    if (!m_study) return view->setVertexPaints({});
    const bool onB = mapOnReference();
    std::map<std::string, std::shared_ptr<const VertexPaint>> paints;
    const Scale s = scale();
    std::vector<std::string> clouds;
    if (!onB) {
      // A where it was measured: moved by a previewed fit (the viewport draws it moved; its arrays stay in its own frame).
      auto paint = std::make_shared<deviation_map::Paint>(m_study->to, m_fitPreview ? m_study->alignment : gp_Trsf(), maxDistance(), s, m_cache);
      for (const auto& id : m_study->from_bodies)
        if (isCloud(id)) clouds.push_back(id);  // no triangles to paint: the cloud display colours its points
        else paints[id] = paint;
    } else {
      if (!m_backCache) m_backCache = std::make_shared<deviation_map::Cache>();
      // B against A as A was measured (moved by a previewed fit): B's vertices taken back into A's frame of that study.
      auto paint = std::make_shared<deviation_map::Paint>(m_study->from, gp_Trsf(), maxDistance(), s, m_backCache);
      for (const auto& id : m_study->to_bodies) paints[id] = paint;
    }
    if (trace::enabled()) trace::log(QStringLiteral("deviation: map on %1, tolerance %2 mm, range %3 mm").arg(onB ? "the reference" : "the measured body").arg(s.tolerance).arg(s.range));
    setRunning(tr("Colouring the map…"));
    view->setVertexPaints(std::move(paints));
    colourClouds(clouds, s);
    showPins();
    const json back = m_report.value("reverse", json());
    if (m_histogram) m_histogram->set(onB && back.is_object() ? back.value("histogram", json()) : m_report.value("histogram", json()), s);
  }

  bool isCloud(const std::string& id) const {
    const AppDocument* doc = services().document();
    const opad::Node* n = doc->scene.node(id);
    try {
      return n && cloud_map::isCloud(opad::body_shape(doc->doc, n->body_key));  // cached: a lookup and a walk over its few faces
    } catch (const std::exception&) {
      return false;
    }
  }

  // Each cloud of A coloured per point, as the map colours a mesh: its points (node order) placed where A was measured,
  // their distances to B on a worker, handed to the cloud display. Clouds no longer listed are given back their colours.
  void colourClouds(const std::vector<std::string>& clouds, const Scale& s) {
    Viewport* view = services().viewport();
    for (const auto& id : m_cloudsColoured)
      if (std::find(clouds.begin(), clouds.end(), id) == clouds.end()) cloud_map::set(view, id, nullptr);
    m_cloudsColoured = clouds;
    if (Job* old = std::exchange(m_cloudJob, nullptr)) old->cancel();
    if (clouds.empty() || !m_study) return;
    struct Item {
      std::string id;
      TopoDS_Shape shape;
      gp_Trsf world;
      cloud_map::Colours colours;
    };
    auto items = std::make_shared<std::vector<Item>>();
    const AppDocument* doc = services().document();
    const gp_Trsf motion = m_fitPreview ? m_study->alignment : gp_Trsf();
    for (const auto& id : clouds)
      try {
        const opad::Mat4 world = doc->scene.world(id);
        if (opad::mat_is_rigid(world)) items->push_back({id, opad::body_shape(doc->doc, doc->scene.node(id)->body_key), motion * opad::trsf_from_mat(world), nullptr});
      } catch (const std::exception&) {
      }
    auto to = m_study->to;
    const double reach = maxDistance() > 0 ? maxDistance() : std::numeric_limits<double>::infinity();
    auto study = m_study;
    m_cloudJob = services().jobs()->async(tr("Colouring the deviation map"), [items, to, reach, s](Progress p) {
      for (auto& item : *items) {
        std::vector<gp_Pnt> points;
        for (TopExp_Explorer f(item.shape, TopAbs_FACE); f.More(); f.Next()) {
          TopLoc_Location location;
          const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), location);
          if (t.IsNull()) continue;
          const gp_Trsf place = item.world * location.Transformation();
          for (int i = 1; i <= t->NbNodes(); ++i) points.push_back(t->Node(i).Transformed(place));
        }
        auto colours = std::make_shared<std::vector<uint32_t>>(points.size(), 0u);
        const int n = int(points.size());
        for (int start = 0; start < n; start += 1 << 16) {
          if (p.cancelled()) return;
          OSD_Parallel::For(start, std::min(n, start + (1 << 16)), [&](int i) {
            const dev::Surface::Hit hit = to->nearest(points[size_t(i)], reach);
            if (!hit.found()) return;  // alpha 0: the cloud's own colour
            (*colours)[size_t(i)] = s.color(hit.distance, hit.sign_defined).rgba() | 0xFF000000u;
          });
        }
        item.colours = std::move(colours);
      }
    }, [this, items, study](bool ok, const QString&) {
      m_cloudJob = nullptr;
      if (!ok || study != m_study) return;
      for (const auto& item : *items)
        if (item.colours && std::find(m_cloudsColoured.begin(), m_cloudsColoured.end(), item.id) != m_cloudsColoured.end())
          cloud_map::set(services().viewport(), item.id, item.colours);
      setRunning(QString());
    });
  }

  void applyLooks() {
    Viewport* view = services().viewport();
    if (!m_study) return view->clearLookLayer(LookSource::Deviation);
    const bool onB = mapOnReference();
    std::map<std::string, LookDelta> layer;
    LookDelta mapped;
    mapped.visible = true;
    mapped.opacity = 1;
    mapped.color = std::array<double, 3>{1, 1, 1};  // the map's colours as they are
    LookDelta behind;
    behind.visible = m_showReference->isChecked();
    behind.fade = 0.22;
    behind.pickable = false;  // a click on the map reaches the map
    for (const auto& id : m_study->from_bodies) layer[id] = onB ? behind : mapped;
    for (const auto& id : m_study->to_bodies) layer[id] = onB ? mapped : behind;
    view->setLookLayer(LookSource::Deviation, std::move(layer));
  }

  void showPins() {
    std::vector<Viewport::Pin> pins;
    const bool onB = mapOnReference();
    const json worst = onB ? m_report.value("reverse", json::object()).value("worst", json::array()) : m_report.value("worst", json::array());
    const Scale s = scale();
    int n = 0;
    for (const auto& spot : worst) {
      const double v = spot.value("value", 0.0);
      const auto& p = spot["position"];
      pins.push_back({{p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}, QStringLiteral("%1  %2").arg(++n).arg(signedText(v)), s.color(v)});
    }
    for (const auto& probe : m_probes) pins.push_back({probe.at, probe.text, probe.color});
    services().viewport()->setDeviationPins(pins);
  }

  void clearMap() {
    if (Job* old = std::exchange(m_job, nullptr)) old->cancel();
    Viewport* view = services().viewport();
    view->setVertexPaints({});
    colourClouds({}, scale());
    view->clearLookLayer(LookSource::Deviation);
    view->setDeviationPins({});
    dropFitPreview();
    m_study.reset();
    m_cache.reset();
    m_backCache.reset();
    m_report = json::object();
    m_probes.clear();
    if (m_stats) {
      m_stats->clear();
      m_worst->clear();
      m_histogram->set(json(), scale());
      m_state->clear();
      m_fitText->setVisible(false);
      m_accept->setVisible(false);
      m_discard->setVisible(false);
    }
    showProbe({});
  }

  void showProbe(const QString& text) {
    if (m_probe) m_probe->setText(text.isEmpty() ? tr("Probe: rest the mouse on the measured body; click to pin the value") : text);
    Viewport* view = services().viewport();
    if (text == m_probeHint) return;
    m_probeHint = text;
    emit view->hoverChanged(text.isEmpty() ? view->hoverText() : text);  // in the status bar's hover text, over the viewport's
  }

  QString summaryLine() const {
    if (!m_report.is_object() || !m_report.contains("stats")) return {};
    const json& st = m_report["stats"];
    const json& sampling = m_report.value("sampling", json::object());
    QString line = tr("%1 points").arg(QLocale().toString(qlonglong(sampling.value("points", size_t(0)))));
    if (sampling.value("subsampled", false) && sampling.value("source", std::string()) == "mesh vertices")
      line = tr("%1 of %2 vertices").arg(QLocale().toString(qlonglong(sampling.value("points", size_t(0)))), QLocale().toString(qlonglong(sampling.value("vertices", size_t(0)))));
    else if (sampling.value("source", std::string()) == "surface samples")
      line = tr("%1 points sampled on the surface").arg(QLocale().toString(qlonglong(sampling.value("points", size_t(0)))));
    line += " · " + tr("%1% within ±%2").arg(QString::number(st.value("within_tolerance_pct", 0.0), 'f', 1), units::compact(units::Kind::Length, tolerance()));
    if (!m_report.value("sign_defined", true)) line += " · " + tr("sign undefined at the reference's open border");
    return line;
  }

  void showReport() {
    const json& st = m_report.value("stats", json::object());
    m_stats->clear();
    auto row = [this](const QString& name, const QString& value) {
      auto* item = new QTreeWidgetItem(m_stats, {name, value});
      item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    };
    auto len = [this, &st](const char* key) { return signedText(st.value(key, 0.0)); };
    auto abs = [&st](const char* key) { return units::compact(units::Kind::Length, st.value(key, 0.0)); };
    auto pct = [&st](const char* key) { return QString::number(st.value(key, 0.0), 'f', 1) + "%"; };
    row(tr("Within tolerance"), pct("within_tolerance_pct"));
    row(tr("Outside (material added)"), pct("outside_pct"));
    row(tr("Inside (material missing)"), pct("inside_pct"));
    row(tr("Mean"), len("mean"));
    row(tr("Median"), len("median"));
    row(tr("Median |d|"), abs("median_abs"));
    row(tr("RMS"), abs("rms"));
    row(tr("Std deviation"), abs("std_dev"));
    row(tr("Max outside"), len("max"));
    row(tr("Max inside"), len("min"));
    row(tr("P90 |d|"), abs("p90_abs"));
    row(tr("P95 |d|"), abs("p95_abs"));
    if (m_report.value("ignored", size_t(0)) > 0) row(tr("Ignored (beyond reach)"), QLocale().toString(qlonglong(m_report["ignored"].get<size_t>())));
    if (const json back = m_report.value("reverse", json()); back.is_object()) {
      const json& bs = back["stats"];
      row(tr("Back: within tolerance"), QString::number(bs.value("within_tolerance_pct", 0.0), 'f', 1) + "%");
      row(tr("Back: median |d|"), units::compact(units::Kind::Length, bs.value("median_abs", 0.0)));
      row(tr("Back: max |d|"), units::compact(units::Kind::Length, bs.value("max_abs", 0.0)));
    }
    m_worst->clear();
    int n = 0;
    for (const auto& spot : m_report.value("worst", json::array())) {
      auto* item = new QTreeWidgetItem(m_worst, {QString::number(++n), signedText(spot.value("value", 0.0)), QLocale().toString(qlonglong(spot.value("points", size_t(0))))});
      item->setData(0, Qt::UserRole, n - 1);
      item->setForeground(1, scale().color(spot.value("value", 0.0)));
    }
    fitRows(m_stats, false);
    fitRows(m_worst, true);
    setRunning(QString());
  }

  // A list as tall as its rows (the panel scrolls, not the list).
  static void fitRows(QTreeWidget* tree, bool header) {
    const int rows = tree->topLevelItemCount();
    const int row = rows ? std::max(tree->sizeHintForRow(0), 1) : tree->fontMetrics().height() + 6;
    tree->setFixedHeight(std::max(1, rows) * row + (header && !tree->isHeaderHidden() ? tree->header()->sizeHint().height() : 0) + 2 * tree->frameWidth() + 2);
  }

  void showSpot(int index) {
    const json worst = m_report.value("worst", json::array());
    if (index < 0 || size_t(index) >= worst.size()) return;
    const auto& p = worst[size_t(index)]["position"];
    const double r = std::max(m_study ? m_study->size * 0.08 : 10.0, 10 * tolerance());
    Bnd_Box box;
    box.Update(p[0].get<double>() - r, p[1].get<double>() - r, p[2].get<double>() - r, p[0].get<double>() + r, p[1].get<double>() + r, p[2].get<double>() + r);
    services().viewport()->fitBox(box);
  }

  // ---------------------------------------------------------------- best fit
  void previewFit() {
    const json fit = m_report.value("alignment", json::object());
    m_fitPreview = true;
    std::vector<std::pair<std::string, gp_Trsf>> moved;
    for (const auto& id : m_study->from_bodies) moved.emplace_back(id, m_study->alignment);
    services().viewport()->setPreviewMotion(moved);
    m_fitText->setText(tr("Best fit: rms %1 → %2, turned %3°. Shown moved; accept to keep it.")
                           .arg(units::compact(units::Kind::Length, fit.value("rms_before", 0.0)), units::compact(units::Kind::Length, fit.value("rms_after", 0.0)),
                                QString::number(fit.value("turned_deg", 0.0), 'f', 2)));
    m_fitText->setVisible(true);
    m_accept->setVisible(true);
    m_discard->setVisible(true);
  }

  void dropFitPreview() {
    if (!m_fitPreview) return;
    m_fitPreview = false;
    services().viewport()->setPreviewMotion({});
    if (m_accept) {
      m_accept->setVisible(false);
      m_discard->setVisible(false);
    }
  }

  void discardFit() {
    dropFitPreview();
    m_fitText->setVisible(false);
    measure();
  }

  // The fit becomes the measured node's placement: one transform step (world motion composed with where it stands).
  void acceptFit() {
    if (!m_study || !m_fitPreview) return;
    if (!services().requireEditable([this] { acceptFit(); })) return;
    AppDocument* doc = services().document();
    const opad::Node* node = doc->scene.node(m_a);
    if (!node) return;
    // A part of a linked file (a scan imported linked) is the file's: its top node, the file's placement, moves instead.
    for (const opad::Node* up = node->parent.empty() ? nullptr : doc->scene.node(node->parent); node->linked && up && up->linked && up->source_op == node->source_op;
         up = node->parent.empty() ? nullptr : doc->scene.node(node->parent))
      node = up;
    const std::string target = node->id;
    const gp_Trsf world = opad::trsf_from_mat(doc->scene.world(target));
    const gp_Trsf parent = node->parent.empty() ? gp_Trsf() : opad::trsf_from_mat(doc->scene.world(node->parent));
    const gp_Trsf local = parent.Inverted() * m_study->alignment * world;
    dropFitPreview();
    m_fitText->setVisible(false);
    try {
      doc->run("transform", json{{"target", target}, {"matrix", opad::mat_from_trsf(local).to_json()}});
    } catch (const std::exception& e) {  // said in the panel and the status bar (a message box would stop a bench)
      if (trace::enabled()) trace::log(QStringLiteral("deviation: accepting the fit failed: %1").arg(QString::fromUtf8(e.what())));
      m_state->setText(i18n::t(QString::fromUtf8(e.what())));
      services().showMessage(i18n::t(QString::fromUtf8(e.what())), 8000);
      return;
    }
    services().undoToast(tr("Fitted %1 onto %2").arg(doc->nodeName(m_a), doc->nodeName(m_b)));
  }

  static inline Deviation* s_instance = nullptr;
  QAction* m_action = nullptr;
  ToolPanel* m_panel = nullptr;
  PanelFooter* m_footer = nullptr;
  QLabel *m_aName = nullptr, *m_bName = nullptr, *m_state = nullptr, *m_probe = nullptr, *m_fitText = nullptr;
  QPushButton *m_aPick = nullptr, *m_bPick = nullptr, *m_fit = nullptr, *m_accept = nullptr, *m_discard = nullptr, *m_clearPins = nullptr;
  QDoubleSpinBox *m_tolerance = nullptr, *m_range = nullptr, *m_reach = nullptr;
  QCheckBox *m_both = nullptr, *m_regionBox = nullptr, *m_showReference = nullptr;
  QComboBox* m_mapOn = nullptr;
  QTreeWidget *m_stats = nullptr, *m_worst = nullptr;
  deviation_map::Histogram* m_histogram = nullptr;
  std::string m_a, m_b, m_signature;  // m_signature: where A and B stood when measured (signature())
  int m_waiting = 0;  // 1: the measured body, 2: the reference
  std::vector<std::string> m_region;
  std::shared_ptr<const dev::Study> m_study;
  std::shared_ptr<deviation_map::Cache> m_cache, m_backCache;
  json m_report = json::object();  // the last study's report (an object always: value() on null throws)
  bool m_fitPreview = false;
  Job *m_job = nullptr, *m_summaryJob = nullptr, *m_cloudJob = nullptr;
  std::vector<std::string> m_cloudsColoured;  // clouds handed colours (cloud_map::set), cleared with the map
  QTimer m_probeTimer;
  QPointF m_probeAt, m_pressAt;
  QString m_probeHint;
  std::vector<ProbePin> m_probes;
};

OPAD_AREA(Deviation)

// ---------------------------------------------------------------- bench
// OPAD_BENCH_DEVIATION=<prefix> on a document holding a big mesh (a scan) and a solid (its reference): the tool measures the
// mesh against the solid, the map is drawn on the mesh (its vertex colours checked against the values), the stats are those
// the generated data must give, a probe reads a spot, the pins show, a new tolerance recolours, a best fit is previewed and
// accepted, then a point cloud is measured against the solid (its points coloured by the cloud display, given back on
// close), and no step stalls the UI
// thread past 250 ms (trace::stalls). Logs "bench: deviation: ... PASS/FAIL", saves <prefix>.panel.png.

OPAD_BENCH(OPAD_BENCH_DEVIATION, deviation) {
  struct State {
    int phase = 0, ticks = 0;
    size_t ops = 0;
    bool all = true;
    std::string mesh, solid, cloud;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  const QString prefix = value;
  auto* timer = new QTimer(&w);
  timer->setInterval(25);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, prefix] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    Deviation* tool = Deviation::instance();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: deviation: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    auto stalls = [require](const QString& step) {
      const trace::Stalls s = trace::stalls();
      require(s.longest < 250, QStringLiteral("%1: longest stall %2 ms (%3 stalls)").arg(step).arg(s.longest).arg(s.count));
      trace::resetStalls();
    };
    ++st->ticks;
    if (st->phase == 0) {  // the loaded document: its mesh and its solid
      if (doc->loading || w.m_jobs->busy()) return;
      for (const auto& id : doc->scene.all_bodies())
        try {
          const TopoDS_Shape shape = opad::body_shape(doc->doc, doc->scene.node(id)->body_key);
          (cloud_map::isCloud(shape) ? st->cloud : opad::is_mesh_shape(shape) ? st->mesh : st->solid) = id;
        } catch (const std::exception&) {
        }
      if (!require(tool && !st->mesh.empty() && !st->solid.empty() && !st->cloud.empty(), "a mesh, a cloud and a solid in the document")) return finish();
      // A fit of the plain model first: what the window's own fit costs with this mesh (compared with the one after the map).
      trace::resetStalls();
      view->fitAll();
      st->phase = -1;
      st->ticks = 0;
      return;
    }
    if (st->phase == -1) {
      if (st->ticks < 8) return;
      trace::resetStalls();
      { QElapsedTimer frame; frame.start(); view->grabImage(); trace::log(QStringLiteral("bench: deviation: (a frame of the plain model: %1 ms, longest stall %2 ms)").arg(frame.elapsed()).arg(trace::stalls().longest)); }
      trace::resetStalls();
      st->clock.start();
      tool->setTolerance(0.3);  // the ripple (0.2 mm) inside the band
      tool->startWith(st->mesh, st->solid);
      st->phase = 1;
      return;
    }
    if (st->phase == 1) {  // measured and drawn
      if (st->clock.elapsed() > 300000) {
        require(false, "measured and drawn within 5 minutes");
        return finish();
      }
      if (tool->busy() || !view->vertexPaintDrawn(st->mesh)) return;
      trace::log(QStringLiteral("bench: deviation: measured and drawn in %1 ms").arg(st->clock.elapsed()));
      stalls("measure and draw the map");
      const json& r = tool->report();
      const json& s = r["stats"];
      trace::log(QString::fromStdString("bench: deviation: report " + json{{"stats", s}, {"sampling", r["sampling"]}, {"to", r["to"]}}.dump()));
      // The tube's outer wall ripples 0.2 mm about the reference's side; its bore (radius 73) lies 3 mm inside the solid.
      require(r["sampling"]["source"] == "mesh vertices" && r["sampling"]["points"].get<size_t>() >= 400000, "sampled the scan's vertices (400k or more)");
      require(s["min"].get<double>() < -2.7 && s["min"].get<double>() > -3.3, "the bore reads about -3 mm (material missing)");
      require(s["max"].get<double>() > 0.1 && s["max"].get<double>() < 0.3, "the outer ripple reads up to +0.2 mm");
      require(std::abs(s["inside_pct"].get<double>() - 50) < 8, "about half the points lie inside (the bore)");
      require(s["within_tolerance_pct"].get<double>() > 40, "the outer wall lies within tolerance");
      size_t counted = 0;
      for (const auto& c : r["histogram"]["counts"]) counted += c.get<size_t>();
      require(counted + r["histogram"]["below"].get<size_t>() + r["histogram"]["above"].get<size_t>() == s["count"].get<size_t>(), "the histogram counts every point");
      require(!r["worst"].empty() && r["worst"][0]["value"].get<double>() < -2.5, "the worst spot is in the bore");
      require(view->deviationPinCount() == r["worst"].size(), "a pin at each worst spot");
      // The map's colours: a sample of the drawn vertices, each coloured as its value says.
      const json map = view->benchVertexPaint(st->mesh, 997);  // a prime: the rows of the tube's grid all get some
      size_t green = 0, blue = 0, odd = 0;
      for (const auto& v : map["vertices"]) {
        const double red = v["color"][0], gr = v["color"][1], bl = v["color"][2], radius = std::hypot(v["at"][0].get<double>(), v["at"][1].get<double>());
        if (radius > 75 && gr > red && gr > bl) ++green;
        const double z = v["at"][2].get<double>();
        if (radius < 74 && z > 5 && z < 115 && bl > red && bl > gr) ++blue;  // the bore away from the caps (near a cap the cap is nearer)
        if (radius < 74 && z > 5 && z < 115 && !(bl > red && bl > gr) && ++odd <= 3) trace::log(QString::fromStdString("bench: deviation: bore vertex " + v.dump()));
      }
      trace::log(QStringLiteral("bench: deviation: map sample %1 vertices, %2 green outside, %3 blue in the bore").arg(map["vertices"].size()).arg(green).arg(blue));
      require(map["drawn"] == true && green > 300 && blue > 300, "the map is drawn green on the outer wall and blue in the bore");
      if (!prefix.isEmpty()) {
        tool->panel()->grab().save(prefix + ".panel.png");
        view->grabImage().save(prefix + ".map.png");
      }
      st->phase = 10;  // the pictures above are the bench's own (grab and PNG encoding): not timed
      st->ticks = 0;
      return;
    }
    if (st->phase == 10) {
      if (st->ticks < 4) return;
      trace::resetStalls();
      view->fitAll();  // the probe at a point of the outer wall, projected into the fitted view
      st->phase = 2;
      st->ticks = 0;
      return;
    }
    if (st->phase == 2) {
      if (st->ticks < 8) return;  // a frame or two after the fit
      trace::log(QStringLiteral("bench: deviation: (the view fit before the probe: longest stall %1 ms)").arg(trace::stalls().longest));
      trace::resetStalls();
      { QElapsedTimer frame; frame.start(); view->grabImage(); trace::log(QStringLiteral("bench: deviation: (a frame with the map: %1 ms, longest stall %2 ms)").arg(frame.elapsed()).arg(trace::stalls().longest)); }
      trace::resetStalls();
      double found = std::nan("");
      for (double a = 0; a < 2 * M_PI && !std::isfinite(found); a += M_PI / 12)
        for (double z : {60.0, 40.0, 80.0}) {
          const QPoint at = view->widgetPoint({76 * std::cos(a), 76 * std::sin(a), z});
          found = tool->probe(QPointF(at), true);
          if (std::isfinite(found)) break;
        }
      require(std::isfinite(found) && std::abs(found) < 3.3, QStringLiteral("the probe reads a spot (%1 mm)").arg(found));
      stalls("probe and pin");
      st->clock.start();
      tool->setTolerance(0.5);  // recoloured, stats again from the values
      st->phase = 3;
      st->ticks = 0;
      return;
    }
    if (st->phase == 3) {
      if (st->ticks < 4) return;
      if ((tool->busy() || !view->vertexPaintDrawn(st->mesh) || tool->report().value("tolerance_mm", 0.0) != 0.5) && st->clock.elapsed() < 120000) return;
      require(tool->report().value("tolerance_mm", 0.0) == 0.5 && view->vertexPaintDrawn(st->mesh), QStringLiteral("a new tolerance recolours the map (%1 ms)").arg(st->clock.elapsed()));
      require(tool->report()["stats"]["within_tolerance_pct"].get<double>() > 45, "more points within the wider tolerance");
      stalls("recolour");
      st->clock.start();
      st->ops = doc->doc.ops.size();
      tool->bestFit();  // ICP first, A shown moved as a preview
      st->phase = 4;
      st->ticks = 0;
      return;
    }
    if (st->phase == 4) {
      if (st->ticks < 4 || ((tool->busy() || !view->vertexPaintDrawn(st->mesh)) && st->clock.elapsed() < 120000)) return;
      const json fit = tool->report().value("alignment", json());
      trace::log(QString::fromStdString("bench: deviation: best fit " + fit.dump()));
      require(fit.is_object() && tool->fitPreviewed() && fit.value("rms_after", 1e9) <= fit.value("rms_before", 0.0) + 1e-9, QStringLiteral("best fit previewed (%1 ms)").arg(st->clock.elapsed()));
      stalls("best fit");
      st->clock.start();
      tool->acceptFitNow();  // one transform step, then measured again where it now stands
      st->phase = 5;
      st->ticks = 0;
      return;
    }
    if (st->phase == 5) {
      if (st->ticks < 4 || ((tool->busy() || !view->vertexPaintDrawn(st->mesh) || tool->report().contains("alignment")) && st->clock.elapsed() < 120000)) return;
      require(doc->doc.ops.size() == st->ops + 1 && doc->doc.ops.back().data.value("op", "") == "transform", "accepting the fit adds one transform step");
      require(!tool->fitPreviewed() && !tool->report().contains("alignment") && view->vertexPaintDrawn(st->mesh), QStringLiteral("measured again where the fit put it (%1 ms)").arg(st->clock.elapsed()));
      stalls("accept the fit");
      tool->panel()->hide();
      require(!view->vertexPaintDrawn(st->mesh) && view->deviationPinCount() == 0, "closing the panel takes the map and the pins away");
      trace::resetStalls();
      st->clock.start();
      tool->startWith(st->cloud, st->solid);  // a point cloud as the measured side: its points coloured by the cloud display
      st->phase = 6;
      st->ticks = 0;
      return;
    }
    if (st->phase == 6) {
      if (st->ticks < 4 || ((tool->busy() || !view->benchCloudOverlaid(st->cloud)) && st->clock.elapsed() < 120000)) return;
      const json& r = tool->report();
      trace::log(QString::fromStdString("bench: deviation: cloud " + json{{"stats", r["stats"]}, {"sampling", r["sampling"]}}.dump()));
      require(view->benchCloudOverlaid(st->cloud), QStringLiteral("the cloud's points are coloured by the map (%1 ms)").arg(st->clock.elapsed()));
      require(r["sampling"]["source"] == "cloud points" && r["sampling"]["points"].get<size_t>() == 100000, "the cloud's points are the samples, every one");
      // The bench's cloud lies 0.15 mm outside the reference's side; the side is measured meshed (its chords inside the
      // circle: up to ~0.03 mm more between the mesh's vertices).
      const double median = r["stats"]["median"].get<double>();
      require(median > 0.14 && median < 0.2 && r["stats"]["within_tolerance_pct"].get<double>() > 99, "the cloud reads about +0.15 mm, within tolerance");
      require(!view->vertexPaintDrawn(st->cloud), "a cloud gets no vertex paint (no triangles)");
      stalls("measure and colour the cloud");
      tool->panel()->hide();
      st->clock.start();
      st->phase = 7;
      st->ticks = 0;
      return;
    }
    if (st->phase == 7) {
      if (view->benchCloudOverlaid(st->cloud) && st->clock.elapsed() < 30000) return;
      require(!view->benchCloudOverlaid(st->cloud), QStringLiteral("closing the panel gives the cloud its own colours back (%1 ms)").arg(st->clock.elapsed()));
      stalls("clear the cloud's colours");
      finish();
    }
  });
  timer->start();
  return true;
}
