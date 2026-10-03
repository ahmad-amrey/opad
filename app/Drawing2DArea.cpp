// The 2D drawing area (drawing2d): the Layers manager (UI-89) and its commands, ribbon slots, context menu entries and
// the layer walk's chip; the 2D vocabulary (UI-118): in a drawing-only scene or 2D mode the Faces filter and the 3D
// display chips go, drawings are worded as objects on layers and a rollover card tells what is under the mouse. The view
// side of drawings (ink, line weights and types, hover words) is in ViewportLooks.cpp and ViewportDrawing.cpp; the model
// is Drawing2D.hpp.
#include <QAction>
#include <QCursor>
#include <QGuiApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QTimer>
#include <QToolButton>

#include <set>

#include "AreaController.hpp"
#include "BrowserDelegate.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "Drawing2D.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "LayersPanel.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
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
// What is under the mouse in a drawing, after a moment's rest (a rollover tooltip): its type, layer, colour, linetype,
// lineweight and size. A tooltip window of the main window; it never takes the focus or the mouse.
class RolloverCard : public QLabel {
 public:
  explicit RolloverCard(QWidget* owner) : QLabel(owner, Qt::ToolTip | Qt::FramelessWindowHint) {
    setObjectName("rolloverCard");
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setTextFormat(Qt::RichText);
    setMargin(8);
    restyle();
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] { restyle(); });
  }
  void restyle() {
    const Tokens& t = theme::current();
    setStyleSheet(QString("QLabel#rolloverCard { background: %1; color: %2; border: 1px solid %3; border-radius: 4px; }").arg(theme::css(t.bg2), theme::css(t.fg), theme::css(t.line)));
  }
};

