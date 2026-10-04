#include "DesignController.hpp"
#include "opad/inspect.hpp"
#include "DimensionHandle.hpp"
#include "ToolValues.hpp"
#include "CurveSamples.hpp"
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <QMenu>
#include <TopoDS.hxx>

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <gp.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <QMessageBox>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <atomic>

#include "I18n.hpp"
#include "SketchSteps.hpp"
#include "Units.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"

using namespace opad::design;

namespace {

// Kernel workers that may still be reading the document. A cancelled job reports back at once while its thread
// runs on until the next cancel poll, so "the job finished" is not "nobody reads the document": commits wait
// for this to reach zero.
std::shared_ptr<std::atomic<int>> g_readers = std::make_shared<std::atomic<int>>(0);
struct Reading {
  std::shared_ptr<std::atomic<int>> n = g_readers;
  Reading() { ++*n; }
  ~Reading() { --*n; }
};

void whenNobodyReads(QObject* ctx, std::function<void()> fn) {
  if (*g_readers == 0) return fn();
  QTimer::singleShot(20, ctx, [ctx, fn] { whenNobodyReads(ctx, fn); });
}

Viewport::SelFilter filterFor(const std::string& type) {
  if (type == "faces" || type == "profiles" || type == "plane") return Viewport::SelFilter::Face;
  if (type == "edges" || type == "axis" || type == "path") return Viewport::SelFilter::Edge;
  if (type == "points") return Viewport::SelFilter::Vertex;
  return Viewport::SelFilter::Body;
}

gp_Pnt pnt(const opad::Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }

// What a plan says the op `target` makes (its own result, or a regeneration's). An edit that changed nothing is not
// recomputed: then the stored `fallback`, so its handle shows as well.
std::vector<opad::json> resultsFor(const Plan& plan, const std::string& target, const opad::json& fallback) {
  std::vector<opad::json> out;
  for (const auto& op : plan.ops) {
    opad::json result;
    if (op.value("id", "") == target) result = op.value("result", opad::json());
    if (op.value("op", "") == "regen" && op.at("results").contains(target)) result = op.at("results").at(target);
    if (result.is_object()) out.push_back(result);
  }
  if (out.empty() && fallback.is_object()) out.push_back(fallback);
  return out;
}

// A preview pulled along the extrusion to k times its distance without planning again: vertices between the profile
// plane and the moving end, inside the profile's footprint, scale along the axis; everything else (the other side of
// a two-sided extrusion, the faces of a joined or cut body away from the profile) stays. Exact for a straight
// extrusion; the next exact preview replaces it anyway.
std::shared_ptr<const BodyPrs> stretchedPrs(const BodyPrs& base, const DesignController::Stretch& s, double k) {
  const gp_Pnt origin(s.origin[0], s.origin[1], s.origin[2]);
  const gp_Vec axis(s.axis[0], s.axis[1], s.axis[2]), u(s.u[0], s.u[1], s.u[2]), v(s.v[0], s.v[1], s.v[2]);
  const double lo = s.symmetric ? -std::fabs(s.from) / 2 : std::min(0.0, s.from), hi = s.symmetric ? std::fabs(s.from) / 2 : std::max(0.0, s.from);
  const double eps = 1e-6 * std::max(1.0, std::fabs(s.from));
  auto moved = [&](const gp_Pnt& p) {
    const gp_Vec rel(origin, p);
    const double h = rel.Dot(axis);
    if (h < lo - eps || h > hi + eps) return p;
    if (s.footprint) {
      const double a = rel.Dot(u), b = rel.Dot(v);
      if (a < s.u0 || a > s.u1 || b < s.v0 || b > s.v1) return p;
    }
    return p.Translated(axis * (h * (k - 1)));
  };
  auto out = std::make_shared<BodyPrs>();
  out->closed = base.closed;
  Bnd_Box box;
  if (!base.triangles.IsNull()) {
    const auto& src = base.triangles;
    const bool normals = src->HasVertexNormals();
    Handle(Graphic3d_ArrayOfTriangles) dst = new Graphic3d_ArrayOfTriangles(src->VertexNumber(), src->EdgeNumber(), normals ? Graphic3d_ArrayFlags_VertexNormal : Graphic3d_ArrayFlags_None);
    for (int i = 1; i <= src->VertexNumber(); ++i) {
      const gp_Pnt p = moved(src->Vertice(i));
      box.Add(p);
      if (normals) dst->AddVertex(p, src->VertexNormal(i));
      else dst->AddVertex(p);
    }
    for (int i = 1; i <= src->EdgeNumber(); ++i) dst->AddEdge(src->Edge(i));
    out->triangles = dst;
  }
  if (!base.boundaries.IsNull()) {
    const auto& src = base.boundaries;
    Handle(Graphic3d_ArrayOfSegments) dst = new Graphic3d_ArrayOfSegments(src->VertexNumber(), src->EdgeNumber());
    for (int i = 1; i <= src->VertexNumber(); ++i) dst->AddVertex(moved(src->Vertice(i)));
    for (int i = 1; i <= src->EdgeNumber(); ++i) dst->AddEdge(src->Edge(i));
    out->boundaries = dst;
  }
  out->box = box;
  return out;
}

}  // namespace

DesignController::DesignController(AppDocument* doc, Viewport* viewport, JobRunner* jobs, QWidget* window)
    : QObject(window), m_doc(doc), m_viewport(viewport), m_jobs(jobs), m_window(window) {
  m_form = new FeaturePanel(doc, window);
  m_distanceHandle=new DimensionHandle(viewport,jobs);
  connect(m_distanceHandle,&DimensionHandle::valueChanged,this,[this](const QString& text){
    if(!m_featureOn)return;
    m_form->setValue("distance",text.toStdString());m_distanceHandle->setProblem("value",m_form->problem("distance"));
  });
  connect(m_distanceHandle,&DimensionHandle::extraEdited,this,&DesignController::typeValue);  // the taper and the others by the arrow
  connect(m_distanceHandle,&DimensionHandle::accepted,this,[this]{if(m_featureOn)runPreview(true);});  // Enter in the box: OK
  // Typed values (UI-122): while a feature with values is open, digits and Tab over the view or a panel are its, never the
  // filters' or the display styles' keys; the boxes beside the pointer, or by the extrude's arrow while it shows.
  m_values = new ToolValues(viewport, this);
  m_values->setHandle(m_distanceHandle);
  m_distanceHandle->setCapturesKeys(false);
  m_values->fields = [this] {
    if (!m_featureOn || m_pickPlane || m_sketch->active()) return QList<DynamicInput::Field>{};
    // The arrow's boxes take over once it shows, unless these are being typed into (keys typed before the preview came).
    const DynamicInput* typing = m_values->input();
    return m_distanceHandle->isVisible() && !typing->typed() && !typing->editing() ? QList<DynamicInput::Field>{} : valueFields();
  };
  m_values->edited = [this](const QString& key, const QString& value) { typeValue(key, value); };
  m_values->commit = [this] { if (m_featureOn) runPreview(true); };
  m_values->escape = [this] { escape(); };
  m_sketch = new SketchEditor(doc, viewport, jobs, this);
  m_planePicker=new PlanePicker(doc,viewport,jobs,window);
  m_planePicker->accepted=[this](const opad::json& plane,const opad::Frame& frame){
    auto picked=std::move(m_planePicked);m_pickPlane=false;m_replaning=false;
    if(picked)picked(plane,frame);
    else enterSketch({},QString::fromStdString(next_name(m_doc->scene,"Sketch")),plane,frame,opad::json::object());
    emit stateChanged();
  };
  connect(m_planePicker,&PlanePicker::cancelled,this,[this]{
    m_pickPlane=false;m_planePicked={};
    if(m_replaning){m_replaning=false;m_viewport->beginSketchInput(m_sketch,m_sketch->frame(),m_sketch->sketchId());showSketchPanel();}
    if(m_featureOn && m_panel){m_openPanel(m_panel);m_form->activate(QString());}
    emit stateChanged();
  });
  connect(doc, &AppDocument::aboutToReplace, this, [this] {
    endFeature();
    if (m_pickPlane) escape();
    m_sketch->end();
    // The panel closes, not the table inside it: hiding that left the Parameters panel empty after any New or Open.
    if (m_parametersPanel) m_parametersPanel->hide();
    m_doc->designBusy = false;
    emit stateChanged();
  });
  m_previewTimer.setSingleShot(true);
  m_previewTimer.setInterval(280);
  connect(&m_previewTimer, &QTimer::timeout, this, [this] { runPreview(false); });
  connect(m_form, &FeaturePanel::inputsChanged, this, &DesignController::schedulePreview);
  connect(m_form, &FeaturePanel::inputsChanged, this, &DesignController::refreshValues);
  connect(m_form, &FeaturePanel::inputsChanged, this, &DesignController::inputsSettled);
  connect(m_form, &FeaturePanel::activeInputChanged, this, &DesignController::activateInput);
  connect(m_form, &FeaturePanel::ruleRequested, this, &DesignController::offerRules);
  connect(m_form, &FeaturePanel::accepted, this, [this] { runPreview(true); });
  connect(m_form, &FeaturePanel::cancelled, this, &DesignController::endFeature);
  connect(m_sketch, &SketchEditor::status, this, &DesignController::status);
  connect(m_sketch, &SketchEditor::changed, this, &DesignController::stateChanged);
  connect(m_sketch, &SketchEditor::toolChanged, this, &DesignController::stateChanged);
  m_viewport->installEventFilter(this);
}

bool DesignController::eventFilter(QObject* watched, QEvent* event) {
  // The panel says "OK Enter", but after a pick in the view the view has the keyboard: Enter there accepts too.
  if (watched == m_viewport && event->type() == QEvent::KeyPress && m_featureOn && !m_pickPlane && !m_sketch->active()) {
    auto* key = static_cast<QKeyEvent*>(event);
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && key->modifiers() == Qt::NoModifier) {
      runPreview(true);
      return true;
    }
  }
  return QObject::eventFilter(watched, event);
}

void DesignController::setPanel(ToolPanel* panel, std::function<void(ToolPanel*)> open) {
  m_panel = panel;
  m_openPanel = std::move(open);
  connect(panel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on && m_featureOn && !m_pickPlane) endFeature();  // closing the panel cancels the feature
  });
}

