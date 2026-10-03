// The 2D drawing area (drawing2d): the Layers manager (UI-89) and its commands, ribbon slots, context menu entries and
// the layer walk's chip. The view side of drawings (ink, line weights and types) is in ViewportLooks.cpp; the model is
// Drawing2D.hpp.
#include <QAction>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>

#include "AreaController.hpp"
#include "Commands.hpp"
#include "Drawing2D.hpp"
#include "Icons.hpp"
#include "LayersPanel.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"
#include "ViewportChips.hpp"

OPAD_ICON_TABLE(drawing2d,
                {"layers", R"(<path d="M12 3l9 5-9 5-9-5z"/><path d="M3 12l9 5 9-5"/><path d="M3 16l9 5 9-5"/>)"},
                {"layerWalk", R"(<path d="M12 3l9 5-9 5-9-5z"/><path d="M3 12l9 5 4-2.2"/><path d="M14 19h7M18 16l3 3-3 3"/>)"},
                {"freeze", R"(<path d="M12 2v20M3.5 7l17 10M3.5 17l17-10"/><path d="M9 3.5l3 2 3-2M9 20.5l3-2 3 2"/>)"},
                {"thaw", R"(<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>)"},
                {"unlock", R"(<rect x="5" y="11" width="14" height="10"/><path d="M8 11V7a4 4 0 0 1 7.5-2"/>)"},
                {"plot", R"(<path d="M7 9V3h10v6"/><rect x="3" y="9" width="18" height="8"/><path d="M7 14h10v7H7z"/>)"},
                {"palette", R"(<path d="M12 3a9 9 0 1 0 0 18c1.1 0 1.6-.9 1-1.8-.6-1-.1-2.2 1.1-2.2H17a4 4 0 0 0 4-4c0-5.5-4-10-9-10z"/><circle cx="7.5" cy="11" r="1"/><circle cx="12" cy="7.5" r="1"/><circle cx="16.5" cy="11" r="1"/>)"},
                {"noPlot", R"(<path d="M7 9V3h10v6"/><rect x="3" y="9" width="18" height="8"/><path d="M7 14h10v7H7z"/><path d="M2 2l20 20"/>)"});

namespace {
class Drawing2DArea : public AreaController {
 public:
  using AreaController::AreaController;
  LayersPanel* layersPanel() const { return m_layers; }
  ToolPanel* layersTool() const { return m_tool; }
  QLabel* walkChip() const { return m_walkChip; }

  void buildActions() override {
    auto has = [this](const CommandContext& c) { return c.document && m_hasLayers; };
    CommandInfo layers{"drawing2d.layers", tr("Layers"), "layers"};
    layers.checkable = true;
    layers.keywords = {"layer manager", "layer properties", "freeze", "thaw", "lock", "linetype", "lineweight", "plot", "layer state"};
    layers.enabledWhen = has;
    m_layersAction = services().addCommand(layers, [this] { showLayers(m_layersAction->isChecked()); });
    CommandInfo walk{"drawing2d.layerWalk", tr("Layer walk"), "layerWalk"};
    walk.keywords = {"laywalk", "step through layers", "one layer at a time"};
    walk.enabledWhen = has;
    m_walkAction = services().addCommand(walk, [this] {
      showLayers(true);
      m_layers->walking() ? m_layers->stopWalk() : m_layers->startWalk();
    });
    CommandInfo isolate{"drawing2d.isolateLayer", tr("Isolate layer"), "isolate"};
    isolate.keywords = {"layiso", "show only this layer"};
    isolate.enabledWhen = [this](const CommandContext& c) { return c.document && m_hasLayers && !selectedLayers(c.selection).empty(); };
    m_isolateAction = services().addCommand(isolate, [this] { m_layers->isolate(selectedLayers(services().selection())); });
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* view = menus.value("view");
    if (!view) return;
    QAction* before = nullptr;  // after Exit isolate
    const QList<QAction*> entries = view->actions();
    for (int i = 0; i + 1 < entries.size(); ++i)
      if (entries[i]->objectName() == "view.unisolate") before = entries[i + 1];
    view->insertActions(before, {m_layersAction, m_walkAction, m_isolateAction});
  }

