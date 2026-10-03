// The view: view and navigation commands, the central viewport and its overlays, theme, named views.
#include "MainWindow.hpp"

#include <QActionGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

#include "I18n.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "Units.hpp"

void MainWindow::buildViewActions() {
  addAction("view.fit", tr("Fit"), "fit", QKeySequence("F"), [this] { if(m_design&&m_design->sketchActive())m_design->sketch()->fitSketch();else m_viewport->fitSelection(); });  // the selection, or everything when nothing is selected
  addAction("view.fitall", tr("Fit all"), "fit", QKeySequence("Shift+F"), [this] { if(m_design&&m_design->sketchActive())m_design->sketch()->fitSketch();else m_viewport->fitAll(); });
  addAction("view.home", tr("Home"), "home", QKeySequence("H"), [this] { m_viewport->home(); });
  addAction("view.alignPlane",tr("Align view to plane"),"plane",QKeySequence("Shift+A"),[this] {
    if(m_design->sketchActive()) {m_viewport->lookAt(m_design->sketch()->frame(),false,false);return;}
    cancelTool();
    m_design->pickSketchPlane([this](opad::json,opad::Frame frame) { m_viewport->lookAt(frame,true,false); });
  });
  addAction("view.rollleft", tr("Turn 90° left"), "rollLeft", QKeySequence("Alt+Left"), [this] { m_viewport->rollView(90); });
  addAction("view.rollright", tr("Turn 90° right"), "rollRight", QKeySequence("Alt+Right"), [this] { m_viewport->rollView(-90); });
  for (const auto& [name, key] : std::vector<std::pair<QString, QString>>{{"top", "Shift+Up"}, {"front", "Shift+PgUp"}, {"right", "Shift+Right"}, {"iso", "Shift+H"}, {"bottom", "Shift+Down"}, {"back", "Shift+PgDown"}, {"left", "Shift+Left"}})
    addAction("view." + name, tr("View: %1").arg(name), "home", QKeySequence(key), [this, n = name] { m_viewport->standardView(n); });
  auto* flat = addAction("view.2d",tr("2D mode"),"drawing",QKeySequence("Shift+2"),[this]{},true);
  flat->setObjectName("view.2d");
  flat->setCheckable(true);
  connect(flat, &QAction::toggled, this, [this](bool on) {
    if (!m_settingTwoD) m_autoTwoD = false;  // set by hand: stays as the user left it
    m_viewport->setTwoDimensional(on);

    if (m_browserOverlay && action("panel.browser")->isChecked()) { m_browserOverlay->setVisible(m_doc->hasDocument); m_browserOverlay->raise(); }
    m_homeBtn->setVisible(!on); m_rollLeft->setVisible(!on); m_rollRight->setVisible(!on); m_alignPlane->setVisible(!on);
    updateChips();
  });
  QAction* ortho = addAction("view.ortho", tr("Orthographic"), "ortho", QKeySequence("Shift+3"), [this] {}, true);
  ortho->setChecked(true);
  connect(ortho, &QAction::toggled, this, [this](bool on) { m_viewport->setOrthographic(on); m_settings.setValue("view/orthographic",on); updateChips(); });
  QAction* shaded = addAction("view.shaded", tr("Shaded"), "shaded", QKeySequence("5"), [this] {}, true);
  QAction* edges = addAction("view.edges", tr("Shaded + edges"), "shadedEdges", QKeySequence("6"), [this] {}, true);
  QAction* wire = addAction("view.wire", tr("Wireframe"), "wireframe", QKeySequence("7"), [this] {}, true);
  auto* styleGroup = new QActionGroup(this);
  for (QAction* a : {shaded, edges, wire}) styleGroup->addAction(a);
  edges->setChecked(true);
  connect(styleGroup, &QActionGroup::triggered, this, [this, shaded, wire](QAction* a) {
    m_settings.setValue("view/style",a->objectName());
    m_viewport->setStyle(a == shaded ? Viewport::Style::Shaded : a == wire ? Viewport::Style::Wireframe : Viewport::Style::ShadedEdges);
    updateChips();
  });
  QAction* grid = addAction("view.grid", tr("Grid"), "grid", QKeySequence("G"), [this] {}, true);
  connect(grid, &QAction::toggled, this, [this](bool on) { m_viewport->setGrid(on); m_settings.setValue("view/grid",on); });
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
  addAction("view.isolate", tr("Isolate"), "isolate", QKeySequence("I"), [this] { m_viewport->isolate(currentNodeIds()); });
  addAction("view.hideothers", tr("Hide others"), "hide", QKeySequence(), [this] { hideOthers(currentNodeIds()); });  // one step (UI-02)
  // macOS treats any action starting with "Exit" as Quit unless its menu role is explicit.
  addAction("view.unisolate", tr("Exit isolate"), "showAll", QKeySequence("Shift+I"), [this] { m_viewport->isolate({}); })->setMenuRole(QAction::NoRole);
  addAction("view.saveview", tr("Save view…"), "home", QKeySequence(), [this] { saveNamedView(); });
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
  for (const auto& [name, preset] : std::vector<std::pair<QString, Viewport::NavPreset>>{{"Fusion", Viewport::NavPreset::Fusion}, {"SolidWorks", Viewport::NavPreset::SolidWorks}, {"Onshape", Viewport::NavPreset::Onshape}, {"Blender", Viewport::NavPreset::Blender}}) {
    QAction* a = addAction("nav." + name.toLower(), tr("Navigation: %1").arg(name), "", QKeySequence(), [this, p = preset, n = name] {
      m_viewport->setNavPreset(p);
      m_settings.setValue("ui/nav", n);
      for (QAction* o : m_actions) if (o->objectName().startsWith("nav.")) o->setChecked(o->objectName() == "nav." + n.toLower());
    }, true);
    if (m_settings.value("ui/nav", "Fusion").toString() == name) a->setChecked(true);
  }
  for (const auto& [name, f, key, icon] : std::vector<std::tuple<QString, Viewport::SelFilter, QString, QString>>{{"Bodies", Viewport::SelFilter::Body, "1", "filterBodies"}, {"Faces", Viewport::SelFilter::Face, "2", "filterFaces"}, {"Edges", Viewport::SelFilter::Edge, "3", "filterEdges"}, {"Vertices", Viewport::SelFilter::Vertex, "4", "filterVertices"}}) {
    QAction* a = addAction("select." + name.toLower(), i18n::t(name), icon, QKeySequence(key), [this, ff = f, n = name] {
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
  // A feature input, a guided tool or a sketch sets the filter it picks with: the chips show what the view picks,
  // whoever chose it (they stayed on Bodies while a pipe's path picked edges).
  connect(m_viewport, &Viewport::filterApplied, this, [this] {
    const Viewport::SelFilter f = m_viewport->selectionFilter();
    for (const auto& [name, filter] : std::vector<std::pair<QString, Viewport::SelFilter>>{{"bodies", Viewport::SelFilter::Body}, {"faces", Viewport::SelFilter::Face}, {"edges", Viewport::SelFilter::Edge}, {"vertices", Viewport::SelFilter::Vertex}})
      if (QAction* a = action("select." + name)) a->setChecked(filter == f);
  });
}

void MainWindow::refreshIcons() {
  icons::clearCache();
  for (QAction* a : m_actions) {
    QString icon = a->data().toString();
    if (!icon.isEmpty()) a->setIcon(icons::themed(icon));
  }
  if (m_statusGitIcon) m_statusGitIcon->setPixmap(icons::pixmap("git", theme::current().fg2, 14, devicePixelRatioF()));
  m_doc->refresh();
}

void MainWindow::buildCentral() {
  m_stack = new QStackedWidget(this);
  m_stack->setObjectName("central");
  m_empty = new EmptyState(m_stack);
  m_viewport = new Viewport(m_doc, m_stack);
  m_viewport->setSelectThrough(action("select.through")->isChecked());
  m_stack->addWidget(m_empty);
  m_stack->addWidget(m_viewport);
  setCentralWidget(m_stack);

  // Native child widgets float above the OpenGL surface: top-left chips, bottom-right measurement card.
  m_chips = new ViewportChips(m_viewport);
  m_chips->setAttribute(Qt::WA_NativeWindow);
  connect(m_chips, &ViewportChips::leaveTwoDimensional, this, [this] { action("view.2d")->setChecked(false); });
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

void MainWindow::updateChips() {
  if (!m_chips) return;
  QString mode = m_viewport->style() == Viewport::Style::Shaded ? tr("Shaded") : m_viewport->style() == Viewport::Style::Wireframe ? tr("Wireframe") : tr("Shaded + edges");
  QString proj = m_viewport->isOrthographic() ? tr("Orthographic") : tr("Perspective");
  QString section;
  if (m_section && m_section->enabled()) {
    opad::Vec3 o = m_section->origin(), n = m_section->normal();
    int axis = std::fabs(n[0]) > 0.9 ? 0 : std::fabs(n[1]) > 0.9 ? 1 : 2;
    const char axes[] = {'X', 'Y', 'Z'};
    section = QString("Section %1 = %2").arg(axes[axis]).arg(units::format(units::Kind::Length, o[axis]));
  }
  m_chips->set(mode, proj, section, m_viewport->isIsolated() ? tr("Isolated · %1 bodies").arg(m_viewport->isolatedCount()) : QString(),
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

void MainWindow::setLoading(bool on) {
  m_stack->setCurrentIndex(on || m_doc->hasDocument ? 1 : 0);  // the viewport (dimmed, spinner) rather than the start page while loading
  m_viewport->setBlocked(on);
  m_loadShade->setVisible(on);
  for (QWidget* w : std::initializer_list<QWidget*>{m_browser, m_timeline, m_propsPanel, m_annotationsPanel, m_sectionPanel, m_toolPanel, m_featurePanel}) w->setEnabled(!on);
  if (on) positionOverlays();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
  if (o == m_viewport && (e->type() == QEvent::Resize || e->type() == QEvent::Show)) positionOverlays();
  return QMainWindow::eventFilter(o, e);
}

// ---------------------------------------------------------------- named views (view op)
void MainWindow::saveNamedView() {
  bool ok = false;
  QString name = QInputDialog::getText(this, tr("Save view"), tr("Name:"), QLineEdit::Normal, tr("View %1").arg(m_doc->scene.views.size() + 1), &ok);
  if (!ok || name.isEmpty()) return;
  m_doc->run("view", opad::json{{"name", name.toStdString()}, {"camera", m_viewport->cameraJson()}});
}

void MainWindow::restoreNamedView(const std::string& id) {
  for (const auto& v : m_doc->scene.views)
    if (v.id == id) m_viewport->setCameraJson(v.camera);
}

void MainWindow::rebuildViewsMenu() {
  if (!m_viewsMenu) return;
  m_viewsMenu->clear();
  for (const auto& v : m_doc->scene.views) {
    QAction* a = m_viewsMenu->addAction(icons::themed("home", 16), QString::fromStdString(v.name));
    connect(a, &QAction::triggered, this, [this, id = v.id] { restoreNamedView(id); });
  }
  if (m_viewsMenu->isEmpty()) m_viewsMenu->addAction(tr("(none saved)"))->setEnabled(false);
}