// ---------------------------------------------------------------- applying changes
void DesignController::applyOps(std::vector<opad::json> ops, const QString& label, std::function<void(bool, const QString&)> done) {
  if (!m_doc->hasDocument || m_doc->browse) return;
  auto report = [this, done](bool ok, const QString& error) {
    if (done) done(ok, error);
    else if (!ok) emit failed(error);
    emit stateChanged();
  };
  if (m_doc->snapshotBusy()) return m_doc->afterCapture([this, ops = std::move(ops), label, done] { applyOps(ops, label, done); });  // a copy being taken
  if (m_doc->designBusy) return report(false, tr("The design is still being recomputed; try again in a moment."));
  m_doc->designBusy = true;
  const auto generation = m_doc->generation;
  auto plan = std::make_shared<Plan>();
  auto doc = std::make_shared<opad::Document>(m_doc->doc);
  auto list = std::make_shared<std::vector<opad::json>>(std::move(ops));  // not copied with the job (a converted drawing's curves)
  m_jobs->async(tr("Updating the design"), [doc, list, plan](Progress p) {
    Reading reading;
    try {
      *plan = plan_ops(*doc, std::move(*list), true, [p] { return p.cancelled(); });
    } catch (const opad::LockedError& e) {
      throw opad::Error(AppDocument::lockedMessage(e).toStdString());
    }
  }, [this, plan, label, report, generation](bool ok, const QString& error) {
    whenNobodyReads(this, [this, plan, label, report, ok, error, generation] {
      if (generation != m_doc->generation) return;
      m_doc->designBusy = false;
      if (!ok) return report(false, error);
      try {
        const opad::json rep = m_doc->commitPlan(std::move(*plan), label);
        const size_t errors = rep.value("errors", opad::json::array()).size();
        if (errors > 0) emit notice(tr("%1 later feature(s) could not be recomputed; they are marked on the timeline.").arg(errors));
        report(true, {});
      } catch (const std::exception& e) {
        report(false, QString::fromUtf8(e.what()));
      }
    });
  });
}

void DesignController::commitPlanned(std::shared_ptr<Plan> plan, const QString& label, std::function<void(bool, const QString&)> done) {
  if (!m_doc->hasDocument || m_doc->browse) return;
  if (m_doc->snapshotBusy()) return m_doc->afterCapture([this, plan, label, done] { commitPlanned(plan, label, done); });
  auto report = [this, done](bool ok, const QString& error) {
    if (done) done(ok, error);
    else if (!ok) emit failed(error);
    emit stateChanged();
  };
  if (m_doc->designBusy) return report(false, tr("The design is still being recomputed; try again in a moment."));
  m_doc->designBusy = true;
  const auto generation = m_doc->generation;
  whenNobodyReads(this, [this, plan, label, report, generation] {
    if (generation != m_doc->generation) return;
    m_doc->designBusy = false;
    try {
      const opad::json rep = m_doc->commitPlan(std::move(*plan), label);
      const size_t errors = rep.value("errors", opad::json::array()).size();
      if (errors > 0) emit status(tr("%1 later feature(s) could not be recomputed; they are marked on the timeline.").arg(errors));
      report(true, {});
    } catch (const std::exception& e) {
      report(false, QString::fromUtf8(e.what()));
    }
  });
}

void DesignController::regenerate(bool force) {
  if (!m_doc->hasDocument || m_doc->browse || m_doc->designBusy) return;
  m_doc->designBusy = true;
  const auto generation = m_doc->generation;
  auto plan = std::make_shared<Plan>();
  auto doc = std::make_shared<opad::Document>(m_doc->doc);
  m_jobs->async(tr("Regenerating the design"), [doc, plan, force](Progress p) {
    Reading reading;
    *plan = plan_regenerate(*doc, force, [p] { return p.cancelled(); });
  }, [this, plan, generation](bool ok, const QString& error) {
    whenNobodyReads(this, [this, plan, ok, error, generation] {
      if (generation != m_doc->generation) return;
      m_doc->designBusy = false;
      if (!ok) return emit failed(error);
      const bool nothing = plan->ops.empty();
      m_doc->commitPlan(std::move(*plan), tr("regenerate"));
      emit notice(nothing ? tr("The design is up to date.") : tr("Design regenerated."));
    });
  });
}

void DesignController::setSuppressed(const std::string& featureId, bool on) {
  applyOps({make_edit_op(featureId, opad::json{{"suppressed", on}})}, on ? tr("suppress") : tr("unsuppress"));
}

ParametersDialog* DesignController::parametersWidget() {
  if (!m_params) {
    m_params = new ParametersDialog(m_doc, [this](std::vector<opad::json> ops, QString label) {
      applyOps(std::move(ops), label, [this](bool ok, const QString& error) {
        if (!ok && m_params) m_params->failed(error);
      });
    }, m_window);
  }
  return m_params;
}
void DesignController::showParameters() {
  parametersWidget();
  m_params->rebuild();
  if(m_parametersPanel && m_openPanel)m_openPanel(m_parametersPanel);
}

// ---------------------------------------------------------------- features
void DesignController::startFeature(const QString& kind) {
  if (!m_doc->hasDocument || m_doc->browse) return;
  if (m_sketch->active()) return finishSketch([this, kind] { startFeature(kind); });  // as in Fusion: a feature ends the sketch
  if (m_featureOn) endFeature();
  if (m_pickPlane) escape();
  const FeatureSpec* spec = feature_spec(kind.toStdString());
  if (!spec) return;
  const std::vector<opad::Ref> before = m_viewport->selection();
  m_editing.clear();
  m_ruleMatches.clear();
  m_newId = opad::new_uuid();
  opad::json inputs = opad::json::object();
  for (const auto& in : spec->inputs)  // lengths offered in the document's unit ("0.5 in" for "10 mm")
    if (!in.def.is_null()) inputs[in.name] = in.type == "length" && in.def.is_string() ? opad::json(units::presetText(QString::fromStdString(in.def.get<std::string>())).toStdString()) : in.def;
  m_featureOn = true;
  m_filterBefore = m_viewport->selectionFilter();
  m_viewport->setPickAccumulate(true);
  resetRouting();
  m_form->begin(*spec, inputs, QString::fromStdString(next_name(m_doc->scene, name_prefix(*spec))), false);
  if (m_currentComponent) m_form->setBodyDefaults(m_currentComponent());
  if (m_panel) {
    m_panel->setHeader(QString::fromStdString(spec->icon), i18n::t(QString::fromStdString(spec->label)));
    m_panel->setContext(tr("new"));
    m_openPanel(m_panel);
  }
  // What was selected before the tool was started becomes its first picks when it fits the first input.
  if (!before.empty() && !m_form->activeInput().isEmpty()) {
    const InputSpec* in = m_form->input(m_form->activeInput());
    const auto want = in ? filterFor(in->type) : Viewport::SelFilter::Body;
    const bool bodies = before.front().kind == opad::Ref::Kind::Body;
    if (in && (in->type == "bodies" || in->type == "faces" || in->type == "edges") && want == m_filterBefore && bodies == (in->type == "bodies")) {
      opad::json picks = opad::json::array();
      for (const auto& r : before) picks.push_back(pickToJson(r));
      const QString active = m_form->activeInput();
      m_form->setPicks(active, picks);
      if (in->advance) m_form->activateNextPick();  // Combine: the selection is the target, the tools come next
      if (m_form->activeInput() == active) activateInput(active);
    }
  }
  if (m_form->activeInput().isEmpty()) activateInput(QString());  // nothing to pick first: a plane input takes a click
  schedulePreview();
  emit stateChanged();
}

void DesignController::startFeature(const QString& kind, const std::vector<std::pair<QString, opad::json>>& given) {
  startFeature(kind);
  if (!m_featureOn || !m_form->spec() || m_form->spec()->kind != kind.toStdString()) return;
  for (const auto& [name, value] : given)
    if (m_form->input(name)) value.is_array() ? m_form->setPicks(name, value) : m_form->setValue(name, value);
  activateInput(m_form->activeInput());  // the view shows the given picks
  schedulePreview();
}

void DesignController::editOp(const std::string& opId) {
  if (m_doc->snapshotBusy()) return m_doc->afterCapture([this, op = opId] { editOp(op); });  // a copy being taken: shortly
  if (!m_doc->hasDocument || m_doc->browse || m_doc->designBusy) return;
  if (m_sketch->active() || m_featureOn) return emit notice(tr("Finish what is open first."));
  if (const opad::SketchItem* s = m_doc->scene.sketch(opId)) {
    const opad::SketchItem sketch = *s;
    // Keep current visibility and the edited sketch in the browser. The editor
    // owns a geometry copy; only Finish commits its changes and regenerates dependants.
    return enterSketch(opId, QString::fromStdString(sketch.name), sketch.plane, sketch.frame, sketch.geometry);
  }
  const opad::Feature* f = m_doc->scene.feature(opId);
  if (!f) return;
  const FeatureSpec* spec = feature_spec(f->kind);
  if (!spec) return;
  const opad::Feature feature = *f;
  bool hidden=false;for(const auto& body:feature.result.value("bodies",opad::json::array())){const auto id=body.value("id",std::string());if(m_doc->scene.node(id) && !m_doc->scene.effectively_visible(id))hidden=true;}
  m_editResult = feature.result;
  m_doc->setRollback(opId);
  m_editing = opId;
  m_featureOn = true;
  m_filterBefore = m_viewport->selectionFilter();
  m_viewport->setPickAccumulate(true);
  resetRouting();
  m_form->begin(*spec, feature.inputs, QString::fromStdString(feature.name), true);m_form->setEditHidden(hidden);
  if (m_form->activeInput().isEmpty()) activateInput(QString());
  // Rules show what they matched when the feature was last computed (its result records it, TODO 10 B7).
  m_ruleMatches.clear();
  for (const auto& sel : feature.result.value("selected", opad::json::array()))
    for (const auto& in : spec->inputs)
      for (const auto& pick : feature.inputs.value(in.name, opad::json::array()))
        if (pick.is_object() && pick.contains("select") && pick["select"] == sel.value("select", opad::json()) && pick.value("body", "") == sel.value("body", ""))
          for (const auto& ordinal : sel.value("ordinals", opad::json::array())) {
            opad::Ref r;
            r.body = sel.value("body", "");
            r.kind = sel.value("kind", "") == "face" ? opad::Ref::Kind::Face : sel.value("kind", "") == "vertex" ? opad::Ref::Kind::Vertex : opad::Ref::Kind::Edge;
            r.index = ordinal.get<int>();
            m_ruleMatches[QString::fromStdString(in.name)].push_back(r);
          }
  if (m_panel) {
    m_panel->setHeader(QString::fromStdString(spec->icon), i18n::t(QString::fromStdString(spec->label)));
    m_panel->setContext(tr("editing"));
    m_openPanel(m_panel);
  }
  if (!feature.error.empty()) m_form->setStatus(i18n::t(QString::fromStdString(feature.error)), true);
  schedulePreview();
  emit stateChanged();
}