  void ribbon(RibbonLayout& layout) override {
    for (const QString tab : {"review.view", "design.view"})
      if (layout.addGroup(tab, tab + ".drawing", tr("Drawing"))) {
        layout.addAction(tab + ".drawing", m_layersAction);
        layout.addAction(tab + ".drawing", m_walkAction, RibbonLayout::Size::Small);
        layout.addAction(tab + ".drawing", m_isolateAction, RibbonLayout::Size::Small);
      }
  }

  void ready() override {
    m_layers = new LayersPanel(services());
    m_tool = new ToolPanel("layers", "layers", &Tokens::sel, tr("Layers"), m_layers, 460, services().window());
    m_tool->setDefaultWidth(460);
    m_tool->hide();
    services().addPanel(m_tool);
    connect(m_tool, &ToolPanel::visibilityChanged, this, [this](bool on) {
      if (m_layersAction->isChecked() != on) m_layersAction->setChecked(on);
      if (on) m_layers->rebuild();
      else m_layers->stopWalk();
    });
    connect(m_layers->footer(), &PanelFooter::accepted, m_tool, &QWidget::hide);
    m_walkChip = new QLabel;
    m_walkChip->setObjectName("chipSel");
    m_walkChip->hide();
    services().chips()->addChip(m_walkChip);
    connect(m_layers, &LayersPanel::walkChanged, this, [this](const QString& text) {
      m_walkChip->setText(text);
      m_walkChip->setVisible(!text.isEmpty());
    });
    // Shift+I, the isolate chip or another isolation ends the walk.
    connect(services().viewport(), &Viewport::isolationChanged, this, [this] {
      if (m_layers->walking() && !services().viewport()->isIsolated()) m_layers->stopWalk();
    });
    documentChanged(true);
  }

  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    if (selection.sketching) return;
    const auto ids = selectedLayers(selection);
    if (ids.empty()) return;
    const auto all = drawing2d::layers(services().document()->scene);
    const drawing2d::Layer* l = drawing2d::find(all, ids.front());
    if (!l) return;
    menu.addSeparator();
    QMenu* sub = menu.addMenu(icons::themed("layers", 16), tr("Layer %1").arg(QString::fromStdString(l->name)));
    sub->addAction(m_isolateAction);
    const std::string id = l->id;
    sub->addAction(icons::themed("freeze", 16), tr("Freeze layer"), this, [this, id] {
      m_layers->rebuild();
      m_layers->toggle(id, LayersPanel::Freeze);
    })->setEnabled(!l->frozen);
    sub->addAction(icons::themed(l->locked ? "unlock" : "lock", 16), l->locked ? tr("Unlock layer") : tr("Lock layer"), this, [this, id] {
      m_layers->rebuild();
      m_layers->toggle(id, LayersPanel::Lock);
    });
    sub->addAction(icons::themed("layers", 16), tr("Layer properties…"), this, [this, id] {
      showLayers(true);
      m_layers->selectLayer(id);
    });
  }

  void documentChanged(bool replaced) override {
    if (replaced && m_layers) m_layers->stopWalk();
    m_hasLayers = !drawing2d::layers(services().document()->scene).empty();
    if (m_tool && m_tool->isVisible()) {
      if (!m_hasLayers) m_tool->hide();
      else m_layers->rebuild();
    }
  }

 private:
  void showLayers(bool on) {
    if (!on) return m_tool->hide();
    services().openPanel(m_tool);
    m_layers->rebuild();
  }
  // The layers the selection lies on (a picked drawing body, its sub-shapes, or a layer row in the browser).
  std::vector<std::string> selectedLayers(const SelectionContext& selection) const {
    std::vector<std::string> out;
    const opad::Scene& scene = services().document()->scene;
    auto add = [&](const std::string& node) {
      const std::string layer = drawing2d::layerOf(scene, node);
      if (!layer.empty() && std::find(out.begin(), out.end(), layer) == out.end()) out.push_back(layer);
    };
    for (const auto& id : selection.ids) add(id);
    for (const auto& ref : selection.refs) add(ref.body);
    return out;
  }
  LayersPanel* m_layers = nullptr;
  ToolPanel* m_tool = nullptr;
  QLabel* m_walkChip = nullptr;
  QAction *m_layersAction = nullptr, *m_walkAction = nullptr, *m_isolateAction = nullptr;
  bool m_hasLayers = false;
};
}  // namespace

OPAD_AREA(Drawing2DArea)