class Drawing2DArea : public AreaController {
 public:
  using AreaController::AreaController;

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
    // A layer's row in the browser: the layers icon, a frozen layer's snowflake (a click opens it in the Layers panel), and
    // a mark when it is not plotted.
    services().browser()->addDecorator([this](const browser::Row& row, browser::Decoration& d) {
      if (row.kind != "component" || !row.node || !m_layerIds.count(row.id)) return;
      d.typeIcon = "layers";
      const opad::json& fields = row.node->layer;
      if (!fields.is_object()) return;
      const std::string id = row.id;
      if (!row.node->visible && fields.value("frozen", false)) {
        browser::Badge frozen;
        frozen.icon = "freeze";
        frozen.color = &Tokens::sel;
        frozen.fill = nullptr;
        frozen.tooltip = tr("Frozen layer: click to see it in the Layers panel");
        frozen.clicked = [this, id] {
          showLayers(true);
          m_layers->selectLayer(id);
        };
        d.badges << frozen;
      }
      if (!fields.value("plot", true)) {
        browser::Badge unplotted;
        unplotted.icon = "noPlot";
        unplotted.fill = nullptr;
        unplotted.tooltip = tr("Not plotted");
        d.badges << unplotted;
      }
    });
    // The 2D vocabulary follows the scene and 2D mode; the rollover card the hovered drawing entity.
    if (QAction* twoD = services().action("view.2d")) connect(twoD, &QAction::toggled, this, [this] { applyVocabulary(); });
    m_card = new RolloverCard(services().window());
    m_cardTimer.setSingleShot(true);
    m_cardTimer.setInterval(450);
    connect(&m_cardTimer, &QTimer::timeout, this, [this] { showCard(); });
    connect(services().viewport(), &Viewport::hoverInfo, this, [this](const opad::json& info) {
      m_hovered = info;
      m_card->hide();
      if (info.is_null()) m_cardTimer.stop();
      else m_cardTimer.start();
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
    m_layerIds.clear();
    for (const auto& l : drawing2d::layers(services().document()->scene)) m_layerIds.insert(l.id);
    m_hasLayers = !m_layerIds.empty();
    applyVocabulary();
    if (m_tool && m_tool->isVisible()) {
      if (!m_hasLayers) m_tool->hide();
      else m_layers->rebuild();
    }
  }

 private:
  // In a drawing-only scene or 2D mode: no Faces filter (a drawing's faces are its fills, picked as objects), no display
  // style or projection chips, and the drawing words in the hover and the status bar.
  void applyVocabulary() {
    if (!m_card) return;  // before ready()
    QAction* twoD = services().action("view.2d");
    const bool on = services().document()->hasDocument && (drawing2d::drawingOnly(services().document()->scene) || (twoD && twoD->isChecked()));
    if (!on) m_card->hide();
    if (on == m_words) return;
    m_words = on;
    services().viewport()->setDrawingWords(on);
    services().chips()->setDisplayChips(!on);
    if (QAction* faces = services().action("select.faces")) {
      faces->setVisible(!on);  // its menu entry, ribbon slot and key go with it
      for (auto* button : services().window()->findChildren<SegmentButton*>())
        if (button->defaultAction() == faces) button->setVisible(!on);  // the Select control's segment (ribbon groups follow the action)
      if (on && services().viewport()->selectionFilter() == Viewport::SelFilter::Face)
        if (QAction* edges = services().action("select.edges")) edges->trigger();
    }
  }

  void showCard() {
    if (!m_words || m_hovered.is_null() || !m_hovered.contains("body")) return;
    const opad::Scene& scene = services().document()->scene;
    const opad::Node* body = scene.node(m_hovered["body"].get<std::string>());
    if (!body) return;
    const auto all = drawing2d::layers(scene);
    const drawing2d::Layer* layer = drawing2d::find(all, drawing2d::layerOf(scene, body->id));
    auto row = [](const QString& name, const QString& value) {
      return QString("<tr><td style=\"padding-right:12px\">%1</td><td>%2</td></tr>").arg(name.toHtmlEscaped(), value);
    };
    QString rows;
    rows += row(tr("Layer"), (layer ? QString::fromStdString(layer->name) : services().document()->nodeName(body->parent)).toHtmlEscaped());
    const QColor colour = body->has_color ? QColor::fromRgbF(body->color[0], body->color[1], body->color[2]) : QColor();
    rows += row(tr("Colour"), colour.isValid() ? QString("<span style=\"color:%1\">&#9632;</span> %1").arg(colour.name()) : tr("Drawing colour").toHtmlEscaped());
    if (layer) {
      rows += row(tr("Linetype"), (layer->linetype.empty() ? tr("Continuous") : QString::fromStdString(layer->linetype)).toHtmlEscaped());
      rows += row(tr("Lineweight"), layer->lineweight < 0 ? tr("Default") : units::format(units::Kind::Length, layer->lineweight));
    }
    if (m_hovered.contains("radius")) rows += row(tr("Radius"), units::format(units::Kind::Length, m_hovered["radius"].get<double>()));
    if (m_hovered.contains("length")) rows += row(tr("Length"), units::format(units::Kind::Length, m_hovered["length"].get<double>()));
    if (m_hovered.contains("area")) rows += row(tr("Area"), units::format(units::Kind::Area, m_hovered["area"].get<double>()));
    m_card->setText(QString("<b>%1</b><table style=\"margin-top:4px\">%2</table>").arg(Viewport::drawingWord(m_hovered.value("type", "")).toHtmlEscaped(), rows));
    m_card->adjustSize();
    QPoint at = QCursor::pos() + QPoint(16, 20);
    if (const QScreen* screen = QGuiApplication::screenAt(QCursor::pos())) {  // kept on the screen
      const QRect room = screen->availableGeometry();
      at.setX(std::min(at.x(), room.right() - m_card->width()));
      if (at.y() + m_card->height() > room.bottom()) at.setY(QCursor::pos().y() - m_card->height() - 8);
    }
    m_card->move(at);
    m_card->show();
  }

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
  bool m_hasLayers = false, m_words = false;
  std::set<std::string> m_layerIds;  // for the browser's rows: asked at every paint
  RolloverCard* m_card = nullptr;
  QTimer m_cardTimer;
  opad::json m_hovered;
};
}  // namespace

OPAD_AREA(Drawing2DArea)
