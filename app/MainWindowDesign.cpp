#include "MainWindow.hpp"
#include "DrawingPlacer.hpp"

#include <QActionGroup>
#include <QColorDialog>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QScopedValueRollback>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "I18n.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/design/feature.hpp"

// ---------------------------------------------------------------- design workspace
// Feature tools come from the core's spec table (one action per kind, the form is generic); sketch tools are
// only live while a sketch is open, when the ribbon shows the contextual Sketch tab set.
void MainWindow::buildDesignActions() {
  addAction("design.convertDrawing",tr("Drawing to sketch"),"drawing",QKeySequence(),[this] { drawingToSketch(); });
  {
    CommandInfo draw{"design.drawOnDrawing", tr("Draw on drawing"), "sketch"};  // Drafting > Home > Draw (UI-104)
    draw.keywords = {tr("sketch on drawing"), tr("draw lines"), "DXF", "DWG", tr("drafting")};
    draw.group = tr("Drafting");
    draw.editsDocument = isEditAction(draw.id);  // a viewed drawing file asks to be saved as an OPAD document (or an unsaved copy) first
    draw.enabledWhen = [](const CommandContext& c) { return c.document && !c.sketching; };
    addCommand(draw, [this] { drawOnDrawing(); });
  }
  addAction("design.sketch", tr("New sketch"), "sketch", QKeySequence(), [this] { m_design->startSketch(); });
  static const std::map<std::string, const char*> kKeys = {{"extrude", "E"}, {"offset_face", "Q"}, {"move", "M"}};
  for (const auto& spec : opad::design::feature_specs()) {
    const QString kind = QString::fromStdString(spec.kind);
    const auto key = kKeys.find(spec.kind);
    QAction* a = addAction("design." + kind, i18n::t(QString::fromStdString(spec.label)), QString::fromStdString(spec.icon), key == kKeys.end() ? QKeySequence() : QKeySequence(key->second),
                           [this, kind] {
                             if (kind == "mesh_solid") meshToSolid();  // its own panel: the preview measured against the mesh
                             else m_design->startFeature(kind);
                           });
    a->setProperty("shortcutHint",i18n::t(QString::fromStdString(spec.hint)));
    shortcuts::updateTooltip(a);
  }
  addAction("design.parameters", tr("Parameters"), "fx", QKeySequence("Ctrl+Shift+U"), [this] { m_design->showParameters(); });
  addAction("design.regenerate", tr("Regenerate"), "regen", QKeySequence(), [this] { m_design->regenerate(true); });
  addAction("design.edit", tr("Edit feature"), "rename", QKeySequence(), [this] {
    const std::string id = m_timeline->currentOp();
    const opad::Op* op = id.empty() ? nullptr : m_doc->doc.find_op(id);
    if (!op || (op->type != "feature" && op->type != "sketch")) throw opad::UserHint("Select a feature or a sketch on the timeline first (or double-click it).", true);
    m_design->editOp(id);
  });
  // Solid > History (UI-104): the marker menu's Suppress as a command, for the feature selected on the timeline, else the
  // one that made the selected body; a suppressed one is brought back.
  addAction("design.suppress", tr("Suppress"), "hide", QKeySequence(), [this] {
    std::string id = m_timeline->currentOp();
    if (!m_doc->scene.feature(id)) {
      const auto ids = currentNodeIds();
      const opad::Node* n = ids.size() == 1 ? m_doc->node(ids.front()) : nullptr;
      id = n && n->kind == opad::Node::Kind::Body ? n->source_op : std::string();
    }
    const opad::Feature* f = m_doc->scene.feature(id);
    if (!f || std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), id) != m_doc->scene.deleted_ops.end())
      throw opad::UserHint("Select a feature on the timeline first, or a body it made.", true);
    if (m_design->busy()) return;
    m_design->setSuppressed(id, !f->suppressed);
  });
  addAction("design.colour", tr("Colour"), "shaded", QKeySequence(), [this] {
    const auto ids = currentNodeIds();
    if (ids.empty()) throw opad::UserHint("Select the objects to colour first.", true);
    const QColor c = QColorDialog::getColor(nodeColour(ids.front()), this, tr("Colour"));
    if (!c.isValid()) return;
    m_doc->run("appearance", opad::json{{"targets", ids}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
  });

  // Sketch mode.
  addAction("sketch.finish", tr("Finish sketch"), "finish", QKeySequence("Ctrl+Return"), [this] { m_design->finishSketch(); });
  addAction("sketch.panel",tr("Sketch tools"),"sketch",QKeySequence("Ctrl+Alt+S"),[this]{m_design->showSketchPanel();});
  addAction("sketch.replane",tr("Redefine sketch plane"),"plane",QKeySequence(),[this]{m_design->redefineSketchPlane();});
  // Align a sketch (the one open, else the one selected): onto a face or plane, a point onto a point, a line along a line.
  QAction* alignSketch = addAction("design.alignSketch", tr("Align sketch"), "align", QKeySequence(), [this] {
    std::string id = m_design->sketchActive() ? m_design->sketch()->sketchId() : std::string();
    if (id.empty())
      for (const auto& s : selectionContext().ids)
        if (m_doc->scene.sketch(s)) { id = s; break; }
    if (id.empty()) throw opad::UserHint("Select the sketch to align first (in the view or the browser).", true);
    if (requireEditable()) m_design->startSketchAlign(id);
  });
  alignSketch->setProperty("shortcutHint", tr("Put the sketch's plane on a face or plane, a point of it on a point, or a line of it along a line; what is built on it follows."));
  shortcuts::updateTooltip(alignSketch);
  addAction("sketch.cancel", tr("Cancel sketch"), "close", QKeySequence(), [this] { m_design->cancelSketch(); });
  addAction("sketch.node",tr("Spline node weights"),"spline",QKeySequence("Alt+W"),[this]{m_design->sketch()->editSplineNode();});
  addAction("sketch.openEnds",tr("Find open ends"),"point",QKeySequence("Alt+E"),[this]{m_design->sketch()->findOpenVertices();});
  auto* tools = new QActionGroup(this);
  for (const auto& [tool, text, icon] : std::vector<std::tuple<QString, QString, QString>>{
           {"select", tr("Select"), "cursor"}, {"line", tr("Line"), "line"}, {"rect", tr("Rectangle"), "rect"}, {"crect", tr("Centre rectangle"), "crect"},
           {"circle", tr("Circle"), "circle"}, {"circle3", tr("3-point circle"), "circle3"}, {"arc3", tr("3-point arc"), "arc3"}, {"arcc", tr("Centre arc"), "arcc"},
           {"polygon", tr("Polygon"), "polygon"}, {"slot", tr("Slot"), "slot"}, {"ellipse", tr("Ellipse"), "ellipse"}, {"spline", tr("Spline"), "spline"},
           {"point", tr("Point"), "point"}, {"fillet", tr("Sketch fillet"), "fillet"}, {"trim", tr("Trim"), "trim"}, {"mirror", tr("Mirror"), "mirror"},
           {"offset", tr("Offset"), "offset"}, {"project", tr("Project"), "project"},
           {"dimension", tr("Dimension"), "dimension"}, {"c:horizontal", tr("Horizontal"), "cHorizontal"}, {"c:vertical", tr("Vertical"), "cVertical"},
           {"c:coincident", tr("Coincident"), "cCoincident"}, {"c:parallel", tr("Parallel"), "cParallel"}, {"c:perpendicular", tr("Perpendicular"), "cPerpendicular"},
           {"c:tangent", tr("Tangent"), "cTangent"}, {"c:equal", tr("Equal"), "cEqual"}, {"c:concentric", tr("Concentric"), "cConcentric"}, {"c:midpoint", tr("Midpoint"), "cMidpoint"},
           {"c:symmetric", tr("Symmetric"), "cSymmetric"}, {"c:collinear", tr("Collinear"), "cCollinear"}, {"c:fix", tr("Fix"), "cFix"}}) {
    const QMap<QString,QString> keys{{"line","L"},{"rect","R"},{"circle","C"},{"arc3","A"},{"dimension","D"},{"trim","T"},{"offset","O"},{"spline","B"},{"project","P"},{"mirror","Shift+M"},{"slot","U"},{"polygon","N"}};
    QAction* a = addAction("sketch." + QString(tool).replace(':', '.'), text, icon, QKeySequence(keys.value(tool)), [this, t = tool] { m_design->sketch()->setTool(t); }, true);
    a->setProperty("sketchTool", tool);
    tools->addAction(a);
  }
  addAction("sketch.construction", tr("Construction"), "construction", QKeySequence("X"), [this] { m_design->sketch()->toggleConstruction(); });
  {
    CommandInfo info{"sketch.showConstraints", tr("Show or hide constraints"), "cHorizontal"};
    info.keywords = {tr("constraint badges"), tr("glyphs")};
    info.editsDocument = isEditAction(info.id);
    addCommand(info, [this] { m_design->sketch()->setShowConstraints(!m_design->sketch()->showConstraints()); });
  }
  const auto registry=SketchPanel::tools();
  // Each tool its own icon (they all showed the generic sketch one): its own name where the table has it.
  const QMap<QString,QString> toolIcons{{"tangent_circle","tangentCircle"},{"tangent_arc","tangentArc"},{"polygon_outer","polygonOuter"},{"control_spline","controlSpline"},
    {"select","cursor"},{"rect_pattern","patternRect"},{"polar_pattern","patternCirc"},{"break","breakCurve"},{"union","combine"},{"intersect_body","project"},
    {"silhouette","project"},{"include3d","project"},{"break_link","breakLink"},{"image_insert","image"},{"image_edit","image"},{"image_calibrate","image"},
    {"image_trace","image"},{"image_remove","image"},{"vector_import","import"},{"vector_export","export"}};
  for(const auto& tool:registry) {
    const auto id="sketch."+QString(tool.id).replace(':','.');
    if(action(id))continue;
    const QMap<QString,QString> keys{{"move","M"},{"rotate","Shift+R"},{"scale","Shift+S"},{"copy","Shift+C"}};
    const QString icon=toolIcons.value(tool.id,icons::has(tool.id)?QString(tool.id):QString("sketch"));
    auto* a=addAction(id,tool.label,icon,QKeySequence(keys.value(tool.id)),[this,id=tool.id]{m_design->sketch()->setTool(id);},true);
    a->setProperty("sketchTool",tool.id);tools->addAction(a);
  }
  // Every tool of a group, in the menu bar's Sketch menu (the ribbon's Sketch tab has them in its groups' menus); run by
  // name (the palette, a key of one's own) the list opens at the pointer.
  for(const auto& [id,group,label]:std::vector<std::tuple<QString,QString,QString>>{{"Create",tr("Create"),tr("More create tools")},{"Modify",tr("Modify"),tr("More modify tools")},
        {"Constrain",tr("Constrain"),tr("More constraints")},{"Reference",tr("Reference"),tr("More reference tools")},{"Files",tr("Images and files"),tr("Images and files")}}) {
    auto* menu=new QMenu(this);menu->setObjectName(id.toLower());
    for(const auto& tool:registry)if(tool.group==group)menu->addAction(action("sketch."+QString(tool.id).replace(':','.')));
    CommandInfo info{"sketch.more"+id,label,id=="Files"?QString("image"):QString("more")};
    info.editsDocument=isEditAction(info.id);
    menuCommand(info,menu);
  }
  int page=1;
  const QMap<QString,QString> pageIcons{{"selectionOptions","cursor"},{"constraints","list"},{"snaps","magnet"}};
  for(const auto& pair:QList<QPair<QString,QString>>{{"selectionOptions",tr("Selection")},{"constraints",tr("Constraints")},{"snaps",tr("Snaps")}}) {
    addAction("sketch."+pair.first,pair.second,pageIcons.value(pair.first),{},[this,page,title=pair.second]{m_design->showSketchPanel(title);findChild<SketchPanel*>()->showPage(page);});++page;
  }
}

void MainWindow::buildDesign() {
  m_design = new DesignController(m_doc, m_viewport, m_jobs, this);
  m_featurePanel = new ToolPanel("feature", "extrude", &Tokens::sel, tr("Feature"), m_design->featurePanel(), 560, this);
  m_panels << m_featurePanel;
  {
    FeaturePanel* form = m_design->featurePanel();
    m_featurePanel->setContentSizeHint([form](int width) { return form->preferredSize(width); });
    connect(form, &FeaturePanel::contentResized, m_featurePanel, &ToolPanel::requestContentFit);
  }
  m_design->setPanel(m_featurePanel, [this](ToolPanel* p) { openPanel(p); });
  // Esc over the panel is Esc in the view: a primitive being placed steps back a stage (TODO 11 P1), else the feature closes.
  m_featurePanel->setEscapeHandler([this] { if (!m_design->escape()) m_featurePanel->hide(); });
  m_design->setCurrentComponent([this] {
    if (!m_doc->activeComponent().empty()) return m_doc->activeComponent();  // UI-33
    const auto ids = m_browser->selectedIds();
    const opad::Node* n = ids.size() == 1 ? m_doc->node(ids.front()) : nullptr;
    return n && n->kind == opad::Node::Kind::Component ? n->id : std::string();
  });
  auto* parameters=m_design->parametersWidget();
  auto* parametersPanel=new ToolPanel("parameters","fx",&Tokens::sel,tr("Parameters"),parameters,480,this);
  m_panels<<parametersPanel;m_design->setParametersPanel(parametersPanel);
  connect(parameters,&ParametersDialog::closeRequested,parametersPanel,&ToolPanel::hide);
  auto* sketchContent=new SketchPanel(m_design->sketch(),this);
  auto* sketchPanel=new ToolPanel("sketch","sketch",&Tokens::sel,tr("Sketch tools"),sketchContent,620,this);
  m_panels<<sketchPanel;
  sketchPanel->setContentSizeHint([sketchContent](int width){return sketchContent->toolSizeHint(width);});
  connect(m_design->sketch(),&SketchEditor::toolChanged,sketchPanel,&ToolPanel::requestContentFit);
  connect(m_design->sketch(),&SketchEditor::workflowChanged,sketchPanel,&ToolPanel::requestContentFit);
  // A message that wraps to more lines takes room from the tool's fields: fit again (Project's Preview was cut off).
  connect(m_design->sketch(),&SketchEditor::status,sketchPanel,&ToolPanel::requestContentFit);
  connect(sketchContent,&SketchPanel::contentChanged,sketchPanel,&ToolPanel::requestContentFit);
  m_design->setSketchPanel(sketchPanel);
  sketchPanel->setEscapeHandler([this]{m_design->sketch()->escape();});  // the same Esc ladder as in the view
  connect(sketchContent,&SketchPanel::finishRequested,this,[this]{m_design->finishSketch();});
  m_panels<<m_design->planePanel();
  m_drawingPlacer = new DrawingPlacer(m_doc, m_viewport, m_jobs, this);
  m_panels << m_drawingPlacer->panel();

  connect(m_design, &DesignController::status, this, &MainWindow::setPrompt);
  connect(m_design, &DesignController::notice, this, [this](const QString& text) { resultToast(text); });
  connect(m_design, &DesignController::failed, this, [this](const QString& error) { if (error != "cancelled") failedToast(i18n::t(error)); });  // cancelled by the user
  connect(m_design, &DesignController::stateChanged, this, &MainWindow::updateDesignState);
  connect(m_design->sketch(), &SketchEditor::hintsChanged, this, [this] { if (m_design->sketchActive() && !m_design->pickingPlane()) updateSketchPrompt(); });
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] { if (m_design->sketchActive() && !m_design->pickingPlane()) updateSketchPrompt(); });  // Finish sketch's key
  auto editOp = [this](const std::string& id) {  // a read-only document asks for a copy first
    auto edit = [this, id] { guarded([&] { m_design->editOp(id); }); };
    if (requireEditable(edit)) edit();
  };
  connect(m_timeline, &TimelineWidget::opActivated, this, editOp);
  connect(m_timeline, &TimelineWidget::suppressRequested, this, [this](const std::string& id) {
    if (m_doc->browse || m_design->busy() || !requireEditable()) return;  // read-only: a copy first
    if (const opad::Feature* f = m_doc->scene.feature(id)) guarded([&] { m_design->setSuppressed(id, !f->suppressed); });
  });
  connect(m_timeline, &TimelineWidget::deleteRequested, this, [this](const std::string& id, bool restore) {
    if (m_doc->browse || !requireEditable()) return;
    const bool deleted = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), id) != m_doc->scene.deleted_ops.end();
    // Shift+Del restores a tombstoned marker, and also what a live delete or Remove step took out (as Restore does).
    const opad::Op* op = m_doc->doc.find_op(id);
    const opad::Feature* f = m_doc->scene.feature(id);
    const bool removes = !deleted && op && (op->type == "delete" || (f && f->kind == "remove"));
    if (restore ? deleted || removes : !deleted) guarded([&] { restore ? restoreOp(id) : deleteOp(id); });
  });
  connect(m_browser, &BrowserPanel::sketchActivated, this, editOp);
  connect(m_browser,&BrowserPanel::editedSketchVisibilityRequested,this,[this]{auto* sketch=m_design->sketch();sketch->setVisible(!sketch->visible());});
  updateDesignState();
}

