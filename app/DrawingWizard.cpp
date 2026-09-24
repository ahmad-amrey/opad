#include "MainWindow.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/feature.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QMessageBox>
#include <QTimer>

void MainWindow::drawingToSketch() {
  if(!m_doc->hasDocument || m_design->busy() || m_design->sketchActive()) return;
  QDialog dialog(this); dialog.setWindowTitle(tr("Drawing to sketch")); dialog.resize(640,480);
  auto* layout=new QVBoxLayout(&dialog);
  auto* hint=new QLabel(tr("Choose vector layers to flatten into one editable sketch. Construction curves remain guides and do not form extrusion profiles. Original layers are preserved."),&dialog);
  hint->setWordWrap(true); layout->addWidget(hint);
  auto* tree=new QTreeWidget(&dialog); tree->setHeaderLabels({tr("Include layer"),tr("Construction only")});
  const auto& colors=theme::current();
  tree->setStyleSheet(QString("QTreeView::indicator { width: 14px; height: 14px; border: 1px solid %1; background: %2; } QTreeView::indicator:checked { background: %3; border-color: %3; image: url(%4); }").arg(colors.fg3.name(),colors.bg2.name(),colors.sel.name(),icons::file("check",colors.onsel,14)));
  tree->setRootIsDecorated(false); tree->header()->setSectionResizeMode(0,QHeaderView::Stretch); layout->addWidget(tree,1);
  std::set<std::string> selected;
  for(const auto& id:currentNodeIds()) for(const auto& body:m_doc->scene.bodies_under(id)) selected.insert(body);
  for(const auto& id:m_doc->scene.all_bodies()) {
    const auto* n=m_doc->scene.node(id); if(n->representation!="drawing2d") continue;
    auto* row=new QTreeWidgetItem(tree); row->setText(0,QString::fromStdString(n->name)); row->setData(0,Qt::UserRole,QString::fromStdString(id));
    row->setIcon(0,icons::themed("drawing",16)); row->setCheckState(0,selected.empty() || selected.count(id)?Qt::Checked:Qt::Unchecked); row->setCheckState(1,Qt::Unchecked);
    if(!n->raster.is_null()) { row->setCheckState(0,Qt::Unchecked); row->setDisabled(true); row->setText(0,row->text(0)+tr(" (image: no vector curves)")); }
  }
  if(!tree->topLevelItemCount()) throw opad::Error("Import or select a 2D drawing before converting to a sketch.");
  auto* form=new QFormLayout; auto* name=new QLineEdit(tr("Converted drawing"),&dialog);
  auto* plane=new QComboBox(&dialog); plane->addItems({"XY","XZ","YZ"});
  auto* tolerance=new QDoubleSpinBox(&dialog); tolerance->setDecimals(4); tolerance->setRange(0.0001,10); tolerance->setValue(0.01); tolerance->setSuffix(" mm");
  form->addRow(tr("Sketch name"),name); form->addRow(tr("Flatten onto plane"),plane); form->addRow(tr("Curve tolerance"),tolerance); layout->addLayout(form);
  auto* note=new QLabel(tr("Lines and planar circular arcs stay analytic. Other curves become editable line segments within the chosen tolerance. Images are excluded."),&dialog); note->setWordWrap(true); layout->addWidget(note);
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); buttons->button(QDialogButtonBox::Ok)->setText(tr("Create sketch")); layout->addWidget(buttons);
  auto update=[&] { bool any=false; for(int i=0;i<tree->topLevelItemCount();++i) any|=tree->topLevelItem(i)->checkState(0)==Qt::Checked; buttons->button(QDialogButtonBox::Ok)->setEnabled(any && !name->text().trimmed().isEmpty()); };
  connect(tree,&QTreeWidget::itemChanged,&dialog,update); connect(name,&QLineEdit::textChanged,&dialog,update); update();
  connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_WIZARD");!shot.isEmpty()) QTimer::singleShot(250,&dialog,[&dialog,shot] { dialog.grab().save(shot); if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD_CREATE")) dialog.accept(); else dialog.reject(); });
  if(dialog.exec()!=QDialog::Accepted) return;
  std::vector<opad::design::DrawingLayer> layers;
  for(int i=0;i<tree->topLevelItemCount();++i) { auto* row=tree->topLevelItem(i); if(row->checkState(0)==Qt::Checked) layers.push_back({row->data(0,Qt::UserRole).toString().toStdString(),row->checkState(1)==Qt::Checked}); }
  const auto base=plane->currentText().toLower().toStdString(); const auto frame=opad::design::base_frame(base); const auto title=name->text().trimmed().toStdString(); const double tol=tolerance->value();
  auto snapshot=std::make_shared<opad::Document>(m_doc->doc); auto operation=std::make_shared<opad::json>();
  m_jobs->async(tr("Converting drawing layers"),[snapshot,operation,layers,frame,base,title,tol](Progress p) {
    auto sketch=opad::design::drawing_sketch(*snapshot,opad::resolve(*snapshot),layers,frame,tol); if(p.cancelled()) return;
    *operation=opad::design::make_sketch_op(title,{{"base",base},{"frame",frame.to_json()}},sketch.to_json());
    (*operation)["id"]=opad::new_uuid();
  },[this,snapshot,operation](bool ok,const QString& error) {
    if(!ok) { if(!error.isEmpty()) QMessageBox::warning(this,tr("Drawing to sketch"),error); return; }
    if(m_doc->doc.shape_cache!=snapshot->shape_cache || m_doc->doc.ops.size()!=snapshot->ops.size()) { QMessageBox::information(this,tr("Drawing to sketch"),tr("The document changed during conversion. Please run the wizard again.")); return; }
    const auto id=operation->at("id").get<std::string>();
    m_design->applyOps({*operation},tr("Convert drawing to sketch"),[this,id](bool applied,const QString& failure) {
      if(applied) { m_design->editOp(id); if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD_CREATE")) QTimer::singleShot(500,this,[this] { QCoreApplication::exit(m_design->sketchActive()?0:2); }); } else if(!failure.isEmpty()) QMessageBox::warning(this,tr("Drawing to sketch"),failure);
    });
  });
}
