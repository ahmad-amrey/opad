// Smart selection's candidate (UI-95): the faces and edges a click on its chip would select, in the theme's candidate
// amber, drawn like the selection's own highlight (one SubHighlight in the Topmost layer, copied from the meshes the
// bodies are drawn with, never pickable); whole bodies through the look compositor's candidate layer.
#include "Viewport.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <map>
#include <utility>

#include "Jobs.hpp"

namespace {
// An edge as segment end pairs: the polyline it is drawn with, else a sampling of its curve.
void edgeLine(const TopoDS_Edge& e, const gp_Trsf& body, std::vector<gp_Pnt>& out) {
  TopLoc_Location loc;
  std::vector<gp_Pnt> line;
  Handle(Poly_PolygonOnTriangulation) polygon;
  Handle(Poly_Triangulation) mesh;
  BRep_Tool::PolygonOnTriangulation(e, polygon, mesh, loc);
  if (!polygon.IsNull() && !mesh.IsNull()) {
    for (int n = 1; n <= polygon->NbNodes(); ++n) line.push_back(mesh->Node(polygon->Node(n)));
  } else if (Handle(Poly_Polygon3D) p3 = BRep_Tool::Polygon3D(e, loc); !p3.IsNull()) {
    for (int n = 1; n <= p3->NbNodes(); ++n) line.push_back(p3->Nodes().Value(n));
  } else if (!BRep_Tool::Degenerated(e)) {
    loc = TopLoc_Location();
    BRepAdaptor_Curve c(e);
    for (int n = 0; n <= 24; ++n) line.push_back(c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * n / 24));
  }
  const gp_Trsf w = body * loc.Transformation();
  for (size_t n = 1; n < line.size(); ++n) {
    out.push_back(line[n - 1].Transformed(w));
    out.push_back(line[n].Transformed(w));
  }
}
}  // namespace

