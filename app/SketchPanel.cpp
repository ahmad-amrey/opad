#include "SketchPanel.hpp"
#include "SketchEditor.hpp"
#include "I18n.hpp"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QFontComboBox>

namespace {
struct Tool { QString group, id, label; };
QList<Tool> tools() {
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
    {QObject::tr("Constrain"),"c:concentric",QObject::tr("Concentric")}, {QObject::tr("Constrain"),"c:midpoint",QObject::tr("Midpoint")},
    {QObject::tr("Constrain"),"c:symmetric",QObject::tr("Symmetric")}, {QObject::tr("Constrain"),"c:fix",QObject::tr("Fix")},
    {QObject::tr("Reference"),"project",QObject::tr("Project")}
  };
}
}

SketchPanel::SketchPanel(SketchEditor* editor, QWidget* parent) : QWidget(parent), m_editor(editor) {
  auto* layout=new QVBoxLayout(this); layout->setContentsMargins(8,8,8,8); layout->setSpacing(6);
  m_state=new QLabel(this); m_state->setWordWrap(true); layout->addWidget(m_state);
  auto* tabs=new QTabWidget(this); layout->addWidget(tabs,1);
  auto page=[&](const QString& title) {
    auto* scroll=new QScrollArea(tabs); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* body=new QWidget(scroll); scroll->setWidget(body); tabs->addTab(scroll,title);
    auto* box=new QVBoxLayout(body); box->setContentsMargins(4,8,4,4); return box;
  };
  auto* tool=page(tr("Tool"));
  m_group=new QComboBox(this); m_tools=new QComboBox(this);
  for(const auto& t:tools()) if(m_group->findText(t.group)<0) m_group->addItem(t.group);
  tool->addWidget(m_group); tool->addWidget(m_tools);
  m_steps=new ToolStepsPanel(this); tool->addWidget(m_steps);
  m_fields=new QFormLayout; tool->addLayout(m_fields);
  auto* apply=new QPushButton(tr("Apply"),this); apply->setObjectName("primary"); tool->addWidget(apply);
  connect(apply,&QPushButton::clicked,editor,&SketchEditor::applyTool);
  auto* precise=new QFormLayout; tool->addLayout(precise);
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
  connect(constraintFilter,&QLineEdit::textChanged,this,[this](const QString& text){for(int i=0;i<m_constraints->topLevelItemCount();++i) {auto* r=m_constraints->topLevelItem(i);r->setHidden(!r->text(1).contains(text,Qt::CaseInsensitive));}});
  connect(m_constraints,&QTreeWidget::itemSelectionChanged,this,[this] {
    if(m_refreshing || !m_constraints->currentItem()) return;
    m_editor->m_sel={m_constraints->currentItem()->data(0,Qt::UserRole).toInt()};m_editor->rebuild();
  });
  button(constraints,tr("Driving / reference"),[this]{m_editor->toggleReference();});
  button(constraints,tr("Edit dimension"),[this]{if(m_editor->m_sel.size()==1) m_editor->editDimension(m_editor->m_sel.front(),false);});
  button(constraints,tr("Delete"),[this]{m_editor->deleteSelection();});
  auto* settings=page(tr("Snaps"));
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
  auto* back=new QPushButton(tr("Back"),this);auto* cancel=new QPushButton(tr("Cancel tool"),this);auto* finish=new QPushButton(tr("Finish sketch"),this);
  footer->addWidget(back);footer->addWidget(cancel);footer->addWidget(finish);
  connect(back,&QPushButton::clicked,editor,&SketchEditor::stepBack);
  connect(cancel,&QPushButton::clicked,this,[this]{m_editor->setTool("select");});
  connect(finish,&QPushButton::clicked,this,&SketchPanel::finishRequested);
  connect(editor,&SketchEditor::status,m_status,&QLabel::setText);
  connect(editor,&SketchEditor::changed,this,&SketchPanel::refresh);
  connect(editor,&SketchEditor::toolChanged,this,&SketchPanel::refresh);
  connect(editor,&SketchEditor::workflowChanged,this,&SketchPanel::refresh);
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
    auto* edit=new QLineEdit(m_editor->option(key,value),this);m_fields->addRow(label,edit);
    connect(edit,&QLineEdit::textChanged,this,[this,key](const QString& text){m_editor->m_options[key]=text;});
  };
  if(m_shown=="polygon" || m_shown=="polygon_outer")field("sides",tr("Number of sides:"),"6");
  if(m_shown=="fillet" || m_shown=="tangent_circle")field("radius",tr("Radius"),"2 mm");
  if(m_shown=="arcslot")field("width",tr("Slot width"),"2 mm");
  if(m_shown=="conic")field("rho",tr("Conic rho (0 to 1)"),"0.5");
  if(m_shown=="control_spline")field("degree",tr("Spline degree"),"3");
  if(m_shown=="text") {
    field("text",tr("Text"),"OPAD");field("height",tr("Text height"),"10 mm");
    auto* style=new QComboBox(this);style->addItem(tr("Outline font"),"outline");style->addItem(tr("Single-stroke font"),"stroke");style->setCurrentIndex(style->findData(m_editor->option("textStyle","outline")));m_fields->addRow(tr("Style"),style);
    connect(style,&QComboBox::currentIndexChanged,this,[this,style]{m_editor->m_options["textStyle"]=style->currentData().toString();});
    auto* font=new QFontComboBox(this);font->setCurrentFont(QFont(m_editor->option("font","Arial")));m_fields->addRow(tr("Font"),font);
    connect(font,&QFontComboBox::currentFontChanged,this,[this](const QFont& f){m_editor->m_options["font"]=f.family();});
  }
  auto choice=[&](const QString& key,const QString& label,const QList<QPair<QString,QString>>& choices) {
    auto* combo=new QComboBox(this);for(const auto& [id,text]:choices)combo->addItem(text,id);
    combo->setCurrentIndex(std::max(0,combo->findData(m_editor->option(key,choices.front().first))));m_fields->addRow(label,combo);
    connect(combo,&QComboBox::currentIndexChanged,this,[this,key,combo]{m_editor->m_options[key]=combo->currentData().toString();});
  };
  if(m_shown=="offset") {field("distance",tr("Distance"),"5 mm");choice("corners",tr("Corners"),{{"round",tr("Round")},{"sharp",tr("Sharp")}});}
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
  if(m_shown=="dimension" && m_editor->m_dimEditing) {
    field("expression",tr("Expression"),m_editor->m_dimEdit?m_editor->m_dimEdit->text():"10 mm");
    auto* reference=new QCheckBox(tr("Reference dimension"),this);m_fields->addRow(reference);
    if(auto* c=m_editor->m_sk.constraint(m_editor->m_dimEditing))reference->setChecked(c->reference);
    connect(reference,&QCheckBox::toggled,this,[this](bool on){m_editor->m_options["reference"]=on?"1":"0";});
  }
}
void SketchPanel::refresh() {
  if(!m_editor->active())return;
  m_refreshing=true;
  const QString tool=m_editor->tool();
  for(const auto& t:tools())if(t.id==tool) {
    {QSignalBlocker block(m_group);m_group->setCurrentText(t.group);}chooseGroup();
    QSignalBlocker block(m_tools);m_tools->setCurrentIndex(m_tools->findData(tool));break;
  }
  if(m_shown!=tool || m_editor->m_panelFieldsDirty) {m_shown=tool;m_editor->m_panelFieldsDirty=false;buildFields();}
  m_steps->setSteps(steps(),{});
  // The measurement widget owns an inner scroll area: give its numbered rows room before Qt's deferred
  // show/layout pass (minimumSizeHint otherwise sees newly created rows as hidden and collapses them).
  int stepsHeight=52;
  for(const auto& step:steps())stepsHeight+=fontMetrics().boundingRect(QRect(0,0,std::max(200,width()-90),1000),Qt::TextWordWrap,step.label).height()+14+(step.picked.isEmpty()?0:fontMetrics().height()+3);
  m_steps->setFixedHeight(stepsHeight);
  m_state->setText((m_editor->modified()?tr("Modified sketch"):tr("Sketch"))+tr(" · %1 degrees of freedom").arg(m_editor->dof()));
  const int selected=m_constraints->currentItem()?m_constraints->currentItem()->data(0,Qt::UserRole).toInt():0;
  m_constraints->clear();
  for(const auto& c:m_editor->m_sk.constraints) {
    auto* row=new QTreeWidgetItem(m_constraints);row->setData(0,Qt::UserRole,c.id);
    row->setText(0,QString(c.is_dimension()?"d%1":"%1").arg(c.id));
    row->setText(1,i18n::t(QString::fromLatin1(opad::design::SkConstraint::type_name(c.type))));
    if(c.is_dimension())row->setText(2,m_editor->dimensionText(c));
    if(c.id==selected)m_constraints->setCurrentItem(row);
  }
  m_refreshing=false;
}
