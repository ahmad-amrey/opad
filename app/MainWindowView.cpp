// The view: view and navigation commands, the central viewport and its overlays, theme, named views.
#include "MainWindow.hpp"

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  #include <QAccessibilityHints>
#endif
#include <QActionGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QToolButton>
#include <QWindowStateChangeEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "Drawing2D.hpp"
#include "DrawingPlacer.hpp"
#include "I18n.hpp"
#include "PlanePicker.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "Theme.hpp"
#include "Units.hpp"

void MainWindow::buildViewActions() {
  // The selection, else the active component (UI-33), else everything; animated on screen (UI-47).
  addAction("view.fit", tr("Fit"), "fit", QKeySequence("F"), [this] {
    if (m_design && m_design->sketchActive()) m_design->sketch()->fitSketch();
    else if (!m_doc->activeComponent().empty() && m_viewport->selection().empty()) m_viewport->fitNodes({m_doc->activeComponent()}, true);
    else m_viewport->fitSelection(true);
  });
  addAction("view.fitall", tr("Fit all"), "fit", QKeySequence("Shift+F"), [this] { if(m_design&&m_design->sketchActive())m_design->sketch()->fitSketch();else m_viewport->fitAll(true); });
  addAction("view.home", tr("Home"), "home", QKeySequence("H"), [this] { m_viewport->home(true); });
  addAction("view.alignPlane",tr("Align view to plane"),"plane",QKeySequence("Shift+A"),[this] {
    if(m_design->sketchActive()) {m_viewport->lookAt(m_design->sketch()->frame(),false,true);return;}
    cancelTool();
    m_design->pickSketchPlane([this](opad::json,opad::Frame frame) { m_viewport->lookAt(frame,true,true); },false,"view.alignPlane");
  });
  addAction("view.rollleft", tr("Turn 90° left"), "rollLeft", QKeySequence("Alt+Left"), [this] { m_viewport->rollView(90); });
  addAction("view.rollright", tr("Turn 90° right"), "rollRight", QKeySequence("Alt+Right"), [this] { m_viewport->rollView(-90); });
  for (const auto& [name, key, label] : std::vector<std::tuple<QString, QString, QString>>{
           {"top", "Shift+Up", tr("Top view")}, {"front", "Shift+PgUp", tr("Front view")}, {"right", "Shift+Right", tr("Right view")}, {"iso", "Shift+H", tr("Isometric view")},
           {"bottom", "Shift+Down", tr("Bottom view")}, {"back", "Shift+PgDown", tr("Back view")}, {"left", "Shift+Left", tr("Left view")}})
    addAction("view." + name, label, "home", QKeySequence(key), [this, n = name] { m_viewport->standardView(n, true); });
  auto* flat = addAction("view.2d",tr("2D mode"),"drawing",QKeySequence("Shift+2"),[this]{},true);
  flat->setObjectName("view.2d");
  flat->setCheckable(true);
  connect(flat, &QAction::toggled, this, [this](bool on) {
    if (!m_settingTwoD) m_autoTwoD = false;  // set by hand: stays as the user left it until the document is replaced
    m_viewport->setTwoDimensional(on);

    if (m_browserOverlay && action("panel.browser")->isChecked()) { m_browserOverlay->setVisible(m_doc->hasDocument); m_browserOverlay->raise(); }
    m_homeBtn->setVisible(!on); m_alignPlane->setVisible(!on);  // the turn buttons stay: in 2D they twist the view (UI-47)
    action("view.iso")->setEnabled(!on && m_doc->hasDocument);  // a corner view has no plane to lock to
    updateChips();
  });
  QAction* ortho = addAction("view.ortho", tr("Orthographic"), "ortho", QKeySequence("Shift+3"), [this] {}, true);
  ortho->setChecked(true);
  connect(ortho, &QAction::toggled, this, [this](bool on) { m_viewport->setOrthographic(on); m_settings.setValue("view/orthographic",on); updateChips(); });
  QAction* shaded = addAction("view.shaded", tr("Shaded"), "shaded", QKeySequence("5"), [this] {}, true);
  QAction* edges = addAction("view.edges", tr("Shaded + edges"), "shadedEdges", QKeySequence("6"), [this] {}, true);
  QAction* wire = addAction("view.wire", tr("Wireframe"), "wireframe", QKeySequence("7"), [this] {}, true);
  QAction* hidden = addAction("view.hidden", tr("Hidden line"), "hiddenLine", QKeySequence("8"), [this] {}, true);  // UI-48
  QAction* dashed = addAction("view.hiddenEdges", tr("Hidden edges visible"), "hiddenEdges", QKeySequence("9"), [this] {}, true);
  auto* styleGroup = new QActionGroup(this);
  for (QAction* a : {shaded, edges, wire, hidden, dashed}) styleGroup->addAction(a);
  edges->setChecked(true);
  connect(styleGroup, &QActionGroup::triggered, this, [this, shaded, wire, hidden, dashed](QAction* a) {
    m_settings.setValue("view/style",a->objectName());
    m_viewport->setStyle(a == shaded ? Viewport::Style::Shaded : a == wire ? Viewport::Style::Wireframe : a == hidden ? Viewport::Style::HiddenLine
                         : a == dashed ? Viewport::Style::HiddenEdges : Viewport::Style::ShadedEdges);
    updateChips();
  });
  QAction* grid = addAction("view.grid", tr("Grid"), "grid", QKeySequence("G"), [this] {}, true);
  connect(grid, &QAction::toggled, this, [this](bool on) { m_viewport->setGrid(on); m_settings.setValue(m_viewport->sketching()?"sketch/grid":"view/grid",on); });
  addAction("view.gridSettings",tr("Grid settings"),"grid",QKeySequence("Shift+G"),[this] {
    auto* dialog=new QDialog(this,Qt::Tool);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("Grid settings"));
    auto* form=new QFormLayout(dialog);auto* spacing=new QDoubleSpinBox(dialog);spacing->setRange(0,units::toDisplay(units::Kind::Length,100000));spacing->setDecimals(units::decimalsFor(0.001));spacing->setSpecialValueText(tr("Automatic"));
    auto* extent=new QDoubleSpinBox(dialog);extent->setRange(units::toDisplay(units::Kind::Length,1),units::toDisplay(units::Kind::Length,1000000));extent->setDecimals(units::decimalsFor(1));
    for(auto* box:{spacing,extent})box->setSuffix(' '+units::symbol(units::Kind::Length));  // mm in the settings, the shown unit here
    spacing->setValue(units::toDisplay(units::Kind::Length,m_settings.value("view/gridSpacing",0).toDouble()));extent->setValue(units::toDisplay(units::Kind::Length,m_settings.value("view/gridExtent",100).toDouble()));
    auto* automatic=new QCheckBox(tr("Automatic spacing"),dialog);automatic->setChecked(spacing->value()==0);spacing->setEnabled(!automatic->isChecked());
    form->addRow(automatic);form->addRow(tr("Spacing"),spacing);form->addRow(tr("Minimum extent"),extent);
    auto changed=[this,spacing,extent,automatic]{m_viewport->configureGrid(automatic->isChecked()?0:units::fromDisplay(units::Kind::Length,spacing->value()),units::fromDisplay(units::Kind::Length,extent->value()));};
    connect(automatic,&QCheckBox::toggled,dialog,[=](bool on){spacing->setEnabled(!on);if(!on && spacing->value()==0)spacing->setValue(units::toDisplay(units::Kind::Length,1));changed();});
    connect(spacing,&QDoubleSpinBox::valueChanged,dialog,changed);connect(extent,&QDoubleSpinBox::valueChanged,dialog,changed);
    action("view.grid")->setChecked(true);dialog->show();
  });
  auto* through=addAction("select.through",tr("Select through objects"),"wireframe",QKeySequence("Alt+X"),[this]{},true);
  through->setChecked(m_settings.value("view/selectThrough",false).toBool());
  connect(through,&QAction::toggled,this,[this](bool on){m_settings.setValue("view/selectThrough",on);m_viewport->setSelectThrough(on);});
  CommandInfo cubeInfo;  // off: faces only (UI-54)
  cubeInfo.id="view.cubeEdgesCorners";cubeInfo.label=tr("View cube edges and corners turn the view");cubeInfo.checkable=true;
  cubeInfo.keywords={"view cube","navigation cube","corner","edge","faces only"};
  auto* cubeParts=addCommand(cubeInfo,[]{});
  cubeParts->setChecked(m_settings.value("view/cubeEdgesCorners",true).toBool());
  connect(cubeParts,&QAction::toggled,this,[this](bool on){m_settings.setValue("view/cubeEdgesCorners",on);m_viewport->setCubeEdgesCorners(on);});
  addAction("view.isolate", tr("Isolate"), "isolate", QKeySequence("I"), [this] { m_viewport->isolate(currentNodeIds()); });
  addAction("view.hideothers", tr("Hide others"), "hide", QKeySequence(), [this] { hideOthers(currentNodeIds()); });  // one step (UI-02)
  // macOS treats any action starting with "Exit" as Quit unless its menu role is explicit.
  addAction("view.unisolate", tr("Exit isolate"), "showAll", QKeySequence("Shift+I"), [this] { m_viewport->isolate({}); })->setMenuRole(QAction::NoRole);
  addAction("view.saveview", tr("Save view…"), "home", QKeySequence("Shift+Alt+V"), [this] { saveNamedView(); });
  // The Named views list's first nine by key, in its order (the list shows each one's key).
  for (int n = 1; n <= 9; ++n) {
    const QString id = QString("view.named%1").arg(n);
    addAction(id, tr("Named view %1").arg(n), "home", QKeySequence(QString("Shift+Alt+%1").arg(n)), [this, n] { recallNamedView(n); });
  }
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] { rebuildViewsMenu(); });  // the keys it lists, rebound
  m_darkAction = addAction("view.dark", tr("&Dark theme"), "", QKeySequence(), [this] {}, true);
  // Panel toggles: always enabled, so a closed dock can be reopened even with no document.
  // Ctrl+1/2/3 belong to the workspaces (handoff), so the panels use Alt.
  addAction("panel.browser", tr("Browser"), "browse", QKeySequence("Alt+1"), [this] {}, true);
  addAction("panel.annotations", tr("Annotations"), "annotate", QKeySequence("Alt+2"), [this] {}, true);
  addAction("panel.section", tr("Section panel"), "section", QKeySequence(), [this] {}, true);
  addAction("panel.timeline", tr("Timeline"), "commit", QKeySequence("Alt+3"), [this] {}, true);
  // Workspaces: one document, one timeline; a workspace only changes the ribbon's tabs and tools.
  // The areas' workspaces get theirs in buildRibbon.
  m_workspaceGroup = new QActionGroup(this);
  m_workspaceGroup->addAction(addAction("workspace.review", tr("Review workspace"), "eye", QKeySequence("Ctrl+1"), [this] { setWorkspace("review"); }, true));
  m_workspaceGroup->addAction(addAction("workspace.design", tr("Design workspace"), "component", QKeySequence("Ctrl+2"), [this] { setWorkspace("design"); }, true));
}