void DesignController::endFeature() {
  m_distanceHandle->hide();
  m_values->reset();
  if (!m_featureOn) return;
  m_featureOn = false;
  ++m_planSerial;
  m_previewTimer.stop();
  if (Job* j = std::exchange(m_planJob, nullptr)) j->cancel();
  if (Job* j = std::exchange(m_candidateJob, nullptr)) j->cancel();
  resetRouting();
  m_readyPlan.reset();
  m_stretch = {};
  m_planDoc.reset();
  m_planScene.reset();
  m_viewport->clearPreviewBodies();
  m_viewport->clearCandidates();
  m_viewport->setPickAccumulate(false);
  m_viewport->setBodiesPickable(true);
  m_activating = true;  // the clean-up below is not a pick
  m_viewport->clearSelection();
  if (m_viewport->selectionFilter() != m_filterBefore || m_viewport->roundFacesPickable()) m_viewport->setSelectionFilter(m_filterBefore);
  m_activating = false;
  m_form->activate(QString());
  m_editing.clear();
  m_editResult = opad::json();
  if (m_panel && m_panel->isVisible()) m_panel->hide();
  m_doc->setRollback({});
  emit status(QString());
  emit stateChanged();
}

opad::json DesignController::pickToJson(const opad::Ref& ref) const { return ref.to_json(); }

// A little more than the model: how long axes and how wide planes are drawn.
double DesignController::modelReach() const {
  double reach = 50;
  for (const auto& id : m_doc->scene.all_bodies()) {
    try {
      const Bnd_Box b = opad::node_world_bbox(m_doc->doc, m_doc->scene, id);  // cached per key: O(1)
      if (!b.IsVoid()) reach = std::max(reach, std::sqrt(b.SquareExtent()) * 0.75);
    } catch (const std::exception&) {
    }
  }
  return reach;
}

// The origin axes and construction axes for an axis, the origin planes and construction planes for a plane: shapes made here
// (a few edges and squares), nothing from the bodies.
std::vector<Viewport::Candidate> DesignController::quickCandidates(const std::string& type) const {
  std::vector<Viewport::Candidate> quick;
  if (type == "axis") {
    const double reach = modelReach();  // size of axis candidates
    for (const auto& [base, dir] : {std::pair{"x", gp_Dir(1, 0, 0)}, std::pair{"y", gp_Dir(0, 1, 0)}, std::pair{"z", gp_Dir(0, 0, 1)}})
      quick.push_back({opad::json{{"base", base}}.dump(), BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0).Translated(gp_Vec(dir) * -reach), gp_Pnt(0, 0, 0).Translated(gp_Vec(dir) * reach)).Edge(), false});
    for (const auto& f : m_doc->scene.features)
      if (f.result.contains("axis")) {
        const auto& a = f.result["axis"];
        const gp_Pnt o(a["origin"][0], a["origin"][1], a["origin"][2]);
        const gp_Vec d(a["dir"][0], a["dir"][1], a["dir"][2]);
        quick.push_back({opad::json{{"feature", f.id}}.dump(), BRepBuilderAPI_MakeEdge(o.Translated(d * -reach), o.Translated(d * reach)).Edge(), true});
      }
  } else if (type == "plane") {  // as the plane picker shows them: faint origin planes, construction planes more solid
    const double size = std::max(10.0, m_viewport->pixelSize() * 70);
    quick = PlanePicker::originPlanes(size);
    for (const auto& f : m_doc->scene.features)
      if (!f.suppressed && f.result.contains("plane"))
        quick.push_back({opad::json{{"feature", f.id}}.dump(), BRepBuilderAPI_MakeFace(frame_plane(opad::Frame::from_json(f.result["plane"])), -size, size, -size, size).Face(), true});
  }
  return quick;
}

void DesignController::showAllCandidates() {
  std::vector<Viewport::Candidate> all = m_activeCandidates;
  all.insert(all.end(), m_routeCandidates.begin(), m_routeCandidates.end());
  m_viewport->showCandidates(all);
}

void DesignController::showCandidatesFor(const QString& typeName) {
  if (Job* j = std::exchange(m_candidateJob, nullptr)) j->cancel();
  m_nothingToPick.clear();
  const std::string type = typeName.toStdString();
  m_activeCandidates = quickCandidates(type == "plane" ? std::string() : type);  // a plane input has the plane picker
  const bool fromSketches = type == "profiles" || type == "points" || type == "axis" || type == "path";
  if (!fromSketches) return showAllCandidates();
  auto found = std::make_shared<std::vector<Viewport::Candidate>>(m_activeCandidates);
  m_candidateJob = sketchCandidates(type, found, [this, found, type](bool ok) {
    m_candidateJob = nullptr;
    if (!ok || !m_featureOn) return;
    m_activeCandidates = *found;
    showAllCandidates();
    syncSelectionToInput();
    if (found->empty() && (type == "points" || type == "profiles")) {  // else the input waits for a pick that cannot come
      m_nothingToPick = type == "points" ? tr("No sketch points yet: sketch points first (or pick vertices).")
                                         : tr("No sketch profiles yet: draw a closed shape in a sketch first (or pick a planar face).");
      if (!m_form->complete()) m_form->setStatus(m_nothingToPick, false);
    }
  });
  showAllCandidates();
}

Job* DesignController::sketchCandidates(const std::string& type, std::shared_ptr<std::vector<Viewport::Candidate>> found, std::function<void(bool)> done) {
  // Sketch regions, points, lines and whole sketches: kernel work, so on a worker from copies of the sketches.
  struct Source { std::string id; opad::json geometry; opad::Frame frame; };
  auto sources = std::make_shared<std::vector<Source>>();
  for (const auto& s : m_doc->scene.sketches)
    if (s.visible || !s.consumed) sources->push_back({s.id, s.geometry, s.frame});
  return m_jobs->async(tr("Finding what can be picked"), [sources, found, type](Progress p) {
    for (const auto& src : *sources) {
      if (p.cancelled()) return;
      const Sketch sk = Sketch::from_json(src.geometry);
      if (type == "profiles") {
        // Meshed and turned into arrays here: meshing a document's worth of regions on the UI thread stalled it for 1.2 s.
        for (const auto& r : sketch_regions(sk, src.frame)) {
          if (p.cancelled()) return;
          BRepMesh_IncrementalMesh(r.face, 0.05, Standard_False, 0.3, Standard_False);
          Bnd_Box box;
          BRepBndLib::Add(r.face, box);
          found->push_back({opad::json{{"sketch", src.id}, {"at", {r.u, r.v}}, {"boundary",r.boundary}}.dump(), r.face, false, BodyPrs::build(r.face, box, true)});
        }
      } else if (type == "points") {
        for (const auto& pt : sk.points) {
          const opad::Vec3 w = src.frame.to_world(pt.x, pt.y);
          found->push_back({opad::json{{"sketch", src.id}, {"point", pt.id}}.dump(), BRepBuilderAPI_MakeVertex(gp_Pnt(w[0], w[1], w[2])).Vertex(), false});
        }
      } else if (type == "axis") {
        for (const auto& e : sk.entities)
          if (e.type == SkEntity::Type::Line) {
            const TopoDS_Edge edge = entity_edge(sk, e, src.frame);
            if (!edge.IsNull()) found->push_back({opad::json{{"sketch", src.id}, {"entity", e.id}}.dump(), edge, false});
          }
      } else {  // path: the sketch's curves as one pick
        TopoDS_Compound comp;
        BRep_Builder bb;
        bb.MakeCompound(comp);
        bool any = false;
        for (const auto& e : sketch_edges(sk, src.frame)) { bb.Add(comp, e); any = true; }
        if (any) found->push_back({opad::json{{"sketch", src.id}}.dump(), comp, false});
      }
    }
  }, [done](bool ok, const QString&) { done(ok); });
}

// Makes the viewport's selection show the active input's picks (after switching inputs, or when candidates
// arrive).
void DesignController::syncSelectionToInput() {
  const QString name = !m_form->activeInput().isEmpty() ? m_form->activeInput() : m_idlePlane;  // or the plane a click fills
  if (name.isEmpty()) return;
  opad::json picks = m_form->picks(name);
  if (picks.is_object()) picks = opad::json::array({picks});
  std::vector<opad::Ref> refs;
  std::vector<std::string> candidates;
  if (picks.is_array())
    for (const auto& p : picks) {
      if (p.is_object() && p.contains("select")) {  // a rule shows what it matched
        if (auto it = m_ruleMatches.find(name); it != m_ruleMatches.end()) refs.insert(refs.end(), it->second.begin(), it->second.end());
        continue;
      }
      const opad::json* ref = &p;
      if (p.contains("face")) ref = &p["face"];
      if (p.contains("edge")) ref = &p["edge"];
      if (ref->contains("body")) {
        try {
          refs.push_back(opad::Ref::from_json(*ref));
        } catch (const std::exception&) {
        }
      } else {
        opad::json key = p;
        key.erase("frame");
        candidates.push_back(key.dump());
      }
    }
  // The routed input's axis or plane stays marked (the default Z of a circular pattern too): a click on it takes it back.
  if (!m_routeInput.isEmpty())
    if (const opad::json routed = m_form->picks(m_routeInput); routed.is_object() && m_routeIds.count(routed.dump())) candidates.push_back(routed.dump());
  const bool was = std::exchange(m_activating, true);  // also while a filter switch is pending: that one re-applies later
  m_viewport->selectRefs(refs, candidates);
  m_activating = was;
}

