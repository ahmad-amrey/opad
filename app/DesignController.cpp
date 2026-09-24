#include "DesignController.hpp"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <QMessageBox>
#include <atomic>

#include "I18n.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"

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

QString titleCase(const std::string& label) { return QString::fromStdString(label).section(' ', 0, 0); }

gp_Pnt pnt(const opad::Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }

}  // namespace

DesignController::DesignController(AppDocument* doc, Viewport* viewport, JobRunner* jobs, QWidget* window)
    : QObject(window), m_doc(doc), m_viewport(viewport), m_jobs(jobs), m_window(window) {
  m_form = new FeaturePanel(doc, window);
  m_sketch = new SketchEditor(doc, viewport, jobs, this);
  connect(doc, &AppDocument::aboutToReplace, this, [this] {
    endFeature();
    if (m_pickPlane) escape();
    m_sketch->end();
    if (m_params) m_params->hide();
    m_doc->designBusy = false;
    emit stateChanged();
  });
  m_previewTimer.setSingleShot(true);
  m_previewTimer.setInterval(280);
  connect(&m_previewTimer, &QTimer::timeout, this, [this] { runPreview(false); });
  connect(m_form, &FeaturePanel::inputsChanged, this, &DesignController::schedulePreview);
  connect(m_form, &FeaturePanel::activeInputChanged, this, &DesignController::activateInput);
  connect(m_form, &FeaturePanel::accepted, this, [this] { runPreview(true); });
  connect(m_form, &FeaturePanel::cancelled, this, &DesignController::endFeature);
  connect(m_sketch, &SketchEditor::status, this, &DesignController::status);
  connect(m_sketch, &SketchEditor::changed, this, &DesignController::stateChanged);
  connect(m_sketch, &SketchEditor::toolChanged, this, &DesignController::stateChanged);
}

void DesignController::setPanel(ToolPanel* panel, std::function<void(ToolPanel*)> open) {
  m_panel = panel;
  m_openPanel = std::move(open);
  connect(panel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on && m_featureOn) endFeature();  // closing the panel cancels the feature
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
  if (m_doc->designBusy) return report(false, tr("The design is still being recomputed; try again in a moment."));
  m_doc->designBusy = true;
  const auto generation = m_doc->generation;
  auto plan = std::make_shared<Plan>();
  auto doc = std::make_shared<opad::Document>(m_doc->doc);
  m_jobs->async(tr("Updating the design"), [doc, ops, plan](Progress p) {
    Reading reading;
    *plan = plan_ops(*doc, ops, true, [p] { return p.cancelled(); });
  }, [this, plan, label, report, generation](bool ok, const QString& error) {
    whenNobodyReads(this, [this, plan, label, report, ok, error, generation] {
      if (generation != m_doc->generation) return;
      m_doc->designBusy = false;
      if (!ok) return report(false, error);
      try {
        const opad::json rep = m_doc->commitPlan(std::move(*plan), label);
        const size_t errors = rep.value("errors", opad::json::array()).size();
        if (errors > 0) emit status(tr("%1 later feature(s) could not be recomputed; they are marked on the timeline.").arg(errors));
        report(true, {});
      } catch (const std::exception& e) {
        report(false, QString::fromUtf8(e.what()));
      }
    });
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
      emit status(nothing ? tr("The design is up to date.") : tr("Design regenerated."));
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
  m_newId = opad::new_uuid();
  opad::json inputs = opad::json::object();
  for (const auto& in : spec->inputs)
    if (!in.def.is_null()) inputs[in.name] = in.def;
  m_featureOn = true;
  m_filterBefore = m_viewport->selectionFilter();
  m_viewport->setPickAccumulate(true);
  m_form->begin(*spec, inputs, QString::fromStdString(next_name(m_doc->scene, titleCase(spec->label).toStdString())), false);
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
      m_form->setPicks(m_form->activeInput(), picks);
      activateInput(m_form->activeInput());
    }
  }
  schedulePreview();
  emit stateChanged();
}

