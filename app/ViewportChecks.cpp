// The design checks' findings on the model (TODO 11 wave 3, help audit P8). Print check: the faces of each finding tinted,
// overhangs in the theme's warning amber and thin walls in its error red, one SubHighlight per colour copied from the meshes
// the bodies are drawn with (a sliced job, a body a step), never pickable. They show through the model as a selection does,
// and over a body selected whole too: TopOSD, which draws after Topmost with no depth test (in Topmost a tint shared the
// selected body's depth and z-fought with it). Interference: the volume two bodies share, in the error red over everything
// (TopOSD: the pair it lies inside is selected and drawn in Topmost).
#include "Viewport.hpp"

#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <utility>

#include "BodyShape.hpp"
#include "Jobs.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }

// Triangles in world coordinates, as a SubHighlight fills them.
struct Mesh {
  std::vector<gp_Pnt> nodes;
  std::vector<int> indices;  // 1-based, three a triangle
  size_t triangles() const { return indices.size() / 3; }
};

void addFace(const TopoDS_Face& face, const gp_Trsf& body, Mesh& out) {
  TopLoc_Location loc;
  const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(face, loc);
  if (t.IsNull()) return;
  const gp_Trsf f = body * loc.Transformation();
  const int base = static_cast<int>(out.nodes.size());
  for (int n = 1; n <= t->NbNodes(); ++n) out.nodes.push_back(t->Node(n).Transformed(f));
  for (int k = 1; k <= t->NbTriangles(); ++k) {
    int a, b, c;
    t->Triangle(k).Get(a, b, c);
    out.indices.insert(out.indices.end(), {base + a, base + b, base + c});
  }
}
}  // namespace

void Viewport::showCheckTints(const std::vector<CheckTint>& tints) {
  m_tintWanted = tints;
  buildCheckTints();
}