// "By rule…" (TODO 10 B7): rules that the picked face or edge suggests, each with how many it matches on the body, counted
// on a worker; choosing one makes the input that rule (with that count expected).
void DesignController::offerRules(const QString& input, QWidget* anchor) {
  opad::json picks = m_form->picks(input);
  if (!picks.is_array() || picks.size() != 1) return;
  opad::Ref picked;
  try {
    picked = opad::Ref::from_json(picks[0]);
  } catch (const std::exception&) {
    return;
  }
  if (picked.kind != opad::Ref::Kind::Face && picked.kind != opad::Ref::Kind::Edge) return;
  struct Rule { QString label; opad::json select; std::vector<int> ordinals; };
  auto rules = std::make_shared<std::vector<Rule>>();
  auto doc = std::make_shared<opad::Document>(m_doc->doc);
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  const QPointer<QWidget> where(anchor);
  m_jobs->async(tr("Finding matching entities"), [doc, scene, picked, rules, shown = units::current()](Progress p) {
    Reading reading;
    const TopoDS_Shape body = opad::node_world_shape(*doc, *scene, picked.body);
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(body, picked.kind == opad::Ref::Kind::Face ? TopAbs_FACE : TopAbs_EDGE, map);
    if (picked.index < 0 || picked.index >= map.Extent()) return;
    const opad::json d = opad::describe_entity(map(picked.index + 1));
    auto fixed = [&shown](double v) { return units::format(units::Kind::Length, v, -1, shown); };
    if (picked.kind == opad::Ref::Kind::Edge) {
      if (d.contains("direction")) rules->push_back({tr("Straight edges parallel to this one"), {{"curve", "line"}, {"parallel_to", d["direction"]}}, {}});
      if (d.contains("radius") && d.value("curve", "") == "circle") {
        const double r = d["radius"];
        rules->push_back({tr("Circular edges of radius %1").arg(fixed(r)), {{"curve", "circle"}, {"radius_min", r}, {"radius_max", r}}, {}});
      }
      rules->push_back({tr("All %1 edges").arg(QString::fromStdString(d.value("curve", ""))), {{"curve", d.value("curve", "")}}, {}});
    } else {
      if (d.contains("normal")) rules->push_back({tr("Faces with this normal"), {{"normal", d["normal"]}}, {}});
      if (d.contains("radius") && d.value("surface", "") == "cylinder") {
        const double r = d["radius"];
        rules->push_back({tr("Cylindrical faces of radius %1").arg(fixed(r)), {{"surface", "cylinder"}, {"radius_min", r}, {"radius_max", r}}, {}});
      }
      rules->push_back({tr("All %1 faces").arg(QString::fromStdString(d.value("surface", ""))), {{"surface", d.value("surface", "")}}, {}});
    }
    for (int i = 1; i <= map.Extent(); ++i) {
      if (p.cancelled()) throw opad::Error("cancelled");
      const opad::json e = opad::describe_entity(map(i));
      for (auto& rule : *rules)
        if (opad::entity_matches(e, rule.select)) rule.ordinals.push_back(i - 1);
    }
  }, [this, input, picked, rules, where](bool ok, const QString&) {
    if (!ok || rules->empty() || !m_featureOn || !where) return;
    QMenu menu;
    for (const auto& rule : *rules) {
      QAction* a = menu.addAction(tr("%1 (%2)").arg(rule.label).arg(rule.ordinals.size()));
      connect(a, &QAction::triggered, this, [this, input, picked, rule] {
        if (!m_featureOn) return;
        std::vector<opad::Ref> matched;
        for (int i : rule.ordinals) {
          opad::Ref r = picked;
          r.index = i;
          matched.push_back(r);
        }
        m_ruleMatches[input] = matched;
        const std::string kind = picked.kind == opad::Ref::Kind::Face ? "face" : "edge";
        m_form->setPicks(input, opad::json::array({{{"body", picked.body}, {"kind", kind}, {"select", rule.select}, {"expect", rule.ordinals.size()}}}));
        if (m_form->activeInput() == input) syncSelectionToInput();
        schedulePreview();
      });
    }
    if (qEnvironmentVariableIsSet("OPAD_BENCH_RULE")) return menu.actions().front()->trigger();  // the bench takes the first
    menu.exec(where->mapToGlobal(QPoint(0, where->height())));
  });
}

void DesignController::activateInput(const QString& name) {
  if (!m_featureOn) return;
  const InputSpec* in = m_form->input(name);
  if (!in) {
    if (Job* j = std::exchange(m_candidateJob, nullptr)) j->cancel();
    m_activeCandidates.clear();
    refreshRoute();  // nothing routed without an active input
    // A construction plane's From plane (or any plane input) takes a click on a planar face, an origin plane or a
    // construction plane while nothing else is being picked (TODO 11 P3): the guide's face click.
    m_idlePlane = idlePlaneInput();
    if (m_idlePlane.isEmpty()) return showAllCandidates();
    m_activeCandidates = quickCandidates("plane");
    showAllCandidates();
    if (m_viewport->selectionFilter() != Viewport::SelFilter::Face || m_viewport->roundFacesPickable()) {
      m_activating = true;
      auto once = std::make_shared<QMetaObject::Connection>();
      *once = connect(m_viewport, &Viewport::filterApplied, this, [this, once] {
        disconnect(*once);
        m_activating = false;
        syncSelectionToInput();
      });
      m_viewport->setSelectionFilter(Viewport::SelFilter::Face);
    } else {
      syncSelectionToInput();
    }
    if (const InputSpec* plane = m_form->input(m_idlePlane)) emit status(tr("%1: click a plane or a planar face in the view").arg(i18n::t(QString::fromStdString(plane->label))));
    return;
  }
  m_idlePlane.clear();
  if (Job* j = std::exchange(m_idleJob, nullptr)) j->cancel();
  if(in->type=="plane") {
    m_pickPlane=true;m_activating=false;
    m_planePicked=[this,name](opad::json plane,opad::Frame){
      // The plane input lets go once it has its plane, so a click on it opens the picker again (not deactivates it).
      if(!m_featureOn)return;m_form->setPicks(name,plane);schedulePreview();m_openPanel(m_panel);m_form->activate(QString());m_form->activateNextPick();
    };
    // Not the selection: that is the feature's other picks (a single draft face became its own neutral plane).
    emit stateChanged();QTimer::singleShot(0,this,[this]{if(m_pickPlane&&m_featureOn){m_planePicker->panel()->setHeader("plane",tr("Choose plane"));m_planePicker->start(false,m_openPanel,false);}});return;
  }
  const Viewport::SelFilter want = filterFor(in->type);
  const bool roundFaces = in->type == "axis";  // a cylinder's, cone's or torus's face gives its axis (the guide's face click)
  showCandidatesFor(QString::fromStdString(in->type));
  refreshRoute();
  if (m_viewport->selectionFilter() != want || m_viewport->roundFacesPickable() != roundFaces) {
    m_activating = true;
    auto once = std::make_shared<QMetaObject::Connection>();
    *once = connect(m_viewport, &Viewport::filterApplied, this, [this, once] {
      disconnect(*once);
      m_activating = false;
      syncSelectionToInput();
    });
    m_viewport->setSelectionFilter(want, roundFaces);  // sliced; the old picks are re-applied once every body is in the new mode
  } else {
    syncSelectionToInput();
  }
  pickStatus();
}

void DesignController::pickStatus() {
  const InputSpec* active = m_form->input(m_form->activeInput());
  if (!active) return;
  const QString label = i18n::t(QString::fromStdString(active->label));
  if (const InputSpec* routed = m_form->input(m_routeInput))
    return emit status(tr("%1: pick in the view · %2: click one in the view").arg(label, i18n::t(QString::fromStdString(routed->label))));
  emit status(tr("%1: pick in the view").arg(label));
}

bool DesignController::placedPlane(const InputSpec& in) const { return in.type == "plane" && m_form->input("x") && m_form->input("y"); }

QString DesignController::routeTarget() const {
  if (!m_featureOn || !m_form->spec()) return {};
  const QString active = m_form->activeInput();
  const InputSpec* in = m_form->input(active);
  const bool many = in && (in->type == "bodies" || in->type == "faces" || in->type == "edges" || in->type == "profiles" || in->type == "points") && in->max_count != 1;
  if (!many) return {};
  const opad::json picks = m_form->picks(active);
  if (!picks.is_array() || picks.empty()) return {};
  const opad::json inputs = m_form->inputs();
  for (const auto& other : m_form->spec()->inputs) {
    if ((other.type != "axis" && other.type != "plane") || other.optional || other.name == in->name || placedPlane(other) || !input_active(other, inputs)) continue;
    const QString name = QString::fromStdString(other.name);
    const opad::json value = m_form->picks(name);
    if (value.is_null() || value == other.def || m_routed.count(name)) return name;
  }
  return {};
}

void DesignController::refreshRoute() {
  const QString target = routeTarget();
  if (target == m_routeInput) return;
  m_routeInput = target;
  ++m_routeSerial;
  if (Job* j = std::exchange(m_routeJob, nullptr)) j->cancel();
  m_routeCandidates.clear();
  m_routeIds.clear();
  const InputSpec* in = m_form->input(target);
  if (in) {
    m_routeCandidates = quickCandidates(in->type);
    for (const auto& c : m_routeCandidates) m_routeIds.insert(c.id);
    if (in->type == "axis") {  // and the sketches' lines, from a worker
      auto found = std::make_shared<std::vector<Viewport::Candidate>>();
      const int serial = m_routeSerial;
      m_routeJob = sketchCandidates("axis", found, [this, found, serial](bool ok) {
        if (serial != m_routeSerial) return;
        m_routeJob = nullptr;
        if (!ok || !m_featureOn || found->empty()) return;
        for (const auto& c : *found) m_routeIds.insert(c.id);
        m_routeCandidates.insert(m_routeCandidates.end(), found->begin(), found->end());
        showAllCandidates();
        syncSelectionToInput();
      });
    }
  }
  showAllCandidates();
  syncSelectionToInput();
  pickStatus();
}

bool DesignController::routeClick() {
  const InputSpec* target = m_form->input(m_routeInput);
  if (!target) return false;
  const opad::json value = m_form->picks(m_routeInput);
  const std::string current = value.is_object() ? value.dump() : std::string();
  std::vector<std::string> routed;
  for (const auto& c : m_viewport->selectedCandidates())
    if (m_routeIds.count(c)) routed.push_back(c);
  std::string chosen;
  for (const auto& c : routed)
    if (c != current) chosen = c;
  if (!chosen.empty()) {
    m_form->setPicks(m_routeInput, opad::json::parse(chosen));
    m_routed.insert(m_routeInput);
  } else if (routed.empty() && m_routeIds.count(current) && m_viewport->lastClickHit()) {
    m_form->setPicks(m_routeInput, target->def);  // its marked axis or plane clicked again: back to the default, or none
  } else {
    return false;  // the click was for the active input
  }
  syncSelectionToInput();
  schedulePreview();
  return true;
}

// Only for a feature that picks nothing but planes and axes (a construction plane): where bodies, faces or profiles are
// picked too (Draft, Mirror), a click with nothing active is not taken for the plane, and the routing above serves.
QString DesignController::idlePlaneInput() const {
  if (!m_featureOn || !m_form->spec() || !m_form->activeInput().isEmpty()) return {};
  const opad::json inputs = m_form->inputs();
  for (const auto& in : m_form->spec()->inputs)
    if (FeaturePanel::isPick(in.type) && in.type != "plane" && in.type != "axis" && input_active(in, inputs)) return {};
  QString first;
  for (const auto& in : m_form->spec()->inputs) {
    if (in.type != "plane" || placedPlane(in) || !input_active(in, inputs)) continue;
    const QString name = QString::fromStdString(in.name);
    if (!in.optional && m_form->picks(name).is_null()) return name;  // an empty one first (a midplane's second plane)
    if (first.isEmpty()) first = name;
  }
  return first;
}

