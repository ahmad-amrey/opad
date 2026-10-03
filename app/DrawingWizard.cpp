#include "MainWindow.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "I18n.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/inspect.hpp"
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
#include "DrawingPlacer.hpp"

namespace {
bool sameFrame(const opad::Frame& a, const opad::Frame& b) {
  for (int i = 0; i < 3; ++i)
    if (std::abs(a.origin[i] - b.origin[i]) > 1e-9 || std::abs(a.x[i] - b.x[i]) > 1e-12 || std::abs(a.y[i] - b.y[i]) > 1e-12) return false;
  return true;
}
}  // namespace

void MainWindow::importDrawing(const QString& path, const QString& parent) {
  if (!m_doc->hasDocument || m_doc->browse) return openPath(path);  // nothing to place it among: as Open
  // A planar face selected: straight onto it, nothing asked (the face's frame: its lower-left corner and axes).
  const auto refs = m_viewport->selection();
  if (refs.size() == 1 && refs.front().kind == opad::Ref::Kind::Face) {
    beginLoad([this, path] { addRecent(path); m_viewport->fitWhenReady(); });
    m_doc->startImport(path, parent, {}, opad::json{{"face", refs.front().to_json()}});
    return;
  }
  // Otherwise the plane is chosen first, then the drawing is moved on it before the import op is written.
  cancelTool();
  m_design->pickSketchPlane([this, path, parent](opad::json, opad::Frame frame) {
    m_drawingPlacer->placed = [this, path, parent](const opad::Mat4& placement) {
      beginLoad([this, path] { addRecent(path); m_viewport->fitWhenReady(); });
      m_doc->startImport(path, parent, placement);
    };
    m_drawingPlacer->back = [this, path, parent] { QTimer::singleShot(0, this, [this, path, parent] { importDrawing(path, parent); }); };
    m_drawingPlacer->start(path, frame, [this](ToolPanel* panel) { openPanel(panel); });
  }, false);
}

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
  // The sketch takes the drawing's own plane and origin (where its import placed it): nothing to pick (TODO 10 A12).
  auto* hint=new QLabel(tr("The sketch takes the drawing's plane and origin. Choose the layers and preview the converted curves."),dialog);
  hint->setWordWrap(true); layout->addWidget(hint);
  auto* tree=new QTreeWidget(dialog); tree->setHeaderLabels({tr("Include layer"),tr("Construction only")});
  const auto& colors=theme::current();
  tree->setStyleSheet(QString("QTreeView::indicator { width: 14px; height: 14px; border: 1px solid %1; background: %2; } QTreeView::indicator:checked { background: %3; image: url(%4); }").arg(colors.fg3.name(),colors.bg2.name(),colors.sel.name(),icons::file("check",colors.onsel,14)));
  tree->setRootIsDecorated(false); tree->header()->setSectionResizeMode(0,QHeaderView::Stretch); layout->addWidget(tree,1);
  std::set<std::string> selected;
  for(const auto& id:currentNodeIds()) for(const auto& body:m_doc->scene.bodies_under(id)) selected.insert(body);
  std::set<std::string> selectedSources;for(const auto& id:selected)if(const auto* n=m_doc->scene.node(id);n && n->representation=="drawing2d")selectedSources.insert(n->source_op);
  for(const auto& id:m_doc->scene.all_bodies()) {
    const auto* n=m_doc->scene.node(id); if(n->representation!="drawing2d") continue;
    auto* row=new QTreeWidgetItem(tree); row->setText(0,QString::fromStdString(n->name)); row->setData(0,Qt::UserRole,QString::fromStdString(id));
    row->setIcon(0,icons::themed("drawing",16)); row->setCheckState(0,selectedSources.empty() || selectedSources.count(n->source_op)?Qt::Checked:Qt::Unchecked); row->setCheckState(1,Qt::Unchecked);
    if(!n->raster.is_null()) { row->setCheckState(0,Qt::Unchecked); row->setDisabled(true); row->setText(0,row->text(0)+tr(" (image: no vector curves)")); }
  }
  if(!tree->topLevelItemCount()) { panel->deleteLater(); throw opad::Error("Import a 2D drawing before converting to a sketch."); }
  auto* form=new QFormLayout; auto* name=new QLineEdit(tr("Converted drawing"),dialog);
  for(int i=0;i<tree->topLevelItemCount();++i)if(tree->topLevelItem(i)->checkState(0)==Qt::Checked){const auto* n=m_doc->scene.node(tree->topLevelItem(i)->data(0,Qt::UserRole).toString().toStdString());while(n && !n->parent.empty()){const auto* parent=m_doc->scene.node(n->parent);if(!parent || parent->source_op!=n->source_op)break;n=parent;}name->setText(n?QString::fromStdString(n->name):tree->topLevelItem(i)->text(0));break;}
  // In the shown unit (UI-123); the conversion takes mm.
  auto* tolerance=new QDoubleSpinBox(dialog); tolerance->setDecimals(units::decimalsFor(0.0001)); tolerance->setRange(units::toDisplay(units::Kind::Length,0.0001),units::toDisplay(units::Kind::Length,10));
  tolerance->setValue(units::toDisplay(units::Kind::Length,0.01)); tolerance->setSuffix(' '+units::symbol(units::Kind::Length));
  form->addRow(tr("Sketch name"),name); form->addRow(tr("Curve tolerance"),tolerance); layout->addLayout(form);
  auto* preview=new QCheckBox(tr("Preview converted curves"),dialog); layout->addWidget(preview);
  auto* removeSource=new QCheckBox(tr("Remove source drawing after conversion"),dialog);layout->addWidget(removeSource);
  auto* note=new QLabel(tr("Native curves stay exact. Tolerance controls reconstruction of segmented curves. Corners and construction layers are preserved."),dialog); note->setWordWrap(true); layout->addWidget(note);
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,dialog); buttons->button(QDialogButtonBox::Ok)->setText(tr("Create sketch"));buttons->button(QDialogButtonBox::Ok)->setObjectName("primary"); layout->addWidget(buttons);
  struct State { opad::json plane; opad::Frame frame; int serial=0; QPointer<Job> job; bool applying=false,closed=false; QString frameError; };
  auto state=std::make_shared<State>(); QPointer<QWidget> guard(dialog);
  const auto generation=m_doc->generation;
  auto layers=[tree] { std::vector<opad::design::DrawingLayer> out; for(int i=0;i<tree->topLevelItemCount();++i) {auto* r=tree->topLevelItem(i); if(r->checkState(0)==Qt::Checked) out.push_back({r->data(0,Qt::UserRole).toString().toStdString(),r->checkState(1)==Qt::Checked});} return out; };
  auto frameOf=[=,this] {  // every chosen layer must lie in one frame: drawings placed apart convert separately
    state->plane=nullptr;state->frameError.clear();
    if(layers().empty())return;
    try{state->frame=opad::design::drawing_frame(m_doc->scene,layers());state->plane={{"frame",state->frame.to_json()}};}
    catch(const std::exception& e){state->frameError=i18n::t(QString::fromUtf8(e.what()));}
  };
  auto update=[=,this] {
    std::set<std::string> chosen;for(const auto& l:layers())chosen.insert(l.id);
    bool complete=!chosen.empty();
    for(const auto& id:chosen)for(const auto& body:m_doc->scene.all_bodies())
      if(m_doc->scene.node(body)->source_op==m_doc->scene.node(id)->source_op && !chosen.count(body))complete=false;
    removeSource->setEnabled(complete);if(!complete)removeSource->setChecked(false);
    removeSource->setToolTip(tr("Select all layers of a source drawing to remove it."));
    if(!state->closed && !state->applying) {
      m_prompt->set("drawing",tr("Drawing to sketch"),{{tr("Choose layers and create sketch"),{}}},tr("Esc cancels"));
      m_prompt->show();positionOverlays();
    }
    if(!state->frameError.isEmpty())note->setText(state->frameError);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!state->applying && !state->plane.is_null() && !layers().empty() && !name->text().trimmed().isEmpty()); };
  auto* debounce=new QTimer(dialog); debounce->setSingleShot(true); debounce->setInterval(250);
  auto convert=[=,this](bool commit) {
    if(state->plane.is_null() || layers().empty() || state->applying) return;
    const int serial=++state->serial; if(state->job) state->job->cancel();
    auto snapshot=std::make_shared<opad::Document>(m_doc->doc); auto geometry=std::make_shared<opad::design::Sketch>(); auto shape=std::make_shared<TopoDS_Compound>();
    auto presentation=std::make_shared<std::shared_ptr<BodyPrs>>();
    const auto chosen=layers(); const auto plane=state->plane; const auto frame=state->frame; const double tol=units::fromDisplay(units::Kind::Length,tolerance->value()); const auto title=name->text().trimmed().toStdString();
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
      // Conversion copies geometry. Its placement must not retain a snap reference to
      // the drawing that can be removed now or later. Keep independent model supports.
      auto placement=plane;
      std::set<std::string> sources;for(const auto& layer:chosen)sources.insert(m_doc->scene.node(layer.id)->source_op);
      std::function<bool(const opad::json&)> fromSource=[&](const opad::json& value){
        if(value.is_object() && value.contains("body") && value["body"].is_string()){const auto* n=m_doc->scene.node(value["body"].get<std::string>());if(n && sources.count(n->source_op))return true;}
        if(value.is_structured())for(const auto& child:value)if(fromSource(child))return true;return false;
      };
      if(fromSource(placement))placement={{"frame",frame.to_json()}};
      auto op=opad::design::make_sketch_op(title,placement,geometry->to_json());
      std::vector<opad::json> ops{op};
      if(removeSource->isChecked()) {
        std::set<std::string> sources;for(const auto& layer:chosen)sources.insert(m_doc->scene.node(layer.id)->source_op);
        for(const auto& source:sources)ops.push_back({{"op","delete"},{"target",source}});
      }
      m_design->applyOps(ops,tr("Convert drawing to sketch"),[=,this](bool applied,const QString& failure) {
        if(!guard || state->closed) return;
        state->applying=false;
        if(applied) { panel->hide(); if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD_CREATE")) QTimer::singleShot(500,this,[this,title,state] {
          bool valid=!m_design->sketchActive() && !m_doc->scene.sketches.empty() && m_doc->scene.sketches.back().name==title;
          valid=valid&&sameFrame(m_doc->scene.sketches.back().frame,state->frame);  // exactly the drawing's plane and origin
          if(!valid)trace::log("bench: converted sketch is not in the drawing's frame, or not created");
          valid=valid&&m_doc->scene.all_bodies().empty()&&m_doc->scene.unresolved.empty();
          try{const auto restored=opad::Document::parse(m_doc->doc.serialize());valid=valid&&opad::resolve(restored).unresolved.empty();}catch(...){valid=false;}
          m_doc->undo();valid=valid&&!m_doc->scene.all_bodies().empty()&&m_doc->scene.sketches.empty();m_doc->redo();valid=valid&&!m_doc->scene.sketches.empty()&&m_doc->scene.all_bodies().empty();
          trace::log(valid?"bench: conversion source removal, name, atomic undo/redo PASS":"bench: conversion source removal FAIL");QCoreApplication::exit(valid?0:2);
        }); }
        else {note->setText(failure); update();}
      });
    });
  };
  connect(debounce,&QTimer::timeout,dialog,[=] { if(preview->isChecked()) convert(false); });
  auto changed=[=,this] { ++state->serial; if(state->job) state->job->cancel(); m_viewport->clearPreviewBodies(); frameOf(); update(); if(preview->isChecked()) debounce->start(); };
  connect(tree,&QTreeWidget::itemChanged,dialog,changed); connect(tolerance,&QDoubleSpinBox::valueChanged,dialog,changed); connect(preview,&QCheckBox::toggled,dialog,changed); connect(name,&QLineEdit::textChanged,dialog,update);
  connect(buttons,&QDialogButtonBox::accepted,dialog,[=] { debounce->stop(); convert(true); });
  connect(buttons,&QDialogButtonBox::rejected,panel,&QWidget::hide);
  connect(panel,&ToolPanel::visibilityChanged,this,[=,this](bool on) {
    if(on || state->closed) return;
    state->closed=true;++state->serial;debounce->stop();if(state->job) state->job->cancel();
    m_viewport->clearPreviewBodies();
    m_prompt->hide();panel->deleteLater();
  });
  connect(m_doc,&AppDocument::aboutToReplace,panel,[=]{panel->hide();});
  frameOf(); openPanel(panel); update();
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_WIZARD");!shot.isEmpty()) QTimer::singleShot(350,dialog,[=,this] {
    // TODO 10 A12: nothing is asked; the sketch plane is the drawing's own frame.
    if(state->plane.is_null() || !panel->isVisible()){trace::log("bench: drawing frame taken from the drawing FAIL");QCoreApplication::exit(2);return;}
    trace::log("bench: drawing frame taken from the drawing PASS");
    panel->grab().save(shot);m_prompt->grab().save(shot+".prompt.png");
    if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD_CREATE")){removeSource->setChecked(true);convert(true);}else{panel->hide();QCoreApplication::exit(0);}
  });
}

