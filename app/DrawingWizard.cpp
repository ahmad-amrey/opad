#include "MainWindow.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "I18n.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/inspect.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/feature.hpp"
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
#include <QElapsedTimer>
#include "DrawingPlacer.hpp"
#include "SketchGeometryCache.hpp"
#include <atomic>

namespace {
bool sameFrame(const opad::Frame& a, const opad::Frame& b) {
  for (int i = 0; i < 3; ++i)
    if (std::abs(a.origin[i] - b.origin[i]) > 1e-9 || std::abs(a.x[i] - b.x[i]) > 1e-12 || std::abs(a.y[i] - b.y[i]) > 1e-12) return false;
  return true;
}

// The converted curves as a drawing layer is drawn (UI-29): segments and points from the sketch's own samples, built on
// the worker. Edges made per curve looked every point up by scanning (n² for 30,000 lines) and were then made into a
// pickable presentation the preview never needed.
struct CurvePreview { std::shared_ptr<BodyPrs> curves,construction; size_t lines=0,arcs=0,splines=0,points=0; };
CurvePreview curvePreview(const opad::design::Sketch& sk,const opad::Frame& frame,const Progress& p) {
  using Type=opad::design::SkEntity::Type;
  CurvePreview out; if(sk.points.empty()) return out;
  double u0=1e300,v0=1e300,u1=-1e300,v1=-1e300;
  for(const auto& q:sk.points) { u0=std::min(u0,q.x); v0=std::min(v0,q.y); u1=std::max(u1,q.x); v1=std::max(v1,q.y); }
  SketchGeometryCache cache; cache.update(sk,std::max(1e-6,std::hypot(u1-u0,v1-v0)*1e-5));
  std::vector<gp_Pnt> segments[2],points;
  auto world=[&](const std::pair<double,double>& uv) { const auto w=frame.to_world(uv.first,uv.second); return gp_Pnt(w[0],w[1],w[2]); };
  for(size_t i=0;i<sk.entities.size();++i) {
    if(i%4096==0 && p.cancelled()) return {};
    const auto& e=sk.entities[i];
    ++(e.type==Type::Point?out.points:e.type==Type::Line?out.lines:e.type==Type::Arc || e.type==Type::Circle?out.arcs:out.splines);
    const auto* poly=cache.samples(sk,e); if(!poly) continue;
    if(e.type==Type::Point) { for(const auto& uv:*poly) points.push_back(world(uv)); continue; }
    auto& to=segments[e.construction?1:0];
    for(size_t j=1;j<poly->size();++j) { to.push_back(world((*poly)[j-1])); to.push_back(world((*poly)[j])); }
  }
  auto arrays=[](const std::vector<gp_Pnt>& segments,const std::vector<gp_Pnt>& points) -> std::shared_ptr<BodyPrs> {
    if(segments.empty() && points.empty()) return nullptr;
    auto prs=std::make_shared<BodyPrs>();
    if(!segments.empty()) { prs->boundaries=new Graphic3d_ArrayOfSegments(int(segments.size())); for(const auto& q:segments) prs->boundaries->AddVertex(q); }
    if(!points.empty()) { prs->loosePoints=new Graphic3d_ArrayOfPoints(int(points.size())); for(const auto& q:points) prs->loosePoints->AddVertex(q); }
    return prs;
  };
  out.curves=arrays(segments[0],points); out.construction=arrays(segments[1],{});
  return out;
}
std::atomic<int> runningConversions{0};  // workers still converting (the preview bench checks a cancelled one stops)
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
  auto* tolerance=new QDoubleSpinBox(dialog); tolerance->setDecimals(4); tolerance->setRange(0.0001,10); tolerance->setValue(0.01); tolerance->setSuffix(" mm");
  form->addRow(tr("Sketch name"),name); form->addRow(tr("Curve tolerance"),tolerance); layout->addLayout(form);
  auto* preview=new QCheckBox(tr("Preview converted curves"),dialog); layout->addWidget(preview);
  auto* summary=new QLabel(dialog); summary->setObjectName("previewSummary"); summary->setWordWrap(true); summary->hide(); layout->addWidget(summary);
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
    auto snapshot=std::make_shared<opad::Document>(m_doc->doc); auto geometry=std::make_shared<opad::design::Sketch>(); auto drawn=std::make_shared<CurvePreview>();
    const auto chosen=layers(); const auto frame=state->frame; const double tol=tolerance->value(); const auto title=name->text().trimmed().toStdString();
    // Conversion copies geometry. Its placement must not retain a snap reference to
    // the drawing that can be removed now or later. Keep independent model supports.
    auto placement=state->plane;
    std::set<std::string> sources;for(const auto& layer:chosen)if(const auto* n=m_doc->scene.node(layer.id))sources.insert(n->source_op);
    std::function<bool(const opad::json&)> fromSource=[&](const opad::json& value){
      if(value.is_object() && value.contains("body") && value["body"].is_string()){const auto* n=m_doc->scene.node(value["body"].get<std::string>());if(n && sources.count(n->source_op))return true;}
      if(value.is_structured())for(const auto& child:value)if(fromSource(child))return true;return false;
    };
    if(fromSource(placement))placement={{"frame",frame.to_json()}};
    // The op is made on the worker as well: the curves' JSON and its copies held the window half a second for 30,000.
    auto op=std::make_shared<opad::json>();
    if(commit) { state->applying=true; update(); }
    state->job=m_jobs->async(commit?tr("Converting drawing layers"):tr("Previewing curves"),[=](Progress p) {
      ++runningConversions; struct Running { ~Running() { --runningConversions; } } running;
      // A preview the next change made stale stops at once (UI-29: they ran on for 45 s each, stacking up).
      *geometry=opad::design::drawing_sketch(*snapshot,opad::resolve(*snapshot),chosen,frame,tol,[p]{return p.cancelled();}); if(p.cancelled()) return;
      if(!commit) *drawn=curvePreview(*geometry,frame,p);
      else *op=opad::design::make_sketch_op(title,placement,geometry->to_json());
    },[=,this](bool ok,const QString& error) {
      if(!guard || state->closed || serial!=state->serial || generation!=m_doc->generation) return;
      state->job=nullptr;
      if(!ok) { state->applying=false; note->setText(error); update(); return; }
      if(!commit) {
        if(!preview->isChecked()) return;
        std::vector<std::string> hidden; for(const auto& l:chosen) hidden.push_back(l.id);
        m_viewport->setPreviewCurves(drawn->curves,drawn->construction,hidden);
        summary->setText(tr("The sketch gets %L1 lines, %L2 arcs and circles, %L3 splines and %L4 points.").arg(drawn->lines).arg(drawn->arcs).arg(drawn->splines).arg(drawn->points));
        summary->show(); return;
      }
      if(snapshot->ops.size()!=m_doc->doc.ops.size()) { state->applying=false; note->setText(tr("Document changed. Please retry.")); update(); return; }
      std::vector<opad::json> ops; ops.push_back(std::move(*op));
      if(removeSource->isChecked()) for(const auto& source:sources) ops.push_back({{"op","delete"},{"target",source}});
      m_design->applyOps(std::move(ops),tr("Convert drawing to sketch"),[=,this](bool applied,const QString& failure) {
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
  auto changed=[=,this] { ++state->serial; if(state->job) state->job->cancel(); m_viewport->clearPreviewBodies(); summary->hide(); frameOf(); update(); if(preview->isChecked()) debounce->start(); };
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
  // OPAD_BENCH_WIZARD_PREVIEW=<segments> (UI-29): a big drawing's preview comes without stalling the window and draws every
  // converted segment; a change while one is being made stops that one before the next starts (they ran on, 45 s each).
  auto benchPreview=[=,this](const QString& png,size_t expected) {
    struct Run { int step=0; QElapsedTimer clock,tick,since; qint64 worst=0; };
    auto run=std::make_shared<Run>(); run->clock.start(); run->tick.start();
    auto* timer=new QTimer(dialog); timer->setInterval(15);
    auto end=[=,this](const QString& failure) {
      timer->stop(); if(!failure.isEmpty()) trace::log("bench: drawing preview FAIL: "+failure);
      panel->hide(); QCoreApplication::exit(failure.isEmpty()?0:2);
    };
    connect(timer,&QTimer::timeout,dialog,[=,this] {
      run->worst=std::max(run->worst,run->tick.restart());
      if(run->clock.elapsed()>60000) return end(QString("timed out at step %1").arg(run->step));
      switch(run->step) {
        case 0: preview->setChecked(true); run->since.start(); run->worst=0; run->step=1; return;
        case 1: {
          if(!m_viewport->previewSegments()) return;
          const size_t shown=m_viewport->previewSegments();
          trace::log(QString("bench: drawing preview: %1 segments %2 ms after it was asked, longest UI pause %3 ms; %4").arg(shown).arg(run->since.elapsed()).arg(run->worst).arg(summary->text()));
          if(shown!=expected) return end(QString("the preview draws %1 segments, the drawing has %2").arg(shown).arg(expected));
          if(!summary->isVisible() || !summary->text().contains(QLocale().toString(qulonglong(expected)))) return end("the preview does not say what the sketch gets: "+summary->text());
          if(run->worst>=250) return end(QString("the window paused %1 ms while the preview was made").arg(run->worst));
          if(run->since.elapsed()>5000) return end(QString("the preview took %1 ms").arg(run->since.elapsed()));
          trace::log("bench: drawing preview of every converted segment without a stall PASS");
          m_viewport->grabImage().save(png); run->tick.restart();  // the bench's own frame is no pause of the wizard's
          tolerance->setValue(0.02);
          if(m_viewport->previewSegments() || summary->isVisible()) return end("a change left the old preview shown");
          run->step=2; return;
        }
        case 2:  // the debounced preview runs; a change now cancels it
          if(runningConversions==0) return;
          tolerance->setValue(0.03); run->since.restart(); run->step=3; return;
        case 3:
          if(runningConversions==0) { trace::log(QString("bench: drawing preview: the cancelled one stopped within %1 ms").arg(run->since.elapsed())); run->step=4; return; }
          if(!debounce->isActive()) return end("a preview a newer change cancelled ran on until the next one started");
          return;
        case 4:
          if(!m_viewport->previewSegments()) return;
          if(m_viewport->previewSegments()!=expected || !summary->isVisible()) return end("the newest preview is not the whole drawing");
          if(run->worst>=250) return end(QString("the window paused %1 ms").arg(run->worst));
          trace::log("bench: drawing preview: a change stops the preview being made, the newest one is shown PASS");
          return end({});
      }
    });
    timer->start();
  };
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_WIZARD");!shot.isEmpty()) QTimer::singleShot(350,dialog,[=,this] {
    // TODO 10 A12: nothing is asked; the sketch plane is the drawing's own frame.
    if(state->plane.is_null() || !panel->isVisible()){trace::log("bench: drawing frame taken from the drawing FAIL");QCoreApplication::exit(2);return;}
    trace::log("bench: drawing frame taken from the drawing PASS");
    panel->grab().save(shot);m_prompt->grab().save(shot+".prompt.png");
    if(const auto expected=qEnvironmentVariable("OPAD_BENCH_WIZARD_PREVIEW");!expected.isEmpty()) benchPreview(shot+".preview.png",expected.toULongLong());
    else if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD_CREATE")){removeSource->setChecked(true);convert(true);}else{panel->hide();QCoreApplication::exit(0);}
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
        m_drawingPlacer->setOffset(7, 3);
        const opad::Vec3 from = m_drawingPlacer->placement().apply({0, 0, 0}), to{100, -20, 50};
        m_drawingPlacer->snap(from, to);  // the drawing's origin onto (100, 50) of the XZ plane; -20 is off the plane
        *expected = m_drawingPlacer->placement();
        const opad::Vec3 origin = expected->apply({0, 0, 0});
        if (std::abs(origin[0] - 100) > 1e-9 || std::abs(origin[1]) > 1e-9 || std::abs(origin[2] - 50) > 1e-9) return fail("snapping did not move the vertex onto the target");
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