void DesignController::idlePlaneClick() {
  const auto refs = m_viewport->selection();
  const auto candidates = m_viewport->selectedCandidates();
  if (refs.empty() && candidates.empty() && !m_viewport->lastClickHit()) return syncSelectionToInput();  // empty space
  opad::json current = m_form->picks(m_idlePlane);
  if (current.is_object()) current.erase("frame");
  // What the click added beside the plane shown (picks accumulate): a candidate or a face that is not the current one. A
  // click on the current one keeps it: a plane input always has a plane.
  opad::json chosen;
  for (const auto& c : candidates)
    if (opad::json::parse(c) != current) chosen = opad::json::parse(c);
  for (const auto& r : refs) {
    if (r.kind != opad::Ref::Kind::Face) continue;
    const bool same = current.contains("face") && current["face"].value("body", "") == r.body && current["face"].value("index", -1) == r.index;
    if (!same) chosen = opad::json{{"face", pickToJson(r)}};
  }
  if (chosen.is_null()) return syncSelectionToInput();
  pickIdlePlane(m_idlePlane, chosen);
}

void DesignController::pickIdlePlane(const QString& input, opad::json support) {
  const int serial = ++m_idleSerial;
  if (Job* j = std::exchange(m_idleJob, nullptr)) j->cancel();
  if (!support.contains("face")) {  // an origin or a construction plane: as it is
    m_form->setPicks(input, support);
    syncSelectionToInput();
    schedulePreview();
    return;
  }
  // A face: planar? Its reference with the hint that finds it again. On a worker, as the plane picker resolves its face.
  refreshPlanCopies();
  auto doc = m_planDoc;
  auto scene = m_planScene;
  auto plane = std::make_shared<opad::json>(std::move(support));
  m_idleJob = m_jobs->async(tr("Resolving the plane"), [doc, scene, plane](Progress p) {
    Reading reading;
    if (p.cancelled()) return;
    resolve_plane(*doc, *scene, *plane);
    (*plane)["face"] = make_ref(*doc, *scene, opad::Ref::from_json(plane->at("face")));
  }, [this, serial, input, plane](bool ok, const QString& error) {
    if (serial != m_idleSerial || !m_featureOn) return;
    m_idleJob = nullptr;
    if (!ok) {
      if (error != "cancelled") m_form->setStatus(i18n::t(error), true);
      return syncSelectionToInput();
    }
    m_form->setPicks(input, *plane);
    syncSelectionToInput();
    schedulePreview();
  });
}

void DesignController::inputsSettled() {
  if (!m_featureOn) return;
  if (!m_form->activeInput().isEmpty()) return refreshRoute();  // Rotate ticked on a move: its axis can be clicked
  if (idlePlaneInput() != m_idlePlane) activateInput(QString());  // the Type changed which plane a click fills
}

void DesignController::resetRouting() {
  ++m_routeSerial;
  ++m_idleSerial;
  if (Job* j = std::exchange(m_routeJob, nullptr)) j->cancel();
  if (Job* j = std::exchange(m_idleJob, nullptr)) j->cancel();
  m_routeInput.clear();
  m_routeIds.clear();
  m_routeCandidates.clear();
  m_activeCandidates.clear();
  m_routed.clear();
  m_idlePlane.clear();
}

void DesignController::refreshPlanCopies() {
  // The rolled-back state the picks were made in. Copied once per document state, not once per plan.
  const auto stamp_now = std::make_tuple(m_doc->generation, m_doc->revision, m_doc->doc.ops.size());
  if (!m_planDoc || !m_planScene || m_planStamp != stamp_now) {
    m_planDoc = std::make_shared<const opad::Document>(m_doc->doc);
    m_planScene = std::make_shared<const opad::Scene>(m_doc->scene);
    m_planStamp = stamp_now;
  }
}

void DesignController::viewportSelectionChanged() {
  if (m_activating) return;
  if (m_pickPlane) {m_planePicker->selectionChanged();return;}
  if (!m_featureOn) return;
  const QString name = m_form->activeInput();
  if (name.isEmpty() && !m_idlePlane.isEmpty()) return idlePlaneClick();
  const InputSpec* in = m_form->input(name);
  if (!in) return;
  // A plane comes from the plane picker only. Its clearing the selection on the way out used to arrive here late and
  // wipe the plane it had just set (a box on XY then waited for "Pick: Plane" with no preview).
  if (in->type == "plane") return;
  // A click on empty space picks nothing: the input keeps what it has. (OCCT drops the whole selection on such a
  // click, which wiped a combine's tool bodies when the view was clicked before pressing Enter.) Clicking a picked
  // item still un-picks it: that click hits something.
  if (m_viewport->selection().empty() && m_viewport->selectedCandidates().empty() && !m_viewport->lastClickHit()) {
    syncSelectionToInput();
    return;
  }
  if (!m_routeInput.isEmpty() && routeClick()) return;  // the next input's axis or plane, clicked
  // The viewport selection is the pick list: bodies and sub-shapes by reference, everything else by candidate.
  opad::json picks = opad::json::array();
  for (const auto& r : m_viewport->selection()) {
    if (in->type == "plane") picks.push_back(opad::json{{"face", pickToJson(r)}});
    else if (in->type == "axis") picks.push_back(opad::json{{r.kind == opad::Ref::Kind::Face ? "face" : "edge", pickToJson(r)}});  // faces: after 2 (Faces)
    else picks.push_back(pickToJson(r));
  }
  for (const auto& c : m_viewport->selectedCandidates())
    if (!m_routeIds.count(c)) picks.push_back(opad::json::parse(c));
  const bool single = in->type == "plane" || in->type == "axis" || in->type == "path" || in->max_count == 1;
  if (single && picks.size() > 1) {
    m_activating = true;
    m_viewport->keepLastSelected();  // a new pick replaces the old one
    m_activating = false;
    return viewportSelectionChanged();
  }
  if (in->max_count > 1 && static_cast<int>(picks.size()) > in->max_count) {
    m_activating = true;
    m_viewport->deselectLast();
    m_activating = false;
    return;
  }
  const opad::json had = m_form->picks(name);
  const bool first = had.is_null() || (had.is_array() && had.empty());
  m_form->setPicks(name, picks);
  if (!picks.empty()) m_nothingToPick.clear();  // a vertex or a face did it
  schedulePreview();
  if (single && !picks.empty()) m_form->activateNextPick();
  else if (in->max_count > 0 && static_cast<int>(picks.size()) == in->max_count) m_form->activateNextPick();
  else if (in->advance && first && !picks.empty()) m_form->activateNextPick();  // Combine: the target, then the tools
  refreshRoute();  // the first pick shows the next input's axes or planes
}

DimensionHandle* DesignController::distanceHandle() const { return m_distanceHandle; }

// The panel's values that show, in its order (UI-122): one box each, grey with what the panel holds until typed into.
QList<DynamicInput::Field> DesignController::valueFields(const QString& except) const {
  QList<DynamicInput::Field> out;
  for (const QString& name : m_form->valueInputs()) {
    const InputSpec* in = m_form->input(name);
    if (name != except && in) out << ToolValues::box(name, i18n::t(QString::fromStdString(in->label)), m_form->valueText(name));
  }
  return out;
}

// What is typed goes into the panel at once (the preview follows as for a value typed there); a bare number in an angle's
// box is in the shown unit (UI-123). A box whose text does not evaluate turns red and says why.
void DesignController::typeValue(const QString& key, QString value) {
  const InputSpec* in = m_featureOn ? m_form->input(key) : nullptr;
  if (!in) return;
  bool plain = false;
  value.trimmed().toDouble(&plain);
  if (plain && in->type == "angle") value = value.trimmed() + (units::current().radians ? " rad" : " deg");
  m_form->setValue(key, value.toStdString());
  const QString problem = m_form->problem(key);
  m_values->input()->setProblem(key, problem);
  m_distanceHandle->setProblem(key, problem);
}

void DesignController::refreshValues() {
  if (!m_featureOn) return;
  m_values->refresh();
  if (m_distanceHandle->isVisible()) m_distanceHandle->setExtraFields(valueFields("distance"));
}

void DesignController::schedulePreview() {
  if (!m_featureOn) return;
  m_readyPlan.reset();
  // While the handle is pulled, preview as fast as plans come back (the latest value wins) instead of waiting for
  // the pointer to rest: the body follows the drag as if its face were dragged. Otherwise inputs settle first.
  if (m_distanceHandle && m_distanceHandle->dragging()) {
    stretchPreview(m_distanceHandle->value());  // at once: plans take 15 ms here, 150 ms on a large model
    m_previewTimer.stop();
    if (m_planJob) m_previewPending = true;
    else runPreview(false);
    return;
  }
  m_previewTimer.start();
}

void DesignController::stretchPreview(double value) {
  if (!m_stretch.valid || std::fabs(m_stretch.from) < 1e-9) return;
  const double k = value / m_stretch.from;
  size_t vertices = 0;
  for (const auto& base : m_stretch.base)
    if (base && !base->triangles.IsNull()) vertices += size_t(base->triangles->VertexNumber());
  if (vertices > 600000) return;  // copying that many per mouse move would itself lag: the plans alone update it
  std::vector<std::shared_ptr<const BodyPrs>> shown;
  for (const auto& base : m_stretch.base) shown.push_back(base && std::fabs(k - 1) > 1e-12 ? stretchedPrs(*base, m_stretch, k) : nullptr);
  m_viewport->setPreviewDisplay(shown);
}

