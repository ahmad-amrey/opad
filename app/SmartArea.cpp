// Smart selection (TODO 11 UI-97): Select similar and the places of Remove faces (SmartArea.hpp).
#include "SmartArea.hpp"

#include <QAction>
#include <QMenu>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <tuple>

#include "AppDocument.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "Ribbon.hpp"
#include "ShortcutEditor.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"

OPAD_ICON_TABLE(smart,
  {"removeFaces", R"(<path d="M3 12l9-4 9 4-9 4z"/><path d="M3 12v4l9 4 9-4v-4" opacity=".45"/><ellipse cx="12" cy="12" rx="3.2" ry="1.5" stroke-dasharray="1.6 1.4"/><path d="M15.5 2.5l4 4M19.5 2.5l-4 4"/>)"},
  {"similar", R"(<circle cx="6.5" cy="6.5" r="3" fill="currentColor" fill-opacity=".5"/><circle cx="17.5" cy="6.5" r="3"/><circle cx="6.5" cy="17.5" r="3"/><circle cx="17.5" cy="17.5" r="3"/>)"});

namespace {
QString amount(double v) { return QString::number(std::round(v * 1000) / 1000, 'g', 10); }
// What a rule selected, in the user's language (the core's labels are English data).
QString similarText(const opad::Recognized& r) {
  const auto& p = r.params;
  auto value = [&](const char* key) { return p.contains(key) && p[key].is_number() ? amount(p[key].get<double>()) : QString(); };
  if (r.rule == "hole") return p.value("through", false) ? SmartArea::tr("Holes Ø%1 through").arg(value("diameter")) : SmartArea::tr("Holes Ø%1").arg(value("diameter"));
  if (r.rule == "fillet") return SmartArea::tr("Fillets R%1").arg(value("radius"));
  if (r.rule == "chamfer") return SmartArea::tr("Chamfers %1 mm").arg(value("distance"));
  if (r.rule == "wall") return SmartArea::tr("Walls %1 mm").arg(value("thickness"));
  if (r.rule == "radius") return r.faces.empty() ? SmartArea::tr("Circular edges R%1").arg(value("radius")) : SmartArea::tr("Round faces R%1").arg(value("radius"));
  if (r.rule == "angle") return SmartArea::tr("Conical faces %1°").arg(value("angle"));
  if (r.rule == "normal") return SmartArea::tr("Faces facing the same way");
  if (r.rule == "area") return SmartArea::tr("Faces of the same area");
  if (r.rule == "direction") return SmartArea::tr("Parallel edges");
  // A body's rules (Recognizer::body_rules), in world coordinates.
  if (r.rule == "top") return SmartArea::tr("Top perimeter");
  if (r.rule == "bottom") return SmartArea::tr("Bottom perimeter");
  if (r.rule == "x") return SmartArea::tr("Edges parallel to X");
  if (r.rule == "y") return SmartArea::tr("Edges parallel to Y");
  if (r.rule == "z") return SmartArea::tr("Edges parallel to Z");
  if (r.rule == "circle") return SmartArea::tr("Circular edges");
  if (r.rule == "up") return SmartArea::tr("Upward planar faces");
  if (r.rule == "holes") return SmartArea::tr("All holes (%1)").arg(p.value("count", 0));
  if (r.rule == "fillets") return SmartArea::tr("All fillets");
  if (r.rule == "chamfers") return SmartArea::tr("All chamfers");
  return SmartArea::tr("Edges of the same length");
}
bool sameRefs(std::vector<opad::Ref> a, std::vector<opad::Ref> b) {
  auto key = [](std::vector<opad::Ref>& v) { std::sort(v.begin(), v.end(), [](const opad::Ref& x, const opad::Ref& y) { return std::tie(x.body, x.kind, x.index) < std::tie(y.body, y.kind, y.index); }); };
  key(a);
  key(b);
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const opad::Ref& x, const opad::Ref& y) { return x.body == y.body && x.kind == y.kind && x.index == y.index; });
}
// Puts actions into a menu right after `after` (at the end when it is not there).
void insertAfter(QMenu& menu, QAction* after, QList<QAction*> actions) {
  actions.removeAll(nullptr);
  const QList<QAction*> all = menu.actions();
  const int at = after ? int(all.indexOf(after)) : -1;
  menu.insertActions(at >= 0 && at + 1 < all.size() ? all[at + 1] : nullptr, actions);
}
}  // namespace

