// Smart selection and the timeline (SmartSelect.hpp, TODO 11 UI-99): a hovered marker shows what its op made, a click on
// a feature that changed bodies selects the faces it made.
#include "SmartSelect.hpp"

#include <QMenu>

#include <algorithm>

#include "AppDocument.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
#include "TimelineWidget.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"

namespace {
std::vector<opad::Ref> bodyRefs(const std::vector<std::string>& ids) {
  std::vector<opad::Ref> out;
  for (const auto& id : ids) {
    opad::Ref r;
    r.body = id;
    out.push_back(r);
  }
  return out;
}
}  // namespace

const std::map<std::string, smart::Made>* SmartSelect::made() const {
  const AppDocument* doc = services().document();
  return m_made && m_madeRevision == doc->revision && m_madeGeneration == doc->generation ? m_made.get() : nullptr;
}

// Once per document state, on a worker; whatever asks meanwhile waits for the same answer.
void SmartSelect::withMade(std::function<void(const std::map<std::string, smart::Made>&)> then) {
  if (const auto* m = made()) return then(*m);
  if (m_afterMade.size() >= 8) m_afterMade.erase(m_afterMade.begin());
  m_afterMade.push_back(std::move(then));
  if (m_madeRunning) return;
  m_madeRunning = true;  // from the snapshot on (busy() counts it)
  const unsigned token = ++m_madeToken;
  withSnapshot([this, token](std::shared_ptr<const opad::Document> document) {
    if (token != m_madeToken) return;
    const auto revision = m_snap.revision, generation = m_snap.generation;  // the copy's state
    auto result = std::make_shared<std::map<std::string, smart::Made>>();
    services().jobs()->async(tr("Finding what the timeline's steps made"), [document, result](Progress p) {
      *result = smart::madeBy(*document, [p] { return p.cancelled(); });
    }, [this, token, result, revision, generation](bool ok, const QString& error) {
      if (token != m_madeToken) return;  // another document meanwhile: asked again for it
      m_madeRunning = false;
      const AppDocument* d = services().document();
      if (!ok) {
        if (error != "cancelled") trace::log("smart select: timeline: " + error);
        m_afterMade.clear();
        return;
      }
      if (d->revision != revision || d->generation != generation) {  // changed meanwhile: what it is now
        for (auto& fn : std::exchange(m_afterMade, {})) withMade(std::move(fn));
        return;
      }
      m_made = result;
      m_madeRevision = revision;
      m_madeGeneration = generation;
      trace::log(QString("smart select: timeline: what %1 op(s) made found").arg(result->size()));
      for (auto& fn : std::exchange(m_afterMade, {})) fn(*result);
    });
  });
}

void SmartSelect::markerHovered(const std::string& op) {
  m_marker = op;
  m_markerWait.stop();
  if (!op.empty()) return m_markerWait.start();
  if (std::exchange(m_markerShown, false)) services().viewport()->showCandidateRefs({});
}

std::vector<opad::Ref> SmartSelect::markerBodies(const std::string& op) const {
  const AppDocument* doc = services().document();
  const opad::Scene& scene = doc->scene;
  std::vector<std::string> ids;
  auto add = [&](const std::string& id) {
    for (const auto& b : scene.bodies_under(id))
      if (std::find(ids.begin(), ids.end(), b) == ids.end()) ids.push_back(b);
  };
  if (const opad::Feature* f = scene.feature(op)) {
    if (!f->suppressed)
      for (const auto& b : f->result.value("bodies", opad::json::array()))
        if (b.is_object() && scene.node(b.value("id", ""))) add(b.value("id", ""));
  } else if (const opad::Op* o = doc->doc.find_op(op); o && o->type == "import") {
    for (const auto& id : scene.all_bodies())
      if (scene.node(id)->source_op == op) ids.push_back(id);
  } else if (o && o->data.contains("target") && o->data["target"].is_string() && o->type != "delete" && scene.node(o->data["target"].get<std::string>())) {
    add(o->data["target"].get<std::string>());  // a move, a rename, a colour: what it is about
  }
  return bodyRefs(ids);
}

void SmartSelect::showMarker() {
  const std::string op = m_marker;
  const AppDocument* doc = services().document();
  const DesignController* design = services().design();
  if (op.empty() || !doc->hasDocument || doc->loading || doc->annotationEditing || design->sketchActive() || design->ownsSelection()) return;
  auto show = [this, op](std::vector<opad::Ref> refs) {
    if (op != m_marker) return;
    if (smart::sameRefs(refs, m_current)) refs.clear();  // selected: shown already
    services().viewport()->showCandidateRefs(refs);
    m_markerShown = !refs.empty();
    trace::log(QString("smart select: timeline marker %1 shows %2 ref(s)").arg(QString::fromStdString(op.substr(0, 8))).arg(refs.size()));
  };
  const auto& deleted = doc->scene.deleted_ops;
  if (std::find(deleted.begin(), deleted.end(), op) != deleted.end()) return show({});
  // A feature's own faces in the last state; rolled back, or anything else, the bodies as shown.
  if (!doc->rollback().empty() || !doc->scene.feature(op)) return show(markerBodies(op));
  withMade([this, op, show](const std::map<std::string, smart::Made>& made) {
    const auto it = made.find(op);
    if (it == made.end() || it->second.faces.empty()) return show(markerBodies(op));
    show(it->second.faces);
  });
}

// A feature that changed bodies (a boss, a hole, a fillet): its faces are what it is; one that made bodies, an import, a
// move: the window selects what it touches.
bool SmartSelect::markerClicked(const std::string& op) {
  const AppDocument* doc = services().document();
  const opad::Feature* f = doc->scene.feature(op);
  if (!f || f->suppressed) return false;
  bool any = false, own = false;
  for (const auto& b : f->result.value("bodies", opad::json::array()))
    if (const opad::Node* n = b.is_object() ? doc->scene.node(b.value("id", "")) : nullptr) {
      any = true;
      own = own || n->source_op == op;
    }
  if (!any || own) return false;
  smart::Candidate c;
  c.kind = "feature";
  c.op = op;
  c.name = f->name;
  c.featureKind = f->kind;
  if (const auto* spec = opad::design::feature_spec(f->kind)) c.icon = spec->icon;
  c.containsSelection = true;
  const unsigned token = ++m_markerToken;
  const auto bodies = markerBodies(op);
  if (!made()) services().select(bodies);  // at once, the faces once they are known
  const auto picks = services().selection().refs;
  withMade([this, c, token, bodies, picks](const std::map<std::string, smart::Made>& made) mutable {
    if (token != m_markerToken || !idle() || services().timeline()->currentOp() != c.op || !smart::sameRefs(services().selection().refs, picks)) return;
    const auto it = made.find(c.op);
    if (it == made.end() || it->second.faces.empty()) return services().select(bodies);
    c.refs = it->second.faces;
    c.count = c.refs.size();
    m_chosen = c;  // its actions on the chip at once; a double-click on one of them edits it
    m_stack.clear();
    trace::log(QString("smart select: timeline click selects %1 (%2 faces)").arg(QString::fromStdString(c.name)).arg(c.refs.size()));
    select(c.refs);
  });
  return true;
}
