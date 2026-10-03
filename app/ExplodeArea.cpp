// Exploded views (UI-36): the Explode area, see ExplodeArea.hpp.
#include "ExplodeArea.hpp"

#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <SelectMgr_Selection.hxx>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "ExplodePanel.hpp"
#include "GuidedTool.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "ViewportChips.hpp"
#include "opad/geometry.hpp"

OPAD_ICON_TABLE(explode,
                {"explodedView", R"(<rect x="3" y="11" width="9" height="9"/><rect x="14" y="3" width="7" height="7"/><path d="M12 12l2.5-2.5" stroke-dasharray="2 2"/><path d="M3 11l2-2h9v2M12 20l2-2v-4"/>)"},
                {"explodePlay", R"(<path d="M8 5l11 7-11 7z" fill="currentColor"/>)"},
                {"explodePause", R"(<path d="M8 5v14M16 5v14"/>)"},
                {"explodeCollapse", R"(<rect x="8" y="8" width="8" height="8"/><path d="M3 3l4 4M21 3l-4 4M3 21l4-4M21 21l-4-4"/>)"},
                {"explodeKeep", R"(<rect x="4" y="4" width="16" height="16"/><rect x="9" y="9" width="6" height="6" fill="currentColor"/>)"},
                {"explodeSplit", R"(<rect x="4" y="4" width="16" height="16"/><path d="M12 4v16M4 12h16"/>)"},
                {"explodeLevel", R"(<rect x="4" y="4" width="16" height="16" stroke-dasharray="3 3"/>)"},
                {"explodeGroup", R"(<rect x="3" y="3" width="18" height="18" stroke-dasharray="2 2"/><rect x="7" y="7" width="4" height="4"/><rect x="13" y="13" width="4" height="4"/>)"},
                {"explodeUngroup", R"(<rect x="3" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><path d="M13 11l-2 2" stroke-dasharray="1.5 1.5"/>)"});

namespace {
// The trail lines: one dashed segment per moving unit, in the default layer, so parts in front of a line hide it.
class TrailLines : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(TrailLines, AIS_InteractiveObject)
 public:
  std::vector<std::pair<gp_Pnt, gp_Pnt>> segments;
  Quantity_Color color;
  double width = 1;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    if (segments.empty()) return;
    Handle(Graphic3d_ArrayOfSegments) lines = new Graphic3d_ArrayOfSegments(static_cast<int>(2 * segments.size()));
    for (const auto& [a, b] : segments) {
      lines->AddVertex(a);
      lines->AddVertex(b);
    }
    const Handle(Graphic3d_Group) group = prs->NewGroup();
    group->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(color, Aspect_TOL_DASH, width));
    group->AddPrimitiveArray(lines);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}
};

Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
gp_Pnt point(const opad::Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }
bool zero(const opad::Vec3& v) { return v[0] == 0 && v[1] == 0 && v[2] == 0; }
opad::ExplodeRule nextRule(opad::ExplodeRule r) {
  return r == opad::ExplodeRule::Level ? opad::ExplodeRule::Keep : r == opad::ExplodeRule::Keep ? opad::ExplodeRule::Split : opad::ExplodeRule::Level;
}
}  // namespace

namespace {
// What a new explode starts from: every level moving with the distance at once (never a stretch of the slider per level,
// TODO 11 D4), screws and pins out along their axis.
opad::ExplodeSpec fresh() {
  opad::ExplodeSpec spec;
  spec.fasteners = true;
  return spec;
}
}  // namespace

Explode::Explode(AreaServices& services) : AreaController(services), m_spec(fresh()) {
  m_tick.setInterval(16);
  m_tick.setTimerType(Qt::PreciseTimer);
  connect(&m_tick, &QTimer::timeout, this, &Explode::tick);
  m_lines = QSettings().value("view/explodeLines", true).toBool();
  m_trails = new TrailLines;
}

Explode::~Explode() {
  if (m_job) m_job->cancel();
  if (m_measure) m_measure->cancel();
}

