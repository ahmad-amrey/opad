#include "SketchPanel.hpp"
#include "SketchEditor.hpp"
#include "HelpClip.hpp"
#include "I18n.hpp"
#include "Units.hpp"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTabBar>
#include <QFontComboBox>
#include <QFileDialog>

QList<SketchPanel::Tool> SketchPanel::tools() {
  return {
    {QObject::tr("Create"),"line",QObject::tr("Line")}, {QObject::tr("Create"),"rect",QObject::tr("Rectangle")},
    {QObject::tr("Create"),"crect",QObject::tr("Centre rectangle")}, {QObject::tr("Create"),"circle",QObject::tr("Circle")},
    {QObject::tr("Create"),"circle3",QObject::tr("3-point circle")}, {QObject::tr("Create"),"arc3",QObject::tr("3-point arc")},
    {QObject::tr("Create"),"arcc",QObject::tr("Centre arc")}, {QObject::tr("Create"),"ellipse",QObject::tr("Ellipse")},
    {QObject::tr("Create"),"slot",QObject::tr("Slot")}, {QObject::tr("Create"),"polygon",QObject::tr("Polygon")},
    {QObject::tr("Create"),"spline",QObject::tr("Spline")}, {QObject::tr("Create"),"point",QObject::tr("Point")},
    {QObject::tr("Create"),"rect3",QObject::tr("3-point rectangle")}, {QObject::tr("Create"),"circle2",QObject::tr("2-point circle")},
    {QObject::tr("Create"),"tangent_circle",QObject::tr("Tangent circle")}, {QObject::tr("Create"),"tangent_arc",QObject::tr("Tangent arc")},
    {QObject::tr("Create"),"cslot",QObject::tr("Centre slot")}, {QObject::tr("Create"),"arcslot",QObject::tr("Arc slot")},
    {QObject::tr("Create"),"polygon_outer",QObject::tr("Circumscribed polygon")}, {QObject::tr("Create"),"control_spline",QObject::tr("Control-point spline")},
    {QObject::tr("Create"),"conic",QObject::tr("Conic")}, {QObject::tr("Create"),"text",QObject::tr("Text outlines")},
    {QObject::tr("Modify"),"select",QObject::tr("Select")}, {QObject::tr("Modify"),"trim",QObject::tr("Trim")},
    {QObject::tr("Modify"),"fillet",QObject::tr("Sketch fillet")}, {QObject::tr("Modify"),"mirror",QObject::tr("Mirror")},
    {QObject::tr("Modify"),"offset",QObject::tr("Offset")},
    {QObject::tr("Modify"),"move",QObject::tr("Move")}, {QObject::tr("Modify"),"rotate",QObject::tr("Rotate")},
    {QObject::tr("Modify"),"scale",QObject::tr("Scale")}, {QObject::tr("Modify"),"copy",QObject::tr("Copy with offset")},
    {QObject::tr("Modify"),"rect_pattern",QObject::tr("Rectangular pattern")}, {QObject::tr("Modify"),"polar_pattern",QObject::tr("Polar pattern")},
    {QObject::tr("Modify"),"split",QObject::tr("Split curve")}, {QObject::tr("Modify"),"extend",QObject::tr("Extend curve")},
    {QObject::tr("Modify"),"break",QObject::tr("Break at intersections")}, {QObject::tr("Modify"),"chamfer",QObject::tr("Chamfer")},
    {QObject::tr("Modify"),"union",QObject::tr("Region union")}, {QObject::tr("Modify"),"subtract",QObject::tr("Region subtract")},
    {QObject::tr("Modify"),"intersect",QObject::tr("Region intersection")}, {QObject::tr("Modify"),"heal",QObject::tr("Heal endpoints")},
    {QObject::tr("Modify"),"explode",QObject::tr("Explode pattern")},
    {QObject::tr("Constrain"),"dimension",QObject::tr("Dimension")},
    {QObject::tr("Constrain"),"c:horizontal",QObject::tr("Horizontal")}, {QObject::tr("Constrain"),"c:vertical",QObject::tr("Vertical")},
    {QObject::tr("Constrain"),"c:coincident",QObject::tr("Coincident")}, {QObject::tr("Constrain"),"c:collinear",QObject::tr("Collinear")},
    {QObject::tr("Constrain"),"c:parallel",QObject::tr("Parallel")}, {QObject::tr("Constrain"),"c:perpendicular",QObject::tr("Perpendicular")},
    {QObject::tr("Constrain"),"c:tangent",QObject::tr("Tangent")}, {QObject::tr("Constrain"),"c:equal",QObject::tr("Equal")},
    {QObject::tr("Constrain"),"c:smooth",QObject::tr("Smooth spline join (G2)")},
    {QObject::tr("Constrain"),"c:curvature",QObject::tr("Equal endpoint curvature")},
    {QObject::tr("Constrain"),"c:concentric",QObject::tr("Concentric")}, {QObject::tr("Constrain"),"c:midpoint",QObject::tr("Midpoint")},
    {QObject::tr("Constrain"),"c:symmetric",QObject::tr("Symmetric")}, {QObject::tr("Constrain"),"c:fix",QObject::tr("Fix")},
    {QObject::tr("Reference"),"project",QObject::tr("Project")},
    {QObject::tr("Reference"),"intersect_body",QObject::tr("Body-plane intersection")},
    {QObject::tr("Reference"),"silhouette",QObject::tr("Silhouette")},
    {QObject::tr("Reference"),"include3d",QObject::tr("Include reference curves")},
    {QObject::tr("Reference"),"break_link",QObject::tr("Break projection link")},
    {QObject::tr("Images and files"),"image_insert",QObject::tr("Insert image")},
    {QObject::tr("Images and files"),"image_edit",QObject::tr("Transform image")},
    {QObject::tr("Images and files"),"image_calibrate",QObject::tr("Calibrate image")},
    {QObject::tr("Images and files"),"image_trace",QObject::tr("Trace image")},
    {QObject::tr("Images and files"),"image_remove",QObject::tr("Remove image")},
    {QObject::tr("Images and files"),"simplify",QObject::tr("Simplify curves")},
    {QObject::tr("Images and files"),"vector_import",QObject::tr("Import SVG / DXF")},
    {QObject::tr("Images and files"),"vector_export",QObject::tr("Export SVG / DXF")}
  };
}