// Draw on drawing (Drafting > Home, UI-104 phase 1): a new sketch on the drawing's own plane and origin (the layers' frame,
// design::drawing_frame, as Drawing to sketch takes it), drawn with the sketch tools over the drawing; the Sketch tab comes
// first in Drafting. The selected drawing's, else the one drawing there is; drawings in several planes ask for a pick.
void MainWindow::drawOnDrawing() {
  if (!m_doc->hasDocument || m_doc->browse || m_design->busy() || m_design->sketchActive()) return;
  std::set<std::string> chosen;  // the drawings (import ops) of what is selected
  for (const auto& id : currentNodeIds())
    for (const auto& body : m_doc->scene.bodies_under(id))
      if (const opad::Node* n = m_doc->scene.node(body); n && n->representation == "drawing2d") chosen.insert(n->source_op);
  std::vector<opad::design::DrawingLayer> layers;
  for (const auto& id : m_doc->scene.all_bodies()) {
    const opad::Node* n = m_doc->scene.node(id);
    if (n && n->representation == "drawing2d" && n->raster.is_null() && (chosen.empty() || chosen.count(n->source_op))) layers.push_back({id, false});
  }
  if (layers.empty()) throw opad::UserHint("Open or import a 2D drawing first: the sketch is drawn on its plane.");
  opad::Frame frame;
  try {
    frame = opad::design::drawing_frame(m_doc->scene, layers);
  } catch (const std::exception&) {
    throw opad::UserHint("The drawings here lie in different planes: select a layer of the one to draw on.", true);
  }
  cancelTool();
  m_design->startSketchOn(opad::json{{"frame", frame.to_json()}}, frame);
}