void Explode::buildActions() {
  CommandInfo info{"assembly.explode", tr("Exploded view"), "explodedView", QKeySequence("Shift+E")};
  info.group = tr("Assembly");
  info.keywords = {"explode", "exploded", "spread", "apart", "assembly"};
  info.enabledWhen = [](const CommandContext& c) { return c.document && !c.sketching; };
  m_explode = services().addCommand(info, [this] { open(); });
  m_explode->setProperty("shortcutHint", tr("Moves the parts of the assembly apart: the level, the distance and what moves together are set in the Explode panel."));
  shortcuts::updateTooltip(m_explode);
  info.id = "assembly.explodePlay";
  info.label = tr("Play explode");
  info.icon = "explodePlay";
  info.key = QKeySequence();
  info.keywords = {"animate", "explode", "assemble"};
  m_play = services().addCommand(info, [this] { play(); });
  m_play->setProperty("shortcutHint", tr("The parts move out, or back together when they are out (Space in the Explode panel)."));
  shortcuts::updateTooltip(m_play);
  info.id = "assembly.explodeOff";
  info.label = tr("Collapse exploded view");
  info.icon = "explodeCollapse";
  info.keywords = {"explode", "assemble", "collapse", "unexplode"};
  info.enabledWhen = [this](const CommandContext& c) { return c.document && m_on; };
  m_off = services().addCommand(info, [this] { setOn(false); });
  info.id = "assembly.explodeSave";
  info.label = tr("Save exploded view…");
  info.icon = "explodedView";
  info.keywords = {"explode", "view", "bookmark", "save"};
  m_save = services().addCommand(info, [this] {
    if (!services().requireEditable([this] { m_save->trigger(); })) return;  // a view op: a viewed file is saved as OPAD first
    bool ok = false;
    const QString name = QInputDialog::getText(services().window(), tr("Save exploded view"), tr("Name:"), QLineEdit::Normal,
                                               tr("Exploded %1").arg(services().document()->scene.views.size() + 1), &ok);
    if (ok && !name.trimmed().isEmpty()) saveView(name.trimmed());
  });
  // On the selection (the context menu, the palette): what moves together.
  auto components = [this](const SelectionContext& s) {
    std::vector<std::string> out;
    for (const auto& id : s.ids)
      if (const opad::Node* n = services().document()->scene.node(id); n && n->kind == opad::Node::Kind::Component && belowRoot(id)) out.push_back(id);
    return out;
  };
  info.id = "assembly.explodeKeep";
  info.label = tr("Keep together");
  info.icon = "explodeKeep";
  info.keywords = {"explode", "rigid", "subassembly", "one unit"};
  info.checkable = true;
  info.enabledWhen = [this, components](const CommandContext& c) { return c.document && m_on && !components(c.selection).empty(); };
  m_keep = services().addCommand(info, [this, components] {
    for (const auto& id : components(services().selection())) setRule(id, m_keep->isChecked() ? opad::ExplodeRule::Keep : opad::ExplodeRule::Level);
  });
  m_keep->setProperty("shortcutHint", tr("The component moves as one part at any level (a circuit board with its parts)."));
  shortcuts::updateTooltip(m_keep);
  info.id = "assembly.explodeSplit";
  info.label = tr("Explode its parts");
  info.icon = "explodeSplit";
  info.keywords = {"explode", "split", "spread", "parts"};
  m_split = services().addCommand(info, [this, components] {
    for (const auto& id : components(services().selection())) setRule(id, m_split->isChecked() ? opad::ExplodeRule::Split : opad::ExplodeRule::Level);
  });
  m_split->setProperty("shortcutHint", tr("The component's parts move apart whatever the level (the screws)."));
  shortcuts::updateTooltip(m_split);
  info.checkable = false;
  info.id = "assembly.explodeGroup";
  info.label = tr("Group (explode as one)");
  info.icon = "explodeGroup";
  info.keywords = {"explode", "group", "together", "one unit"};
  info.enabledWhen = [this](const CommandContext& c) {
    int nodes = 0;
    for (const auto& id : c.selection.ids) nodes += services().document()->scene.node(id) != nullptr;
    return c.document && m_on && nodes >= 2;
  };
  m_group = services().addCommand(info, [this] {
    std::vector<std::string> ids;
    for (const auto& id : services().selection().ids)
      if (services().document()->scene.node(id)) ids.push_back(id);
    group(ids);
  });
  info.id = "assembly.explodeUngroup";
  info.label = tr("Ungroup");
  info.icon = "explodeUngroup";
  info.keywords = {"explode", "ungroup", "split"};
  info.enabledWhen = [this](const CommandContext& c) {
    return c.document && m_on && std::any_of(c.selection.ids.begin(), c.selection.ids.end(), [this](const std::string& id) { return opad::explode_group_of(m_spec, id) >= 0; });
  };
  m_ungroup = services().addCommand(info, [this] {
    for (const auto& id : services().selection().ids) ungroup(id);
  });
  m_chipMenu = new QMenu(services().window());
  m_chipMenu->setObjectName("explodeChipMenu");
  m_chipMenu->addActions({m_explode, m_play, m_off, m_save});
}

void Explode::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  QMenu* view = menus.value("view");
  if (!view) return;
  const QList<QAction*> items = view->actions();
  const qsizetype at = items.indexOf(services().action("view.unisolate"));
  view->insertActions(at >= 0 && at + 1 < items.size() ? items[at + 1] : nullptr, {m_explode, m_play, m_off});
  // View > Named views: an exploded one shows its explode too (the window sets its camera).
  if (QMenu* views = view->findChild<QMenu*>("views"))
    connect(views, &QMenu::triggered, this, [this](QAction* a) {
      const std::string id = a->data().toString().toStdString();
      for (const auto& v : services().document()->scene.views)
        if (v.id == id && v.explode.is_object()) return loadView(id);
    });
}

// Design > Assemble: under Activate's small tools (the tab has no room for a group more at 1600 px); Review > View: beside
// Isolate; Design > View: a group with Play and Collapse. Save is in the panel, the chip's menu and the palette.
void Explode::ribbon(RibbonLayout& layout) {
  layout.addAction("design.assemble.components", m_explode, RibbonLayout::Size::Small);
  layout.addAction("review.view.isolate", m_explode);
  layout.addGroup("design.view", "design.view.explode", tr("Explode"));
  layout.addAction("design.view.explode", m_explode);
  layout.addAction("design.view.explode", m_play, RibbonLayout::Size::Small);
  layout.addAction("design.view.explode", m_off, RibbonLayout::Size::Small);
}