SketchPanel::SketchPanel(SketchEditor* editor, QWidget* parent) : QWidget(parent), m_editor(editor) {
  auto* layout=new QVBoxLayout(this); layout->setContentsMargins(8,8,8,8); layout->setSpacing(6);
  m_state=new QLabel(this); m_state->setWordWrap(true); layout->addWidget(m_state);
  auto* tabs=new QTabWidget(this);m_pages=tabs;tabs->tabBar()->hide();layout->addWidget(tabs,1);
  auto page=[&](const QString& title) {
    auto* scroll=new QScrollArea(tabs); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* body=new QWidget(scroll); scroll->setWidget(body); tabs->addTab(scroll,title);
    auto* box=new QVBoxLayout(body); box->setContentsMargins(4,8,4,4); return box;
  };
  auto* tool=page(tr("Tool"));
  m_group=new QComboBox(this); m_tools=new QComboBox(this);
  for(const auto& t:tools()) if(m_group->findText(t.group)<0) m_group->addItem(t.group);
  m_group->hide();m_tools->hide();
  m_guide=new ToolGuide(this);tool->addWidget(m_guide);connect(m_guide,&ToolGuide::resized,this,&SketchPanel::contentChanged);
  m_steps=new ToolStepsPanel(this);m_steps->setSummary({},{},{});tool->addWidget(m_steps);
  m_fields=new QFormLayout; tool->addLayout(m_fields);
  auto* apply=new QPushButton(tr("Apply"),this); apply->setObjectName("primary"); layout->addWidget(apply);
  connect(tabs,&QTabWidget::currentChanged,apply,[apply](int index){apply->setVisible(index==0);});
  connect(apply,&QPushButton::clicked,editor,&SketchEditor::applyTool);
  m_precise=new QWidget(this);auto* precise=new QFormLayout(m_precise);tool->addWidget(m_precise);
  m_coordinates=new QComboBox(this); m_coordinates->addItems({tr("Absolute coordinates"),tr("Relative coordinates"),tr("Polar: length and angle")});
  m_u=new QLineEdit("0",this); m_v=new QLineEdit("0",this);
  precise->addRow(tr("Input"),m_coordinates); precise->addRow(tr("X / length"),m_u); precise->addRow(tr("Y / angle"),m_v);
  auto* place=new QPushButton(tr("Place point"),this); precise->addRow(place);
  auto submit=[this] { m_editor->placePrecise(m_u->text(),m_v->text(),m_coordinates->currentIndex()); };
  connect(place,&QPushButton::clicked,this,submit); connect(m_v,&QLineEdit::returnPressed,this,submit);
  tool->addStretch();
  auto* selection=page(tr("Select"));
  auto* filter=new QComboBox(this);
  for(const auto& [id,title]:QList<QPair<QString,QString>>{{"all",tr("All geometry")},{"point",tr("Points")},{"line",tr("Lines")},{"arc",tr("Arcs and circles")},{"spline",tr("Splines")},{"construction",tr("Construction")},{"constraint",tr("Constraints")},{"dimension",tr("Dimensions")}}) filter->addItem(title,id);
  selection->addWidget(filter);
  connect(filter,&QComboBox::currentIndexChanged,this,[this,filter]{m_editor->m_selectionFilter=filter->currentData().toString();});
  auto button=[&](QVBoxLayout* box,const QString& title,auto fn) {auto* b=new QPushButton(title,this);box->addWidget(b);connect(b,&QPushButton::clicked,this,fn);};
  button(selection,tr("Select connected chain"),[this]{m_editor->selectConnected();});
  button(selection,tr("Select all of type"),[this]{m_editor->selectType();});
  button(selection,tr("Construction"),[this]{m_editor->toggleConstruction();});
  button(selection,tr("Find open ends"),[this]{m_editor->findOpenVertices();});
  button(selection,tr("Spline node weights"),[this]{m_editor->editSplineNode();});
  button(selection,tr("Delete curve node"),[this]{m_editor->deleteNode();});
  button(selection,tr("Delete"),[this]{m_editor->deleteSelection();});
  selection->addStretch();
  auto* constraints=page(tr("Constraints"));
  auto* constraintFilter=new QLineEdit(this);constraintFilter->setPlaceholderText(tr("Filter constraints"));constraints->addWidget(constraintFilter);
  m_constraints=new QTreeWidget(this);m_constraints->setRootIsDecorated(false);m_constraints->setColumnCount(3);
  m_constraints->setHeaderLabels({tr("ID"),tr("Type"),tr("Value")}); m_constraints->setColumnWidth(0,45);m_constraints->setColumnWidth(1,105);
  constraints->addWidget(m_constraints,1);
  connect(constraintFilter,&QLineEdit::textChanged,this,[this](const QString& text){m_editor->m_constraintFilter=text;m_editor->rebuild();for(int i=0;i<m_constraints->topLevelItemCount();++i) {auto* r=m_constraints->topLevelItem(i);r->setHidden(!r->text(1).contains(text,Qt::CaseInsensitive));}});
  connect(m_constraints,&QTreeWidget::itemSelectionChanged,this,[this] {
    if(m_refreshing || !m_constraints->currentItem()) return;
    m_editor->invalidatePreview();m_editor->m_sel={m_constraints->currentItem()->data(0,Qt::UserRole).toInt()};m_editor->rebuild();
  });
  button(constraints,tr("Driving / reference"),[this]{m_editor->toggleReference();});
  button(constraints,tr("Edit dimension"),[this]{if(m_editor->m_sel.size()==1) m_editor->editDimension(m_editor->m_sel.front(),false);});
  button(constraints,tr("Delete"),[this]{m_editor->deleteSelection();});
  auto* settings=page(tr("Snaps"));
  auto* section=new QCheckBox(tr("Section at sketch plane"),this);settings->addWidget(section);
  connect(section,&QCheckBox::toggled,this,[this](bool on){auto normal=m_editor->m_frame.normal();for(auto& v:normal)v=-v;if(on)m_editor->m_viewport->setSection(true,m_editor->m_frame.origin,normal,false);else m_editor->m_viewport->restoreSection(m_editor->m_sectionBefore);});
  for(const auto& [key,title]:QList<QPair<QString,QString>>{{"endpoint",tr("Endpoints")},{"midpoint",tr("Midpoints")},{"center",tr("Centres")},{"quadrant",tr("Quadrants")},{"intersection",tr("Intersections")},{"nearest",tr("Nearest on curve")},{"grid",tr("Grid snapping")},{"angle",tr("Angle increments")},{"inference",tr("Automatic constraints")}}) {
    auto* check=new QCheckBox(title,this);check->setChecked(QSettings().value("sketch/snap/"+key,true).toBool());settings->addWidget(check);
    connect(check,&QCheckBox::toggled,this,[key](bool on){QSettings().setValue("sketch/snap/"+key,on);});
  }
  auto* advanced=new QFormLayout;settings->addLayout(advanced);
  auto* angle=new QDoubleSpinBox(this);angle->setRange(1,90);angle->setValue(QSettings().value("sketch/angleStep",15).toDouble());advanced->addRow(tr("Angle step"),angle);
  connect(angle,&QDoubleSpinBox::valueChanged,this,[](double v){QSettings().setValue("sketch/angleStep",v);});
  auto* tolerance=new QLineEdit(QSettings().value("sketch/tolerance","1e-8").toString(),this);advanced->addRow(tr("Solver tolerance"),tolerance);
  connect(tolerance,&QLineEdit::editingFinished,this,[tolerance]{bool ok=false;double v=tolerance->text().toDouble(&ok);if(ok && v>=1e-12 && v<=1e-2) QSettings().setValue("sketch/tolerance",v);else tolerance->setText(QSettings().value("sketch/tolerance","1e-8").toString());});
  auto* iterations=new QSpinBox(this);iterations->setRange(1,1000);iterations->setValue(QSettings().value("sketch/iterations",100).toInt());advanced->addRow(tr("Solver iterations"),iterations);
  connect(iterations,&QSpinBox::valueChanged,this,[](int v){QSettings().setValue("sketch/iterations",v);});settings->addStretch();
  m_status=new QLabel(this);m_status->setWordWrap(true);layout->addWidget(m_status);
  auto* footer=new QHBoxLayout;layout->addLayout(footer);
  // Finish sketch lives in the ribbon, next to Cancel sketch; the tool panel only steps back or leaves the tool.
  auto* back=new QPushButton(tr("Back"),this);auto* cancel=new QPushButton(tr("Cancel tool"),this);
  footer->addWidget(back);footer->addWidget(cancel);
  connect(back,&QPushButton::clicked,editor,&SketchEditor::stepBack);
  connect(cancel,&QPushButton::clicked,this,[this]{m_editor->setTool("select");});
  connect(editor,&SketchEditor::status,m_status,&QLabel::setText);
  connect(editor,&SketchEditor::changed,this,&SketchPanel::refresh);
  connect(editor,&SketchEditor::toolChanged,this,[this]{m_pages->setCurrentIndex(0);refresh();});
  connect(editor,&SketchEditor::workflowChanged,this,&SketchPanel::refresh);
  connect(units::notifier(),&units::Notifier::changed,this,[this]{m_editor->m_panelFieldsDirty=true;refresh();});  // values and defaults in the shown unit
  connect(m_group,&QComboBox::currentIndexChanged,this,&SketchPanel::chooseGroup);
  connect(m_tools,&QComboBox::currentIndexChanged,this,&SketchPanel::chooseTool);
  chooseGroup();
}