void Viewport::buildCheckTints() {
  m_tintAgain = false;
  if (Job* job = std::exchange(m_tintJob, nullptr)) job->cancel();
  for (Handle(SubHighlight)& hl : m_checkTints) {
    if (m_initialised && !hl.IsNull()) m_ctx->Remove(hl, Standard_False);
    hl.Nullify();
  }
  m_tintTriangles = {0, 0};
  for (Bnd_Box& box : m_tintBoxes) box.SetVoid();
  if (m_initialised) redrawScene();
  if (!m_initialised || m_tintWanted.empty()) return;
  struct Wanted {
    Handle(AIS_Shape) ais;
    bool triangles = false;           // a mesh: the ordinals are its triangles, counted face after face
    std::vector<int> faces[2];        // overhang, thin wall
  };
  auto wanted = std::make_shared<std::vector<Wanted>>();
  std::map<std::string, size_t> at;
  for (const CheckTint& t : m_tintWanted) {
    if (t.faces.empty()) continue;
    // Only on a body drawn as it is now: one hidden (isolation) has none; one still on its way to the view (its mesh, the
    // display pump) gets them once the view has settled (finishSync).
    const opad::Node* n = m_doc->scene.node(t.body);
    if (!n || n->body_missing) continue;
    const auto item = m_items.find(t.body);
    const bool shown = (item == m_items.end() || item->second.look.visible) &&  // a look hides it: none until it shows it again
                       (!m_isolated.empty() ? m_isolated.count(t.body) > 0 : m_doc->scene.effectively_visible(t.body));
    if (item == m_items.end() || item->second.key != n->body_key || !m_ctx->IsDisplayed(item->second.ais)) {
      m_tintAgain = m_tintAgain || shown;
      continue;
    }
    auto [it, added] = at.try_emplace(t.body, wanted->size());
    if (added) wanted->push_back({item->second.ais, t.triangles, {}});
    auto& list = (*wanted)[it->second].faces[t.error ? 1 : 0];
    list.insert(list.end(), t.faces.begin(), t.faces.end());
  }
  if (wanted->empty()) return;
  struct State {
    size_t b = 0;
    Mesh mesh[2];
  };
  auto st = std::make_shared<State>();
  // One body per step: its faces mapped once (a mesh's triangles counted per face), the wanted ones copied out.
  auto step = [wanted, st]() -> bool {
    if (st->b >= wanted->size()) return false;
    const Wanted& w = (*wanted)[st->b++];
    const gp_Trsf body = w.ais->LocalTransformation();  // rigid placements and explode offsets live on the object (displayBody)
    if (w.triangles) {
      // As the check numbers them (opad::check_print, print_mesh): every triangle of every face, in explorer order.
      std::vector<std::pair<TopoDS_Face, int>> faces;  // face, triangles before it
      int count = 0;
      for (TopExp_Explorer f(w.ais->Shape(), TopAbs_FACE); f.More(); f.Next()) {
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc);
        faces.push_back({TopoDS::Face(f.Current()), count});
        count += t.IsNull() ? 0 : t->NbTriangles();
      }
      for (int c = 0; c < 2; ++c)
        for (const int ordinal : w.faces[c]) {
          if (ordinal < 0 || ordinal >= count) continue;
          const auto face = std::prev(std::upper_bound(faces.begin(), faces.end(), ordinal, [](int o, const auto& f) { return o < f.second; }));
          TopLoc_Location loc;
          const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(face->first, loc);
          const gp_Trsf f = body * loc.Transformation();
          int a, b, d;
          t->Triangle(ordinal - face->second + 1).Get(a, b, d);
          if (face->first.Orientation() == TopAbs_REVERSED) std::swap(b, d);
          Mesh& m = st->mesh[c];
          const int base = static_cast<int>(m.nodes.size());
          for (const int n : {a, b, d}) m.nodes.push_back(t->Node(n).Transformed(f));
          m.indices.insert(m.indices.end(), {base + 1, base + 2, base + 3});
        }
      return st->b < wanted->size();
    }
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(w.ais->Shape(), TopAbs_FACE, faces);
    for (int c = 0; c < 2; ++c)
      for (const int index : w.faces[c])
        if (index >= 0 && index < faces.Extent()) addFace(TopoDS::Face(faces(index + 1)), body, st->mesh[c]);
    return st->b < wanted->size();
  };
  auto done = [this, st](bool completed) {
    m_tintJob = nullptr;
    if (!completed) return;
    const QColor colours[2] = {m_tokens.warning, m_tokens.error};
    for (int c = 0; c < 2; ++c) {
      const Mesh& m = st->mesh[c];
      if (m.indices.empty()) continue;
      GlowStyle style;  // a tint alone: the faces' own edges stay as the body draws them
      style.fill = style.edge = style.halo = occ(colours[c]);
      style.fillAlpha = 0.55f;
      Handle(SubHighlight) hl = new SubHighlight(style);
      Handle(Graphic3d_ArrayOfTriangles) a = new Graphic3d_ArrayOfTriangles(static_cast<int>(m.nodes.size()), static_cast<int>(m.indices.size()));
      for (const gp_Pnt& p : m.nodes) a->AddVertex(p);
      for (const int k : m.indices) a->AddEdge(k);
      hl->m_triangles.push_back(a);
      hl->SetZLayer(Graphic3d_ZLayerId_TopOSD);
      hl->SetInfiniteState(Standard_True);  // never part of Fit All
      m_ctx->Display(hl, 0, -1, Standard_False);  // selection mode -1: never pickable
      m_checkTints[c] = hl;
      m_tintTriangles[c] = m.triangles();
      for (const gp_Pnt& p : m.nodes) m_tintBoxes[c].Add(p);
    }
    redrawScene();
  };
  m_tintJob = m_jobs->sliced(tr("Colouring the findings"), [step](Job&) { return step(); }, done);
}