void SmartArea::buildActions() {
  CommandInfo info;
  info.id = "select.similar";
  info.label = tr("Select similar");
  info.icon = "similar";
  info.keywords = {tr("select by geometry"), tr("holes"), tr("fillets"), tr("same size")};
  m_similarAction = services().addCommand(info, [this] { selectSimilar(); });
  m_similarAction->setProperty("shortcutHint", tr("The faces or edges like the picked one: holes of its size, fillets of its radius, faces facing its way. On a body: its top perimeter, edges along an axis, upward faces, all holes. Again: the next rule."));
  shortcuts::updateTooltip(m_similarAction);
}

void SmartArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  if (QMenu* edit = menus.value("edit")) insertAfter(*edit, services().action("edit.selecttouched"), {m_similarAction});
}

void SmartArea::ribbon(RibbonLayout& layout) {
  layout.addAction("review.inspect.results", m_similarAction);
  // Remove faces under Offset face's arrow (both change faces in place): the Modify row stays large at 1600 px.
  if (RibbonLayout::Group* modify = layout.group("design.modify.modify"))
    for (RibbonLayout::Item& item : modify->items)
      if (item.action && item.action == services().action("design.offset_face")) item.variants << services().action("design.remove_faces");
}

// Picked faces or edges: the ones like them, and taking the faces away; one body: its edges and faces by rule. After
// Properties.
void SmartArea::contextMenu(const SelectionContext& selection, QMenu& menu) {
  if (selection.sketching || selection.ids.empty()) return;
  QList<QAction*> entries;
  const auto picks = services().viewport()->selection();
  if (!picks.empty() && (picks.front().kind == opad::Ref::Kind::Face || picks.front().kind == opad::Ref::Kind::Edge)) {
    entries << m_similarAction;
    if (picks.front().kind == opad::Ref::Kind::Face) entries << services().action("design.remove_faces");
  } else if (const opad::Node* n = selection.ids.size() == 1 ? services().document()->node(selection.ids.front()) : nullptr; n && n->kind == opad::Node::Kind::Body)
    entries << m_similarAction;
  insertAfter(menu, services().action("inspect.properties"), entries);
}