void DesignController::editOp(const std::string& opId) {
  if (!m_doc->hasDocument || m_doc->browse || m_doc->designBusy) return;
  if (m_sketch->active() || m_featureOn) return emit status(tr("Finish what is open first."));
  if (const opad::SketchItem* s = m_doc->scene.sketch(opId)) {
    const opad::SketchItem sketch = *s;
    m_doc->setRollback(opId);  // the model as it was when the sketch was made: what its plane refers to
    return enterSketch(opId, QString::fromStdString(sketch.name), sketch.plane, sketch.frame, sketch.geometry);
  }
  const opad::Feature* f = m_doc->scene.feature(opId);
  if (!f) return;
  const FeatureSpec* spec = feature_spec(f->kind);
  if (!spec) return;
  const opad::Feature feature = *f;
  m_doc->setRollback(opId);
  m_editing = opId;
  m_featureOn = true;
  m_filterBefore = m_viewport->selectionFilter();
  m_viewport->setPickAccumulate(true);
  m_form->begin(*spec, feature.inputs, QString::fromStdString(feature.name), true);
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
  if (!m_featureOn) return;
  m_featureOn = false;
  ++m_planSerial;
  m_previewTimer.stop();
  if (Job* j = std::exchange(m_planJob, nullptr)) j->cancel();
  if (Job* j = std::exchange(m_candidateJob, nullptr)) j->cancel();
  m_readyPlan.reset();
  m_viewport->clearPreviewBodies();
  m_viewport->clearCandidates();
  m_viewport->setPickAccumulate(false);
  m_viewport->setBodiesPickable(true);
  m_activating = true;  // the clean-up below is not a pick
  m_viewport->clearSelection();
  if (m_viewport->selectionFilter() != m_filterBefore) m_viewport->setSelectionFilter(m_filterBefore);
  m_activating = false;
  m_form->activate(QString());
  m_editing.clear();
  if (m_panel && m_panel->isVisible()) m_panel->hide();
  m_doc->setRollback({});
  emit status(QString());
  emit stateChanged();
}

opad::json DesignController::pickToJson(const opad::Ref& ref) const { return ref.to_json(); }