void SketchPanel::chooseGroup() {
  QSignalBlocker block(m_tools);m_tools->clear();
  for(const auto& t:tools()) if(t.group==m_group->currentText()) m_tools->addItem(t.label,t.id);
  if(!m_refreshing)chooseTool();
}
void SketchPanel::chooseTool() {if(!m_refreshing && m_tools->currentIndex()>=0)m_editor->setTool(m_tools->currentData().toString());}
QList<ToolStep> SketchPanel::steps() const {return m_editor->toolSteps();}
void SketchPanel::buildFields() {
  while(m_fields->rowCount()) m_fields->removeRow(0);
  auto field=[&](const QString& key,const QString& label,const QString& value) {
    auto* edit=new QLineEdit(m_editor->option(key,value),this);edit->setObjectName("sketchOption-"+key);m_fields->addRow(label,edit);
    connect(edit,&QLineEdit::textChanged,this,[this,key](const QString& text){m_editor->m_options[key]=text;m_editor->scheduleToolPreview();});
  };
  if(m_shown=="polygon" || m_shown=="polygon_outer")field("sides",tr("Number of sides:"),"6");
  if(m_shown=="fillet" || m_shown=="tangent_circle")field("radius",tr("Radius"),"2 mm");
  if(m_shown=="arcslot")field("width",tr("Slot width"),"2 mm");
  if(m_shown=="conic")field("rho",tr("Conic rho (0 to 1)"),"0.5");
  if(m_shown=="control_spline")field("degree",tr("Spline degree"),"3");
  if(m_shown=="text") {
    field("text",tr("Text"),"OPAD");field("height",tr("Text height"),"10 mm");
    auto* style=new QComboBox(this);style->addItem(tr("Outline font"),"outline");style->addItem(tr("Single-stroke font"),"stroke");style->addItem(tr("Built-in block letters"),"block");style->setCurrentIndex(style->findData(m_editor->option("textStyle","outline")));m_fields->addRow(tr("Style"),style);
    auto* font=new QFontComboBox(this);font->setCurrentFont(QFont(m_editor->option("font","Arial")));m_fields->addRow(tr("Font"),font);
    // The built-in letters have no font to choose: the list is off for them (it looked like it applied).
    font->setEnabled(style->currentData().toString()=="outline");
    connect(style,&QComboBox::currentIndexChanged,this,[this,style,font]{m_editor->m_options["textStyle"]=style->currentData().toString();font->setEnabled(style->currentData().toString()=="outline");});
    connect(font,&QFontComboBox::currentFontChanged,this,[this](const QFont& f){m_editor->m_options["font"]=f.family();});
  }
  auto choice=[&](const QString& key,const QString& label,const QList<QPair<QString,QString>>& choices) {
    auto* combo=new QComboBox(this);for(const auto& [id,text]:choices)combo->addItem(text,id);
    combo->setCurrentIndex(std::max(0,combo->findData(m_editor->option(key,choices.front().first))));m_fields->addRow(label,combo);
    connect(combo,&QComboBox::currentIndexChanged,this,[this,key,combo]{m_editor->m_options[key]=combo->currentData().toString();m_editor->scheduleToolPreview();if(key=="projectionPick")m_editor->referenceHover();});
  };
  if(m_shown=="project"||m_shown=="intersect_body"||m_shown=="silhouette"||m_shown=="include3d") {
    choice("projectionPick",tr("Pick filter"),{{"edge",tr("Edges")},{"face",tr("Faces")},{"vertex",tr("Vertices")},{"body",tr("Bodies")}});
    auto* sources=new QComboBox(this);sources->addItem(tr("Pick in the view"),m_editor->option("projectionSource"));
    auto add=[&](const QString& label,const opad::json& source){sources->addItem(label,QString::fromStdString(source.dump()));};
    for(const auto& id:m_editor->m_doc->scene.all_bodies())add(m_editor->m_doc->nodeName(id),{{"body",id},{"kind","body"}});
    for(const auto& sk:m_editor->m_doc->scene.sketches)if(sk.id!=m_editor->m_id)add(QString::fromStdString(sk.name),{{"sketch",sk.id}});
    for(const auto& feature:m_editor->m_doc->scene.features)if(feature.result.contains("axis"))add(QString::fromStdString(feature.name),{{"feature",feature.id}});
    for(const auto* axis:{"x","y","z"})add(tr("Origin axis %1").arg(axis),{{"base",axis}});
    m_fields->addRow(tr("Source"),sources);connect(sources,&QComboBox::currentIndexChanged,this,[this,sources]{m_editor->m_options["projectionSource"]=sources->currentData().toString();m_editor->invalidatePreview();m_editor->toolPrompt();});
    choice("projectionLinked",tr("Link behavior"),{{"1",tr("Associative link")},{"0",tr("Editable copy")}});
  }
  auto fileField=[&](const QString& key,bool image,bool save) {
    auto* row=new QWidget(this);auto* box=new QHBoxLayout(row);box->setContentsMargins(0,0,0,0);auto* path=new QLineEdit(m_editor->option(key),row);path->setReadOnly(true);auto* browse=new QPushButton(tr("Browse"),row);box->addWidget(path,1);box->addWidget(browse);m_fields->addRow(tr("File"),row);
    connect(browse,&QPushButton::clicked,this,[this,key,image,save,path]{const auto filter=image?tr("Images (*.png *.jpg *.jpeg *.bmp)"):tr("SVG (*.svg);;DXF (*.dxf)");const auto file=save?QFileDialog::getSaveFileName(this,tr("Export sketch"),path->text(),filter):QFileDialog::getOpenFileName(this,tr("Choose source file"),path->text(),filter);if(!file.isEmpty()){path->setText(file);m_editor->m_options[key]=file;m_editor->invalidatePreview();}});
  };
  if(m_shown=="image_insert"){fileField("imageFile",true,false);field("imageWidth",tr("Image width"),"100 mm");}
  if(m_shown.startsWith("image_")&&m_shown!="image_insert") {
    auto* images=new QComboBox(this);for(const auto& image:m_editor->m_sk.images)images->addItem(tr("Image %1").arg(image.at("id").get<int>()),image.at("id").get<int>());
    const int id=m_editor->option("imageId",m_editor->m_sk.images.empty()?"0":QString::number(m_editor->m_sk.images.back().at("id").get<int>())).toInt();images->setCurrentIndex(images->findData(id));m_fields->addRow(tr("Backdrop"),images);
    connect(images,&QComboBox::currentIndexChanged,this,[this,images]{m_editor->m_options["imageId"]=images->currentData().toString();m_editor->invalidatePreview();m_editor->m_panelFieldsDirty=true;refresh();});
    if(m_shown=="image_edit") {
      for(const auto& image:m_editor->m_sk.images)if(image.at("id").get<int>()==id) {
        m_editor->m_options["imageX"]=units::editable(units::Kind::Length,image.at("position")[0].get<double>());m_editor->m_options["imageY"]=units::editable(units::Kind::Length,image.at("position")[1].get<double>());
        m_editor->m_options["imageWidth"]=units::editable(units::Kind::Length,image.at("width").get<double>());m_editor->m_options["imageAngle"]=units::editable(units::Kind::Angle,image.value("angle",0.0)*180/M_PI);m_editor->m_options["imageOpacity"]=QString::number(image.value("opacity",.5));
      }
      field("imageX",tr("X position"),"0 mm");field("imageY",tr("Y position"),"0 mm");field("imageWidth",tr("Image width"),"100 mm");field("imageAngle",tr("Rotation"),"0 deg");field("imageOpacity",tr("Opacity (0 to 1)"),"0.5");
    }
  }
  if(m_shown=="image_calibrate")field("knownDistance",tr("Known distance"),"10 mm");
  if(m_shown=="image_trace") {
    field("threshold",tr("Threshold (0 to 255)"),"128");field("smoothing",tr("Smoothing (0 to 10 pixels)"),"1");field("noise",tr("Minimum area in pixels"),"8");field("traceTolerance",tr("Trace tolerance in pixels"),"0.75");field("cornerAngle",tr("Preserve corners above (degrees)"),"60");choice("invert",tr("Foreground"),{{"0",tr("Dark")},{"1",tr("Light")}});
  }
  if(m_shown=="vector_import"||m_shown=="vector_export")fileField("vectorFile",false,m_shown=="vector_export");
  if(m_shown=="vector_import"||m_shown=="simplify")field("curveTolerance",tr("Curve tolerance"),"0.01 mm");
  if(QStringList{"image_trace","project","intersect_body","silhouette","include3d","offset","move","rotate","scale","copy","mirror","rect_pattern","polar_pattern","union","subtract","intersect","heal","chamfer","simplify"}.contains(m_shown)) {
    auto* preview=new QPushButton(tr("Preview"),this);m_fields->addRow(preview);connect(preview,&QPushButton::clicked,m_editor,&SketchEditor::previewTool);
  }
  if(m_shown=="offset") {field("distance",tr("Distance"),"5 mm");choice("corners",tr("Corners"),{{"round",tr("Round")},{"sharp",tr("Sharp")}});}
  if(QStringList{"offset","move","copy","rotate","scale","mirror","rect_pattern","polar_pattern"}.contains(m_shown)) {
    auto* chain=new QCheckBox(tr("Select connected chain on click"),this);chain->setChecked(m_editor->option("chain",m_shown=="offset"?"1":"0")=="1");m_fields->addRow(chain);
    connect(chain,&QCheckBox::toggled,this,[this](bool on){m_editor->m_options["chain"]=on?"1":"0";});
  }
  if(m_shown=="move"||m_shown=="copy"||m_shown=="rect_pattern") {field("dx",tr("X offset"),"10 mm");field("dy",tr("Y offset"),"0 mm");}
  if(m_shown=="rotate"||m_shown=="scale"||m_shown=="polar_pattern") {field("cx",tr("Centre X"),"0 mm");field("cy",tr("Centre Y"),"0 mm");}
  if(m_shown=="rotate"||m_shown=="polar_pattern")field("angle",tr("Angle"),m_shown=="rotate"?"45 deg":"360 deg");
  if(m_shown=="scale")field("scale",tr("Scale factor"),"2");
  if(m_shown=="rect_pattern"||m_shown=="polar_pattern")field("count",tr("Count"),"3");
  if(m_shown=="rect_pattern")field("rows",tr("Rows"),"1");
  if(m_shown=="chamfer") {field("first",tr("First distance"),"2 mm");field("second",tr("Second distance"),"2 mm");}
  if(m_shown=="heal")field("healTolerance",tr("Gap tolerance"),"0.05 mm");
  if(m_shown=="mirror") {
    choice("mirrorAxis",tr("Mirror axis"),{{"picked",tr("Picked line")},{"x",tr("X axis")},{"y",tr("Y axis")}});
    auto* pick=new QPushButton(tr("Pick mirror line"),this);m_fields->addRow(pick);
    connect(pick,&QPushButton::clicked,this,[this]{m_editor->m_options["mirrorStage"]="axis";m_editor->m_options["mirrorAxis"]="picked";m_editor->toolPrompt();});
  }
  if(m_shown=="node") {
    field("weight",tr("Node weight"),"1");field("incoming",tr("Incoming handle weight"),"1");field("outgoing",tr("Outgoing handle weight"),"1");
  }
  if(m_shown=="dimension") {
    if(!m_editor->m_dimEditing)choice("dimensionType",tr("Dimension type"),{{"auto",tr("Automatic")},{"distance",tr("Aligned distance")},{"hdistance",tr("Horizontal distance")},{"vdistance",tr("Vertical distance")},{"angle",tr("Angle")},{"radius",tr("Radius")},{"diameter",tr("Diameter")},{"arc_length",tr("Arc length")}});
    else field("expression",tr("Expression"),m_editor->m_dimEdit?m_editor->m_dimEdit->text():"10 mm");
    auto* reference=new QCheckBox(tr("Reference dimension"),this);m_fields->addRow(reference);
    reference->setChecked(m_editor->option("reference","0")=="1");
    if(auto* c=m_editor->m_sk.constraint(m_editor->m_dimEditing))reference->setChecked(c->reference);
    connect(reference,&QCheckBox::toggled,this,[this](bool on){m_editor->m_options["reference"]=on?"1":"0";});
  }
}
void SketchPanel::refresh() {
  if(!m_editor->active())return;
  m_refreshing=true;
  const QString tool=m_editor->tool();
  m_precise->setVisible(QStringList{"line","rect","crect","circle","circle2","circle3","arc3","arcc","polygon","polygon_outer","slot","cslot","arcslot","ellipse","spline","control_spline","point","text","conic","rect3"}.contains(tool));
  for(const auto& t:tools())if(t.id==tool) {
    {QSignalBlocker block(m_group);m_group->setCurrentText(t.group);}chooseGroup();
    QSignalBlocker block(m_tools);m_tools->setCurrentIndex(m_tools->findData(tool));break;
  }
  if(m_shown!=tool)m_guide->setCommand("sketch."+QString(tool).replace(':','.'));
  if(m_shown!=tool || m_editor->m_panelFieldsDirty) {m_shown=tool;m_editor->m_panelFieldsDirty=false;buildFields();}
  for(auto* edit:findChildren<QLineEdit*>())if(edit->objectName().startsWith("sketchOption-") && !edit->hasFocus()) {
    const auto key=edit->objectName().mid(13);if(m_editor->m_options.contains(key)){QSignalBlocker block(edit);edit->setText(m_editor->option(key));}
  }
  const QList<ToolStep> now=steps();
  m_steps->setSteps(now,{});
  m_guide->setWaiting(int(std::find_if(now.begin(),now.end(),[](const ToolStep& s){return s.picked.isEmpty();})-now.begin()),int(now.size()));
  // The measurement widget owns an inner scroll area: give its numbered rows room before Qt's deferred
  // show/layout pass (minimumSizeHint otherwise sees newly created rows as hidden and collapses them).
  int stepsHeight=20;
  for(const auto& step:steps())stepsHeight+=fontMetrics().boundingRect(QRect(0,0,std::max(200,width()-90),1000),Qt::TextWordWrap,step.label).height()+14+(step.picked.isEmpty()?0:fontMetrics().height()+3);
  m_steps->setFixedHeight(stepsHeight);
  m_state->setText((m_editor->visible()?QString():tr("This sketch is hidden. Show it in the browser to see your edits.")+"\n")+(m_editor->modified()?tr("Modified sketch"):tr("Sketch"))+tr(" · %1 degrees of freedom").arg(m_editor->dof()));
  const int selected=m_constraints->currentItem()?m_constraints->currentItem()->data(0,Qt::UserRole).toInt():0;
  m_constraints->clear();
  for(const auto& c:m_editor->m_sk.constraints) {
    auto* row=new QTreeWidgetItem(m_constraints);row->setData(0,Qt::UserRole,c.id);
    row->setText(0,QString(c.is_dimension()?"d%1":"%1").arg(c.id));
    {  // The type's id is data; untranslated it is shown as a word ("point_on_curve" -> "Point on curve")
      const QString id=QString::fromLatin1(opad::design::SkConstraint::type_name(c.type));QString type=i18n::t(id);
      if(type==id){type.replace('_',' ');if(!type.isEmpty())type[0]=type[0].toUpper();}
      row->setText(1,type);
    }
    if(c.is_dimension())row->setText(2,m_editor->dimensionText(c));
    row->setHidden(!row->text(1).contains(m_editor->m_constraintFilter,Qt::CaseInsensitive));
    if(m_editor->m_conflicts.count(c.id))row->setForeground(1,Qt::red);
    if(c.id==selected)m_constraints->setCurrentItem(row);
  }
  m_refreshing=false;
  // Rebuilt fields after an edit: the panel was fitted to the old ones and cut the new off. A turn later: Qt shows new
  // children of a visible widget by a queued call, and until then the layout counts them as hidden.
  QTimer::singleShot(0,this,[this]{emit contentChanged();});
}
void SketchPanel::showPage(int page){m_pages->setCurrentIndex(page);refresh();}
QSize SketchPanel::toolSizeHint(int width) const {
  if(m_pages->currentIndex()!=0)return {width,440};
  // The panel as laid out at this width, with the scrolled tool page at its full height. A fixed allowance for the
  // rest cut off a tool's last fields (Project's Preview) once its prompt wrapped to two lines.
  const int w=std::max(120,width>0?width:340);
  QLayout* box=layout();
  const int outer=box->hasHeightForWidth()?box->heightForWidth(w):box->sizeHint().height();
  const auto* page=qobject_cast<QScrollArea*>(m_pages->widget(0));
  const QWidget* body=page?page->widget():nullptr;
  const int inner=!body?0:body->hasHeightForWidth()?body->heightForWidth(w-8):body->sizeHint().height();
  return {width,std::clamp(outer-m_pages->sizeHint().height()+inner+8,300,640)};
}
