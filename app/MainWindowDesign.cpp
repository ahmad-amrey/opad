#include "MainWindow.hpp"
#include "DrawingPlacer.hpp"

#include <QActionGroup>
#include <QColorDialog>
#include <QMap>
#include <QMenu>
#include <QMessageBox>

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

#include "I18n.hpp"
#include "Icons.hpp"
#include "opad/design/feature.hpp"

// ---------------------------------------------------------------- design workspace
// Feature tools come from the core's spec table (one action per kind, the form is generic); sketch tools are
// only live while a sketch is open, when the ribbon shows the contextual Sketch tab set.
void MainWindow::buildDesignActions() {
  addAction("design.convertDrawing",tr("Drawing to sketch"),"drawing",QKeySequence(),[this] { drawingToSketch(); });
  addAction("design.sketch", tr("New sketch"), "sketch", QKeySequence(), [this] { m_design->startSketch(); });
  static const std::map<std::string, const char*> kKeys = {{"extrude", "E"}, {"offset_face", "Q"}, {"move", "M"}};
  for (const auto& spec : opad::design::feature_specs()) {
    const QString kind = QString::fromStdString(spec.kind);
    const auto key = kKeys.find(spec.kind);
    QAction* a = addAction("design." + kind, i18n::t(QString::fromStdString(spec.label)), QString::fromStdString(spec.icon), key == kKeys.end() ? QKeySequence() : QKeySequence(key->second),
                           [this, kind] { m_design->startFeature(kind); });
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
  for(const auto& group:QList<QPair<QString,QString>>{{"Create",tr("Create")},{"Modify",tr("Modify")},{"Constrain",tr("Constrain")},{"Reference",tr("Reference")},{"Files",tr("Images and files")}}) {
    auto* a=addAction("sketch.more"+group.first,group.first=="Files"?group.second:tr("More tools"),group.first=="Files"?"image":"more",{},[]{});
    auto* menu=new QMenu(this);for(const auto& tool:registry)if(tool.group==group.second)menu->addAction(action("sketch."+QString(tool.id).replace(':','.')));
    a->setMenu(menu);
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
  connect(m_design, &DesignController::failed, this, [this](const QString& error) { QMessageBox::warning(this, tr("OPAD"), i18n::t(error)); });
  connect(m_design, &DesignController::stateChanged, this, &MainWindow::updateDesignState);
  connect(m_design->sketch(), &SketchEditor::hintsChanged, this, [this] { if (m_design->sketchActive() && !m_design->pickingPlane()) updateSketchPrompt(); });
  auto editOp = [this](const std::string& id) {  // a read-only document asks for a copy first
    auto edit = [this, id] { guarded([&] { m_design->editOp(id); }); };
    if (requireEditable(edit)) edit();
  };
  connect(m_timeline, &TimelineWidget::opActivated, this, editOp);
  connect(m_browser, &BrowserPanel::sketchActivated, this, editOp);
  connect(m_browser,&BrowserPanel::editedSketchVisibilityRequested,this,[this]{auto* sketch=m_design->sketch();sketch->setVisible(!sketch->visible());});
  updateDesignState();
}

// Sketch mode swaps the ribbon to its own tab set and back; tool buttons follow the editor's tool.
void MainWindow::updateDesignState() {
  const bool sketching = m_design->sketchActive();
  const bool has = m_doc->hasDocument;  // viewer mode too: the tools say that the file has to be saved first
  m_timeline->setEditingOp(m_design->editingOp());
  if (sketching && m_ribbon->workspace() != m_sketchWorkspace) {
    m_workspaceBeforeSketch = m_ribbon->workspace();
    m_ribbon->setWorkspace(m_sketchWorkspace);
  } else if (!sketching && m_ribbon->workspace() == m_sketchWorkspace) {
    m_ribbon->setWorkspace(m_workspaceBeforeSketch);
  }
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
  if(hints.isEmpty())hints=tr("%1 finish sketch").arg(action("sketch.finish")->shortcut().toString(QKeySequence::NativeText));
  m_prompt->set("sketch",tr("Sketch"),m_design->sketch()->toolSteps(),m_design->sketch()->visible()?hints:tr("This sketch is hidden. Show it in the browser to see your edits."));
  m_prompt->show();positionOverlays();
}