void DesignController::runPreview(bool commit) {
  if (!m_featureOn) return;
  m_previewTimer.stop();
  QString missing;
  if (!m_form->complete(&missing)) {
    if(!m_distanceHandle->interacting())m_distanceHandle->hide();
    m_form->setStatus(m_nothingToPick.isEmpty() ? missing : m_nothingToPick, commit);
    m_viewport->clearPreviewBodies();
    return;
  }
  const opad::json inputs = m_form->inputs();
  const std::string name = m_form->name().toStdString();
  // A new feature is made in the active component (UI-33): its new bodies, plane or axis go there, in its frame.
  const std::string component = m_editing.empty() ? m_doc->activeComponent() : std::string();
  const std::string stamp = inputs.dump() + "|" + name + "|" + component;
  auto commitReady = [this, component] {
    auto plan = m_readyPlan;
    const QString label = m_form->spec() ? i18n::t(QString::fromStdString(m_form->spec()->label)).toLower() : tr("feature");
    // The new bodies' name, colour and component: the rename / appearance / reparent ops of the same step (B14). No
    // reparent into the component the feature is made in: its bodies are there already.
    opad::json style = m_editing.empty() ? m_form->bodyStyle() : opad::json::object();
    if (!component.empty() && style.value("parent", opad::json()) == opad::json(component)) style.erase("parent");
    const std::string op = m_newId;
    whenNobodyReads(this, [this, plan, label, style, op] {
      try {
        m_viewport->clearPreviewBodies();
        m_doc->setRollback({});
        if (!style.empty()) style_new_bodies(*plan, op, style);
        const opad::json rep = m_doc->commitPlan(std::move(*plan), label);
        const size_t errors = rep.value("errors", opad::json::array()).size();
        endFeature();
        if (errors > 0) emit notice(tr("%1 later feature(s) could not be recomputed; they are marked on the timeline.").arg(errors));
      } catch (const std::exception& e) {
        m_form->setStatus(i18n::t(QString::fromUtf8(e.what())), true);
      }
    });
  };
  if (commit && m_readyPlan && m_readyInputs == stamp && m_readyOps == m_doc->doc.ops.size()) return commitReady();

  if (Job* j = std::exchange(m_planJob, nullptr)) j->cancel();
  const int serial = ++m_planSerial;
  const std::string target = m_editing.empty() ? m_newId : m_editing;
  const std::string kind = m_form->spec()->kind;
  const bool editing = !m_editing.empty();
  auto plan = std::make_shared<Plan>();
  auto anchors=std::make_shared<std::vector<DimensionHandle::Segment>>();
  auto meshes = std::make_shared<std::vector<std::shared_ptr<const BodyPrs>>>();  // per plan->changed entry
  refreshPlanCopies();
  auto scene = m_planScene;
  auto doc = m_planDoc;
  const bool symmetric = inputs.value("direction", "") == "symmetric";
  const opad::json editResult = editing ? m_editResult : opad::json();
  // A construction plane or axis makes no body: its preview is the plane (a square about the model's size) or the axis
  // line, else nothing showed where it would go.
  const double reach = kind == "plane" || kind == "axis" ? modelReach() : 0.0;
  auto construction = std::make_shared<std::vector<Viewport::PreviewPart>>();
  m_form->setStatus(tr("Computing…"), false);
  m_planJob = m_jobs->async(tr("Computing %1").arg(m_form->name()), [doc, scene, inputs, name, target, kind, editing, plan, anchors, meshes, symmetric, editResult, reach, construction, component](Progress p) {
    Reading reading;
    const opad::json hinted = hint_refs(*doc, *scene, inputs);
    opad::json op = editing ? make_edit_op(target, opad::json{{"inputs", hinted}, {"name", name}}) : make_feature_op(kind, name, hinted);
    if (!editing) op["id"] = target;
    if (!component.empty()) op["component"] = component;
    try {
      *plan = plan_ops(*doc, {op}, true, [p] { return p.cancelled(); });
    } catch (const opad::LockedError& e) {
      throw opad::Error(AppDocument::lockedMessage(e).toStdString());
    }
    // An edit that changes nothing is not recomputed (its fingerprint matches), so the plan has nothing to show and the
    // rolled-back view was empty while the feature was open: show what it makes now. Copies are meshed, not the cached
    // prototypes the view draws.
    if (editing && std::none_of(plan->changed.begin(), plan->changed.end(), [&](const Plan::Changed& c) { return c.op == target; })) {
      for (const auto& entry : editResult.value("bodies", opad::json::array())) {
        if (p.cancelled()) return;
        const std::string id = entry.value("id", ""), key = entry.value("key", "");
        if (key.empty() || !doc->has_body(key)) continue;
        TopoDS_Shape shape = BRepBuilderAPI_Copy(opad::body_shape(*doc, key)).Shape();
        const opad::Node* node = scene->node(id);
        const std::string parent = node ? node->parent : entry.value("parent", "");
        if (!parent.empty() && scene->node(parent)) try {
          shape = BRepBuilderAPI_Transform(shape, opad::trsf_from_mat(scene->world(parent)), Standard_True).Shape();
        } catch (const std::exception&) {
        }
        plan->changed.push_back({target, id, std::make_shared<TopoDS_Shape>(shape), false});
      }
      for (const auto& removed : editResult.value("removed", opad::json::array()))
        if (removed.is_string()) plan->changed.push_back({target, removed.get<std::string>(), nullptr, true});
    }
    meshes->resize(plan->changed.size());
    for (size_t i = 0; i < plan->changed.size(); ++i) {  // the preview is displayed without meshing or walking meshes on the UI thread
      auto& c = plan->changed[i];
      if (c.op == target && c.shape && !c.shape->IsNull()) {
        if (p.cancelled()) return;
        Bnd_Box box;
        BRepBndLib::Add(*c.shape, box, Standard_False);
        const double defl = box.IsVoid() ? 0.1 : std::clamp(std::sqrt(box.SquareExtent()) * 0.002, 0.02, 2.0);
        BodyPrs::meshForDisplay(*c.shape, defl);
        (*meshes)[i] = BodyPrs::build(*c.shape, box, true);
      }
    }
    if (reach > 0) for (const auto& result : resultsFor(*plan, target, editResult)) {
      if (p.cancelled() || !result.is_object()) continue;
      TopoDS_Shape shape;
      if (result.contains("plane")) {
        const opad::Frame f = opad::Frame::from_json(result.at("plane"));
        const opad::Vec3 n = f.normal();
        const double h = reach * 0.6;
        shape = BRepBuilderAPI_MakeFace(gp_Pln(gp_Ax3(gp_Pnt(f.origin[0], f.origin[1], f.origin[2]), gp_Dir(n[0], n[1], n[2]), gp_Dir(f.x[0], f.x[1], f.x[2]))), -h, h, -h, h).Face();
      } else if (result.contains("axis")) {
        const auto& a = result.at("axis");
        const gp_Pnt o(a["origin"][0], a["origin"][1], a["origin"][2]);
        const gp_Vec d = gp_Vec(a["dir"][0], a["dir"][1], a["dir"][2]).Normalized() * reach;
        shape = BRepBuilderAPI_MakeEdge(o.Translated(-d), o.Translated(d)).Edge();
      }
      if (shape.IsNull()) continue;
      Bnd_Box box;
      BRepBndLib::Add(shape, box, Standard_False);
      BodyPrs::meshForDisplay(shape, std::max(0.01, reach * 0.001));
      construction->push_back({std::string(), shape, BodyPrs::build(shape, box, true)});
    }
    if(kind=="extrude")for(const auto& result:resultsFor(*plan,target,editResult)){
      if(!result.contains("distance_handle"))continue;const auto& h=result.at("distance_handle");
      const auto origin=h.at("origin").get<opad::Vec3>(),axis=h.at("axis").get<opad::Vec3>();const double value=h.at("value").get<double>()*(symmetric?0.5:1.0);
      const gp_Vec direction(axis[0],axis[1],axis[2]);const auto tip=gp_Pnt(origin[0],origin[1],origin[2]).Translated(direction*value);
      for(const auto& c:plan->changed)if(c.op==target && c.shape)for(TopExp_Explorer edges(*c.shape,TopAbs_EDGE);edges.More();edges.Next()){
        if(p.cancelled())return;const auto points=curveSamples(TopoDS::Edge(edges.Current()),.05);
        for(size_t i=1;i<points.size();++i)if(std::abs(gp_Vec(tip,points[i-1]).Dot(direction))<1e-6 && std::abs(gp_Vec(tip,points[i]).Dot(direction))<1e-6){
          const auto a=points[i-1].Translated(-direction*value),b=points[i].Translated(-direction*value);anchors->push_back({opad::Vec3{a.X(),a.Y(),a.Z()},opad::Vec3{b.X(),b.Y(),b.Z()}});
        }
      }
    }
  }, [this, serial, plan, stamp, target, commit, commitReady, anchors, meshes, symmetric, editResult, construction](bool ok, const QString& error) {
    if (serial != m_planSerial || !m_featureOn) return;  // superseded
    m_planJob = nullptr;
    // A drag moved on while this plan ran: show this one, then plan the latest value.
    if (std::exchange(m_previewPending, false) && !commit) QTimer::singleShot(0, this, [this] { if (m_featureOn && !m_planJob) runPreview(false); });
    if (!ok) {
      if(!m_distanceHandle->interacting())m_distanceHandle->hide();
      m_readyPlan.reset();
      m_viewport->clearPreviewBodies();
      // Before anything is picked a refusal is the guidance (a shell: "pick faces to remove, or a body to hollow"),
      // not an error: it was red as the panel opened.
      bool picked = false, picks = false;
      for (const auto& in : m_form->spec()->inputs)
        if (in.type == "bodies" || in.type == "faces" || in.type == "edges" || in.type == "points" || in.type == "profiles") {
          picks = true;
          const opad::json p = m_form->picks(QString::fromStdString(in.name));
          picked = picked || (p.is_array() && !p.empty());
        }
      if (error != "cancelled") m_form->setStatus(i18n::t(error), picked || !picks);
      return;
    }
    m_readyPlan = plan;
    m_readyInputs = stamp;
    m_readyOps = m_doc->doc.ops.size();
    m_form->setStatus(QString(), false);
    if (commit) return commitReady();
    bool hasHandle=false;
    m_stretch = {};
    for(const auto& result:resultsFor(*plan,target,editResult)) {
      if(result.is_object() && result.contains("check")) {  // an interference check: what it found (gap log #10)
        const auto& found=result.at("check");const int overlaps=found.value("interferences",0),close=found.value("too_close",0);
        m_form->setStatus(overlaps||close?tr("%1 interference(s), %2 pair(s) too close").arg(overlaps).arg(close):tr("No interference"),result.contains("error"));
      }
      if(result.is_object() && result.contains("distance_handle")) {
        hasHandle=true;const auto& handle=result.at("distance_handle");
        const auto origin=handle.at("origin").get<opad::Vec3>(),axis=handle.at("axis").get<opad::Vec3>();const double value=handle.at("value").get<double>();
        // What the live stretch needs: the profile plane, the axis, this plan's distance and the profile's footprint.
        const gp_Vec a(axis[0],axis[1],axis[2]);
        if(a.Magnitude()>1e-12) {
          const gp_Dir n(a);const gp_Dir u=std::abs(n.Z())<0.9?n.Crossed(gp::DZ()):n.Crossed(gp::DX());const gp_Dir v=n.Crossed(u);
          m_stretch.valid=true;m_stretch.symmetric=symmetric;m_stretch.from=value;m_stretch.origin=origin;
          m_stretch.axis={n.X(),n.Y(),n.Z()};m_stretch.u={u.X(),u.Y(),u.Z()};m_stretch.v={v.X(),v.Y(),v.Z()};
          const gp_Pnt o(origin[0],origin[1],origin[2]);double u0=1e300,u1=-1e300,v0=1e300,v1=-1e300;
          for(const auto& segment:*anchors)for(const auto& end:segment){const gp_Vec rel(o,gp_Pnt(end[0],end[1],end[2]));
            u0=std::min(u0,rel.Dot(gp_Vec(u)));u1=std::max(u1,rel.Dot(gp_Vec(u)));v0=std::min(v0,rel.Dot(gp_Vec(v)));v1=std::max(v1,rel.Dot(gp_Vec(v)));}
          if(u0<=u1 && v0<=v1) {
            const double margin=std::max(1e-6,0.02*std::hypot(u1-u0,v1-v0));
            m_stretch.footprint=true;m_stretch.u0=u0-margin;m_stretch.u1=u1+margin;m_stretch.v0=v0-margin;m_stretch.v1=v1+margin;
          }
        }
        m_distanceHandle->setScale(symmetric?0.5:1.0);  // a symmetric extrusion's end moves half the distance: so does the arrow
        m_distanceHandle->setAnchorSegments(std::move(*anchors));
        m_distanceHandle->setExtraFields(valueFields("distance"));  // Tab goes on to the taper (UI-122)
        m_distanceHandle->configure(origin,axis,value,QString::fromStdString(m_form->inputs().at("distance").get<std::string>()));
      }
    }
    if(!hasHandle)m_distanceHandle->hide();
    m_values->refresh();  // the boxes beside the pointer give way to the handle's
    std::vector<Viewport::PreviewPart> parts;
    std::vector<std::string> hidden;
    for (size_t i = 0; i < plan->changed.size(); ++i) {
      const auto& c = plan->changed[i];
      if (c.op != target) continue;
      if (c.removed) hidden.push_back(c.node);
      else if (c.shape && !c.shape->IsNull()) parts.push_back({m_doc->scene.node(c.node) ? c.node : std::string(), *c.shape, i < meshes->size() ? (*meshes)[i] : nullptr});
    }
    parts.insert(parts.end(), construction->begin(), construction->end());
    m_viewport->setPreviewBodies(parts, hidden);
    if (m_stretch.valid) for (const auto& part : parts) m_stretch.base.push_back(part.prs);
    if (m_distanceHandle->dragging()) stretchPreview(m_distanceHandle->value());  // this plan is for an older value
  });
}