// Layout reset, theme, navigation presets and the selection filters (after the design commands in the command order).
void MainWindow::buildNavigationActions() {
  addAction("panel.reset", tr("Reset layout"), "restore", QKeySequence(), [this] { resetLayout(); });
  m_darkAction->setChecked(m_settings.value("ui/dark", true).toBool());
  connect(m_darkAction, &QAction::toggled, this, [this](bool on) { applyTheme(on); refreshIcons(); });
  // High contrast and the text size (UI-124): a Preferences row or the system's high-contrast switch applies the theme again.
  connect(theme::notifier(), &theme::Notifier::refreshRequested, this, [this] { applyTheme(m_settings.value("ui/dark", true).toBool()); refreshIcons(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  connect(QGuiApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this, [] { theme::refresh(); });
#endif
  for (const auto& [name, preset] : std::vector<std::pair<QString, Viewport::NavPreset>>{{"Fusion", Viewport::NavPreset::Fusion}, {"SolidWorks", Viewport::NavPreset::SolidWorks}, {"Onshape", Viewport::NavPreset::Onshape}, {"Blender", Viewport::NavPreset::Blender}}) {
    // Named after the products whose mouse controls they mimic, never as them (trademarks): "SOLIDWORKS-style".
    QAction* a = addAction("nav." + name.toLower(), tr("Navigation: %1-style").arg(name == "SolidWorks" ? "SOLIDWORKS" : name), "", QKeySequence(), [this, p = preset, n = name] {
      m_viewport->setNavPreset(p);
      m_settings.setValue("ui/nav", n);
      for (QAction* o : m_actions) if (o->objectName().startsWith("nav.")) o->setChecked(o->objectName() == "nav." + n.toLower());
    }, true);
    if (m_settings.value("ui/nav", "Fusion").toString() == name) a->setChecked(true);
  }
  for (const auto& [name, f, key, icon] : std::vector<std::tuple<QString, Viewport::SelFilter, QString, QString>>{{"Bodies", Viewport::SelFilter::Body, "1", "filterBodies"}, {"Faces", Viewport::SelFilter::Face, "2", "filterFaces"}, {"Edges", Viewport::SelFilter::Edge, "3", "filterEdges"}, {"Vertices", Viewport::SelFilter::Vertex, "4", "filterVertices"}}) {
    QAction* a = addAction("select." + name.toLower(), i18n::t(name), icon, QKeySequence(key), [this, ff = f, n = name] {
      m_autoEdges = false;  // chosen by hand: stays
      m_viewport->setSelectionFilter(ff);
      if (!m_tool.id.isEmpty()) {  // mid-tool: the steps are reworded for the new filter and start over
        m_viewport->clearSelection();
        m_toolPicks.clear();
        m_toolPoints.clear();
        refreshToolUi();
      }
      for (const QString other : {"bodies", "faces", "edges", "vertices"})  // not select.through: a setting of its own
        if (QAction* o = action("select." + other)) o->setChecked(other == n.toLower());
    }, true);
    if (f == Viewport::SelFilter::Body) a->setChecked(true);
  }
}

void MainWindow::refreshIcons() {
  icons::clearCache();
  for (QAction* a : m_actions) {
    QString icon = a->data().toString();
    if (!icon.isEmpty()) a->setIcon(icons::themed(icon));
  }
  m_doc->refresh();
}

void MainWindow::buildCentral() {
  m_stack = new QStackedWidget(this);
  m_stack->setObjectName("central");
  m_empty = new EmptyState(m_stack);
  m_viewport = new Viewport(m_doc, m_stack);
  m_viewport->setSelectThrough(action("select.through")->isChecked());
  ToolPanel::setKeyboardHome(m_viewport);  // a click on a panel's button leaves the keys with the view (UI-05)
  // A feature input, a guided tool, a sketch or Select similar sets the filter it picks with: the chips show what the
  // view picks, whoever chose it (they stayed on Bodies while a pipe's path picked edges). Here, not in buildActions:
  // there the viewport did not exist yet and the connection was never made.
  connect(m_viewport, &Viewport::filterApplied, this, [this] {
    const Viewport::SelFilter f = m_viewport->selectionFilter();
    for (const auto& [name, filter] : std::vector<std::pair<QString, Viewport::SelFilter>>{{"bodies", Viewport::SelFilter::Body}, {"faces", Viewport::SelFilter::Face}, {"edges", Viewport::SelFilter::Edge}, {"vertices", Viewport::SelFilter::Vertex}})
      if (QAction* a = action("select." + name)) a->setChecked(filter == f);
  });
  m_stack->addWidget(m_empty);
  m_stack->addWidget(m_viewport);
  setCentralWidget(m_stack);

  // Native child widgets float above the OpenGL surface: top-left chips, bottom-right measurement card.
  m_chips = new ViewportChips(m_viewport);
  m_chips->setAttribute(Qt::WA_NativeWindow);
  connect(m_chips, &ViewportChips::leaveTwoDimensional, this, [this] { action("view.2d")->setChecked(false); });
  connect(m_chips, &ViewportChips::exitIsolation, this, [this] { action("view.unisolate")->trigger(); });
  m_prompt = new PromptBar(m_viewport);
  m_prompt->setAttribute(Qt::WA_NativeWindow);
  m_prompt->hide();
  m_toasts = new ToastStack(m_viewport);
  m_loadShade = new LoadShade(this);
  m_loadShade->hide();
  // Home button with its shortcut hint, floating at the top-left of the view cube (design: navigation cube).
  m_homeBtn = new QWidget(m_viewport);
  m_homeBtn->setAttribute(Qt::WA_NativeWindow);
  m_homeBtn->setAutoFillBackground(true);
  auto* hl = new QVBoxLayout(m_homeBtn);
  hl->setContentsMargins(0, 0, 0, 0);
  hl->setSpacing(2);
  auto* home = new QToolButton(m_homeBtn);
  home->setObjectName("vpButton");
  home->setFixedSize(30, 30);
  home->setIconSize(QSize(16, 16));
  home->setCursor(Qt::PointingHandCursor);
  home->setDefaultAction(action("view.home"));
  auto* hint = new QLabel(QStringLiteral("H"), m_homeBtn);
  hint->setObjectName("tertiary");
  hint->setFont(theme::mono(11));
  hint->setAlignment(Qt::AlignHCenter);
  auto updateHomeHint=[this,hint]{hint->setText(action("view.home")->shortcut().toString(QKeySequence::NativeText));};
  connect(action("view.home"),&QAction::changed,hint,updateHomeHint);updateHomeHint();
  hl->addWidget(home, 0, Qt::AlignHCenter);
  hl->addWidget(hint);
  // Turn-90° buttons on either side of the cube (the arc arrows of the design).
  auto rollButton = [this](const char* icon, const QString& tip, double degrees) {
    auto* b = new QToolButton(m_viewport);
    b->setAttribute(Qt::WA_NativeWindow);
    b->setObjectName("vpButton");
    b->setFixedSize(30, 30);
    b->setIconSize(QSize(18, 18));
    b->setCursor(Qt::PointingHandCursor);
    b->setToolTip(tip);
    b->setIcon(icons::icon(icon, theme::current().fg));
    connect(b, &QToolButton::clicked, this, [this, degrees] { m_viewport->rollView(degrees); });
    return b;
  };
  m_rollLeft = rollButton("rollLeft", tr("Turn the view 90° left"), 90);
  m_rollRight = rollButton("rollRight", tr("Turn the view 90° right"), -90);
  m_alignPlane = new QToolButton(m_viewport);
  m_alignPlane->setAttribute(Qt::WA_NativeWindow);
  m_alignPlane->setObjectName("vpButton");
  m_alignPlane->setFixedSize(30,30);
  m_alignPlane->setDefaultAction(action("view.alignPlane"));
  auto paintHome = [this, home] {
    QPalette pal = m_homeBtn->palette();
    pal.setColor(QPalette::Window, theme::current().vp);
    m_homeBtn->setPalette(pal);
    home->setIcon(icons::icon("home", theme::current().fg));
    m_rollLeft->setIcon(icons::icon("rollLeft", theme::current().fg));
    m_rollRight->setIcon(icons::icon("rollRight", theme::current().fg));
  };
  paintHome();
  connect(theme::notifier(), &theme::Notifier::changed, this, paintHome);
  m_viewport->installEventFilter(this);
}

// ---------------------------------------------------------------- theme (F31)
void MainWindow::applyTheme(bool dark) {
  m_settings.setValue("ui/dark", dark);
  theme::apply(dark);
  if (m_viewport) m_viewport->setTokens(theme::current());
  if (m_darkAction && m_darkAction->isChecked() != dark) m_darkAction->setChecked(dark);
}

bool MainWindow::viewingDrawing() const {
  if (!m_doc->browse) return false;
  const auto bodies = m_doc->scene.all_bodies();
  return !bodies.empty() && std::all_of(bodies.begin(), bodies.end(), [this](const auto& id) { return m_doc->scene.node(id)->representation == "drawing2d"; });
}

void MainWindow::setAutoTwoD(bool on) {
  m_settingTwoD = true;
  action("view.2d")->setChecked(on);
  m_settingTwoD = false;
  m_autoTwoD = on;
}

void MainWindow::updateChips() {
  if (!m_chips) return;
  const Viewport::Style style = m_viewport->style();
  QString mode = style == Viewport::Style::Shaded ? tr("Shaded") : style == Viewport::Style::Wireframe ? tr("Wireframe") : style == Viewport::Style::HiddenLine ? tr("Hidden line")
                 : style == Viewport::Style::HiddenEdges ? tr("Hidden edges visible") : tr("Shaded + edges");
  QString proj = m_viewport->isOrthographic() ? tr("Orthographic") : tr("Perspective");
  QString section;
  if (m_section && m_section->enabled()) {
    opad::Vec3 o = m_section->origin(), n = m_section->normal();
    int axis = std::fabs(n[0]) > 0.9 ? 0 : std::fabs(n[1]) > 0.9 ? 1 : 2;
    const char axes[] = {'X', 'Y', 'Z'};
    section = tr("Section %1 = %2").arg(axes[axis]).arg(units::format(units::Kind::Length, o[axis]));
  }
  const int isolated = m_viewport->isIsolated() ? m_viewport->isolatedCount() : 0;
  m_chips->set(mode, proj, section, isolated == 1 ? tr("Isolated · 1 body") : isolated ? tr("Isolated · %1 bodies").arg(isolated) : QString(),
               action("view.2d")->isChecked());
  positionOverlays();
}

void MainWindow::positionOverlays() {
  if (!m_viewport) return;
  if (m_browserOverlay) m_browserOverlay->place();
  m_chips->move(0, 0);
  m_chips->raise();
  m_homeBtn->adjustSize();
  m_homeBtn->move(m_viewport->width() - 200, 12);  // left of the cube's axes, level with its top
  m_homeBtn->raise();
  m_rollLeft->move(m_viewport->width() - 200, 150);  // lower-left and lower-right of the cube
  m_rollLeft->raise();
  m_rollRight->move(m_viewport->width() - 40, 150);
  m_rollRight->raise();
  m_alignPlane->move(m_viewport->width()-120,150);
  m_alignPlane->raise();
  if (m_loadShade->isVisible()) {
    QRect area = m_stack->geometry();  // the workspace: central area plus the docked panels
    for (QDockWidget* d : {m_timelineDock})
      if (d && d->isVisible() && !d->isFloating()) area |= d->geometry();
    m_loadShade->place(QRect(mapToGlobal(area.topLeft()), area.size()), m_viewport->mapToGlobal(m_viewport->rect().center()));
  }
  if (trace::enabled()) trace::log(QStringLiteral("viewport at %1,%2 size %3x%4").arg(m_viewport->mapToGlobal(QPoint(0, 0)).x()).arg(m_viewport->mapToGlobal(QPoint(0, 0)).y()).arg(m_viewport->width()).arg(m_viewport->height()));
  if (m_prompt->isVisible()) {
    m_prompt->move(std::max(8, (m_viewport->width() - m_prompt->width()) / 2), 44);
    m_prompt->raise();
  }
  m_toasts->place();
  const QRect vp(m_viewport->mapToGlobal(QPoint(0, 0)), m_viewport->size());
  for (ToolPanel* p : m_panels)
    if (p->isVisible()) p->anchorTo(vp);  // the panels follow the viewport's top-right corner
  if (m_annotationPanel && m_annotationPanel->isVisible()) m_annotationPanel->anchorTo(vp);
  forEachArea([&vp](AreaController* area) { area->positionOverlays(vp); });
}

void MainWindow::resizeEvent(QResizeEvent* e) {
  QMainWindow::resizeEvent(e);
  positionOverlays();
}

void MainWindow::moveEvent(QMoveEvent* e) {
  QMainWindow::moveEvent(e);
  positionOverlays();
}

// Minimised and restored: the 3D view's native surface lost its frame, and nothing changed in the scene, so it stayed black
// until the pointer moved over it. Its whole frame now, and once more when the window has settled (the surface may come
// back after the state change).
void MainWindow::changeEvent(QEvent* e) {
  QMainWindow::changeEvent(e);
  if (e->type() != QEvent::WindowStateChange || !m_viewport) return;
  if (!(static_cast<QWindowStateChangeEvent*>(e)->oldState() & Qt::WindowMinimized) || (windowState() & Qt::WindowMinimized)) return;
  m_viewport->exposedAgain();
  QTimer::singleShot(100, m_viewport, [view = m_viewport] { view->exposedAgain(); });
}

void MainWindow::setLoading(bool on) {
  m_loadShown = on;
  showCentral();  // the viewport (dimmed, spinner) rather than the start page while loading
  m_viewport->setBlocked(on);
  m_loadShade->setVisible(on);
  for (QWidget* w : std::initializer_list<QWidget*>{m_browser, m_timeline, m_propsPanel, m_annotationsPanel, m_sectionPanel, m_toolPanel, m_featurePanel}) w->setEnabled(!on);
  if (on) positionOverlays();
}

void MainWindow::showCentral() {
  QWidget* page = m_loadShown ? m_viewport : !m_doc->hasDocument ? static_cast<QWidget*>(m_empty) : m_centralPage ? m_centralPage : m_viewport;
  m_stack->setCurrentWidget(page);
  if (m_browserOverlay) m_browserOverlay->setScene(page == m_empty ? m_viewport : page);
  if (m_toasts) m_toasts->setHost(page);  // over whatever is shown (the sheet canvas too)
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
  if (o == m_viewport && (e->type() == QEvent::Resize || e->type() == QEvent::Show)) positionOverlays();
  if (o == m_viewport && e->type() == QEvent::KeyPress && repeatOnEnter(static_cast<QKeyEvent*>(e))) return true;
  return QMainWindow::eventFilter(o, e);
}

// Enter in the view while nothing runs repeats the last command, as in SOLIDWORKS and AutoCAD (UI-111). While a tool, a
// feature, a sketch, a note or a plane pick runs, Enter is theirs (OK, finish, the typed value); a held key does nothing.
// Delete, Restore, Hide and Show all repeat only from Repeat itself (Shift+Enter, the menu): a stray Enter must not act on
// whatever is selected now.
bool MainWindow::repeatOnEnter(const QKeyEvent* key) {
  static const QStringList asked{"edit.delete", "edit.restore", "edit.hide", "edit.showall"};
  if ((key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) || (key->modifiers() & ~Qt::KeypadModifier) || key->isAutoRepeat() || asked.contains(m_lastCommand))
    return false;
  if (!m_doc->hasDocument || !m_tool.id.isEmpty() || m_annotationEditor || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane() ||
      m_design->planePicker()->active() || m_drawingPlacer->active() || m_toolPanel->isVisible())
    return false;
  QAction* repeat = action("edit.repeat");
  if (!repeat || !repeat->isEnabled()) return false;
  repeat->trigger();
  return true;
}

// ---------------------------------------------------------------- named views (view op)
void MainWindow::saveNamedView() {
  const auto named = std::count_if(m_doc->scene.views.begin(), m_doc->scene.views.end(), [](const opad::ViewBookmark& v) { return !v.home; });
  QDialog dialog(this);
  dialog.setObjectName("saveViewDialog");
  dialog.setWindowTitle(tr("Save view"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* form = new QFormLayout;
  auto* name = new QLineEdit(tr("View %1").arg(named + 1));
  name->setObjectName("saveViewName");
  name->selectAll();
  form->addRow(tr("Name:"), name);
  layout->addLayout(form);
  // Off at first; the choice is remembered for the next view saved.
  auto* visibility = new QCheckBox(tr("Also keep which objects are hidden and shown"));
  visibility->setObjectName("saveViewVisibility");
  visibility->setToolTip(tr("Choosing the view later hides what is hidden now and shows everything else, in one step you can undo."));
  visibility->setChecked(m_settings.value("view/namedViewVisibility", false).toBool());
  layout->addWidget(visibility);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
  m_settings.setValue("view/namedViewVisibility", visibility->isChecked());
  saveNamedView(name->text().trimmed(), visibility->isChecked());
}

void MainWindow::saveNamedView(const QString& name, bool visibility) {
  opad::json args{{"name", name.toStdString()}, {"camera", m_viewport->cameraJson()}};
  if (visibility) {  // the nodes hidden now (components and bodies); an older build reads the view as a camera only
    std::vector<std::string> hidden;
    for (const auto& [id, n] : m_doc->scene.nodes)
      if (!n.visible) hidden.push_back(id);
    std::sort(hidden.begin(), hidden.end());
    args["display"] = opad::json{{"hidden", hidden}};
  }
  m_doc->run("view", args);
}

void MainWindow::restoreNamedView(const std::string& id) {
  for (const auto& v : m_doc->scene.views)
    if (v.id == id) {
      m_viewport->setCameraJson(v.camera);
      std::vector<std::pair<std::string, opad::json>> ops;  // a layer state saved with it (UI-89) and what it hid: one step
      for (auto& args : drawing2d::restoreState(m_doc->scene, v.display)) ops.push_back({"appearance", std::move(args)});
      bool visibility = false;
      if (v.display.is_object() && v.display.contains("hidden") && v.display["hidden"].is_array()) {
        std::set<std::string> hidden;
        for (const auto& h : v.display["hidden"])
          if (h.is_string()) hidden.insert(h.get<std::string>());
        std::vector<std::string> hide, show;  // only what differs: the nodes saved hidden, everything else shown
        for (const auto& [nid, n] : m_doc->scene.nodes)
          if (n.visible == bool(hidden.count(nid))) (n.visible ? hide : show).push_back(nid);
        std::sort(hide.begin(), hide.end());
        std::sort(show.begin(), show.end());
        if (!hide.empty()) ops.push_back({"appearance", opad::json{{"targets", hide}, {"visible", false}}});
        if (!show.empty()) ops.push_back({"appearance", opad::json{{"targets", show}, {"visible", true}}});
        visibility = !hide.empty() || !show.empty();
      }
      if (!ops.empty()) guarded([&] { m_doc->runAll(ops, visibility ? tr("restore view") : tr("restore layer state")); });
      return;
    }
}

void MainWindow::recallNamedView(int n) {
  int seen = 0;
  for (const auto& v : m_doc->scene.views) {
    if (v.home || ++seen < n) continue;
    // As a click on its entry in Named views: what listens there runs too (an exploded view explodes again, ExplodeArea).
    if (m_viewsMenu)
      for (QAction* a : m_viewsMenu->actions())
        if (a->data().toString().toStdString() == v.id) return a->trigger();
    return restoreNamedView(v.id);
  }
  const QString key = keys::text("view.saveview");
  statusBar()->showMessage(key.isEmpty() ? tr("There is no named view %1 yet: Save view keeps the current one.").arg(n)
                                         : tr("There is no named view %1 yet: Save view (%2) keeps the current one.").arg(n).arg(key), 6000);
}

void MainWindow::rebuildViewsMenu() {
  if (!m_viewsMenu) return;
  m_viewsMenu->clear();
  int n = 0;
  for (const auto& v : m_doc->scene.views) {
    if (v.home) continue;  // the document's Home (H), not a bookmark
    ++n;  // the first nine with their key (Named view 1-9), in the menu's key column
    const QString text = n <= 9 ? keys::menuText(QString::fromStdString(v.name), QString("view.named%1").arg(n)) : QString::fromStdString(v.name);
    QAction* a = m_viewsMenu->addAction(icons::themed(v.explode.is_object() ? "explodedView" : "home", 16), text);
    a->setData(QString::fromStdString(v.id));  // areas show more of a view (an exploded one: Explode, ExplodeArea.cpp)
    connect(a, &QAction::triggered, this, [this, id = v.id] { restoreNamedView(id); });
  }
  if (m_viewsMenu->isEmpty()) m_viewsMenu->addAction(tr("(none saved)"))->setEnabled(false);
}