void Viewport::showCandidateRefs(const std::vector<opad::Ref>& refs) {
  const bool same = refs.size() == m_candidateRefs.size() && std::equal(refs.begin(), refs.end(), m_candidateRefs.begin(), [](const opad::Ref& a, const opad::Ref& b) {
    return a.body == b.body && a.kind == b.kind && a.index == b.index;
  });
  if (same) return;
  m_candidateRefs = refs;
  if (Job* job = std::exchange(m_candidateJob, nullptr)) job->cancel();
  m_candidateShown = 0;
  if (m_initialised && !m_candidateHl.IsNull()) {
    m_ctx->Remove(m_candidateHl, Standard_False);
    m_candidateHl.Nullify();
    redrawScene();
  }
  // Whole bodies: tinted through the compositor (it applies the look in its own sliced job).
  std::map<std::string, LookDelta> whole;
  const QColor amber = m_tokens.candidate;
  struct Wanted {
    Handle(AIS_Shape) ais;
    std::vector<std::pair<TopAbs_ShapeEnum, int>> subs;
  };
  auto wanted = std::make_shared<std::vector<Wanted>>();
  std::map<std::string, size_t> at;
  size_t total = 0;
  for (const auto& r : refs) {
    if (r.kind == opad::Ref::Kind::Body) {
      LookDelta d;
      d.color = std::array<double, 3>{amber.redF(), amber.greenF(), amber.blueF()};
      whole[r.body] = d;
      continue;
    }
    if (r.kind != opad::Ref::Kind::Face && r.kind != opad::Ref::Kind::Edge) continue;
    const auto item = m_items.find(r.body);
    if (item == m_items.end()) continue;
    auto [it, added] = at.try_emplace(r.body, wanted->size());
    if (added) wanted->push_back({item->second.ais, {}});
    (*wanted)[it->second].subs.push_back({r.kind == opad::Ref::Kind::Face ? TopAbs_FACE : TopAbs_EDGE, r.index});
    ++total;
  }
  if (!whole.empty() || !m_lookLayers[static_cast<size_t>(LookSource::Candidate)].empty()) setLookLayer(LookSource::Candidate, std::move(whole));
  if (!m_initialised || wanted->empty()) return;
  struct State {
    size_t b = 0, shown = 0;
    std::vector<gp_Pnt> tv, sv;
    std::vector<int> ti;
  };
  auto st = std::make_shared<State>();
  // One body per step: its faces and edges mapped once, the wanted ones copied out of their meshes.
  auto step = [wanted, st]() -> bool {
    if (st->b >= wanted->size()) return false;
    const Wanted& w = (*wanted)[st->b++];
    const gp_Trsf body = w.ais->LocalTransformation();  // rigid placements live on the object (displayBody)
    TopTools_IndexedMapOfShape faces, edges;
    for (const auto& [type, index] : w.subs) {
      TopTools_IndexedMapOfShape& map = type == TopAbs_FACE ? faces : edges;
      if (map.IsEmpty()) TopExp::MapShapes(w.ais->Shape(), type, map);
      if (index < 0 || index >= map.Extent()) continue;
      const TopoDS_Shape& sub = map(index + 1);
      ++st->shown;
      if (type == TopAbs_EDGE) {
        edgeLine(TopoDS::Edge(sub), body, st->sv);
        continue;
      }
      TopLoc_Location loc;
      if (const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(sub), loc); !t.IsNull()) {
        const gp_Trsf f = body * loc.Transformation();
        const int base = static_cast<int>(st->tv.size());
        for (int n = 1; n <= t->NbNodes(); ++n) st->tv.push_back(t->Node(n).Transformed(f));
        for (int k = 1; k <= t->NbTriangles(); ++k) {
          int a, b, c;
          t->Triangle(k).Get(a, b, c);
          st->ti.insert(st->ti.end(), {base + a, base + b, base + c});
        }
      }
      for (TopExp_Explorer e(sub, TopAbs_EDGE); e.More(); e.Next()) edgeLine(TopoDS::Edge(e.Current()), body, st->sv);
    }
    return st->b < wanted->size();
  };
  auto done = [this, st](bool completed) {
    m_candidateJob = nullptr;
    if (!completed || (st->tv.empty() && st->sv.empty())) return;
    // The candidate amber alone (UI-95): no halo, the core and its "halo" one line, a stronger tint than the selection's.
    GlowStyle style;
    style.fill = style.edge = style.halo = Quantity_Color(m_tokens.candidate.redF(), m_tokens.candidate.greenF(), m_tokens.candidate.blueF(), Quantity_TOC_sRGB);
    style.fillAlpha = 0.42f;
    const float scale = float(viewScale().x());
    style.edgeWidth = style.haloWidth = 3.0f * scale;
    style.point = style.pointHalo = 2.5f * scale;
    Handle(SubHighlight) hl = new SubHighlight(style);
    if (!st->tv.empty()) {
      Handle(Graphic3d_ArrayOfTriangles) a = new Graphic3d_ArrayOfTriangles(static_cast<int>(st->tv.size()), static_cast<int>(st->ti.size()));
      for (const gp_Pnt& p : st->tv) a->AddVertex(p);
      for (const int k : st->ti) a->AddEdge(k);
      hl->m_triangles.push_back(a);
    }
    if (!st->sv.empty()) {
      Handle(Graphic3d_ArrayOfSegments) a = new Graphic3d_ArrayOfSegments(static_cast<int>(st->sv.size()));
      for (const gp_Pnt& p : st->sv) a->AddVertex(p);
      hl->m_segments.push_back(a);
    }
    m_candidateHl = hl;
    m_candidateShown = st->shown;
    hl->SetZLayer(Graphic3d_ZLayerId_Topmost);
    hl->SetInfiniteState(Standard_True);  // never part of Fit All
    m_ctx->Display(hl, 0, -1, Standard_False);  // selection mode -1: never pickable
    redrawScene();
  };
  if (total <= 64) {  // a feature's faces: shown while the pointer is still on the chip
    while (step()) {}
    return done(true);
  }
  m_candidateJob = m_jobs->sliced(tr("Highlighting %1").arg(total), [step](Job&) { return step(); }, done);
}