void DesignController::showCandidatesFor(const QString& typeName) {
  if (Job* j = std::exchange(m_candidateJob, nullptr)) j->cancel();
  const std::string type = typeName.toStdString();
  std::vector<Viewport::Candidate> quick;
  // Size of the origin planes and axes: a little more than what is on screen.
  double reach = 50;
  for (const auto& id : m_doc->scene.all_bodies()) {
    try {
      const Bnd_Box b = opad::node_world_bbox(m_doc->doc, m_doc->scene, id);  // cached per key: O(1)
      if (!b.IsVoid()) reach = std::max(reach, std::sqrt(b.SquareExtent()) * 0.75);
    } catch (const std::exception&) {
    }
  }
  if (type == "plane") {
    for (const char* base : {"xy", "xz", "yz"}) {
      const opad::Frame f = base_frame(base);
      quick.push_back({opad::json{{"base", base}}.dump(), BRepBuilderAPI_MakeFace(frame_plane(f), -reach, reach, -reach, reach).Face(), false});
    }
    for (const auto& f : m_doc->scene.features)
      if (f.result.contains("plane")) {
        const opad::Frame fr = opad::Frame::from_json(f.result["plane"]);
        quick.push_back({opad::json{{"feature", f.id}}.dump(), BRepBuilderAPI_MakeFace(frame_plane(fr), -reach * 0.6, reach * 0.6, -reach * 0.6, reach * 0.6).Face(), true});
      }
  }
  if (type == "axis") {
    for (const auto& [base, dir] : {std::pair{"x", gp_Dir(1, 0, 0)}, std::pair{"y", gp_Dir(0, 1, 0)}, std::pair{"z", gp_Dir(0, 0, 1)}})
      quick.push_back({opad::json{{"base", base}}.dump(), BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0).Translated(gp_Vec(dir) * -reach), gp_Pnt(0, 0, 0).Translated(gp_Vec(dir) * reach)).Edge(), false});
    for (const auto& f : m_doc->scene.features)
      if (f.result.contains("axis")) {
        const auto& a = f.result["axis"];
        const gp_Pnt o(a["origin"][0], a["origin"][1], a["origin"][2]);
        const gp_Vec d(a["dir"][0], a["dir"][1], a["dir"][2]);
        quick.push_back({opad::json{{"feature", f.id}}.dump(), BRepBuilderAPI_MakeEdge(o.Translated(d * -reach), o.Translated(d * reach)).Edge(), true});
      }
  }
  const bool fromSketches = type == "profiles" || type == "points" || type == "axis" || type == "path";
  if (!fromSketches) return m_viewport->showCandidates(quick);

  // Sketch regions, points, lines and whole sketches: kernel work, so on a worker from copies of the sketches.
  struct Source { std::string id; opad::json geometry; opad::Frame frame; };
  auto sources = std::make_shared<std::vector<Source>>();
  for (const auto& s : m_doc->scene.sketches)
    if (s.visible || !s.consumed) sources->push_back({s.id, s.geometry, s.frame});
  auto found = std::make_shared<std::vector<Viewport::Candidate>>(quick);
  m_candidateJob = m_jobs->async(tr("Finding what can be picked"), [sources, found, type](Progress p) {
    for (const auto& src : *sources) {
      if (p.cancelled()) return;
      const Sketch sk = Sketch::from_json(src.geometry);
      if (type == "profiles") {
        for (const auto& r : sketch_regions(sk, src.frame)) found->push_back({opad::json{{"sketch", src.id}, {"at", {r.u, r.v}}}.dump(), r.face, false});
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
  }, [this, found](bool ok, const QString&) {
    m_candidateJob = nullptr;
    if (!ok || !m_featureOn) return;
    m_viewport->showCandidates(*found);
    syncSelectionToInput();
  });
  m_viewport->showCandidates(quick);
}

// Makes the viewport's selection show the active input's picks (after switching inputs, or when candidates
// arrive).
void DesignController::syncSelectionToInput() {
  const QString name = m_form->activeInput();
  if (name.isEmpty()) return;
  opad::json picks = m_form->picks(name);
  if (picks.is_object()) picks = opad::json::array({picks});
  std::vector<opad::Ref> refs;
  std::vector<std::string> candidates;
  if (picks.is_array())
    for (const auto& p : picks) {
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
  m_activating = true;
  m_viewport->selectRefs(refs, candidates);
  m_activating = false;
}

void DesignController::activateInput(const QString& name) {
  if (!m_featureOn) return;
  const InputSpec* in = m_form->input(name);
  if (!in) {
    m_viewport->clearCandidates();
    return;
  }
  const Viewport::SelFilter want = filterFor(in->type);
  showCandidatesFor(QString::fromStdString(in->type));
  if (m_viewport->selectionFilter() != want) {
    m_activating = true;
    auto once = std::make_shared<QMetaObject::Connection>();
    *once = connect(m_viewport, &Viewport::filterApplied, this, [this, once] {
      disconnect(*once);
      m_activating = false;
      syncSelectionToInput();
    });
    m_viewport->setSelectionFilter(want);  // sliced; the old picks are re-applied once every body is in the new mode
  } else {
    syncSelectionToInput();
  }
  emit status(tr("%1: pick in the view").arg(i18n::t(QString::fromStdString(in->label))));
}

void DesignController::viewportSelectionChanged() {
  if (m_activating) return;
  if (m_pickPlane) {
    opad::json plane;
    const auto cands = m_viewport->selectedCandidates();
    const auto refs = m_viewport->selection();
    if (!cands.empty()) plane = opad::json::parse(cands.back());
    else if (!refs.empty() && refs.back().kind == opad::Ref::Kind::Face) plane = opad::json{{"face", refs.back().to_json()}};
    else return;
    try {
      const opad::Frame frame = resolve_plane(m_doc->doc, m_doc->scene, plane);
      if (plane.contains("face")) plane["face"] = make_ref(m_doc->doc, m_doc->scene, refs.back());
      plane["frame"] = frame.to_json();
      auto picked=std::move(m_planePicked);
      escape();  // leaves the plane pick
      if(picked) { picked(plane,frame); return; }
      enterSketch({}, QString::fromStdString(next_name(m_doc->scene, "Sketch")), plane, frame, opad::json::object());
    } catch (const std::exception& e) {
      m_activating = true;
      m_viewport->clearSelection();
      m_activating = false;
      emit status(i18n::t(QString::fromUtf8(e.what())));
    }
    return;
  }
  if (!m_featureOn) return;
  const QString name = m_form->activeInput();
  const InputSpec* in = m_form->input(name);
  if (!in) return;
  // The viewport selection is the pick list: bodies and sub-shapes by reference, everything else by candidate.
  opad::json picks = opad::json::array();
  for (const auto& r : m_viewport->selection()) {
    if (in->type == "plane") picks.push_back(opad::json{{"face", pickToJson(r)}});
    else if (in->type == "axis") picks.push_back(opad::json{{"edge", pickToJson(r)}});
    else picks.push_back(pickToJson(r));
  }
  for (const auto& c : m_viewport->selectedCandidates()) picks.push_back(opad::json::parse(c));
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
  m_form->setPicks(name, picks);
  schedulePreview();
  if (single && !picks.empty()) m_form->activateNextPick();
  else if (in->max_count > 0 && static_cast<int>(picks.size()) == in->max_count) m_form->activateNextPick();
}

void DesignController::schedulePreview() {
  if (!m_featureOn) return;
  m_readyPlan.reset();
  m_previewTimer.start();
}

void DesignController::runPreview(bool commit) {
  if (!m_featureOn) return;
  m_previewTimer.stop();
  QString missing;
  if (!m_form->complete(&missing)) {
    m_form->setStatus(missing, commit);
    m_viewport->clearPreviewBodies();
    return;
  }
  const opad::json inputs = m_form->inputs();
  const std::string name = m_form->name().toStdString();
  const std::string stamp = inputs.dump() + "|" + name;
  auto commitReady = [this] {
    auto plan = m_readyPlan;
    const QString label = m_form->spec() ? i18n::t(QString::fromStdString(m_form->spec()->label)).toLower() : tr("feature");
    whenNobodyReads(this, [this, plan, label] {
      try {
        m_viewport->clearPreviewBodies();
        m_doc->setRollback({});
        const opad::json rep = m_doc->commitPlan(std::move(*plan), label);
        const size_t errors = rep.value("errors", opad::json::array()).size();
        endFeature();
        if (errors > 0) emit status(tr("%1 later feature(s) could not be recomputed; they are marked on the timeline.").arg(errors));
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
  const auto generation = m_doc->generation;
  auto plan = std::make_shared<Plan>();
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);  // the rolled-back state the picks were made in
  auto doc = std::make_shared<opad::Document>(m_doc->doc);
  m_form->setStatus(tr("Computing…"), false);
  m_planJob = m_jobs->async(tr("Computing %1").arg(m_form->name()), [doc, scene, inputs, name, target, kind, editing, plan](Progress p) {
    Reading reading;
    const opad::json hinted = hint_refs(*doc, *scene, inputs);
    opad::json op = editing ? make_edit_op(target, opad::json{{"inputs", hinted}, {"name", name}}) : make_feature_op(kind, name, hinted);
    if (!editing) op["id"] = target;
    *plan = plan_ops(*doc, {op}, true, [p] { return p.cancelled(); });
    for (auto& c : plan->changed)  // the preview is displayed without meshing on the UI thread
      if (c.op == target && c.shape && !c.shape->IsNull()) {
        Bnd_Box box;
        BRepBndLib::Add(*c.shape, box, Standard_False);
        const double defl = box.IsVoid() ? 0.1 : std::clamp(std::sqrt(box.SquareExtent()) * 0.002, 0.02, 2.0);
        BRepMesh_IncrementalMesh(*c.shape, defl, Standard_False, 0.35, Standard_True);
      }
  }, [this, serial, plan, stamp, target, commit, commitReady](bool ok, const QString& error) {
    if (serial != m_planSerial || !m_featureOn) return;  // superseded
    m_planJob = nullptr;
    if (!ok) {
      m_readyPlan.reset();
      m_viewport->clearPreviewBodies();
      if (error != "cancelled") m_form->setStatus(i18n::t(error), true);
      return;
    }
    m_readyPlan = plan;
    m_readyInputs = stamp;
    m_readyOps = m_doc->doc.ops.size();
    m_form->setStatus(QString(), false);
    if (commit) return commitReady();
    std::vector<std::pair<std::string, TopoDS_Shape>> shapes;
    std::vector<std::string> hidden;
    for (const auto& c : plan->changed) {
      if (c.op != target) continue;
      if (c.removed) hidden.push_back(c.node);
      else if (c.shape) shapes.push_back({m_doc->scene.node(c.node) ? c.node : std::string(), *c.shape});
    }
    m_viewport->setPreviewBodies(shapes, hidden);
  });
}

// ---------------------------------------------------------------- sketches
void DesignController::pickSketchPlane(std::function<void(opad::json,opad::Frame)> done) {
  if(!m_doc->hasDocument || m_sketch->active()) return;
  if(m_pickPlane) escape();
  m_planePicked=std::move(done);
  beginPlanePick();
}

void DesignController::startSketch() {
  m_planePicked={};
  if (!m_doc->hasDocument || m_doc->browse) return;
  if (m_sketch->active()) return;
  beginPlanePick();
}

void DesignController::setSketchPanel(ToolPanel* panel) {
  m_sketchPanel=panel;
  connect(m_sketch,&SketchEditor::toolChanged,this,[this]{showSketchPanel();});
  connect(m_sketch,&SketchEditor::workflowChanged,this,&DesignController::stateChanged);
  connect(m_sketch,&SketchEditor::changed,this,[this]{if(!m_sketch->active() && m_sketchPanel)m_sketchPanel->hide();});
}
void DesignController::showSketchPanel() {
  if(m_sketch->active() && m_sketchPanel && m_openPanel)m_openPanel(m_sketchPanel);
}
void DesignController::redefineSketchPlane() {
  if(!m_sketch->active() || m_doc->designBusy)return;
  m_viewport->endSketchInput();m_replaning=true;
  m_planePicked=[this](opad::json plane,opad::Frame frame){m_sketch->redefinePlane(plane,frame);};
  beginPlanePick();
}

void DesignController::beginPlanePick() {
  if (m_featureOn) endFeature();
  m_pickPlane = true;
  m_filterBefore = m_viewport->selectionFilter();
  m_activating = true;
  auto once = std::make_shared<QMetaObject::Connection>();
  auto ready = [this, once] {
    disconnect(*once);
    m_activating = false;
  };
  if (m_viewport->selectionFilter() != Viewport::SelFilter::Face) {
    *once = connect(m_viewport, &Viewport::filterApplied, this, ready);
    m_viewport->setSelectionFilter(Viewport::SelFilter::Face);
  } else {
    m_viewport->clearSelection();
    m_activating = false;
  }
  m_featureOn = false;
  // Candidates need the feature flag only for the async part; planes are immediate.
  std::vector<Viewport::Candidate> none;
  showCandidatesFor("plane");
  emit status(tr("Select an origin plane or a planar face")+" - "+tr("Esc cancels"));
  emit stateChanged();
}

bool DesignController::escape() {
  if (m_pickPlane) {
    m_pickPlane = false;
    m_planePicked={};
    m_viewport->clearCandidates();
    m_activating = true;
    m_viewport->clearSelection();
    if (m_viewport->selectionFilter() != m_filterBefore) m_viewport->setSelectionFilter(m_filterBefore);
    m_activating = false;
    if(m_replaning) {
      m_replaning=false;
      const auto plane=m_sketch->plane();
      m_viewport->beginSketchInput(m_sketch,opad::Frame::from_json(plane.value("frame",opad::json())),m_sketch->sketchId());
    }
    emit status(QString());
    emit stateChanged();
    return true;
  }
  if (m_featureOn) {
    endFeature();
    return true;
  }
  return false;
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
  if (m_sketch->sketchId().empty()) op = make_sketch_op(m_sketch->name().toStdString(), m_sketch->plane(), m_sketch->geometry());
  else op = make_edit_op(m_sketch->sketchId(), opad::json{{"geometry_delta", m_sketch->geometryDelta()}, {"plane", m_sketch->plane()}});
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
// OPAD_BENCH_DESIGN: a sketch drawn through the editor's tools, extruded through the feature panel's plan and
// commit path, then a parameter-driven edit. No mouse or keyboard driving.
void DesignController::bench() {
  if (!m_doc->hasDocument) m_doc->newDocument();
  const opad::Frame frame = base_frame("xy");
  enterSketch({}, "Sketch1", opad::json{{"base", "xy"}, {"frame", frame.to_json()}}, frame, opad::json::object());
  QTimer::singleShot(700, this, [this] {  // the look-at animation has ended: pick distances are in pixels
  m_sketch->bench({});
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SPLINE") || qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_WORKFLOW") || qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_PRIMITIVES")) return;
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_SKETCHSHOT"); !shot.isEmpty()) m_viewport->grabImage().save(shot);  // the editor's overlay: curves, dimensions, glyphs
  finishSketch([this] {
    trace::log(QStringLiteral("bench: design: sketch committed, %1 sketches in the scene").arg(m_doc->scene.sketches.size()));
    if (m_doc->scene.sketches.empty()) return;
    const std::string sk = m_doc->scene.sketches.back().id;
    startFeature("extrude");
    m_form->setPicks("profiles", opad::json::array({opad::json{{"sketch", sk}, {"at", {3.0, 3.0}}}}));
    runPreview(true);
    // Edit the extrude with the timeline rolled back, then fillet four edges of the result.
    QTimer::singleShot(1500, this, [this] {
      if (m_doc->scene.features.empty()) return trace::log(QStringLiteral("bench: design: the extrude did not commit"));
      const std::string extrude = m_doc->scene.features.front().id;
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
  });
  });
}
