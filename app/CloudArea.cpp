// Point clouds in the window (opad/point_cloud.hpp, ViewportCloud.cpp, BodyPrs::isCloud): Design > Scan holds the cloud
// features (Estimate normals, Remove outliers, Downsample, Slice points: design.cloud_*, from their specs) and three tools of
// this area: Select points (cloud.region: a lasso, a box or a brush over a cloud makes a region of its points, kept or
// deleted as one Crop points step, or copied as a region reference for an AI agent or a fitted reference), Slice to sketch
// (cloud.sliceSketch: a slice's points chained into lines of a new sketch on its plane) and Colour points by
// (cloud.colorBy: the scan's colours, normals, the scalar or the body's colour; setting view/cloudColor). Regions are
// worked out on workers from the drawn camera (cloud::in_polygon, in_sphere); the view only draws them.
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

#include <memory>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "BodyShape.hpp"
#include "CommandHelp.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/design/cloud.hpp"
#include "opad/design/feature.hpp"
#include "opad/point_cloud.hpp"

OPAD_ICON_TABLE(cloud,
                {"cloud_normals", R"(<circle cx="6" cy="17" r="1.2"/><circle cx="12" cy="14" r="1.2"/><circle cx="18" cy="17" r="1.2"/><path d="M6 15.5V9M12 12.5V5M18 15.5V9"/><path d="M4.5 10.5 6 9l1.5 1.5M10.5 6.5 12 5l1.5 1.5M16.5 10.5 18 9l1.5 1.5"/>)"},
                {"cloud_outliers", R"(<circle cx="5" cy="16" r="1"/><circle cx="8" cy="14" r="1"/><circle cx="11" cy="15" r="1"/><circle cx="14" cy="13" r="1"/><circle cx="17" cy="15" r="1"/><circle cx="18" cy="5" r="1.4"/><path d="M15.5 3.5l5 5M20.5 3.5l-5 5"/>)"},
                {"cloud_downsample", R"(<circle cx="5" cy="6" r="1"/><circle cx="9" cy="6" r="1"/><circle cx="5" cy="10" r="1"/><circle cx="9" cy="10" r="1"/><path d="M12 8h3m-1.5-1.5L15 8l-1.5 1.5"/><circle cx="19" cy="8" r="1.4"/><rect x="3" y="14" width="18" height="6" rx="1"/><circle cx="8" cy="17" r="1"/><circle cx="16" cy="17" r="1"/>)"},
                {"cloud_crop", R"(<path d="M6 2v14a2 2 0 0 0 2 2h14"/><path d="M18 22V8a2 2 0 0 0-2-2H2"/><circle cx="11" cy="11" r="1"/><circle cx="14" cy="13" r="1"/><circle cx="11" cy="14.5" r="1"/>)"},
                {"cloud_slice", R"(<circle cx="6" cy="6" r="1"/><circle cx="12" cy="4" r="1"/><circle cx="18" cy="6" r="1"/><circle cx="6" cy="18" r="1"/><circle cx="12" cy="20" r="1"/><circle cx="18" cy="18" r="1"/><path d="M2 12h20" stroke-dasharray="2 2"/><circle cx="5" cy="12" r="1.3"/><circle cx="12" cy="12" r="1.3"/><circle cx="19" cy="12" r="1.3"/>)"},
                {"cloud_region", R"(<path d="M4 9c0-4 5-6 9-5s7 4 6 8-6 7-10 6-5-5-5-9z" stroke-dasharray="2.5 2"/><circle cx="9" cy="10" r="1"/><circle cx="13" cy="9" r="1"/><circle cx="11" cy="13" r="1"/><circle cx="15" cy="13" r="1"/>)"},
                {"cloud_sketch", R"(<circle cx="4" cy="15" r="1"/><circle cx="8" cy="11" r="1"/><circle cx="13" cy="10" r="1"/><circle cx="18" cy="13" r="1"/><path d="M4 19l4-4 5-1 7 3"/><path d="M15 3l3 3-7 7H8v-3z"/>)"},
                {"cloud_color", R"(<circle cx="6" cy="8" r="2" fill="currentColor"/><circle cx="12" cy="5" r="2"/><circle cx="18" cy="8" r="2" fill="currentColor"/><circle cx="8" cy="15" r="2"/><circle cx="16" cy="15" r="2" fill="currentColor"/><circle cx="12" cy="19.5" r="1.5"/>)"});