// Sketch mode puts its Sketch tab first in Design (UI-104: the Design tabs stay beside it) and takes it away again, back in
// the workspace the sketch was started from; in Drafting (Draw on drawing, a drawing's sketch edited there) the tab comes
// first in Drafting instead. Tool buttons follow the editor's tool.
void MainWindow::updateDesignState() {
  const bool sketching = m_design->sketchActive();
  const bool has = m_doc->hasDocument;  // viewer mode too: the tools say that the file has to be saved first
  m_timeline->setEditingOp(m_design->editingOp());
  if (sketching && m_sketchTab.isEmpty()) {
    const bool drafting = m_workspaceId == "drafting" && m_ribbon->contextualTabs(int(m_workspaceIds.indexOf("drafting"))).contains("drafting.sketch");
    m_sketchTab = drafting ? "drafting.sketch" : "design.sketch";
    m_workspaceBeforeSketch = m_workspaceId;
    if (!drafting) {
      QScopedValueRollback<bool> automatic(m_automaticSwitch, true);  // not where the next start opens
      setWorkspace("design");
    }
    m_ribbon->setContextualTab(m_sketchTab, true);
  } else if (!sketching && !m_sketchTab.isEmpty()) {
    m_ribbon->setContextualTab(std::exchange(m_sketchTab, QString()), false);
    if (!m_workspaceBeforeSketch.isEmpty() && m_workspaceBeforeSketch != m_workspaceId) {
      QScopedValueRollback<bool> automatic(m_automaticSwitch, true);
      setWorkspace(m_workspaceBeforeSketch);
    }
    m_workspaceBeforeSketch.clear();
  }
  if (m_sketchMenu) m_sketchMenu->menuAction()->setVisible(sketching);
  const QString tool = sketching ? m_design->sketch()->tool() : QString();
  shortcuts::suspendOutsideSketch(m_actions, sketching);  // 5/6/7 and the filters' digits never act in a sketch (UI-16)
  for (QAction* a : m_actions) {
    const QString id = a->objectName();
    if (id.startsWith("sketch.")) {
      a->setEnabled(sketching);
      if (a->isCheckable()) a->setChecked(sketching && a->property("sketchTool").toString() == tool);
    } else if (id.startsWith("design.")) {
      a->setEnabled(has && !m_doc->loading && !(sketching && shortcuts::scope(id)==shortcuts::OutsideSketch));
    } else if (id.startsWith("select.") || id.startsWith("inspect.") || id.startsWith("annotate.") || id=="edit.selecttouched") {
      if (m_doc->hasDocument) a->setEnabled(!sketching || id == "inspect.clear");  // the left button draws while sketching
    }
  }
  action("view.alignPlane")->setEnabled(m_doc->hasDocument && !m_doc->loading);
  if(m_design->pickingPlane()) {
    m_prompt->hide(); // The side panel guides this flow; leave the corner selector unobstructed.
  } else if(sketching) updateSketchPrompt();
  else if(m_tool.id.isEmpty()) m_prompt->hide();
  m_browser->setEnabled(true);
  m_browser->setEditedSketch(sketching?(m_design->sketch()->sketchId().empty()?"active-sketch":m_design->sketch()->sketchId()):"",sketching?m_design->sketch()->name():QString(),!sketching || m_design->sketch()->visible());
  updateUndoActions();
  if (sketching) {
    const int dof = m_design->sketch()->dof();
    m_statusSel->setText(dof == 0 ? tr("Sketch fully constrained") : tr("Sketch · %1 degrees of freedom").arg(dof));
  }
  updateCommands();
}

// The keys say what they do now (UI-20; Shift as the pointer comes onto a guide, UI-19); with nothing to undo, end or
// close: how to finish the sketch.
void MainWindow::updateSketchPrompt() {
  QString hints=m_design->sketch()->keyHints();
  if(hints.isEmpty())if(const QString key=keys::text("sketch.finish");!key.isEmpty())hints=tr("%1 finish sketch").arg(key);  // its key now, none without one
  m_prompt->set("sketch",tr("Sketch"),m_design->sketch()->toolSteps(),m_design->sketch()->visible()?hints:tr("This sketch is hidden. Show it in the browser to see your edits."));
  m_prompt->show();positionOverlays();
}