// ---------------------------------------------------------------- sketches
opad::json DesignController::recoveryState() const {
  if(!featureActive())return {};
  return {{"type","feature"},{"id",m_editing},{"kind",m_form->spec()->kind},{"inputs",m_form->inputs()}};
}
void DesignController::restoreRecovery(const opad::json& state) {
  if(state.is_null() || state.empty())return;
  if(state.value("type","")=="sketch")m_sketch->restoreRecovery(state);
  else if(state.value("type","")=="feature") {
    const auto id=state.value("id",std::string());
    if(id.empty())startFeature(QString::fromStdString(state.at("kind").get<std::string>()));else editOp(id);
    if(featureActive())for(const auto& [key,value]:state.at("inputs").items()){
      if(value.is_array() || value.is_object())m_form->setPicks(QString::fromStdString(key),value);
      else m_form->setValue(QString::fromStdString(key),value);
    }
  }
  emit stateChanged();
}
void DesignController::pickSketchPlane(std::function<void(opad::json,opad::Frame)> done,bool positionOrigin) {
  m_positionOrigin=positionOrigin;
  if(!m_doc->hasDocument || m_sketch->active()) return;
  if(m_pickPlane) escape();
  m_planePicked=std::move(done);
  beginPlanePick();
}

void DesignController::startSketch() {
  m_positionOrigin=true;
  m_planePicked={};
  if (!m_doc->hasDocument || m_doc->browse) return;
  if (m_sketch->active()) return;
  beginPlanePick();
}

void DesignController::setSketchPanel(ToolPanel* panel) {
  m_sketchPanel=panel;
  connect(m_sketch,&SketchEditor::toolChanged,this,[this]{if(m_sketch->tool()=="select"){if(m_sketchPanel)m_sketchPanel->hide();}else showSketchPanel();});
  connect(m_sketch,&SketchEditor::workflowChanged,this,&DesignController::stateChanged);
  connect(m_sketch,&SketchEditor::changed,this,[this]{if(!m_sketch->active() && m_sketchPanel)m_sketchPanel->hide();});
}
void DesignController::showSketchPanel(const QString& page) {
  if(m_sketch->active() && m_sketchPanel && m_openPanel){
    // A page (Constraints, Snaps, Selection) is named in the header: it read "Select", the tool, above the constraint list.
    if(!page.isEmpty())m_sketchPanel->setHeader("sketch",page);
    else {  // a tool outside the panel's registry (paste, copy with base point) by its steps' name
      const auto& registry=SketchPanel::tools();
      const auto tool=std::find_if(registry.begin(),registry.end(),[this](const SketchPanel::Tool& t){return t.id==m_sketch->tool();});
      const auto* steps=sketchsteps::find(m_sketch->tool().toStdString());
      if(tool!=registry.end())m_sketchPanel->setHeader("sketch",tool->label);else if(steps)m_sketchPanel->setHeader("sketch",i18n::t(steps->name));
    }
    m_openPanel(m_sketchPanel);
  }
}
void DesignController::redefineSketchPlane() {
  if(!m_sketch->active() || m_sketch->busy() || m_doc->designBusy)return;
  m_viewport->endSketchInput();m_replaning=true;m_positionOrigin=true;
  m_planePicked=[this](opad::json plane,opad::Frame frame){m_sketch->redefinePlane(plane,frame);};
  beginPlanePick();
}

void DesignController::beginPlanePick() {
  if(m_featureOn)endFeature();
  m_pickPlane=true;m_activating=false;
  emit stateChanged();
  m_planePicker->panel()->setHeader("plane",tr("Choose sketch plane"));
  m_planePicker->start(m_positionOrigin,m_openPanel);
}

// Ctrl+Z while a feature's panel is open (UI-116): the last pick goes, from the input being picked or, when that has none
// (a one-pick input hands over to the next as soon as it is filled), from the last input before it that has picks. The
// document's own undo waits until the panel is closed. A plane is picked again with its picker, not taken back.
bool DesignController::undoPick() {
  if (!m_featureOn || !m_form->spec() || m_doc->designBusy) return false;
  auto listed = [this](const QString& name) {
    opad::json picks = m_form->picks(name);
    if (picks.is_object()) picks = opad::json::array({picks});
    return picks.is_array() ? picks : opad::json::array();
  };
  QString name = m_form->activeInput();
  const InputSpec* in = m_form->input(name);
  if (!in || listed(name).empty()) {
    in = nullptr;
    const opad::json inputs = m_form->inputs();
    for (const InputSpec& spec : m_form->spec()->inputs) {
      if (QString::fromStdString(spec.name) == name) break;
      if (spec.type != "plane" && FeaturePanel::isPick(spec.type) && opad::design::input_active(spec, inputs) && !listed(QString::fromStdString(spec.name)).empty()) in = &spec;
    }
    if (!in) {
      emit status(tr("No pick to take back · Esc closes the feature"));
      return false;
    }
    name = QString::fromStdString(in->name);
    m_form->activate(name);  // its picks become the selection again
  }
  if (in->type == "plane") {
    emit status(tr("Pick the plane again to change it"));
    return false;
  }
  opad::json picks = listed(name);
  picks.erase(picks.end() - 1);
  m_form->setPicks(name, picks);
  m_ruleMatches.erase(name);
  syncSelectionToInput();
  refreshRoute();  // its last pick gone: the next input's axes or planes go too
  schedulePreview();
  emit status(tr("Took back the last pick of %1 · Ctrl+Z again for the one before").arg(i18n::t(QString::fromStdString(in->label))));
  return true;
}

bool DesignController::escape() {
  if(m_pickPlane){
    if(m_planePicker->active())m_planePicker->cancel();
    else {m_pickPlane=false;m_planePicked={};emit stateChanged();}
    return true;
  }
  if (m_featureOn) {
    endFeature();
    return true;
  }
  return false;
}

std::string DesignController::editingOp() const {
  if (m_sketch->active()) return m_sketch->sketchId();
  return m_featureOn ? m_editing : std::string();
}

void DesignController::enterSketch(const std::string& sketchId, const QString& name, const opad::json& plane, const opad::Frame& frame, const opad::json& geometry) {
  try {
    m_sketch->begin(sketchId, name, plane, frame, geometry);
  } catch (const std::exception& e) {
    m_doc->setRollback({});
    emit failed(QString::fromUtf8(e.what()));
  }
  emit stateChanged();
}

void DesignController::finishSketch(std::function<void()> then) {
  if(m_sketch->busy())return emit notice(tr("Wait for the sketch operation to finish."));
  if (!m_sketch->active()) return;
  auto leave = [this, then] {
    m_sketch->end();
    m_doc->setRollback({});
    emit status(QString());
    emit stateChanged();
    if (then) then();
  };
  if (m_sketch->sketchId().empty() && m_sketch->empty()) return leave();  // nothing was drawn: no op
  if (!m_sketch->sketchId().empty() && !m_sketch->modified()) return leave();
  opad::json op;
  if (m_sketch->sketchId().empty()) {
    op = make_sketch_op(m_sketch->name().toStdString(), m_sketch->plane(), m_sketch->geometry());
    if (!m_doc->activeComponent().empty()) op["component"] = m_doc->activeComponent();  // made in the active component (UI-33)
  } else {
    opad::json plane = m_sketch->plane();  // a plane picked now goes in where the sketch's component was when it was made
    if (const opad::SketchItem* s = m_doc->scene.sketch(m_sketch->sketchId()); s && plane != s->plane) plane = plane_as_made(*s, std::move(plane));
    op = make_edit_op(m_sketch->sketchId(), opad::json{{"geometry_delta", m_sketch->geometryDelta()}, {"plane", plane}});
  }
  applyOps({op}, m_sketch->sketchId().empty() ? tr("sketch") : tr("edit sketch"), [this, leave](bool ok, const QString& error) {
    if (!ok) return emit failed(error);  // stay in the sketch so nothing drawn is lost
    leave();
  });
}

void DesignController::cancelSketch() {
  if (!m_sketch->active()) return;
  if (m_sketch->modified() && QMessageBox::question(m_window, tr("Sketch"), tr("Discard the changes made to this sketch?")) != QMessageBox::Yes) return;
  m_sketch->end();
  m_doc->setRollback({});
  emit status(QString());
  emit stateChanged();
}

// ---------------------------------------------------------------- bench
// Sketch1 on XY in a new document if there is none, then `run` once the look-at animation has ended (pick distances are
// in pixels). OPAD_BENCH_DESIGN and the registered sketch benches (SketchBench.cpp) start here.
void DesignController::benchSketch(std::function<void()> run) {
  if (!m_doc->hasDocument) m_doc->newDocument();
  const opad::Frame frame = base_frame("xy");
  enterSketch({}, "Sketch1", opad::json{{"base", "xy"}, {"frame", frame.to_json()}}, frame, opad::json::object());
  QTimer::singleShot(700, this, std::move(run));
}