namespace {
// The modes of Colour points by, as BodyPrs::cloudColorMode numbers them.
struct ColourMode {
  const char* name;
  const char* label;
};
const ColourMode kModes[] = {{"auto", QT_TRANSLATE_NOOP("Cloud", "Automatic")},
                             {"rgb", QT_TRANSLATE_NOOP("Cloud", "Scan colours")},
                             {"normal", QT_TRANSLATE_NOOP("Cloud", "Normals")},
                             {"scalar", QT_TRANSLATE_NOOP("Cloud", "Intensity or deviation")},
                             {"body", QT_TRANSLATE_NOOP("Cloud", "Body colour")}};
}  // namespace

class Cloud : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    auto cloudSelected = [](const CommandContext& c) { return c.document && !c.sketching; };
    CommandInfo region{"cloud.region", tr("Select points"), "cloud_region"};
    region.keywords = {tr("lasso"), tr("brush"), tr("crop points"), tr("delete points"), tr("point cloud"), tr("scan"), tr("region")};
    region.group = tr("Point cloud");
    region.enabledWhen = cloudSelected;
    m_region = services().addCommand(region, [this] { startRegion(); });
    CommandInfo sketch{"cloud.sliceSketch", tr("Slice to sketch"), "cloud_sketch"};
    sketch.keywords = {tr("profile"), tr("point cloud"), tr("section"), tr("reverse engineering"), tr("trace")};
    sketch.group = tr("Point cloud");
    sketch.editsDocument = true;
    sketch.enabledWhen = cloudSelected;
    services().addCommand(sketch, [this] { sliceToSketch(); });
    CommandInfo colour{"cloud.colorBy", tr("Colour points by"), "cloud_color"};
    colour.keywords = {tr("point colours"), tr("normals"), tr("intensity"), tr("deviation"), tr("point cloud")};
    colour.group = tr("Point cloud");
    colour.enabledWhen = [](const CommandContext& c) { return c.document; };
    m_colour = services().addCommand(colour, [this] { setMode((mode() + 1) % 5, true); });
    m_colourMenu = new QMenu(tr("Colour points by"));
    m_colourMenu->setObjectName("cloudColourMenu");
    for (int i = 0; i < 5; ++i) {
      QAction* a = m_colourMenu->addAction(i18n::t(QString::fromLatin1(kModes[i].label)));
      a->setCheckable(true);
      a->setObjectName(QStringLiteral("cloudColour.%1").arg(kModes[i].name));
      connect(a, &QAction::triggered, this, [this, i] { setMode(i, false); });
      m_modeActions.push_back(a);
    }
    m_colour->setMenu(m_colourMenu);
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    if (QMenu* design = menus.value("design")) {
      QMenu* sub = design->addMenu(icons::themed("cloud_region", 16), tr("Point cloud"));
      sub->setObjectName("cloudMenu");
      for (const char* id : {"design.cloud_normals", "design.cloud_outliers", "design.cloud_downsample", "cloud.region", "design.cloud_crop", "design.cloud_slice",
                             "cloud.sliceSketch"})
        if (QAction* a = services().action(id)) sub->addAction(a);
    }
    if (QMenu* view = menus.value("view")) view->addMenu(m_colourMenu);
  }

  void ribbon(RibbonLayout& layout) override {
    if (!layout.addTab("design", "design.scan", tr("Scan"))) return;
    layout.addGroup("design.scan", "design.scan.points", tr("Points"));
    for (const char* id : {"design.cloud_normals", "design.cloud_outliers", "design.cloud_downsample"})
      layout.addAction("design.scan.points", services().action(id), id == std::string("design.cloud_normals") ? RibbonLayout::Size::Large : RibbonLayout::Size::Small);
    layout.addGroup("design.scan", "design.scan.region", tr("Region"));
    layout.addAction("design.scan.region", m_region);
    layout.addAction("design.scan.region", services().action("design.cloud_crop"), RibbonLayout::Size::Small);
    layout.addGroup("design.scan", "design.scan.slices", tr("Slices"));
    layout.addAction("design.scan.slices", services().action("design.cloud_slice"));
    layout.addAction("design.scan.slices", services().action("cloud.sliceSketch"));
    layout.addGroup("design.scan", "design.scan.show", tr("Show"));
    layout.addAction("design.scan.show", m_colour);
    layout.addGroup("design.scan", "design.scan.mesh", tr("Mesh"));
    layout.addAction("design.scan.mesh", services().action("design.mesh_solid"), RibbonLayout::Size::Small);
  }

  void ready() override {
    BodyPrs::cloudColorMode = mode();
    refreshModes();
    buildPanel();
    services().viewport()->installEventFilter(this);
  }

  void documentChanged(bool replaced) override {
    if (replaced) clearRegion();
    if (m_active && !cloudBody(m_body)) stopRegion();
  }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (!m_active || object != services().viewport()) return AreaController::eventFilter(object, event);
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease && type != QEvent::MouseButtonDblClick) return false;
    auto* e = static_cast<QMouseEvent*>(event);
    // The left button is the tool's; the others still orbit, pan and zoom.
    if (type == QEvent::MouseButtonPress && e->button() == Qt::LeftButton) {
      if (services().viewport()->cubeAt(e->position())) return false;
      m_dragging = true;
      m_combine = e->modifiers() & Qt::ShiftModifier ? 1 : e->modifiers() & Qt::ControlModifier ? -1 : 0;
      m_stroke = {e->position()};
      m_brush.clear();
      if (m_tool == 2) brushAt(e->position());
      return true;
    }
    if (type == QEvent::MouseMove && m_dragging) {
      if (m_tool == 0) m_stroke << e->position();
      else if (m_tool == 1) m_stroke = {m_stroke.first(), e->position()};
      else brushAt(e->position());
      drawStroke();
      return true;
    }
    if (type == QEvent::MouseButtonRelease && e->button() == Qt::LeftButton && m_dragging) {
      m_dragging = false;
      services().viewport()->setCloudLasso({}, false);
      finishStroke();
      return true;
    }
    return m_dragging || (e->button() == Qt::LeftButton && type == QEvent::MouseButtonDblClick);
  }

 private:
  int mode() const { return std::clamp(QSettings().value("view/cloudColor", 0).toInt(), 0, 4); }
  void setMode(int m, bool announce) {
    QSettings().setValue("view/cloudColor", m);
    services().viewport()->setCloudColorMode(m);
    refreshModes();
    if (announce) services().toast(tr("Points coloured by: %1").arg(i18n::t(QString::fromLatin1(kModes[m].label))));
  }
  void refreshModes() {
    const int m = mode();
    for (int i = 0; i < int(m_modeActions.size()); ++i) m_modeActions[size_t(i)]->setChecked(i == m);
  }

  // The cloud a command works on: the selected one, else the only one in the document.
  std::string chosenCloud() const {
    AppDocument* doc = services().document();
    for (const auto& id : services().selection().ids)
      if (cloudBody(id)) return id;
    for (const auto& r : services().selection().refs)
      if (cloudBody(r.body)) return r.body;
    std::string only;
    for (const auto& id : doc->scene.all_bodies())
      if (cloudBody(id) && doc->scene.effectively_visible(id)) {
        if (!only.empty()) return {};
        only = id;
      }
    return only;
  }
  bool cloudBody(const std::string& id) const {
    const opad::Node* n = id.empty() ? nullptr : services().document()->node(id);
    return n && n->kind == opad::Node::Kind::Body && n->representation == "points";
  }

  // ---------------------------------------------------------------- Select points
  void buildPanel() {
    auto* body = new QWidget;
    auto* outer = new QVBoxLayout(body);
    outer->setContentsMargins(12, 8, 12, 8);
    auto* modes = new QHBoxLayout;
    auto* group = new QButtonGroup(body);
    const QString names[] = {tr("Lasso"), tr("Box"), tr("Brush")};
    for (int i = 0; i < 3; ++i) {
      auto* b = new QPushButton(names[i], body);
      b->setCheckable(true);
      b->setObjectName(QStringLiteral("cloudTool%1").arg(i));
      group->addButton(b, i);
      modes->addWidget(b);
      m_toolButtons.push_back(b);
    }
    m_toolButtons.front()->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
      m_tool = id;
      m_brushRow->setVisible(id == 2);
      refreshPrompt();
    });
    outer->addLayout(modes);
    m_brushRow = new QWidget(body);
    auto* br = new QHBoxLayout(m_brushRow);
    br->setContentsMargins(0, 0, 0, 0);
    br->addWidget(new QLabel(tr("Brush"), m_brushRow));
    m_brushSize = new QSlider(Qt::Horizontal, m_brushRow);
    m_brushSize->setRange(1, 40);
    m_brushSize->setValue(8);
    m_brushSize->setObjectName("cloudBrushSize");
    br->addWidget(m_brushSize, 1);
    m_brushLabel = new QLabel(m_brushRow);
    br->addWidget(m_brushLabel);
    connect(m_brushSize, &QSlider::valueChanged, this, [this] { refreshPrompt(); });
    m_brushRow->hide();
    outer->addWidget(m_brushRow);
    m_status = new QLabel(body);
    m_status->setWordWrap(true);
    m_status->setObjectName("cloudRegionStatus");
    outer->addWidget(m_status);
    auto* row = new QHBoxLayout;
    m_keep = new QPushButton(icons::themed("cloud_crop", 16), tr("Keep only these"), body);
    m_delete = new QPushButton(tr("Delete these"), body);
    m_copy = new QPushButton(tr("Copy reference"), body);
    m_keep->setObjectName("cloudKeep");
    m_delete->setObjectName("cloudDelete");
    m_copy->setObjectName("cloudCopyRef");
    m_copy->setToolTip(tr("The region as a reference ({\"body\", \"kind\": \"region\", \"vertices\"}): an AI agent's crop, a fitted plane or axis takes it."));
    for (auto* b : {m_keep, m_delete, m_copy}) row->addWidget(b);
    outer->addLayout(row);
    connect(m_keep, &QPushButton::clicked, this, [this] { crop(true); });
    connect(m_delete, &QPushButton::clicked, this, [this] { crop(false); });
    connect(m_copy, &QPushButton::clicked, this, [this] {
      QApplication::clipboard()->setText(QString::fromStdString(opad::cloud::region_ref(m_body, m_selected).dump()));
      services().showMessage(tr("Region reference copied"));
    });
    m_footer = new PanelFooter(body);
    m_footer->setPrimary(PanelFooter::Primary::Close);
    m_footer->setCancel(tr("Clear"));
    connect(m_footer, &PanelFooter::accepted, this, [this] { stopRegion(); });
    connect(m_footer, &PanelFooter::cancelled, this, [this] {
      if (!m_selected.empty()) clearRegion();
      else stopRegion();
    });
    outer->addWidget(m_footer);
    m_panel = new ToolPanel(QStringLiteral("cloud-region"), QStringLiteral("cloud_region"), &Tokens::sel, tr("Select points"), body, 200, services().window());
    m_panel->setObjectName("cloudRegionPanel");
    m_panel->setHelpId("cloud.region");
    m_panel->setEscapeHandler([this] {
      if (!m_selected.empty()) clearRegion();
      else stopRegion();
    });
    connect(m_panel, &ToolPanel::visibilityChanged, this, [this](bool on) {
      if (!on && m_active) stopRegion();
    });
    services().addPanel(m_panel);
  }

  void startRegion() {
    const std::string body = chosenCloud();
    if (body.empty()) throw opad::UserHint("Select a point cloud first.", true);
    if (m_active && body == m_body) return;
    clearRegion();
    m_body = body;
    m_active = true;
    services().openPanel(m_panel);
    refreshPrompt();
  }
  void stopRegion() {
    if (!m_active) return;
    m_active = false;
    m_dragging = false;
    services().viewport()->setCloudLasso({}, false);
    clearRegion();
    if (m_panel->isVisible()) m_panel->hide();
  }
  void clearRegion() {
    m_selected = {};
    if (!m_body.empty()) services().viewport()->showCloudRegion(m_body, nullptr);
    refreshPrompt();
  }

  double brushRadius() const {
    const auto prs = services().viewport()->cloudPrs(m_body);
    const double spacing = prs ? std::max(prs->cloudSpacing, 1e-3) : 0.5;
    return spacing * m_brushSize->value() * 2;
  }

  void refreshPrompt() {
    if (!m_status) return;
    const size_t n = m_selected.count();
    const opad::Node* node = services().document()->node(m_body);
    const QString name = node ? services().document()->nodeName(m_body) : QString();
    static const char* hints[] = {QT_TRANSLATE_NOOP("Cloud", "Drag a lasso round the points."), QT_TRANSLATE_NOOP("Cloud", "Drag a box over the points."),
                                  QT_TRANSLATE_NOOP("Cloud", "Paint over the points.")};
    QString text = n ? tr("%1 points of %2 selected").arg(QLocale().toString(qulonglong(n)), name) : i18n::t(QString::fromLatin1(hints[m_tool]));
    text += QStringLiteral("\n") + help::expand(tr("{fixed:shift} adds, {fixed:ctrl} takes away."));
    m_status->setText(text);
    if (m_brushLabel) m_brushLabel->setText(units::format(units::Kind::Length, brushRadius()));
    for (auto* b : {m_keep, m_delete, m_copy}) b->setEnabled(n > 0 && !m_busy);
  }

  void drawStroke() {
    if (m_tool == 0) services().viewport()->setCloudLasso(m_stroke, false);
    else if (m_tool == 1 && m_stroke.size() == 2) {
      const QRectF r = QRectF(m_stroke[0], m_stroke[1]).normalized();
      services().viewport()->setCloudLasso({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}, true);
    }
  }

  void brushAt(const QPointF& at) {
    opad::Vec3 p;
    if (!services().viewport()->pickCloudPoint(at, p)) return;
    if (!m_brush.empty()) {
      const auto& q = m_brush.back();
      const double d = std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
      if (d < brushRadius() * 0.4) return;  // close to the last dab
    }
    m_brush.push_back(p);
  }

  // The stroke done: its points found on a worker, then joined with the region (Shift), taken from it (Ctrl) or replacing it.
  void finishStroke() {
    opad::Mat4 world;
    const auto prs = services().viewport()->cloudPrs(m_body, &world);
    if (!prs) return;
    std::vector<std::array<double, 2>> polygon;
    if (m_tool == 0) {
      for (const QPointF& p : m_stroke) {
        const QPointF n = services().viewport()->widgetToNdc(p);
        polygon.push_back({n.x(), n.y()});
      }
      if (polygon.size() < 3) return;
    } else if (m_tool == 1) {
      if (m_stroke.size() < 2) return;
      const QRectF r = QRectF(m_stroke[0], m_stroke[1]).normalized();
      if (r.width() < 2 || r.height() < 2) return;
      for (const QPointF& p : {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}) {
        const QPointF n = services().viewport()->widgetToNdc(p);
        polygon.push_back({n.x(), n.y()});
      }
    } else if (m_brush.empty()) {
      return;
    }
    const auto ndc = services().viewport()->worldToNdc(world);
    const opad::Mat4 local = world.inverse();
    const double radius = brushRadius();
    auto dabs = std::make_shared<std::vector<opad::Vec3>>();
    for (const auto& w : m_brush) dabs->push_back(local.apply(w));
    const int combine = m_combine;
    const opad::cloud::Region before = m_selected;
    auto result = std::make_shared<opad::cloud::Region>();
    m_busy = true;
    refreshPrompt();
    const std::string body = m_body;
    services().jobs()->async(tr("Selecting points"), [prs, polygon, ndc, dabs, radius, combine, before, result](Progress p) {
      const auto& points = *prs->cloudData;
      opad::cloud::Region r;
      if (!polygon.empty()) r = opad::cloud::in_polygon(points, ndc, polygon, [&] { return p.cancelled(); });
      for (const auto& c : *dabs) r = r.unite(opad::cloud::in_sphere(points, c, radius));
      *result = combine > 0 ? before.unite(r) : combine < 0 ? before.subtract(r) : r;
    }, [this, body, prs, result](bool ok, const QString&) {
      m_busy = false;
      if (!ok || body != m_body || !m_active) return refreshPrompt();
      m_selected = *result;
      auto indices = std::make_shared<const std::vector<uint32_t>>(m_selected.indices());
      services().viewport()->showCloudRegion(m_body, indices);
      trace::log(QStringLiteral("cloud: region of %1 points").arg(m_selected.count()));
      refreshPrompt();
    });
  }

  void crop(bool keep) {
    if (m_selected.empty()) return;
    const std::string body = m_body;
    const opad::json ref = opad::cloud::region_ref(body, m_selected);
    auto run = [this, body, ref, keep] {
      AppDocument* doc = services().document();
      const std::string name = opad::design::next_name(doc->scene, keep ? "Crop" : "DeletePoints");
      opad::json op = opad::design::make_feature_op("cloud_crop", name, {{"region", ref}, {"keep", keep ? "inside" : "outside"}});
      m_busy = true;
      refreshPrompt();
      services().design()->applyOps({op}, keep ? tr("Keep only the selected points") : tr("Delete the selected points"), [this](bool ok, const QString& error) {
        m_busy = false;
        if (!ok) services().showMessage(error, 6000);
        else {
          clearRegion();
          services().undoToast(tr("Points cropped"));
        }
        refreshPrompt();
      });
    };
    if (services().requireEditable(run)) run();
  }

  // ---------------------------------------------------------------- Slice to sketch
  void sliceToSketch() {
    const std::string body = chosenCloud();
    if (body.empty()) throw opad::UserHint("Select a slice of a point cloud first (Slice points makes them).", true);
    AppDocument* doc = services().document();
    const opad::Node* n = doc->node(body);
    const opad::Feature* made = n ? doc->scene.feature(n->source_op) : nullptr;
    auto commit = [this, body](const opad::json& plane) {
      services().design()->applyOps({}, tr("Slice to sketch"), [this](bool ok, const QString& error) {
        if (!ok) services().showMessage(error, 6000);
        else services().undoToast(tr("The slice is a sketch"));
      }, [body, plane](opad::Document& d, opad::design::Plan& plan) {
        const opad::Scene scene = opad::resolve(d);
        const opad::json op = opad::design::slice_sketch_op(d, scene, body, plane, 0.25, 0, 0, "");
        plan = opad::design::plan_ops(d, {op});
      });
    };
    if (made && made->kind == "cloud_slice") return commit(opad::json());
    // Any other cloud: sliced on a plane picked now (its points within a quarter millimetre of it).
    services().design()->pickSketchPlane([commit](opad::json plane, opad::Frame) { commit(plane); }, false, "cloud.sliceSketch");
  }

  QAction *m_region = nullptr, *m_colour = nullptr;
  QMenu* m_colourMenu = nullptr;
  std::vector<QAction*> m_modeActions;
  ToolPanel* m_panel = nullptr;
  PanelFooter* m_footer = nullptr;
  std::vector<QPushButton*> m_toolButtons;
  QWidget* m_brushRow = nullptr;
  QSlider* m_brushSize = nullptr;
  QLabel *m_brushLabel = nullptr, *m_status = nullptr;
  QPushButton *m_keep = nullptr, *m_delete = nullptr, *m_copy = nullptr;
  bool m_active = false, m_dragging = false, m_busy = false;
  int m_tool = 0, m_combine = 0;
  std::string m_body;
  QPolygonF m_stroke;
  std::vector<opad::Vec3> m_brush;
  opad::cloud::Region m_selected;
};

OPAD_AREA(Cloud)