void Viewport::showOverlap(const TopoDS_Shape& shape, std::shared_ptr<const BodyPrs> prs, const std::vector<std::string>& pair) {
  if (!m_initialised) return;
  if (!m_overlap.IsNull()) {
    m_ctx->Remove(m_overlap, Standard_False);
    m_overlap.Nullify();
    m_overlapPair.clear();
    redrawScene();
  }
  if (shape.IsNull() || !prs) return;
  Handle(AIS_Shape) ais = new BodyShape(shape, std::move(prs));
  ais->Attributes()->SetAutoTriangulation(Standard_False);  // the worker meshed it
  ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
  ais->SetColor(occ(m_tokens.error));
  ais->SetTransparency(0.2);
  ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
  ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.error.darker(140)), Aspect_TOL_SOLID, 1.0));
  // Inside the pair, which is selected (Topmost, its own depth): drawn over everything, so it shows through them.
  ais->SetZLayer(Graphic3d_ZLayerId_TopOSD);
  ais->SetInfiniteState(Standard_True);
  m_overlap = ais;
  m_overlapPair = pair;
  placeOverlap();
  redrawScene();
}

// The overlap is the assembled pair's: drawn with the explode offset both bodies share, and not at all while they are pulled
// apart or either is not drawn (hidden, isolated away, a look). A pair not named (benches): drawn as computed.
void Viewport::placeOverlap() {
  if (!m_initialised || m_overlap.IsNull()) return;
  bool drawn = true;
  std::optional<std::array<double, 3>> offset;
  for (const auto& id : m_overlapPair) {
    const auto item = m_items.find(id);
    if (item == m_items.end() || !m_ctx->IsDisplayed(item->second.ais) || (offset && *offset != item->second.look.offset)) {
      drawn = false;
      break;
    }
    offset = item->second.look.offset;
  }
  if (!drawn) {
    if (m_ctx->IsDisplayed(m_overlap)) m_ctx->Erase(m_overlap, Standard_False);
    return;
  }
  gp_Trsf placed;
  if (offset && *offset != std::array<double, 3>{0, 0, 0}) placed.SetTranslation(gp_Vec((*offset)[0], (*offset)[1], (*offset)[2]));
  if (!m_overlap->LocalTransformation().TranslationPart().IsEqual(placed.TranslationPart(), 1e-9))  // every sync asks: only a move moves it
    m_ctx->SetLocation(m_overlap, placed.Form() == gp_Identity ? TopLoc_Location() : TopLoc_Location(placed));
  if (!m_ctx->IsDisplayed(m_overlap)) m_ctx->Display(m_overlap, AIS_Shaded, -1, Standard_False);
}

void Viewport::clearCheckOverlays() {
  showCheckTints({});
  showOverlap(TopoDS_Shape(), nullptr);
}

opad::json Viewport::benchCheckOverlays() const {
  opad::json j = {{"overhang_triangles", m_tintTriangles[0]}, {"thin_triangles", m_tintTriangles[1]}, {"tinting", m_tintJob != nullptr},
                  {"overlap", !m_overlap.IsNull() && m_ctx->IsDisplayed(m_overlap)}, {"gap", !m_dimension.empty()}};
  for (int c = 0; c < 2; ++c)
    if (!m_checkTints[c].IsNull()) {
      double r = 0, g = 0, b = 0;
      m_checkTints[c]->style().fill.Values(r, g, b, Quantity_TOC_sRGB);
      j[c ? "thin_colour" : "overhang_colour"] = {r, g, b};
      j[c ? "thin_layer" : "overhang_layer"] = int(m_checkTints[c]->ZLayer());
      const Bnd_Box& box = m_tintBoxes[c];
      if (!box.IsVoid()) j[c ? "thin_box" : "overhang_box"] = {box.CornerMin().X(), box.CornerMin().Y(), box.CornerMin().Z(),
                                                               box.CornerMax().X(), box.CornerMax().Y(), box.CornerMax().Z()};
    }
  if (!m_overlap.IsNull()) {
    Quantity_Color q;
    m_overlap->Color(q);
    double r = 0, g = 0, b = 0;
    q.Values(r, g, b, Quantity_TOC_sRGB);
    j["overlap_colour"] = {r, g, b};
    j["overlap_layer"] = int(m_overlap->ZLayer());
    const gp_XYZ at = m_overlap->LocalTransformation().TranslationPart();  // an explode offset its pair shares
    j["overlap_offset"] = {at.X(), at.Y(), at.Z()};
  }
  return j;
}