// OPAD_BENCH_DRAWING_IMPORT=<drawing file>: TODO 10 A12. Import onto a selected face (nothing asked), then onto a
// picked plane, moved and snapped there before it is imported; the import ops must carry exactly those placements.
bool MainWindow::benchDrawingImport() {
  const QString file = qEnvironmentVariable("OPAD_BENCH_DRAWING_IMPORT");
  if (file.isEmpty()) return false;
  auto phase = std::make_shared<int>(0);
  auto ticks = std::make_shared<int>(0);
  auto imports = std::make_shared<size_t>(0);
  auto expected = std::make_shared<opad::Mat4>();
  auto* timer = new QTimer(this);
  timer->setInterval(100);
  auto fail = [timer](const QString& why) { timer->stop(); trace::log("bench: drawing import FAIL: " + why); QCoreApplication::exit(2); };
  // The placement stored for the newest drawing import: its root component's transform.
  auto lastPlacement = [this]() -> opad::Mat4 {
    for (auto it = m_doc->doc.ops.rbegin(); it != m_doc->doc.ops.rend(); ++it)
      if (it->type == "import" && it->data.contains("nodes")) return opad::Mat4::from_json(it->data["nodes"][0].value("transform", opad::Mat4().to_json()));
    return {};
  };
  auto importCount = [this] { size_t n = 0; for (const auto& op : m_doc->doc.ops) n += op.type == "import"; return n; };
  auto close = [](const opad::Mat4& a, const opad::Mat4& b) { for (size_t i = 0; i < 16; ++i) if (std::abs(a.m[i] - b.m[i]) > 1e-9) return false; return true; };
  connect(timer, &QTimer::timeout, this, [=, this] {
    if (++*ticks > 400) return fail("timed out in phase " + QString::number(*phase));
    if (m_loadJob || m_doc->loading || m_doc->designBusy) return;
    switch (*phase) {
      case 0: {  // a 40 x 30 x 12 box; its top face selected
        m_doc->run("feature", opad::json{{"kind", "box"}, {"inputs", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "12 mm"}}}});
        const std::string body = m_doc->scene.all_bodies().front();
        opad::Ref top;
        double best = -1e300;
        for (int i = 0; i < 6; ++i) {
          opad::Ref ref; ref.body = body; ref.kind = opad::Ref::Kind::Face; ref.index = i;
          const auto info = opad::inspect_ref(m_doc->doc, m_doc->scene, ref);
          const double z = info.contains("center") ? info["center"][2].get<double>() : info["bbox"]["center"][2].get<double>();
          if (z > best) { best = z; top = ref; }
        }
        m_viewport->setProperty("benchTop", QString::fromStdString(top.str()));
        m_viewport->setSelectionFilter(Viewport::SelFilter::Face);
        *imports = importCount();
        ++*phase;
        break;
      }
      case 1: {  // once the box is displayed and faces are pickable
        if (m_meshRemaining > 0 || m_viewport->selectionFilter() != Viewport::SelFilter::Face) return;
        const auto top = opad::Ref::parse(m_viewport->property("benchTop").toString().toStdString());
        if (m_viewport->selection().size() != 1) { if (*ticks % 5 == 0) m_viewport->selectRefs({top}); return; }
        importDrawing(file, {});
        ++*phase;
        break;
      }
      case 2: {  // on the face: the drawing's XY lies in the face's plane (z = 12), nothing was asked
        if (importCount() == *imports) return;
        const opad::Mat4 m = lastPlacement();
        if (std::abs(m.apply({0, 0, 0})[2] - 12) > 1e-9 || std::abs(m.apply_dir({0, 0, 1})[2] - 1) > 1e-9) return fail("not placed on the selected face");
        trace::log("bench: drawing imported onto the selected face PASS");
        *imports = importCount();
        m_viewport->clearSelection();
        ++*phase;
        break;
      }
      case 3:
        if (!m_viewport->selection().empty()) return;
        importDrawing(file, {});
        ++*phase;
        break;
      case 4:  // no face: the plane is picked first
        if (!m_design->pickingPlane()) return;
        m_design->planePicker()->choose({{"base", "xz"}});
        ++*phase;
        break;
      case 5: {  // then moved on it: an offset, then one of its vertices snapped onto a point, then Place
        if (!m_drawingPlacer->active() || !m_drawingPlacer->panel()->findChild<QPushButton*>("primary")->isEnabled()) return;
        auto* offsetX = m_drawingPlacer->panel()->findChild<QLineEdit*>("placeOffsetX");
        offsetX->setText("1 in");  // typed with a unit, read back in the shown one (UI-123)
        offsetX->setModified(true);
        emit offsetX->editingFinished();
        if (offsetX->text() != "25.4 mm") return fail("the offset box read 1 in as " + offsetX->text());
        m_drawingPlacer->setOffset(7.123456789, 3);  // shown rounded; Enter on the box as shown keeps the value
        const opad::Mat4 shown = m_drawingPlacer->placement();
        emit offsetX->editingFinished();
        if (!close(m_drawingPlacer->placement(), shown)) return fail("Enter on the offset box as shown moved the drawing");
        m_drawingPlacer->setOffset(7, 3);
        if (offsetX->text() != "7 mm") return fail("the offset box shows " + offsetX->text());
        const opad::Vec3 from = m_drawingPlacer->placement().apply({0, 0, 0}), to{100, -20, 50};
        m_drawingPlacer->snap(from, to);  // the drawing's origin onto (100, 50) of the XZ plane; -20 is off the plane
        *expected = m_drawingPlacer->placement();
        const opad::Vec3 origin = expected->apply({0, 0, 0});
        if (std::abs(origin[0] - 100) > 1e-9 || std::abs(origin[1]) > 1e-9 || std::abs(origin[2] - 50) > 1e-9) return fail("snapping did not move the vertex onto the target");
        // Fit frames the drawing being placed (an overlay) with the 40 mm box, where it now lies: x from 100 on.
        const Bnd_Box fit = m_viewport->benchFitBox();
        if (fit.IsVoid() || fit.CornerMax().X() < 100 - 1e-6 || fit.CornerMin().X() > 1e-6) return fail("Fit does not frame the drawing being placed");
        trace::log("bench: Fit frames the drawing being placed PASS");
        m_drawingPlacer->panel()->findChild<QPushButton*>("primary")->click();
        ++*phase;
        break;
      }
      case 6: {
        if (importCount() == *imports) return;
        if (!close(lastPlacement(), *expected)) return fail("the import did not keep the placement it was given");
        trace::log("bench: drawing placed on a picked plane, offset and snapped, imported there PASS");
        timer->stop();
        QCoreApplication::exit(0);
        break;
      }
    }
  });
  timer->start();
  return true;
}