void SmartArea::selectSimilar() {
  AppDocument* doc = services().document();
  DesignController* design = services().design();
  Viewport* viewport = services().viewport();
  // A sketch, a feature input or a guided tool owns the picks (the view accumulates them for it).
  if (doc->loading || doc->designBusy || design->sketchActive() || design->featureActive() || design->pickingPlane() || viewport->pickAccumulate())
    throw opad::Error("Finish the current operation before selecting geometry.");
  const auto picks = viewport->selection();
  // Again on what the last rule selected: the next rule of the same pick.
  if (!m_similar.rules.empty() && m_similar.revision == doc->revision && m_similar.generation == doc->generation && sameRefs(picks, m_similar.selected))
    return apply((m_similar.current + 1) % m_similar.rules.size());
  // A face or an edge: the ones like it. A body picked whole (in the view or the browser): its edges and faces by rule,
  // top perimeter first (what the modal Select by geometry offered, without the dialog and the cap of 100).
  opad::Ref seed;
  if (!picks.empty() && (picks.front().kind == opad::Ref::Kind::Face || picks.front().kind == opad::Ref::Kind::Edge)) seed = picks.front();
  else if (picks.empty() || picks.front().kind == opad::Ref::Kind::Body) {
    std::vector<std::string> ids;
    for (const auto& r : picks)
      if (std::find(ids.begin(), ids.end(), r.body) == ids.end()) ids.push_back(r.body);
    if (picks.empty()) ids = services().browser()->selectedIds();
    if (const opad::Node* n = ids.size() == 1 ? doc->node(ids[0]) : nullptr; n && n->kind == opad::Node::Kind::Body) seed.body = ids[0];
  }
  const opad::Node* node = seed.body.empty() ? nullptr : doc->node(seed.body);
  if (!node || node->body_missing) throw opad::Error("Select a body, a face or an edge to find the ones like it.");
  if (seed.kind != opad::Ref::Kind::Body && node->representation != "solid") throw opad::Error("Select similar works on the faces and edges of solids.");
  const auto revision = doc->revision, generation = doc->generation;
  JobRunner* jobs = services().jobs();
  if (!doc->captureSnapshot(jobs, [this, jobs, seed, revision, generation](std::shared_ptr<opad::Document> document, const QString& error) {
        if (!document) return services().showMessage(error, 6000);
        auto rules = std::make_shared<std::vector<opad::Recognized>>();
        jobs->async(tr("Select similar"), [document, rules, seed](Progress progress) {
          const auto scene = opad::resolve(*document);
          opad::Recognizer recognizer(opad::node_world_shape(*document, scene, seed.body), [progress] { return progress.cancelled(); });
          const bool whole = seed.kind == opad::Ref::Kind::Body;
          std::set<std::vector<int>> seen;
          for (auto& r : whole ? recognizer.body_rules() : seed.kind == opad::Ref::Kind::Face ? recognizer.similar_faces(seed.index) : recognizer.similar_edges(seed.index)) {
            const auto& members = r.faces.empty() ? r.edges : r.faces;
            if (members.size() > (whole ? 0u : 1u) && seen.insert(members).second) rules->push_back(std::move(r));
          }
        }, [this, rules, seed, revision, generation](bool ok, const QString& error) {
          if (!ok) return services().showMessage(error, 6000);
          AppDocument* doc = services().document();
          if (doc->revision != revision || doc->generation != generation || services().design()->ownsSelection()) return;
          if (rules->empty())
            return services().showMessage(seed.kind == opad::Ref::Kind::Body ? tr("No edges or faces of this body follow a rule.") : tr("Nothing else on this body is like it."), 6000);
          m_similar = {*rules, seed.body, 0, {}, revision, generation, m_similar.token};
          apply(0);
        });
      }))
    throw opad::Error("Document is busy; try again shortly.");
}

void SmartArea::apply(size_t rule) {
  const opad::Recognized& r = m_similar.rules.at(rule);
  const bool edges = r.faces.empty();
  std::vector<opad::Ref> refs;
  for (int i : edges ? r.edges : r.faces) {
    opad::Ref ref;
    ref.body = m_similar.body;
    ref.kind = edges ? opad::Ref::Kind::Edge : opad::Ref::Kind::Face;
    ref.index = i;
    refs.push_back(ref);
  }
  m_similar.current = rule;
  m_similar.selected = refs;
  const auto token = ++m_similar.token;  // a rule still waiting for its filter is dropped
  const QString next = m_similar.rules.size() > 1 ? tr(" · again: %1").arg(similarText(m_similar.rules[(rule + 1) % m_similar.rules.size()])) : QString();
  const QString text = tr("%1 · %2 selected").arg(similarText(r)).arg(refs.size()) + next;
  const QString log = QString("select similar: %1 %2 (%3 rules)").arg(QString::fromStdString(r.rule)).arg(refs.size()).arg(m_similar.rules.size());
  Viewport* viewport = services().viewport();
  auto show = [this, viewport, refs, text, log] {
    viewport->selectRefs(refs);
    services().showMessage(text, 8000);
    trace::log(log);
  };
  const auto filter = edges ? Viewport::SelFilter::Edge : Viewport::SelFilter::Face;
  if (viewport->selectionFilter() == filter) return show();
  // A body's rules pick edges or faces: the view picks those from now on (the filter chips follow).
  connect(viewport, &Viewport::filterApplied, this, [this, viewport, show, filter, token, revision = m_similar.revision, generation = m_similar.generation] {
    const AppDocument* doc = services().document();
    if (token == m_similar.token && viewport->selectionFilter() == filter && doc->revision == revision && doc->generation == generation && !services().design()->ownsSelection()) show();
  }, Qt::SingleShotConnection);
  viewport->setSelectionFilter(filter);
}

OPAD_AREA(SmartArea)
