#include "MainWindow.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/feature.hpp"
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Compound.hxx>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QTimer>
#include <QPointer>

void MainWindow::drawingToSketch() {
  if(!m_doc->hasDocument || m_design->busy() || m_design->sketchActive()) return;
  if(auto* existing=findChild<ToolPanel*>("drawingWizard")) { existing->show(); existing->raise(); return; }
  cancelTool();
  auto* dialog=new QWidget;
  auto* panel=new ToolPanel("drawing","drawing",&Tokens::sel,tr("Drawing to sketch"),dialog,540,this);
  panel->setObjectName("drawingWizard");
  m_panels<<panel;
  connect(panel,&QObject::destroyed,this,[this,panel] {m_panels.removeAll(panel);});
  auto* layout=new QVBoxLayout(dialog);
  auto* hint=new QLabel(tr("1. Pick an origin plane or planar face in the scene.\n2. Choose layers and preview the converted curves."),dialog);
  hint->setWordWrap(true); layout->addWidget(hint);
  auto* pick=new QPushButton(tr("Pick sketch plane in scene"),dialog); layout->addWidget(pick);
  auto* tree=new QTreeWidget(dialog); tree->setHeaderLabels({tr("Include layer"),tr("Construction only")});
  const auto& colors=theme::current();
  tree->setStyleSheet(QString("QTreeView::indicator { width: 14px; height: 14px; border: 1px solid %1; background: %2; } QTreeView::indicator:checked { background: %3; image: url(%4); }").arg(colors.fg3.name(),colors.bg2.name(),colors.sel.name(),icons::file("check",colors.onsel,14)));
  tree->setRootIsDecorated(false); tree->header()->setSectionResizeMode(0,QHeaderView::Stretch); layout->addWidget(tree,1);
  std::set<std::string> selected;
  for(const auto& id:currentNodeIds()) for(const auto& body:m_doc->scene.bodies_under(id)) selected.insert(body);
  for(const auto& id:m_doc->scene.all_bodies()) {
    const auto* n=m_doc->scene.node(id); if(n->representation!="drawing2d") continue;
    auto* row=new QTreeWidgetItem(tree); row->setText(0,QString::fromStdString(n->name)); row->setData(0,Qt::UserRole,QString::fromStdString(id));
    row->setIcon(0,icons::themed("drawing",16)); row->setCheckState(0,selected.empty() || selected.count(id)?Qt::Checked:Qt::Unchecked); row->setCheckState(1,Qt::Unchecked);
    if(!n->raster.is_null()) { row->setCheckState(0,Qt::Unchecked); row->setDisabled(true); row->setText(0,row->text(0)+tr(" (image: no vector curves)")); }
  }
  if(!tree->topLevelItemCount()) { panel->deleteLater(); throw opad::Error("Import a 2D drawing before converting to a sketch."); }
  auto* form=new QFormLayout; auto* name=new QLineEdit(tr("Converted drawing"),dialog);
  for(int i=0;i<tree->topLevelItemCount();++i)if(tree->topLevelItem(i)->checkState(0)==Qt::Checked){name->setText(tree->topLevelItem(i)->text(0));break;}
  auto* tolerance=new QDoubleSpinBox(dialog); tolerance->setDecimals(4); tolerance->setRange(0.0001,10); tolerance->setValue(0.01); tolerance->setSuffix(" mm");
  form->addRow(tr("Sketch name"),name); form->addRow(tr("Curve tolerance"),tolerance); layout->addLayout(form);
  auto* preview=new QCheckBox(tr("Preview converted curves"),dialog); layout->addWidget(preview);
  auto* removeSource=new QCheckBox(tr("Remove source drawing after conversion"),dialog);layout->addWidget(removeSource);
  auto* note=new QLabel(tr("Native curves stay exact. Tolerance controls reconstruction of segmented curves. Corners and construction layers are preserved."),dialog); note->setWordWrap(true); layout->addWidget(note);
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,dialog); buttons->button(QDialogButtonBox::Ok)->setText(tr("Create sketch")); layout->addWidget(buttons);
  struct State { opad::json plane; opad::Frame frame; int serial=0; QPointer<Job> job; bool applying=false,closed=false,choosing=false; };
  auto state=std::make_shared<State>(); QPointer<QWidget> guard(dialog);
  const auto generation=m_doc->generation;
  auto layers=[tree] { std::vector<opad::design::DrawingLayer> out; for(int i=0;i<tree->topLevelItemCount();++i) {auto* r=tree->topLevelItem(i); if(r->checkState(0)==Qt::Checked) out.push_back({r->data(0,Qt::UserRole).toString().toStdString(),r->checkState(1)==Qt::Checked});} return out; };
  auto update=[=,this] {
    std::set<std::string> chosen;for(const auto& l:layers())chosen.insert(l.id);
    bool complete=!chosen.empty();
    for(const auto& id:chosen)for(const auto& body:m_doc->scene.all_bodies())
      if(m_doc->scene.node(body)->source_op==m_doc->scene.node(id)->source_op && !chosen.count(body))complete=false;
    removeSource->setEnabled(complete);if(!complete)removeSource->setChecked(false);
    removeSource->setToolTip(tr("Select all layers of a source drawing to remove it."));
    if(!state->closed && !m_design->pickingPlane()) {
      m_prompt->set("drawing",tr("Drawing to sketch"),{{tr("Pick a plane"),state->plane.is_null()?QString():tr("Plane selected")},{tr("Choose layers and create sketch"),{}}},tr("Esc cancels"));
      m_prompt->show();positionOverlays();
    }
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!state->applying && !state->plane.is_null() && !layers().empty() && !name->text().trimmed().isEmpty()); };
  auto* debounce=new QTimer(dialog); debounce->setSingleShot(true); debounce->setInterval(250);
  auto convert=[=,this](bool commit) {
    if(state->plane.is_null() || layers().empty() || state->applying) return;
    const int serial=++state->serial; if(state->job) state->job->cancel();
    auto snapshot=std::make_shared<opad::Document>(m_doc->doc); auto geometry=std::make_shared<opad::design::Sketch>(); auto shape=std::make_shared<TopoDS_Compound>();
    auto presentation=std::make_shared<std::shared_ptr<BodyPrs>>();
    const auto chosen=layers(); const auto plane=state->plane; const auto frame=state->frame; const double tol=tolerance->value(); const auto title=name->text().trimmed().toStdString();
    if(commit) { state->applying=true; update(); }
    state->job=m_jobs->async(commit?tr("Converting drawing layers"):tr("Previewing curves"),[=](Progress p) {
      *geometry=opad::design::drawing_sketch(*snapshot,opad::resolve(*snapshot),chosen,frame,tol); if(p.cancelled()) return;
      if(!commit) { BRep_Builder b; b.MakeCompound(*shape); for(const auto& e:opad::design::sketch_edges(*geometry,frame,true)) b.Add(*shape,e);Bnd_Box bounds;BRepBndLib::Add(*shape,bounds);*presentation=BodyPrs::build(*shape,bounds); }
    },[=,this](bool ok,const QString& error) {
      if(!guard || state->closed || serial!=state->serial || generation!=m_doc->generation) return;
      state->job=nullptr;
      if(!ok) { state->applying=false; note->setText(error); update(); return; }
      if(!commit) { if(preview->isChecked()) { std::vector<std::string> hidden; for(const auto& l:chosen) hidden.push_back(l.id); m_viewport->setPreviewCurves(*shape,*presentation,hidden); } return; }
      if(snapshot->ops.size()!=m_doc->doc.ops.size()) { state->applying=false; note->setText(tr("Document changed. Please retry.")); update(); return; }
      auto op=opad::design::make_sketch_op(title,plane,geometry->to_json());
      std::vector<opad::json> ops{op};
      if(removeSource->isChecked()) {
        std::set<std::string> sources;for(const auto& layer:chosen)sources.insert(m_doc->scene.node(layer.id)->source_op);
        for(const auto& source:sources)ops.push_back({{"op","delete"},{"target",source}});
      }
      m_design->applyOps(ops,tr("Convert drawing to sketch"),[=,this](bool applied,const QString& failure) {
        if(!guard || state->closed) return;
        state->applying=false;
        if(applied) { panel->hide(); if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD_CREATE")) QTimer::singleShot(500,this,[this,title] {
          bool valid=!m_design->sketchActive() && !m_doc->scene.sketches.empty() && m_doc->scene.sketches.back().name==title;
          valid=valid&&m_doc->scene.all_bodies().empty();m_doc->undo();valid=valid&&!m_doc->scene.all_bodies().empty()&&m_doc->scene.sketches.empty();m_doc->redo();valid=valid&&!m_doc->scene.sketches.empty()&&m_doc->scene.all_bodies().empty();
          trace::log(valid?"bench: conversion source removal, name, atomic undo/redo PASS":"bench: conversion source removal FAIL");QCoreApplication::exit(valid?0:2);
        }); }
        else {note->setText(failure); update();}
      });
    });
  };
  connect(debounce,&QTimer::timeout,dialog,[=] { if(preview->isChecked()) convert(false); });
  auto changed=[=,this] { ++state->serial; if(state->job) state->job->cancel(); m_viewport->clearPreviewBodies(); update(); if(preview->isChecked()) debounce->start(); };
  connect(tree,&QTreeWidget::itemChanged,dialog,changed); connect(tolerance,&QDoubleSpinBox::valueChanged,dialog,changed); connect(preview,&QCheckBox::toggled,dialog,changed); connect(name,&QLineEdit::textChanged,dialog,update);
  auto choosePlane=[=,this] { state->choosing=true; m_viewport->clearPreviewBodies(); m_design->pickSketchPlane([=,this](opad::json plane,opad::Frame frame) { if(!guard || state->closed) return; state->plane=std::move(plane); state->frame=frame; pick->setText(tr("Plane selected - pick another")); changed(); },true); };
  connect(pick,&QPushButton::clicked,dialog,choosePlane);
  connect(buttons,&QDialogButtonBox::accepted,dialog,[=] { debounce->stop(); convert(true); });
  connect(buttons,&QDialogButtonBox::rejected,panel,&QWidget::hide);
  connect(panel,&ToolPanel::visibilityChanged,this,[=,this](bool on) {
    if(on || state->closed || state->choosing) return;
    state->closed=true;++state->serial;debounce->stop();if(state->job) state->job->cancel();
    m_viewport->clearPreviewBodies();if(m_design->pickingPlane()) m_design->escape();
    m_prompt->hide();panel->deleteLater();
  });
  connect(m_design,&DesignController::stateChanged,dialog,[=,this] { if(state->choosing && !m_design->pickingPlane() && !state->closed){state->choosing=false;panel->show();panel->raise();update();} });
  connect(m_doc,&AppDocument::aboutToReplace,panel,[=]{state->choosing=false;panel->hide();});
  openPanel(panel); update(); choosePlane();
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_WIZARD");!shot.isEmpty()) QTimer::singleShot(350,dialog,[=,this] {
    m_design->planePicker()->cancel();
    if(!panel->isVisible() || state->closed){trace::log("bench: drawing plane cancel FAIL");QCoreApplication::exit(2);return;}
    choosePlane();m_design->planePicker()->choose({{"base","xy"}});
    auto* wait=new QTimer(dialog);wait->setInterval(50);auto attempts=std::make_shared<int>(0);
    connect(wait,&QTimer::timeout,dialog,[=,this]{
      if(++*attempts>200){wait->stop();QCoreApplication::exit(2);return;}
      if(!m_design->planePicker()->positioning())return;
      wait->stop();m_design->planePicker()->apply();
      if(state->plane.is_null() || !panel->isVisible()){trace::log("bench: drawing plane Apply FAIL");QCoreApplication::exit(2);return;}
      trace::log("bench: drawing plane cancel, reselect and Apply PASS");
      panel->grab().save(shot);m_prompt->grab().save(shot+".prompt.png");
      if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD_CREATE")){removeSource->setChecked(true);convert(true);}else{panel->hide();QCoreApplication::exit(0);}
    });wait->start();
  });
}