void Explode::ready() {
  AppDocument* doc = services().document();
  Viewport* view = services().viewport();
  m_form = new ExplodePanel;
  m_panel = new ToolPanel("explode", "explodedView", &Tokens::sel, tr("Explode"), m_form, 600, services().window());
  m_panel->setObjectName("explodeToolPanel");
  auto escape = [this] {  // a play stops first, then the panel closes (the explode stays: the chip says so)
    if (playing()) pause();
    else m_panel->hide();
  };
  m_panel->setEscapeHandler(escape);
  services().addPanel(m_panel);
  connect(m_panel, &ToolPanel::visibilityChanged, this, [this] {
    if (!m_panel->isVisible()) hideHint(false);
    placeHandle();
    services().browser()->refreshDecorations();
  });
  connect(m_form, &ExplodePanel::switched, this, &Explode::setOn);
  connect(m_form, &ExplodePanel::levelsChosen, this, &Explode::setLevels);
  connect(m_form, &ExplodePanel::distanceChosen, this, [this](double t) {
    hideHint(true);
    if (!m_on) setOn(true);
    m_target = -1;
    setT(t);
  });
  connect(m_form, &ExplodePanel::playRequested, this, &Explode::play);
  connect(m_form, &ExplodePanel::modeChosen, this, [this](const QString& mode, int axis) {
    opad::Vec3 a{0, 0, 1};
    if (axis < 3) {
      a = {0, 0, 0};
      a[static_cast<size_t>(axis)] = 1;
    } else if (const opad::json camera = services().viewport()->cameraJson(); camera.contains("up")) {  // the view's up, now
      a = camera["up"].get<opad::Vec3>();
    }
    edit([&](opad::ExplodeSpec& s) {
      s.mode = mode.toStdString();
      if (mode != "radial") s.axis = a;
    });
  });
  connect(m_form, &ExplodePanel::spacingChosen, this, [this](double spacing) { edit([&](opad::ExplodeSpec& s) { s.spacing = spacing; }); });
  connect(m_form, &ExplodePanel::stagesChosen, this, [this](const QString& stages) { edit([&](opad::ExplodeSpec& s) { s.stages = stages.toStdString(); }, false); });
  connect(m_form, &ExplodePanel::attachSmallToggled, this, [this](bool on) { edit([&](opad::ExplodeSpec& s) { s.attach_small = on; }); });
  connect(m_form, &ExplodePanel::fastenersToggled, this, [this](bool on) { edit([&](opad::ExplodeSpec& s) { s.fasteners = on; }); });
  connect(m_form, &ExplodePanel::linesToggled, this, [this](bool on) {
    m_lines = on;
    QSettings().setValue("view/explodeLines", on);
    apply();
  });
  connect(m_form, &ExplodePanel::dragAxisChosen, this, [this] { placeHandle(); });
  connect(m_form, &ExplodePanel::resetRequested, this, &Explode::resetDrags);
  connect(m_form, &ExplodePanel::saveRequested, this, [this] { m_save->trigger(); });  // asks to save a viewed file as OPAD first
  connect(m_form, &ExplodePanel::updateRequested, this, &Explode::updateView);
  connect(m_form, &ExplodePanel::viewChosen, this, &Explode::loadView);
  connect(m_form->footer(), &PanelFooter::cancelled, this, escape);
  // The drag handle: the selected unit's arrow along its direction (or X, Y, Z) and its travel typed or dragged.
  m_handle = new DimensionHandle(view, services().jobs());
  m_handle->setObjectName("explodeHandle");
  m_handle->drawOnTop();  // the part it moves is selected: drawn in Topmost, it would hide the arrow's foot
  m_view = view;
  qApp->installEventFilter(this);  // after the handle's: the triad and a press on the part come first (dragEvent), the chip
  connect(view, &Viewport::notesMoved, this, [this] { if (m_triadShown) placeHandle(); });  // every camera move
  connect(m_handle, &DimensionHandle::valueChanged, this, [this](const QString& text) {
    if (m_dragUnit < 0 || m_dragUnit >= static_cast<int>(m_units.size())) return;
    const std::optional<double> travel = units::parse(units::Kind::Length, text);
    if (!travel) return;
    hideHint(true);
    const opad::ExplodeUnit& u = m_units[static_cast<size_t>(m_dragUnit)];
    opad::set_explode_travel(m_spec, u, dragAxis(u), *travel);
    if (!m_handle->dragging()) opad::explode_stage(m_units, m_spec);  // typed: one after another, its turn now
    apply();
    services().browser()->refreshDecorations();
  });
  connect(m_handle, &DimensionHandle::dragFinished, this, [this] {  // dragged: its turn once let go (the part stays put meanwhile)
    opad::explode_stage(m_units, m_spec);
    apply();
  });
  m_hint = new PromptBar(view);  // the first time: what can be done besides the panel
  m_hint->setObjectName("explodeHint");
  m_hint->setAttribute(Qt::WA_NativeWindow);
  m_hint->hide();
  m_chip = new QLabel;
  m_chip->setObjectName("chipSel");
  m_chip->setCursor(Qt::PointingHandCursor);
  m_chip->hide();
  services().chips()->addChip(m_chip);
  services().browser()->addDecorator([this](const browser::Row& row, browser::Decoration& d) { decorate(row, d); });
  connect(services().design(), &DesignController::stateChanged, this, &Explode::designState);
  connect(doc, &AppDocument::activeComponentChanged, this, [this, doc] {
    if (m_on && m_rootFollows && m_viewId.empty() && m_spec.root != doc->activeComponent()) edit([&](opad::ExplodeSpec& s) { s.root = doc->activeComponent(); });
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { apply(); });
  connect(view, &Viewport::looksApplied, this, [this] { if (m_fitAfter && !playing()) fitIfOutside(); });
  refreshPanel();
}

// ---------------------------------------------------------------- state
void Explode::open() {
  services().openPanel(m_panel);
  setOn(true);
  if (!QSettings().value("hints/explode", false).toBool()) showHint();
}

void Explode::showHint() {
  m_hint->setText("explodedView", tr("Exploded view"), tr("Drag the slider or a part · the badges in the browser choose what moves together"));
  m_hint->show();
  positionOverlays({});
}

void Explode::hideHint(bool seen) {
  if (!m_hint || m_hint->isHidden()) return;
  m_hint->hide();
  if (seen) QSettings().setValue("hints/explode", true);
}

void Explode::positionOverlays(const QRect&) {
  if (!m_hint || m_hint->isHidden()) return;
  m_hint->move(std::max(8, (services().viewport()->width() - m_hint->width()) / 2), 44);  // where a guided tool's prompt goes
  m_hint->raise();
}

void Explode::setOn(bool on) {
  if (on) {
    m_offAfter = false;
    if (!m_on) {
      m_on = true;
      if (m_rootFollows && m_viewId.empty()) m_spec.root = services().document()->activeComponent();
      m_t = 0;
      m_target = m_spec.t > 0 ? m_spec.t : 1;  // out once laid out
      layout();
    }
  } else if (m_on && !m_offAfter) {
    m_target = -1;
    m_resume = -1;
    m_offAfter = true;
    playTo(0, m_units.empty() ? 0 : 0.5 * m_spec.duration * m_t);  // back together, then off
  }
  refreshPanel();
  services().updateCommands();
}

void Explode::setT(double t) {
  m_tick.stop();
  m_form->showPlaying(false);
  m_offAfter = m_fitAfter = false;  // a collapse under way stays on where it is put
  m_t = std::clamp(t, 0.0, 1.0);
  apply();
}

void Explode::pause() {
  if (m_offAfter) return playTo(0, 0);  // a collapse: done at once
  m_tick.stop();
  m_form->showPlaying(false);
  m_fitAfter = false;
}

void Explode::playTo(double t, double seconds, bool frame) {
  m_fitAfter = frame;
  m_from = m_t;
  m_to = std::clamp(t, 0.0, 1.0);
  m_seconds = seconds >= 0 ? seconds : m_spec.duration * std::abs(m_to - m_from);
  m_clock.start();
  if (m_seconds <= 1e-3 || m_units.empty()) {
    m_seconds = 0;
    return tick();  // there at once
  }
  m_tick.start();
  m_form->showPlaying(true);
}

void Explode::play() {
  if (!m_on) return open();
  if (std::exchange(m_offAfter, false)) return playTo(1);  // collapsing: out again
  if (playing()) return pause();
  playTo(m_t < 0.999 ? 1 : 0);
}

void Explode::tick() {
  const double f = m_seconds > 0 ? std::min(1.0, m_clock.elapsed() / 1000.0 / m_seconds) : 1.0;
  m_t = m_from + (m_to - m_from) * f;
  if (f >= 1) {
    m_tick.stop();
    m_form->showPlaying(false);
    m_fitAfter = m_fitAfter && m_t > 0;
  }
  if (f >= 1 && m_offAfter) {  // collapsed: the view lets go of the parts
    m_offAfter = false;
    m_on = false;
    m_t = 0;
    m_dragUnit = -1;
    m_units.clear();
    m_bodyUnits.clear();
    m_sketchUnits.clear();
    if (m_job) m_job->cancel();
    m_job = nullptr;
    m_relayout = false;
    ++m_serial;
    refreshPanel();
    services().browser()->refreshDecorations();
    services().updateCommands();
  }
  apply();
  if (f >= 1 && m_fitAfter && !services().viewport()->looksPending()) fitIfOutside();  // else when the looks are applied
}

void Explode::fitIfOutside() {
  m_fitAfter = false;
  Viewport* view = services().viewport();
  if (!view->showsAll()) view->animateFitAll();
}

void Explode::setLevels(int levels) {
  edit([&](opad::ExplodeSpec& s) { s.levels = std::max(0, levels); });
}

void Explode::setRule(const std::string& component, opad::ExplodeRule rule) {
  hideHint(true);
  edit([&](opad::ExplodeSpec& s) { opad::set_explode_rule(s, component, rule); });
}

void Explode::group(const std::vector<std::string>& ids) {
  edit([&](opad::ExplodeSpec& s) { opad::explode_group(s, ids); });
}

void Explode::ungroup(const std::string& id) {
  edit([&](opad::ExplodeSpec& s) { opad::explode_ungroup(s, id); });
}

void Explode::resetDrags() {
  edit([](opad::ExplodeSpec& s) { s.offsets.clear(); }, false);
}

void Explode::edit(const std::function<void(opad::ExplodeSpec&)>& change, bool relayout) {
  change(m_spec);
  if (!m_on) setOn(true);
  else if (relayout) layout();
  else {
    opad::explode_stage(m_units, m_spec);  // drags reset: one after another, the order without them
    apply();
  }
  refreshPanel();
  services().browser()->refreshDecorations();
  services().updateCommands();
}

// ---------------------------------------------------------------- views (view ops with the explode object)
std::string Explode::saveView(const QString& name) {
  AppDocument* doc = services().document();
  if (!m_on || doc->browse) return {};
  opad::ExplodeSpec spec = m_spec;
  spec.t = m_t > 0 ? m_t : 1;  // a view saved collapsed opens exploded
  std::string id;
  services().guarded([&] {
    id = doc->run("view", {{"name", name.toStdString()}, {"camera", services().viewport()->cameraJson()}, {"explode", spec.to_json()}}).value("id", "");
  });
  if (id.empty()) return id;
  m_viewId = id;
  refreshPanel();
  services().toast(tr("Saved the exploded view %1").arg(name));
  return id;
}

void Explode::updateView() {
  AppDocument* doc = services().document();
  if (m_viewId.empty() || !m_on || !services().requireEditable([this] { updateView(); })) return;
  opad::ExplodeSpec spec = m_spec;
  spec.t = m_t > 0 ? m_t : 1;
  bool done = false;
  services().guarded([&] {
    doc->run("append", {{"op", {{"op", "edit"}, {"target", m_viewId}, {"set", {{"explode", spec.to_json()}}}}}});
    done = true;
  });
  if (!done) return;
  QString name;
  for (const auto& v : doc->scene.views)
    if (v.id == m_viewId) name = QString::fromStdString(v.name);
  services().toast(tr("Updated the view %1").arg(name));
}

void Explode::loadView(const std::string& id) {
  const opad::Scene& scene = services().document()->scene;
  const auto view = std::find_if(scene.views.begin(), scene.views.end(), [&](const opad::ViewBookmark& v) { return v.id == id; });
  if (view == scene.views.end()) return;
  try {
    m_spec = opad::view_explode(scene, id);
  } catch (const std::exception& e) {
    services().showMessage(QString::fromUtf8(e.what()), 6000);
    return;
  }
  m_viewId = id;
  m_rootFollows = false;
  m_offAfter = false;
  if (view->camera.is_object()) services().viewport()->setCameraJson(view->camera);
  m_target = m_spec.t > 0 ? m_spec.t : 1;
  if (!m_on) {
    m_on = true;
    m_t = 0;
  }
  layout();
  refreshPanel();
  services().browser()->refreshDecorations();
  services().updateCommands();
}

// ---------------------------------------------------------------- layout and view
int Explode::unitOf(const std::string& id) const { return m_units.empty() ? -1 : opad::explode_unit_of(services().document()->scene, m_units, id, &m_bodyUnits); }

void Explode::layout() {
  if (!m_on) return;
  if (m_job) {  // one at a time: the latest spec and document when it ends
    m_relayout = true;
    return;
  }
  m_relayout = false;
  // The units on a worker from a copy of the tree, from the parts' tight boxes as `opad-cli explode`, renders and
  // drawings lay them out (the same places for a saved view), once those are measured (cached per shape; a worker
  // measures what is missing, about 20 s once for the Engine). Meanwhile from the boxes the view holds (cached world
  // boxes, refined to the meshes it draws: O(1) per body here), and again when the measuring ends.
  const AppDocument* doc = services().document();
  auto scene = std::make_shared<opad::Scene>();
  scene->nodes = doc->scene.nodes;
  scene->roots = doc->scene.roots;
  auto boxes = std::make_shared<std::unordered_map<std::string, Bnd_Box>>();
  const std::string root = opad::explode_root(doc->scene, m_spec);
  std::vector<std::string> bodies, keys;
  for (const auto& id : root.empty() ? doc->scene.all_bodies() : doc->scene.bodies_under(root)) {
    const opad::Node* n = doc->scene.node(id);
    if (!n || n->body_missing || !doc->scene.effectively_visible(id)) continue;
    bodies.push_back(id);
    keys.push_back(n->body_key);
  }
  const std::vector<std::string> missing = opad::missing_tight_bboxes(doc->doc, keys);
  const bool exact = missing.empty();
  if (!exact) {
    for (const auto& id : bodies) try {
        (*boxes)[id] = opad::node_world_bbox(doc->doc, doc->scene, id);
      } catch (const std::exception&) {
      }
    measureBoxes(missing);
  }
  auto measured = std::make_shared<opad::Document>();  // the shapes and their boxes, cached
  measured->shape_cache = doc->doc.shape_cache;
  const opad::ExplodeSpec spec = m_spec;
  const int serial = ++m_serial;
  struct Out {
    std::vector<opad::ExplodeUnit> units;
    int depth = 1;
    std::string root;
  };
  auto out = std::make_shared<Out>();
  m_job = services().jobs()->async(tr("Laying out the exploded view"), [scene, boxes, spec, out, measured, exact, axes = m_axes](Progress) {
    const opad::ExplodeAxisFn axis = opad::fastener_axes(*measured, *scene, axes);  // each shape once per document
    out->units = exact ? opad::explode_units(*measured, *scene, spec, {}, axis)
                       : opad::explode_units(*measured, *scene, spec, [boxes](const std::string& id) {
                           const auto it = boxes->find(id);
                           return it == boxes->end() ? Bnd_Box() : it->second;
                         }, axis);
    out->depth = opad::explode_depth(*scene, spec);
    out->root = opad::explode_root(*scene, spec);
  }, [this, serial, out, exact](bool ok, const QString& error) {
    if (serial != m_serial) return;
    m_job = nullptr;
    if (m_relayout) return layout();  // the spec or the document changed meanwhile
    if (!ok) return services().showMessage(error, 6000);
    m_units = std::move(out->units);
    m_exact = exact;
    m_bodyUnits = opad::explode_body_units(m_units);
    m_depth = std::max(1, out->depth);
    m_root = out->root;
    m_sketchUnits.clear();
    for (const auto& sketch : services().document()->scene.sketches)
      if (!sketch.component.empty())
        if (const int u = unitOf(sketch.component); u >= 0) m_sketchUnits.push_back({sketch.id, u});
    if (m_dragUnit >= static_cast<int>(m_units.size())) m_dragUnit = -1;
    selectionChanged(services().selection());  // the selected part's unit may be another one now
    apply();
    refreshPanel();
    services().browser()->refreshDecorations();
    emit laidOut();
    if (m_target >= 0) playTo(std::exchange(m_target, -1.0), -1, true);
  });
}

void Explode::measureBoxes(const std::vector<std::string>& keys) {
  if (m_measure || m_measureRefused) return;
  auto measured = std::make_shared<opad::Document>();
  measured->shape_cache = services().document()->doc.shape_cache;
  m_measure = services().jobs()->async(tr("Measuring the parts for the exploded view"), [measured, keys](Progress p) {
    opad::warm_tight_bboxes(*measured, keys, [p] { return p.cancelled(); });
  }, [this](bool ok, const QString&) {
    m_measure = nullptr;
    if (!ok) m_measureRefused = true;  // cancelled: the view's boxes for this document
    else if (m_on) layout();
  });
}

void Explode::apply() {
  Viewport* view = services().viewport();
  std::vector<opad::Vec3> moves;
  if (m_on && !m_units.empty()) moves = opad::explode_unit_offsets(m_units, m_spec, m_t);
  std::map<std::string, LookDelta> layer;
  for (size_t i = 0; i < moves.size(); ++i) {
    if (zero(moves[i])) continue;
    for (const auto& b : m_units[i].bodies) layer[b].offset = {moves[i][0], moves[i][1], moves[i][2]};
  }
  for (const auto& [sketch, u] : m_sketchUnits)  // a component's sketches go with it
    if (static_cast<size_t>(u) < moves.size() && !zero(moves[static_cast<size_t>(u)])) layer[sketch].offset = {moves[static_cast<size_t>(u)][0], moves[static_cast<size_t>(u)][1], moves[static_cast<size_t>(u)][2]};
  view->setLookLayer(LookSource::Explode, std::move(layer));
  showTrails(moves);
  placeHandle();
  refreshChip();
  m_form->showDistance(m_t);
  emit moved();
}

void Explode::showTrails(const std::vector<opad::Vec3>& moves) {
  Viewport* view = services().viewport();
  std::vector<opad::ExplodeTrail> trails;
  if (m_lines && !moves.empty()) trails = opad::explode_trails(m_units, m_spec, m_t);
  m_trailCount = trails.size();
  if (trails.empty()) {
    if (m_trailsShown) view->removeOverlay(m_trails);
    m_trailsShown = false;
    return;
  }
  auto* lines = static_cast<TrailLines*>(m_trails.get());
  lines->segments.clear();
  for (const auto& trail : trails) lines->segments.push_back({point(trail.from), point(trail.to)});
  lines->color = occ(view->tokens().fg2);
  lines->width = view->displayScale();
  lines->SetToUpdate();
  if (!m_trailsShown) {
    view->showOverlay(m_trails);  // never picked; then out of Topmost: parts in front hide the lines
    m_trails->SetZLayer(Graphic3d_ZLayerId_Default);
    m_trailsShown = true;
  }
  view->updateOverlay(m_trails);
}

opad::Vec3 Explode::dragAxis(const opad::ExplodeUnit& unit) const {
  const int axis = m_form->dragAxis();
  if (axis == 0) return unit.dir;
  opad::Vec3 a{0, 0, 0};
  a[static_cast<size_t>(axis - 1)] = 1;
  return a;
}

// The handle sits where the unit is drawn; its value is the unit's own move at full distance along the axis, so
// dragging it moves the part as far as the distance shows (setScale: the part's progress at t).
void Explode::placeHandle() {
  if (!m_handle) return;
  DesignController* design = services().design();
  const bool shown = m_on && m_panel->isVisible() && m_dragUnit >= 0 && m_dragUnit < static_cast<int>(m_units.size()) && !design->sketchActive() &&
                     !design->featureActive() && !services().viewport()->ghostsPickable();
  const opad::ExplodeUnit* u = shown ? &m_units[static_cast<size_t>(m_dragUnit)] : nullptr;
  const double s = u ? opad::explode_progress(*u, m_t) : 0;
  if (!u || s < 0.2) {  // nothing to drag, or the part has hardly started to move
    if (m_handle->isVisible() && !m_handle->interacting()) m_handle->hide();
    placeTriad(false, {}, {});
    return;
  }
  const std::vector<opad::Vec3> moves = opad::explode_unit_offsets(m_units, m_spec, m_t);
  const opad::Vec3 base = u->parent >= 0 ? moves[static_cast<size_t>(u->parent)] : opad::Vec3{0, 0, 0};
  const opad::Vec3 axis = dragAxis(*u);
  const double value = opad::explode_travel(*u, m_spec, axis);
  opad::Vec3 own{u->dir[0] * u->distance, u->dir[1] * u->distance, u->dir[2] * u->distance};
  if (const auto m = m_spec.offsets.find(u->id); m != m_spec.offsets.end())
    for (size_t k = 0; k < 3; ++k) own[k] += m->second[k];
  const double len = std::hypot(axis[0], axis[1], axis[2]);
  opad::Vec3 origin, unitAxis;
  for (size_t k = 0; k < 3; ++k) {
    unitAxis[k] = len > 0 ? axis[k] / len : (k == 2 ? 1 : 0);
    origin[k] = u->centre[k] + base[k] + (own[k] - unitAxis[k] * value) * s;
  }
  m_handle->setScale(s);
  m_handle->setLabel(QString::fromStdString(u->name));
  m_handle->configure(origin, unitAxis, value, units::editable(units::Kind::Length, value));
  const opad::Vec3& moved = moves[static_cast<size_t>(m_dragUnit)];
  placeTriad(true, {u->centre[0] + moved[0], u->centre[1] + moved[1], u->centre[2] + moved[2]}, unitAxis);
}

std::vector<std::string> Explode::members(int unit) const {
  const opad::ExplodeUnit& u = m_units[static_cast<size_t>(unit)];
  if (const int g = opad::explode_group_of(m_spec, u.id); g >= 0) return m_spec.groups[static_cast<size_t>(g)];
  const opad::Node* n = services().document()->scene.node(u.id);
  return n && n->kind == opad::Node::Kind::Component ? std::vector<std::string>{u.id} : u.bodies;
}

bool Explode::belowRoot(const std::string& id) const {
  const opad::Scene& scene = services().document()->scene;
  const opad::Node* n = scene.node(id);
  if (!n || id == m_root) return false;
  for (const opad::Node* at = n; at; at = at->parent.empty() ? nullptr : scene.node(at->parent))
    if (at->parent == m_root) return true;
  return false;
}

// ---------------------------------------------------------------- panel, chip, browser
void Explode::refreshPanel() {
  if (!m_form) return;
  const AppDocument* doc = services().document();
  m_form->showSpec(m_spec, m_depth, m_on, m_lines);
  m_form->showDistance(m_t);
  std::vector<std::pair<std::string, QString>> views;
  for (const auto& v : doc->scene.views)
    if (v.explode.is_object()) views.push_back({v.id, QString::fromStdString(v.name)});
  if (!m_viewId.empty() && std::none_of(views.begin(), views.end(), [this](const auto& v) { return v.first == m_viewId; })) m_viewId.clear();  // undone
  m_form->showViews(views, m_viewId);
  int moving = 0;
  for (const auto& u : m_units) moving += u.distance > 0 || m_spec.offsets.count(u.id);
  const QString level = m_spec.levels == 0 ? tr("every level") : tr("level %1 of %2").arg(m_spec.levels).arg(m_depth);
  m_form->showStatus(!m_on ? tr("Off: the parts are where the model has them.")
                     : m_units.empty() ? tr("Laying out the parts…")
                                       : tr("%1 of %2 parts move apart · %3").arg(moving).arg(m_units.size()).arg(level));
  const std::string root = m_on ? m_root : m_spec.root;
  m_panel->setContext(root.empty() ? doc->title() : doc->nodeName(root));
  m_form->footer()->setPrimaryEnabled(m_on);
}

void Explode::refreshChip() {
  if (!m_chip) return;
  m_chip->setVisible(m_on);
  if (!m_on) return;
  const int percent = static_cast<int>(std::lround(m_t * 100));
  const QString level = m_spec.levels == 0 ? tr("all levels") : tr("level %1").arg(m_spec.levels);
  const QString text = tr("Exploded · %1 · %2 %").arg(level).arg(percent);
  if (m_chip->text() == text) return;
  m_chip->setText(text);
  m_chip->setToolTip(tr("The view is exploded. Click for the Explode panel; right-click to play, collapse or save it as a view."));
}

void Explode::decorate(const browser::Row& row, browser::Decoration& d) {
  if (!m_on || !m_panel->isVisible() || !row.node || m_units.empty()) return;
  if (row.kind == "component" && belowRoot(row.id)) {
    const opad::ExplodeRule rule = opad::explode_rule(m_spec, row.id);
    browser::Badge b;
    b.icon = rule == opad::ExplodeRule::Keep ? "explodeKeep" : rule == opad::ExplodeRule::Split ? "explodeSplit" : "explodeLevel";
    b.color = rule == opad::ExplodeRule::Level ? &Tokens::fg3 : &Tokens::sel;
    b.fill = nullptr;
    b.tooltip = rule == opad::ExplodeRule::Keep    ? tr("Kept together: moves as one part at any level · click: explode its parts")
                : rule == opad::ExplodeRule::Split ? tr("Its parts explode whatever the level · click: follow the level")
                                                   : tr("Follows the level setting · click: keep together");
    b.clicked = [this, id = row.id, rule] { setRule(id, nextRule(rule)); };
    d.badges.push_back(b);
  }
  if (const int g = opad::explode_group_of(m_spec, row.id); g >= 0) {
    QStringList others;
    for (const auto& id : m_spec.groups[static_cast<size_t>(g)])
      if (id != row.id) others << services().document()->nodeName(id);
    browser::Badge b;
    b.icon = "explodeGroup";
    b.color = &Tokens::sel;
    b.fill = nullptr;
    b.tooltip = tr("Moves as one with %1 (explode group) · click to ungroup").arg(others.join(", "));
    b.clicked = [this, id = row.id] { ungroup(id); };
    d.badges.push_back(b);
  }
}

// ---------------------------------------------------------------- hooks
void Explode::contextMenu(const SelectionContext& selection, QMenu& menu) {
  if (selection.sketching || !m_on || selection.ids.empty()) return;
  const opad::Scene& scene = services().document()->scene;
  std::vector<std::string> components;
  int nodes = 0;
  bool grouped = false;
  for (const auto& id : selection.ids) {
    const opad::Node* n = scene.node(id);
    if (!n) continue;
    ++nodes;
    grouped = grouped || opad::explode_group_of(m_spec, id) >= 0;
    if (n->kind == opad::Node::Kind::Component && belowRoot(id)) components.push_back(id);
  }
  const bool any = !components.empty() || nodes >= 2 || grouped || (m_dragUnit >= 0 && m_spec.offsets.count(m_units[static_cast<size_t>(m_dragUnit)].id));
  if (!any) return;
  menu.addSeparator();
  if (!components.empty()) {
    auto all = [&](opad::ExplodeRule rule) { return std::all_of(components.begin(), components.end(), [&](const std::string& id) { return opad::explode_rule(m_spec, id) == rule; }); };
    m_keep->setChecked(all(opad::ExplodeRule::Keep));
    m_split->setChecked(all(opad::ExplodeRule::Split));
    menu.addAction(m_keep);
    menu.addAction(m_split);
  }
  if (nodes >= 2) menu.addAction(m_group);
  if (grouped) menu.addAction(m_ungroup);
  if (m_dragUnit >= 0 && m_spec.offsets.count(m_units[static_cast<size_t>(m_dragUnit)].id)) {
    QAction* a = menu.addAction(icons::themed("restore", 16), tr("Reset its drag"));
    a->setObjectName("assembly.explodeResetOne");
    connect(a, &QAction::triggered, this, [this, id = m_units[static_cast<size_t>(m_dragUnit)].id] { edit([&](opad::ExplodeSpec& s) { s.offsets.erase(id); }, false); });
  }
}

void Explode::selectionChanged(const SelectionContext& selection) {
  m_dragUnit = -1;
  if (m_on && !m_units.empty() && !selection.sketching && !selection.ids.empty()) {
    int unit = -1;
    for (const auto& id : selection.ids) {
      const int u = unitOf(id);
      if (u < 0 || (unit >= 0 && u != unit)) {
        unit = -1;
        break;
      }
      unit = u;
    }
    m_dragUnit = unit;
    // A click on a part of a larger unit selects the whole unit while the panel is open (its rows light up in the browser);
    // not while picks accumulate (a guided tool) or faces, edges, vertices are picked.
    const bool clicked = unit >= 0 && m_panel->isVisible() && !services().viewport()->ghostsPickable() && selection.refs.size() == 1 &&
                         selection.refs.front().kind == opad::Ref::Kind::Body;
    if (clicked)
      if (std::vector<std::string> ids = members(unit); ids != selection.ids)
        QTimer::singleShot(0, this, [this, ids] { services().browser()->selectIds(ids); });
  }
  placeHandle();
}

void Explode::documentChanged(bool replaced) {
  if (replaced) {  // another document: nothing of this one's explode carries over
    m_tick.stop();
    if (m_job) m_job->cancel();
    m_job = nullptr;
    if (m_measure) m_measure->cancel();
    m_measure = nullptr;
    m_measureRefused = m_exact = false;
    m_axes = std::make_shared<opad::FastenerAxes>();
    ++m_serial;
    m_on = m_offAfter = m_relayout = false;
    m_rootFollows = true;
    m_spec = fresh();
    m_t = 0;
    m_target = m_resume = -1;
    m_units.clear();
    m_bodyUnits.clear();
    m_sketchUnits.clear();
    m_viewId.clear();
    m_dragUnit = -1;
    m_root.clear();
    apply();
    refreshPanel();
    return;
  }
  if (m_on) layout();  // bodies added, moved, hidden or gone
  refreshPanel();
}

void Explode::designState() {
  DesignController* design = services().design();
  const bool editing = design->sketchActive() || design->featureActive();
  if (editing && m_on && m_t > 0 && m_resume < 0) {
    m_resume = m_t;
    m_target = -1;
    playTo(0, 0.3);
  } else if (!editing && m_resume >= 0) {
    const double t = std::exchange(m_resume, -1.0);
    if (m_on) playTo(t, 0.3);
  }
  placeHandle();
}

bool Explode::eventFilter(QObject* object, QEvent* event) {
  if (object == m_view && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease))
    return dragEvent(event);
  if (object == m_chip && event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
    open();
    return true;
  }
  if (object == m_chip && event->type() == QEvent::ContextMenu) {
    m_chipMenu->exec(static_cast<QContextMenuEvent*>(event)->globalPos());
    return true;
  }
  return AreaController::eventFilter(object, event);
}

OPAD_AREA(Explode)