// OPAD_BENCH_DESIGN: a sketch drawn through the editor's tools, extruded through the feature panel's plan and
// commit path, then a parameter-driven edit. No mouse or keyboard driving.
void DesignController::bench() {
  benchSketch([this] {
  m_sketch->bench({});
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_HANDLES"))return;
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_REFERENCE") || qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_DRAG") || qEnvironmentVariableIsSet("OPAD_BENCH_SPLINE") || qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_WORKFLOW") || qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_PRIMITIVES") || qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_MODIFY")) return;
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_SKETCHSHOT"); !shot.isEmpty()) m_viewport->grabImage().save(shot);  // the editor's overlay: curves, dimensions, glyphs
  finishSketch([this] {
    trace::log(QStringLiteral("bench: design: sketch committed, %1 sketches in the scene").arg(m_doc->scene.sketches.size()));
    if (m_doc->scene.sketches.empty()) return;
    const std::string sk = m_doc->scene.sketches.back().id;
    startFeature("extrude");
    m_form->setPicks("profiles", opad::json::array({opad::json{{"sketch", sk}, {"at", {3.0, 3.0}}}}));
    if(qEnvironmentVariableIsSet("OPAD_BENCH_EXTRUDE_HANDLE")) {
      m_form->setValue("start","offset");m_form->setValue("start_offset","3 mm");runPreview(false);
      auto phase=std::make_shared<int>(0),ticks=std::make_shared<int>(0);auto* timer=new QTimer(this);timer->setInterval(100);
      connect(timer,&QTimer::timeout,this,[this,phase,ticks,timer]{try{
        if(++*ticks>200)throw opad::Error("extrude handle timed out");
        if(m_planJob || m_previewTimer.isActive() || m_doc->designBusy)return;
        if(*phase==0) {
          if(!m_readyPlan || !m_distanceHandle->isVisible())throw opad::Error("extrude preview has no handle");
          const auto before=m_form->inputs().at("distance");const QPointF local(20,20),global=m_distanceHandle->mapToGlobal(local.toPoint());
          QMouseEvent press(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(m_distanceHandle,&press);
          QMouseEvent move(QEvent::MouseMove,local+QPointF(25,-35),global+QPointF(25,-35),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(m_distanceHandle,&move);
          // TODO 10 A10: the body follows the drag, before the button is released.
          if(!m_planJob && !m_previewPending)throw opad::Error("a handle drag did not start a live preview");
          if(const QString shot=qEnvironmentVariable("OPAD_BENCH_HANDLESHOT");!shot.isEmpty()){
            m_viewport->grabImage().save(shot+".view.png");m_distanceHandle->grab().save(shot+".box.png");
            auto camera=m_viewport->cameraJson();const auto target=camera.at("target").get<opad::Vec3>();  // an oblique look at the flat arrow
            camera["eye"]={target[0]+60,target[1]-80,target[2]+50};camera["up"]={0,0,1};m_viewport->setCameraJson(camera);m_distanceHandle->reposition();
            m_viewport->grabImage().save(shot+".oblique.png");
          }
          QMouseEvent release(QEvent::MouseButtonRelease,local+QPointF(25,-35),global+QPointF(25,-35),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(m_distanceHandle,&release);
          if(m_form->inputs().at("distance")==before)throw opad::Error("extrude drag did not change distance");++*phase;
        }else if(*phase==1){
          if(!m_readyPlan)throw opad::Error("extrude drag preview missing");
          // UI-16: the box by the arrow takes the tools' keys: digits typed over the view (keypad too) replace the value, Up
          // steps it, Enter is OK.
          QApplication::setActiveWindow(m_viewport->window());m_viewport->setFocus();
          auto key=[this](int code,Qt::KeyboardModifiers mods,const QString& text){
            QWidget* to=QApplication::focusWidget();QKeyEvent press(QEvent::KeyPress,code,mods,text);QApplication::sendEvent(to?to:static_cast<QWidget*>(m_viewport),&press);};
          key(Qt::Key_2,Qt::KeypadModifier,"2");key(Qt::Key_5,Qt::NoModifier,"5");
          auto* box=m_distanceHandle->findChild<QLineEdit*>();
          auto distance=[this]{return m_form->inputs().at("distance").dump();};
          if(!box || box->text()!="25" || distance().find("25")==std::string::npos)throw opad::Error("digits typed over the view did not replace the extrude distance: "+distance());
          // UI-26: a plain number typed is stored with its unit as a word (it was "(25) * 1 mm", shown so when edited again).
          if(m_form->inputs().at("distance")!="25 mm")throw opad::Error("a typed 25 is stored as "+distance()+", not \"25 mm\"");
          trace::log("bench: extrude box: a typed 25 is stored as \"25 mm\" PASS");
          if(QApplication::focusWidget()!=box)throw opad::Error("the extrude box did not take the keyboard");
          key(Qt::Key_Up,Qt::NoModifier,{});
          if(box->text()!="26 mm" || distance().find("26")==std::string::npos)throw opad::Error("Up did not step the extrude distance: "+distance());
          trace::log("bench: extrude box: keypad 2 and 5 typed over the view replace the distance, Up steps it to 26 mm PASS");
          ++*phase;
        }else if(*phase==2){
          if(QApplication::focusWidget()!=m_distanceHandle->findChild<QLineEdit*>())throw opad::Error("the extrude box lost the keyboard");
          QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(QApplication::focusWidget(),&enter);
          ++*phase;
        }
        else {if(m_featureOn)return;if(m_doc->scene.features.empty() || m_distanceHandle->isVisible())throw opad::Error("extrude handle commit/cleanup");timer->stop();trace::log("bench: extrusion start offset, drag, typed value, preview and commit by Enter PASS");QCoreApplication::exit(0);}
      }catch(const std::exception& e){timer->stop();trace::log(QString("bench: extrude handle FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});timer->start();return;
    }
    // TODO 10 B14: the panel's New body section names and colours the body in the same step.
    m_form->setBodyName("Bench block");
    m_form->setBodyColour(QColor(30, 110, 200));
    if (const QString shot = qEnvironmentVariable("OPAD_BENCH_FEATURESHOT"); !shot.isEmpty() && m_panel) {
      m_panel->resize(m_panel->width(), 640);
      m_panel->grab().save(shot);
    }
    runPreview(true);
    // Edit the extrude with the timeline rolled back, then fillet four edges of the result.
    QTimer::singleShot(1500, this, [this] {
      if (m_doc->scene.features.empty()) return trace::log(QStringLiteral("bench: design: the extrude did not commit"));
      const std::string extrude = m_doc->scene.features.front().id;
      const auto made = m_doc->scene.features.front().result.value("bodies", opad::json::array());
      const opad::Node* block = made.empty() ? nullptr : m_doc->node(made[0].value("id", ""));
      const bool styled = block && block->name == "Bench block" && block->has_color && std::abs(block->color[2] - 200 / 255.0) < 1e-3;
      trace::log(QStringLiteral("bench: design: new body named and coloured by the panel %1").arg(styled ? "PASS" : "FAIL"));
      editOp(extrude);
      trace::log(QStringLiteral("bench: design: editing, rolled back to %1 bodies").arg(m_doc->scene.all_bodies().size()));
      m_form->setValue("distance", "6 mm * 2");
      runPreview(true);
    });
    QTimer::singleShot(3000, this, [this] {
      if (m_doc->scene.all_bodies().empty()) return;
      const std::string body = m_doc->scene.all_bodies().front();
      startFeature("fillet");
      opad::json edges = opad::json::array();
      for (int i : {0, 2, 4, 6}) edges.push_back(opad::json{{"body", body}, {"kind", "edge"}, {"index", i}});
      m_form->setPicks("edges", edges);
      m_form->setValue("radius", "1.5 mm");
      runPreview(true);
    });
    // TODO 10 B7: one picked face becomes a rule through "By rule…"; the press pull keeps the rule and what it matched.
    if (qEnvironmentVariableIsSet("OPAD_BENCH_RULE")) QTimer::singleShot(7000, this, [this] {
      if (m_featureOn || m_sketch->active() || m_doc->scene.all_bodies().empty()) return trace::log(QStringLiteral("bench: design: press pull by rule FAIL (busy)"));
      const std::string body = m_doc->scene.all_bodies().front();
      int bottom = -1;  // the bench's small body: the face facing down, away from the fillets on top
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(opad::node_world_shape(m_doc->doc, m_doc->scene, body), TopAbs_FACE, faces);
      for (int i = 1; i <= faces.Extent() && bottom < 0; ++i) {
        const opad::json d = opad::describe_entity(faces(i));
        if (d.contains("normal") && d["normal"][2].get<double>() < -0.99) bottom = i - 1;
      }
      const size_t before = m_doc->scene.features.size();
      startFeature("offset_face");
      m_form->setPicks("faces", opad::json::array({{{"body", body}, {"kind", "face"}, {"index", bottom}}}));
      m_form->setValue("distance", "0.5 mm");
      offerRules("faces", m_form);
      QTimer::singleShot(1500, this, [this, before] {
        const opad::json picks = m_form->picks("faces");
        const bool rule = picks.is_array() && !picks.empty() && picks[0].contains("select");
        runPreview(true);
        QTimer::singleShot(2500, this, [this, before, rule] {
          const auto& features = m_doc->scene.features;
          const bool ok = rule && features.size() == before + 1 && features.back().kind == "offset_face" && features.back().error.empty() &&
                          features.back().result.value("selected", opad::json::array()).size() == 1;
          trace::log(QStringLiteral("bench: design: press pull by rule %1 (rule %2, %3 features, open: %4)").arg(ok ? "PASS" : "FAIL").arg(rule).arg(features.size()).arg(m_featureOn ? m_form->statusText() : QString("no")));
        });
      });
    });
    // TODO 10 A4: an edited sketch is marked on the timeline (OPAD_BENCH_UISHOT: <shot>.sketch-edit.png).
    QTimer::singleShot(5000, this, [this] {
      const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT");
      if (shot.isEmpty() || m_doc->scene.sketches.empty() || m_featureOn || m_sketch->active()) return;
      editOp(m_doc->scene.sketches.front().id);
      QTimer::singleShot(1200, this, [this, shot] {
        m_viewport->window()->grab().save(shot + ".sketch-edit.png");
        trace::log(QStringLiteral("bench: design: the edited sketch is %1 on the timeline").arg(editingOp() == m_doc->scene.sketches.front().id ? "marked" : "NOT marked"));
        cancelSketch();
      });
    });
  });
  });
}
