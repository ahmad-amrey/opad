#include "opad/step_io.hpp"

#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <APIHeaderSection_MakeHeader.hxx>
#include <Interface_InterfaceModel.hxx>
#include <StepData_StepModel.hxx>
#include <StepRepr_NextAssemblyUsageOccurrence.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Writer.hxx>
#include <TCollection_HAsciiString.hxx>
#include <STEPControl_StepModelType.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <BRepTools.hxx>
#include <TopTools_FormatVersion.hxx>
#include <sstream>
#if __has_include(<RWGltf_CafWriter.hxx>)
#include <RWGltf_CafWriter.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#define OPAD_HAS_GLTF 1
#endif

#include <OSD_Parallel.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "opad/geometry.hpp"
#include "opad/mesh.hpp"
#include "import_common.hpp"

namespace opad {

json ImportResult::to_json() const {
  json j;
  j["op"] = op_id;
  j["components"] = components;
  j["bodies"] = bodies;
  j["new_entries"] = new_entries;
  j["healed"] = healed;
  j["warnings"] = warnings;
  if (!info.is_null()) j["info"] = info;
  return j;
}

json ExportResult::to_json() const {
  json j;
  json f = json::array();
  for (const auto& p : files) f.push_back(p.string());
  j["files"] = f;
  j["bodies"] = bodies;
  return j;
}

// ---------------------------------------------------------------- import
namespace {

class CallbackProgress : public Message_ProgressIndicator {
 public:
  std::function<bool(double, const std::string&)> cb;
  bool cancelled = false;
  // Reports the overall position plus the outermost named scope (e.g. the root being transferred) as
  // "translating <name> <i>/<n>" so a UI can show that translation is a sequence of steps, not one blob.
  // Reports "translating Part 1/4 > Part 3/57 > Transfer 12/300": every named counter from the outermost
  // scope inwards, plus a nested fraction over those counters, so a UI can show that translation is a
  // sequence of steps and its bar keeps moving inside a long first part.
  void Show(const Message_ProgressScope& scope, const Standard_Boolean) override {
    if (!cb) return;
    std::vector<const Message_ProgressScope*> chain;  // outermost first
    for (const Message_ProgressScope* s = &scope; s; s = s->Parent())
      if (s->Name() && *s->Name() && s->MaxValue() > 2) chain.insert(chain.begin(), s);  // 2-step scopes are noise
    std::string what = "translating";
    double frac = 0.0, weight = 1.0;
    for (size_t i = 0; i < chain.size(); ++i) {
      const double max = chain[i]->MaxValue();
      const double val = std::min(std::max(chain[i]->Value(), 0.0), max - 1.0);  // Value() = steps done, may overshoot
      frac += weight * (val / max);
      weight /= max;
      what += (i == 0 ? " " : " > ") + std::string(chain[i]->Name()) + " " + std::to_string(static_cast<int>(val) + 1) + "/" + std::to_string(static_cast<int>(max));
    }
    if (chain.empty()) frac = GetPosition();
    if (!cb(std::min(frac, 1.0), what)) cancelled = true;
  }
  Standard_Boolean UserBreak() override { return cancelled; }
};

std::string label_name(const TDF_Label& l) {
  Handle(TDataStd_Name) n;
  if (!l.IsNull() && l.FindAttribute(TDataStd_Name::GetID(), n)) {
    TCollection_AsciiString s(n->Get());
    return s.ToCString();
  }
  return "";
}

std::vector<TopoDS_Shape> split_bodies(const TopoDS_Shape& s) {
  std::vector<TopoDS_Shape> out;
  if (s.IsNull()) return out;
  switch (s.ShapeType()) {
    case TopAbs_SOLID:
    case TopAbs_SHELL:
    case TopAbs_FACE:
      out.push_back(s);
      return out;
    default:
      break;
  }
  for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) out.push_back(e.Current());
  if (!out.empty()) return out;
  for (TopExp_Explorer e(s, TopAbs_SHELL, TopAbs_SOLID); e.More(); e.Next()) out.push_back(e.Current());
  if (!out.empty()) return out;
  // Free faces: keep them together as one sheet body so nothing is lost.
  bool any = false;
  for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) any = true;
  if (any) out.push_back(s);
  return out;
}

struct Importer {
  Document& doc;
  const ImportOptions& opt;
  Handle(XCAFDoc_ShapeTool) st;
  Handle(XCAFDoc_ColorTool) ct;
  ImportResult res;
  std::string source;
  std::map<const void*, std::string> key_by_tshape;
  int body_counter = 0;
  int visited = 0, total = 0;  // shape labels walked / present, for a coarse "building" percentage
  Handle(XCAFDoc_VisMaterialTool) vt = nullptr;  // glTF, OBJ and newer STEP files keep colours as materials
  bool mesh = false;                   // the shapes are triangulations (glTF, OBJ, VRML), not B-reps
  double scale = 1.0;                  // file units -> mm, for readers that do not convert (VRML)
  std::map<const void*, TopoDS_Shape> meshes = {};  // product shape -> its plain, scaled triangulation (once per product)
  std::unordered_map<const void*, const void*> plain_source = {};  // plain_mesh face -> the reader's face it copies
  using FaceTable = std::unordered_map<const void*, Quantity_Color>;  // a face (its TShape) -> the colour the file gave it

  json placement(const TopLoc_Location& loc) const {
    Mat4 m = mat_from_trsf(loc.Transformation());
    for (int r = 0; r < 3; ++r) m.at(r, 3) *= scale;
    return m.to_json();
  }

  // A reader's mesh as plain triangulated faces in mm: the readers' own arrays may defer their data (glTF), which a saved
  // document could not hold, and VRML comes in metres. Null when there is not one triangle.
  TopoDS_Shape plain_mesh(const TopoDS_Shape& s) {
    auto known = meshes.find(s.TShape().get());
    if (known != meshes.end() && s.Location().IsIdentity()) return known->second;
    BRep_Builder builder;
    TopoDS_Compound all;
    builder.MakeCompound(all);
    int count = 0;
    TopoDS_Face last;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face& face = TopoDS::Face(e.Current());
      TopLoc_Location loc;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(face, loc);
      if (!t.IsNull() && t->NbNodes() == 0 && t->HasDeferredData()) t = t->DetachedLoadDeferredData();  // glTF loads late
      if (t.IsNull() || t->NbTriangles() == 0 || t->NbNodes() == 0) continue;
      const bool reversed = face.Orientation() == TopAbs_REVERSED;
      const gp_Trsf trsf = loc.Transformation();
      Handle(Poly_Triangulation) copy = new Poly_Triangulation(t->NbNodes(), t->NbTriangles(), Standard_False, t->HasNormals());
      for (int i = 1; i <= t->NbNodes(); ++i) {
        const gp_Pnt p = t->Node(i).Transformed(trsf);
        copy->SetNode(i, gp_Pnt(p.X() * scale, p.Y() * scale, p.Z() * scale));
        if (t->HasNormals()) {
          gp_Dir n = t->Normal(i).Transformed(trsf);
          if (reversed) n.Reverse();
          copy->SetNormal(i, n);
        }
      }
      for (int i = 1; i <= t->NbTriangles(); ++i) {
        int a, b, c;
        t->Triangle(i).Get(a, b, c);
        copy->SetTriangle(i, reversed ? Poly_Triangle(a, c, b) : Poly_Triangle(a, b, c));
      }
      TopoDS_Face plain;
      builder.MakeFace(plain, copy);
      builder.Add(all, plain);
      plain_source[plain.TShape().get()] = face.TShape().get();
      last = plain;
      ++count;
    }
    TopoDS_Shape out = count == 0 ? TopoDS_Shape() : count == 1 ? TopoDS_Shape(last) : TopoDS_Shape(all);
    if (s.Location().IsIdentity()) meshes[s.TShape().get()] = out;
    return out;
  }

  // OPAD keeps colours as they are shown (sRGB): a STEP or VRML colour as the file writes it, a material's (glTF, OBJ),
  // which OCCT holds linear, converted back (taken linear, an OBJ's Kd 0.439 came out 0.162 and the model nearly black).
  bool label_color(const TDF_Label& l, Quantity_Color& c) {
    if (l.IsNull()) return false;
    if (!ct.IsNull() && (ct->GetColor(l, XCAFDoc_ColorSurf, c) || ct->GetColor(l, XCAFDoc_ColorGen, c) ||
                         ct->GetColor(l, XCAFDoc_ColorCurv, c)))
      return true;
    const Handle(XCAFDoc_VisMaterial) m = material(l);
    if (m.IsNull()) return false;
    const NCollection_Vec3<float> shown = Quantity_Color::Convert_LinearRGB_To_sRGB(m->BaseColor().GetRGB().Rgb());
    c = Quantity_Color(shown.r(), shown.g(), shown.b(), Quantity_TOC_RGB);
    return true;
  }
  // The colours of a part's faces (and of its solids and shells, which their faces take unless coloured themselves);
  // `first` gets the first coloured sub-shape's label.
  FaceTable face_table(const TDF_Label& ref, TDF_Label& first) {
    FaceTable table;
    if (ct.IsNull() && vt.IsNull()) return table;
    TDF_LabelSequence subs;
    st->GetSubShapes(ref, subs);
    std::vector<std::pair<const void*, Quantity_Color>> faces;
    for (int i = 1; i <= subs.Length(); ++i) {
      Quantity_Color c;
      if (!label_color(subs.Value(i), c)) continue;
      const TopoDS_Shape s = st->GetShape(subs.Value(i));
      if (s.IsNull()) continue;
      if (first.IsNull()) first = subs.Value(i);
      if (s.ShapeType() == TopAbs_FACE) faces.push_back({s.TShape().get(), c});
      else for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) table.emplace(e.Current().TShape().get(), c);
    }
    for (const auto& [face, c] : faces) table[face] = c;
    return table;
  }
  const Quantity_Color* face_color(const FaceTable& table, const TopoDS_Shape& face) const {
    const void* ts = face.TShape().get();
    if (auto source = plain_source.find(ts); source != plain_source.end()) ts = source->second;
    auto it = table.find(ts);
    return it == table.end() ? nullptr : &it->second;
  }
  static bool same_color(const Quantity_Color& a, const Quantity_Color& b) {
    return std::abs(a.Red() - b.Red()) < 1e-4 && std::abs(a.Green() - b.Green()) < 1e-4 && std::abs(a.Blue() - b.Blue()) < 1e-4;
  }
  // A part whose file colours only its faces: when every face has a colour, the most common one is the part's (a mesh
  // reader's single material, a CAD file that styles each face); otherwise the uncoloured faces keep the default.
  bool common_color(const TopoDS_Shape& shape, const FaceTable& table, Quantity_Color& c) const {
    std::vector<std::pair<Quantity_Color, int>> counts;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
      const Quantity_Color* own = face_color(table, e.Current());
      if (!own) return false;
      auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& k) { return same_color(k.first, *own); });
      if (it == counts.end()) counts.push_back({*own, 1});
      else ++it->second;
    }
    if (counts.empty()) return false;
    c = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    return true;
  }
  // A prototype's "face_colors" (body meta): its faces whose colour is not the body's own.
  json face_colors_of(const TopoDS_Shape& proto, const FaceTable* table, const Quantity_Color* body) const {
    if (!table || table->empty()) return nullptr;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(proto, TopAbs_FACE, faces);
    FaceColors out;
    out.face.assign(static_cast<size_t>(faces.Extent()), -1);
    std::vector<Quantity_Color> palette;
    for (int i = 1; i <= faces.Extent(); ++i) {
      const Quantity_Color* c = face_color(*table, faces(i));
      if (!c || (body && same_color(*c, *body))) continue;
      auto it = std::find_if(palette.begin(), palette.end(), [&](const Quantity_Color& p) { return same_color(p, *c); });
      if (it == palette.end()) {
        palette.push_back(*c);
        out.colors.push_back({c->Red(), c->Green(), c->Blue()});
        it = palette.end() - 1;
      }
      out.face[static_cast<size_t>(i - 1)] = static_cast<int>(it - palette.begin());
    }
    return out.to_json();
  }
  Handle(XCAFDoc_VisMaterial) material(const TDF_Label& l) const {
    if (vt.IsNull() || l.IsNull()) return {};
    return vt->GetShapeMaterial(l);
  }
  // The material's name and opacity, from the occurrence or else the product it refers to.
  void label_look(const TDF_Label& l, const TDF_Label& ref, std::string& name, double& opacity) const {
    Handle(XCAFDoc_VisMaterial) m = material(l);
    if (m.IsNull()) m = material(ref);
    if (m.IsNull()) return;
    if (!m->RawName().IsNull()) name = TCollection_AsciiString(m->RawName()->String()).ToCString();
    opacity = std::clamp(static_cast<double>(m->BaseColor().Alpha()), 0.05, 1.0);
  }

  void progress(double frac, const std::string& what) {
    if (opt.progress && !opt.progress(frac, what)) throw Error("import cancelled");
  }
  double fraction() const { return total > 0 ? std::min(0.99, static_cast<double>(visited) / total) : -1.0; }

  std::string store(TopoDS_Shape proto, const std::string& name, const Quantity_Color* color, const std::string& material, const FaceTable* faces) {
    const void* ts = proto.TShape().get();
    auto it = key_by_tshape.find(ts);
    if (it != key_by_tshape.end() && proto.Location().IsIdentity()) return it->second;
    json meta;
    meta["name"] = name;
    if (color) meta["color"] = {color->Red(), color->Green(), color->Blue()};
    if (json own = face_colors_of(proto, faces, color); !own.is_null()) meta["face_colors"] = std::move(own);
    if (!material.empty()) meta["material"] = material;
    meta["units"] = "mm";
    meta["source"] = source;
    if (mesh) meta["representation"] = "mesh";
    std::string key = detail::store_body(doc, proto, meta, opt, mesh, &res);
    key_by_tshape[ts] = key;
    return key;
  }

  json body_node(const TopoDS_Shape& placed, const std::string& name, const Quantity_Color* color,
                 const TopLoc_Location& outer, const std::string& material = {}, double opacity = 1.0, const FaceTable* faces = nullptr) {
    TopLoc_Location loc = outer * placed.Location();
    TopoDS_Shape proto = placed.Located(TopLoc_Location());
    json n;
    n["type"] = "body";
    n["id"] = new_uuid();
    n["name"] = name;
    n["key"] = store(proto, name, color, material, faces);
    if (mesh) n["representation"] = "mesh";
    if (!loc.IsIdentity()) n["transform"] = placement(loc);
    if (color) n["color"] = {color->Red(), color->Green(), color->Blue()};
    if (opacity < 1.0) n["opacity"] = opacity;
    ++res.bodies;
    if (++body_counter % 25 == 0) progress(fraction(), "building");
    return n;
  }

  json walk(const TDF_Label& label, const std::string& fallback_name) {
    if (++visited % 10 == 1) progress(fraction(), "building");
    TDF_Label ref = label;
    TopLoc_Location loc;
    if (st->IsReference(label)) {
      loc = st->GetLocation(label);
      st->GetReferredShape(label, ref);
    }
    // Prefer the occurrence (instance) name, unless it is empty or one of the auto-generated
    // "=>[0:1:1:3]" placeholders OCCT emits for unnamed occurrences; then use the product name.
    std::string name = label_name(label);
    if (name.empty() || name.rfind("=>", 0) == 0) {
      std::string product = label_name(ref);
      if (!product.empty()) name = product;
    }
    if (name.empty() || name.rfind("=>", 0) == 0) name = fallback_name;
    Quantity_Color col;
    bool has_col = label_color(label, col) || label_color(ref, col);
    std::string material;
    double opacity = 1.0;
    label_look(label, ref, material, opacity);

    if (st->IsAssembly(ref)) {
      json node;
      node["type"] = "component";
      node["id"] = new_uuid();
      node["name"] = name;
      if (!loc.IsIdentity()) node["transform"] = placement(loc);
      json children = json::array();
      TDF_LabelSequence comps;
      st->GetComponents(ref, comps);
      for (int i = 1; i <= comps.Length(); ++i)
        children.push_back(walk(comps.Value(i), name + "." + std::to_string(i)));
      node["children"] = children;
      ++res.components;
      return node;
    }

    TopoDS_Shape shape = st->GetShape(ref);
    if (mesh) shape = plain_mesh(shape);
    TDF_Label first;
    const FaceTable faces = face_table(ref, first);
    if (!has_col && !first.IsNull()) {  // mesh readers and many CAD files colour the part's faces, not the part
      if (material.empty()) label_look(first, first, material, opacity);
      has_col = common_color(shape, faces, col);
    }
    std::vector<TopoDS_Shape> parts = split_bodies(shape);
    if (parts.empty()) {
      res.warnings.push_back("'" + name + "' contains no solids, shells or faces; skipped");
      json node;
      node["type"] = "component";
      node["id"] = new_uuid();
      node["name"] = name;
      node["children"] = json::array();
      return node;
    }
    if (parts.size() == 1) return body_node(parts[0], name, has_col ? &col : nullptr, loc, material, opacity, &faces);

    json node;
    node["type"] = "component";
    node["id"] = new_uuid();
    node["name"] = name;
    if (!loc.IsIdentity()) node["transform"] = placement(loc);
    json children = json::array();
    int i = 1;
    for (const auto& p : parts) {
      Quantity_Color pc;
      const Quantity_Color* pcol = has_col ? &col : nullptr;
      TDF_Label sub;
      if (st->FindSubShape(ref, p, sub) && label_color(sub, pc)) pcol = &pc;
      std::string pname = name + "[" + std::to_string(i++) + "]";
      if (!sub.IsNull() && !label_name(sub).empty()) pname = label_name(sub);
      children.push_back(body_node(p, pname, pcol, TopLoc_Location(), material, opacity, &faces));
    }
    node["children"] = children;
    ++res.components;
    return node;
  }
};

}  // namespace

ImportResult import_step(Document& doc, const std::filesystem::path& step, const ImportOptions& opt) {
  if (!std::filesystem::exists(step)) throw Error("STEP file not found: " + step.string());
  Handle(CallbackProgress) prog = new CallbackProgress();
  prog->cb = opt.progress;

  STEPCAFControl_Reader reader;
  reader.SetColorMode(Standard_True);
  reader.SetNameMode(Standard_True);
  reader.SetLayerMode(Standard_False);
  reader.SetPropsMode(Standard_False);
  Interface_Static::SetCVal("xstep.cascade.unit", "MM");

  if (opt.progress && !opt.progress(-1, "reading")) throw Error("import cancelled");
  IFSelect_ReturnStatus status;
  try {
    const auto utf8 = step.u8string();  // OCCT widens UTF-8 on Windows; the ANSI form failed for non-Latin file names
    status = reader.ReadFile(std::string(utf8.begin(), utf8.end()).c_str());
  } catch (const Standard_Failure& e) {
    throw Error(std::string("STEP read failed: ") + e.GetMessageString());
  }
  if (status != IFSelect_RetDone) throw Error("STEP read failed (not a STEP file or unsupported schema): " + step.string());

  Handle(TDocStd_Document) xdoc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", xdoc);
  bool ok;
  try {
    ok = reader.Transfer(xdoc, prog->Start());
  } catch (const Standard_Failure& e) {
    throw Error(std::string("STEP transfer failed: ") + e.GetMessageString());
  }
  if (prog->cancelled) throw Error("import cancelled");
  if (!ok) throw Error("STEP transfer produced no shapes: " + step.string());
  return detail::import_xcaf(doc, xdoc, step, opt, false);
}

namespace {
// Healing rebuilds the faces it touches (and the ones above edges it fixed): the body's face colours follow each face to
// the one made of it, found by the face itself or else by its surface, which the rebuilt face keeps.
void follow_face_colors(json& meta, const TopoDS_Shape& before, const TopoDS_Shape& after) {
  if (!meta.contains("face_colors")) return;
  const FaceColors old = FaceColors::from_json(meta["face_colors"]);
  TopTools_IndexedMapOfShape was, now;
  TopExp::MapShapes(before, TopAbs_FACE, was);
  TopExp::MapShapes(after, TopAbs_FACE, now);
  auto surface = [](const TopoDS_Shape& f) {
    TopLoc_Location loc;
    return static_cast<const void*>(BRep_Tool::Surface(TopoDS::Face(f), loc).get());
  };
  std::unordered_map<const void*, int> by_face, by_surface;
  for (int i = 1; i <= was.Extent(); ++i) {
    by_face[was(i).TShape().get()] = old.at(i - 1);
    if (const void* s = surface(was(i))) by_surface.emplace(s, old.at(i - 1));
  }
  FaceColors fresh;
  fresh.colors = old.colors;
  fresh.face.assign(static_cast<size_t>(now.Extent()), -1);
  for (int i = 1; i <= now.Extent(); ++i) {
    if (auto f = by_face.find(now(i).TShape().get()); f != by_face.end()) fresh.face[static_cast<size_t>(i - 1)] = f->second;
    else if (auto s = by_surface.find(surface(now(i))); s != by_surface.end() && s->first) fresh.face[static_cast<size_t>(i - 1)] = s->second;
  }
  if (json j = fresh.to_json(); j.is_null()) meta.erase("face_colors");
  else meta["face_colors"] = std::move(j);
}
}  // namespace

namespace detail {

std::string store_body(Document& doc, TopoDS_Shape shape, json meta, const ImportOptions& opt, bool mesh, ImportResult* res) {
  const size_t before = doc.body_count();
  std::string key;
  if (opt.viewer) {
    // Viewer mode skips everything that only serves persistence: the validity check and healing, the BREP text and
    // its SHA-256 key, and the later re-parse of that text. The reader's shape is cached as is, under a random key
    // that looks like a content hash (make_editable() replaces it with the real one).
    key = sha256_hex("live:" + new_uuid());
    doc.add_live_body(key, meta);
  } else {
    std::string brep;
    if (mesh) {
      // A mesh has no surfaces: its triangulation is the geometry, so it goes into the text.
      std::ostringstream ss;
      ss.precision(17);
      BRepTools::Write(shape, ss, Standard_True, Standard_False, TopTools_FormatVersion_VERSION_1);
      for (char c : ss.str())
        if (c != '\r') brep.push_back(c);
      if (brep.empty() || brep.back() != '\n') brep.push_back('\n');
    } else {
      if (opt.heal) {
        BRepCheck_Analyzer ana(shape);
        if (!ana.IsValid()) {
          ShapeFix_Shape fix(shape);
          fix.Perform();
          follow_face_colors(meta, shape, fix.Shape());
          shape = fix.Shape();
          if (res) ++res->healed;
          BRepCheck_Analyzer again(shape);
          if (!again.IsValid() && res) res->warnings.push_back("body '" + meta.value("name", std::string()) + "' still has invalid topology after healing");
        }
      }
      brep = brep_from_shape(shape);
    }
    key = doc.add_body(brep, meta);
  }
  if (res && doc.body_count() > before) ++res->new_entries;
  cache_shape(doc, key, shape);  // retain the translated/healed geometry; do not parse our own BREP again
  return key;
}

std::string persist_body(TopoDS_Shape& shape, json& meta, bool& healed) {
  const std::string representation = meta.value("representation", "solid");
  if (representation == "mesh") {  // the triangulation is the geometry
    std::ostringstream ss;
    ss.precision(17);
    BRepTools::Write(shape, ss, Standard_True, Standard_False, TopTools_FormatVersion_VERSION_1);
    std::string brep;
    for (char c : ss.str())
      if (c != '\r') brep.push_back(c);
    if (brep.empty() || brep.back() != '\n') brep.push_back('\n');
    return brep;
  }
  if (representation == "solid") {
    BRepCheck_Analyzer ana(shape);
    if (!ana.IsValid()) {
      ShapeFix_Shape fix(shape);
      fix.Perform();
      follow_face_colors(meta, shape, fix.Shape());
      shape = fix.Shape();
      healed = true;
    }
  }
  return brep_from_shape(shape);
}

ImportResult import_xcaf(Document& doc, const Handle(TDocStd_Document)& xdoc, const std::filesystem::path& step, const ImportOptions& opt, bool mesh,
                         double scale, const Mat4& root) {
  Importer imp{doc, opt, XCAFDoc_DocumentTool::ShapeTool(xdoc->Main()), XCAFDoc_DocumentTool::ColorTool(xdoc->Main()), {},
               step.filename().string(), {}, 0};
  imp.vt = XCAFDoc_DocumentTool::VisMaterialTool(xdoc->Main());
  imp.mesh = mesh;
  imp.scale = scale;
  TDF_LabelSequence free_shapes;
  imp.st->GetFreeShapes(free_shapes);
  {
    TDF_LabelSequence all;
    imp.st->GetShapes(all);
    imp.total = all.Length();
  }
  imp.progress(0.0, "building");
  json nodes = json::array();
  std::string stem = step.stem().string();
  for (int i = 1; i <= free_shapes.Length(); ++i)
    nodes.push_back(imp.walk(free_shapes.Value(i), free_shapes.Length() == 1 ? stem : stem + "." + std::to_string(i)));

  if (!root.is_identity())  // the file's axes turned to OPAD's (Z up), for readers that do not
    for (auto& n : nodes) n["transform"] = (root * (n.contains("transform") ? Mat4::from_json(n["transform"]) : Mat4{})).to_json();
  if (mesh && imp.res.bodies == 0) throw Error(step.filename().string() + " holds no triangles to show");
  json op;
  op["op"] = "import";
  op["source"] = step.filename().string();
  op["units"] = "mm";
  if (!opt.parent.empty()) op["parent"] = opt.parent;
  op["nodes"] = nodes;
  const Op& o = doc.append(op, opt.author);
  imp.res.op_id = o.id;
  if (imp.res.bodies == 0) imp.res.warnings.push_back("no bodies were imported");
  return imp.res;
}

}  // namespace detail

Document browse_step(const std::filesystem::path& step, const ImportOptions& opt) {
  Document d = Document::create();
  import_step(d, step, opt);
  d.dirty = false;
  return d;
}

ImportResult import_brep(Document& doc, const std::string& brep, const std::string& name, const ImportOptions& opt) {
  std::string text = brep;
  if (text.empty() || text.back() != '\n') text.push_back('\n');
  TopoDS_Shape shape = shape_from_brep(text);
  Importer imp{doc, opt, nullptr, nullptr, {}, name + ".brep", {}, 0};
  std::vector<TopoDS_Shape> parts = split_bodies(shape);
  if (parts.empty()) throw Error("BREP contains no solids, shells or faces");
  json nodes = json::array();
  if (parts.size() == 1) {
    nodes.push_back(imp.body_node(parts[0], name, nullptr, TopLoc_Location()));
  } else {
    json node;
    node["type"] = "component";
    node["id"] = new_uuid();
    node["name"] = name;
    json children = json::array();
    int i = 1;
    for (const auto& p : parts) children.push_back(imp.body_node(p, name + "[" + std::to_string(i++) + "]", nullptr, TopLoc_Location()));
    node["children"] = children;
    nodes.push_back(node);
    ++imp.res.components;
  }
  json op;
  op["op"] = "import";
  op["source"] = name + ".brep";
  op["units"] = "mm";
  if (!opt.parent.empty()) op["parent"] = opt.parent;
  op["nodes"] = nodes;
  imp.res.op_id = doc.append(op, opt.author).id;
  return imp.res;
}

// ---------------------------------------------------------------- viewer -> editable
namespace {
void rename_keys(json& nodes, const std::map<std::string, std::string>& keys) {
  if (!nodes.is_array()) return;
  for (auto& n : nodes) {
    if (!n.is_object()) continue;
    if (auto k = n.find("key"); k != n.end() && k->is_string())
      if (auto it = keys.find(k->get<std::string>()); it != keys.end()) *k = it->second;
    if (auto c = n.find("children"); c != n.end()) rename_keys(*c, keys);
  }
}
}  // namespace

Document make_editable(const Document& viewer, EditableKeys* changed, const std::function<bool(double)>& progress) {
  const auto& bodies = viewer.bodies();
  // Per live body: its BREP text (healed like a full import), side by side; the kernel work is independent per shape.
  struct Work { TopoDS_Shape shape; json meta; std::string brep; bool healed = false; std::string error; };
  std::vector<Work> work(bodies.size());
  std::atomic<size_t> done{0};
  std::atomic<bool> cancelled{false};
  OSD_Parallel::For(0, static_cast<int>(bodies.size()), [&](int i) {
    if (cancelled) return;
    const BodyEntry& b = bodies[static_cast<size_t>(i)];
    if (!b.brep.empty()) return;
    Work& w = work[static_cast<size_t>(i)];
    try {
      w.meta = b.meta;
      w.shape = body_shape(viewer, b.key);
      w.brep = detail::persist_body(w.shape, w.meta, w.healed);
    } catch (const Standard_Failure& e) {
      w.error = e.GetMessageString();
    } catch (const std::exception& e) {
      w.error = e.what();
    }
    if (progress && !progress(static_cast<double>(++done) / static_cast<double>(bodies.size()))) cancelled = true;
  });
  if (cancelled) throw Error("cancelled");
  // The rebuilt store: persisted entries as they are, live ones under their content keys. Same header, path and shape
  // cache, so the shapes on screen stay loaded and meshed.
  Document rebuilt = Document::create(viewer.header.units);
  rebuilt.header = viewer.header;
  rebuilt.path = viewer.path;
  rebuilt.shape_cache = viewer.shape_cache;
  std::map<std::string, std::string> keys;
  for (size_t i = 0; i < bodies.size(); ++i) {
    const BodyEntry& b = bodies[i];
    if (!b.brep.empty()) { rebuilt.add_body(b.brep, b.meta); continue; }
    if (!work[i].error.empty()) throw Error("body '" + b.meta.value("name", b.key.substr(0, 12)) + "' cannot be saved: " + work[i].error);
    const std::string key = rebuilt.add_body(work[i].brep, work[i].meta);
    cache_shape(rebuilt, key, work[i].shape);
    keys[b.key] = key;
    if (changed) {
      if (work[i].healed) changed->healed.insert(key);
      else changed->renamed[b.key] = key;
    }
  }
  for (const Op& o : viewer.ops) {
    json data = o.data;
    if (o.type == "import") rename_keys(data["nodes"], keys);
    rebuilt.append(std::move(data));
  }
  rebuilt.dirty = true;
  return rebuilt;
}

// ---------------------------------------------------------------- export
std::vector<std::string> select_bodies(const Scene& scene, const std::vector<std::string>& select) {
  if (select.empty()) return scene.all_bodies();
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& id : select) {
    if (!scene.node(id)) throw Error("selection: unknown node " + id);
    for (const auto& b : scene.bodies_under(id))
      if (seen.insert(b).second) out.push_back(b);
  }
  return out;
}

namespace {

std::string safe_name(std::string s) {
  for (char& c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) c = '_';
  return s.empty() ? "body" : s;
}

struct PlacedMesh {
  std::string id, name;
  Mesh mesh;   // in world coordinates
  std::array<double, 3> color;
  bool has_color;
  FaceColors faces;  // the body's own face colours (by face ordinal, as mesh.faces)
};

std::vector<PlacedMesh> gather_meshes(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies,
                                      double tol) {
  std::vector<PlacedMesh> out;
  for (const auto& id : bodies) {
    const Node* n = scene.node(id);
    if (!n || n->body_missing) continue;
    PlacedMesh pm{id, n->name, tessellate_body(doc, n->body_key, tol), n->color, n->has_color, face_colors(doc, n->body_key)};
    Mat4 w = scene.world(id);
    if (!w.is_identity()) {
      for (size_t i = 0; i + 2 < pm.mesh.positions.size(); i += 3) {
        Vec3 p = w.apply({pm.mesh.positions[i], pm.mesh.positions[i + 1], pm.mesh.positions[i + 2]});
        Vec3 d = w.apply_dir({pm.mesh.normals[i], pm.mesh.normals[i + 1], pm.mesh.normals[i + 2]});
        double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 0) { d[0] /= len; d[1] /= len; d[2] /= len; }
        for (int k = 0; k < 3; ++k) {
          pm.mesh.positions[i + k] = static_cast<float>(p[k]);
          pm.mesh.normals[i + k] = static_cast<float>(d[k]);
        }
      }
    }
    out.push_back(std::move(pm));
  }
  return out;
}

void write_obj(const std::filesystem::path& out, const std::vector<PlacedMesh>& meshes, bool with_mtl,
               ExportResult& res) {
  std::ofstream f(out, std::ios::binary);
  if (!f) throw Error("cannot write " + out.string());
  f << "# OPAD " << version_string() << " OBJ export, units mm\n";
  std::filesystem::path mtl = out;
  mtl.replace_extension(".mtl");
  if (with_mtl) f << "mtllib " << mtl.filename().string() << "\n";
  size_t offset = 1;
  int mi = 0;
  char buf[128];
  for (const auto& pm : meshes) {
    f << "o " << safe_name(pm.name) << "\n";
    for (size_t i = 0; i + 2 < pm.mesh.positions.size(); i += 3) {
      std::snprintf(buf, sizeof buf, "v %.6g %.6g %.6g\n", pm.mesh.positions[i], pm.mesh.positions[i + 1], pm.mesh.positions[i + 2]);
      f << buf;
    }
    for (size_t i = 0; i + 2 < pm.mesh.normals.size(); i += 3) {
      std::snprintf(buf, sizeof buf, "vn %.4f %.4f %.4f\n", pm.mesh.normals[i], pm.mesh.normals[i + 1], pm.mesh.normals[i + 2]);
      f << buf;
    }
    auto triangles = [&](size_t first, size_t end) {
      for (size_t i = first; i + 2 < end; i += 3) {
        size_t a = offset + pm.mesh.indices[i], b = offset + pm.mesh.indices[i + 1], c = offset + pm.mesh.indices[i + 2];
        f << "f " << a << "//" << a << " " << b << "//" << b << " " << c << "//" << c << "\n";
      }
    };
    if (pm.faces.empty()) {
      if (with_mtl) f << "usemtl m" << mi << "\n";
      triangles(0, pm.mesh.indices.size());
    } else {
      // One group per colour: the body's own (m<i>), then each face colour (m<i>_<c>).
      for (int group = -1; group < static_cast<int>(pm.faces.colors.size()); ++group) {
        bool any = false;
        for (const auto& range : pm.mesh.faces) {
          if (pm.faces.at(range.face) != group) continue;
          if (!any && with_mtl) f << "usemtl m" << mi << (group < 0 ? std::string() : "_" + std::to_string(group)) << "\n";
          any = true;
          triangles(range.first, range.first + range.count);
        }
      }
    }
    ++mi;
    offset += pm.mesh.positions.size() / 3;
  }
  res.files.push_back(out);
  if (with_mtl) {
    std::ofstream m(mtl, std::ios::binary);
    int i = 0;
    for (const auto& pm : meshes) {
      m << "newmtl m" << i << "\n";
      std::snprintf(buf, sizeof buf, "Kd %.4f %.4f %.4f\n", pm.color[0], pm.color[1], pm.color[2]);
      m << buf << "Ka 0.1 0.1 0.1\nKs 0.2 0.2 0.2\nNs 20\n\n";
      for (size_t c = 0; c < pm.faces.colors.size(); ++c) {
        m << "newmtl m" << i << "_" << c << "\n";
        std::snprintf(buf, sizeof buf, "Kd %.4f %.4f %.4f\n", pm.faces.colors[c][0], pm.faces.colors[c][1], pm.faces.colors[c][2]);
        m << buf << "Ka 0.1 0.1 0.1\nKs 0.2 0.2 0.2\nNs 20\n\n";
      }
      ++i;
    }
    res.files.push_back(mtl);
  }
}

void write_stl_file(const std::filesystem::path& out, const std::vector<const PlacedMesh*>& meshes, bool ascii,
                    const std::string& solid_name) {
  std::ofstream f(out, std::ios::binary);
  if (!f) throw Error("cannot write " + out.string());
  auto tri_normal = [](const float* a, const float* b, const float* c, float* n) {
    float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0) { n[0] /= len; n[1] /= len; n[2] /= len; }
  };
  if (ascii) {
    f << "solid " << safe_name(solid_name) << "\n";
    char buf[256];
    for (const auto* pm : meshes)
      for (size_t i = 0; i + 2 < pm->mesh.indices.size(); i += 3) {
        const float* a = &pm->mesh.positions[pm->mesh.indices[i] * 3];
        const float* b = &pm->mesh.positions[pm->mesh.indices[i + 1] * 3];
        const float* c = &pm->mesh.positions[pm->mesh.indices[i + 2] * 3];
        float n[3];
        tri_normal(a, b, c, n);
        std::snprintf(buf, sizeof buf, "  facet normal %.6g %.6g %.6g\n    outer loop\n", n[0], n[1], n[2]);
        f << buf;
        for (const float* v : {a, b, c}) {
          std::snprintf(buf, sizeof buf, "      vertex %.6g %.6g %.6g\n", v[0], v[1], v[2]);
          f << buf;
        }
        f << "    endloop\n  endfacet\n";
      }
    f << "endsolid " << safe_name(solid_name) << "\n";
    return;
  }
  char header[80] = {0};
  std::snprintf(header, sizeof header, "OPAD %s binary STL, units mm", version_string().c_str());
  f.write(header, 80);
  uint32_t count = 0;
  for (const auto* pm : meshes) count += static_cast<uint32_t>(pm->mesh.triangle_count());
  f.write(reinterpret_cast<const char*>(&count), 4);
  const uint16_t attr = 0;
  for (const auto* pm : meshes)
    for (size_t i = 0; i + 2 < pm->mesh.indices.size(); i += 3) {
      const float* a = &pm->mesh.positions[pm->mesh.indices[i] * 3];
      const float* b = &pm->mesh.positions[pm->mesh.indices[i + 1] * 3];
      const float* c = &pm->mesh.positions[pm->mesh.indices[i + 2] * 3];
      float n[3];
      tri_normal(a, b, c, n);
      f.write(reinterpret_cast<const char*>(n), 12);
      f.write(reinterpret_cast<const char*>(a), 12);
      f.write(reinterpret_cast<const char*>(b), 12);
      f.write(reinterpret_cast<const char*>(c), 12);
      f.write(reinterpret_cast<const char*>(&attr), 2);
    }
}

// Builds an XCAF document mirroring the selected part of the scene (names, colours and face colours, placements).
// `with_mesh` (glTF): the bodies meshed, and colours handed over as sRGB, which the writer stores linear as glTF wants;
// STEP keeps OPAD's values as they are, which is how STEP files hold colours.
Handle(TDocStd_Document) build_xcaf(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies,
                                    bool with_mesh, double tol) {
  std::set<std::string> wanted(bodies.begin(), bodies.end());
  // Components that contain at least one selected body.
  std::set<std::string> keep;
  for (const auto& b : bodies)
    for (const auto& id : scene.path_to(b)) keep.insert(id);

  Handle(TDocStd_Document) xdoc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", xdoc);
  Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(xdoc->Main());
  Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(xdoc->Main());

  struct Built { TopoDS_Shape placed; TopoDS_Shape proto; const Node* node; };
  std::vector<Built> built;
  BRep_Builder bb;
  std::map<std::string, TopoDS_Shape> shells;  // a mesh body of several faces (one per colour) as one part: a shell, per body key

  std::function<TopoDS_Shape(const std::string&)> build = [&](const std::string& id) -> TopoDS_Shape {
    const Node* n = scene.node(id);
    if (!n || !keep.count(id)) return TopoDS_Shape();
    TopoDS_Shape proto;
    if (n->kind == Node::Kind::Body) {
      if (n->body_missing) return TopoDS_Shape();
      proto = body_shape(doc, n->body_key);
      if (with_mesh) mesh_shape(proto, tol);
      // A compound would become an assembly, each face a node of its own (glTF): its faces go into a shell, one part.
      if (with_mesh && n->representation == "mesh" && proto.ShapeType() == TopAbs_COMPOUND) {
        auto& shell = shells[n->body_key];
        if (shell.IsNull()) {
          TopoDS_Shell faces;
          bb.MakeShell(faces);
          for (TopExp_Explorer f(proto, TopAbs_FACE); f.More(); f.Next()) bb.Add(faces, f.Current());
          shell = faces;
        }
        proto = shell;
      }
    } else {
      TopoDS_Compound comp;
      bb.MakeCompound(comp);
      for (const auto& c : n->children) {
        TopoDS_Shape cs = build(c);
        if (!cs.IsNull()) bb.Add(comp, cs);
      }
      proto = comp;
    }
    TopoDS_Shape placed = proto;
    if (!n->local.is_identity()) {
      if (mat_is_rigid(n->local)) placed = proto.Located(TopLoc_Location(trsf_from_mat(n->local)));
      else throw Error("node " + id + " has a non-rigid transform; STEP export cannot represent it");
    }
    built.push_back({placed, proto, n});
    return placed;
  };

  for (const auto& r : scene.roots) {
    TopoDS_Shape s = build(r);
    if (s.IsNull()) continue;
    st->AddShape(s, Standard_True);
  }
  st->UpdateAssemblies();
  const auto color = [with_mesh](const std::array<double, 3>& c) { return Quantity_Color(c[0], c[1], c[2], with_mesh ? Quantity_TOC_sRGB : Quantity_TOC_RGB); };
  std::set<std::string> faced;  // body keys whose face colours are set (on the prototype, shared by its instances)
  for (const auto& b : built) {
    TDF_Label proto_label, inst_label;
    if (st->FindShape(b.proto, proto_label, Standard_False) && !proto_label.IsNull()) {
      TDataStd_Name::Set(proto_label, b.node->name.c_str());
      if (b.node->kind == Node::Kind::Body && b.node->has_color) ct->SetColor(proto_label, color(b.node->color), XCAFDoc_ColorSurf);
      const bool body = b.node->kind == Node::Kind::Body && faced.insert(b.node->body_key).second;
      if (const FaceColors faces = body ? face_colors(doc, b.node->body_key) : FaceColors{}; !faces.empty()) {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(b.proto, TopAbs_FACE, map);
        for (int i = 1; i <= map.Extent(); ++i) {
          if (faces.at(i - 1) < 0) continue;
          // A face is a sub-shape of its part (a solid, a mesh body's shell), or a part of its own.
          TDF_Label at;
          if (!st->FindShape(map(i), at, Standard_False) || at.IsNull()) at = st->AddSubShape(proto_label, map(i));
          if (!at.IsNull()) ct->SetColor(at, color(faces.colors[static_cast<size_t>(faces.at(i - 1))]), XCAFDoc_ColorSurf);
        }
      }
    }
    if (st->FindShape(b.placed, inst_label, Standard_True) && !inst_label.IsNull() && inst_label != proto_label)
      TDataStd_Name::Set(inst_label, b.node->name.c_str());
  }
  return xdoc;
}

}  // namespace

ExportResult export_selection(const Document& doc, const Scene& scene, const std::filesystem::path& out,
                              const ExportOptions& opt) {
  for(const auto& id:opt.select) if(scene.sketch(id)) throw Error("Sketches export as DXF, SVG or DWG; select a solid or mesh for this format");
  for(const auto& id:select_bodies(scene,opt.select)) if(scene.node(id)->representation=="drawing2d") throw Error("2D drawings export as DXF, SVG or DWG");
  if (opt.format == "step" || opt.format == "stp")
    for (const auto& id : select_bodies(scene, opt.select))
      if (scene.node(id)->representation == "mesh") throw Error("Mesh objects cannot be exported as CAD solids; choose STL, OBJ or GLB");
  ExportResult res;
  std::vector<std::string> bodies = select_bodies(scene, opt.select);
  bodies.erase(std::remove_if(bodies.begin(), bodies.end(),
                              [&](const std::string& id) { return scene.node(id)->body_missing; }),
               bodies.end());
  if (bodies.empty()) throw Error("nothing to export (selection contains no bodies)");
  res.bodies = static_cast<int>(bodies.size());
  if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path());
  std::string fmt = opt.format;
  std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });

  if (fmt == "step" || fmt == "stp") {
    Handle(TDocStd_Document) xdoc = build_xcaf(doc, scene, bodies, false, opt.tolerance);
    // Set the writer parameters before constructing the writer: OCCT 7.6 stamps FILE_SCHEMA when the writer's
    // model is created, so a schema set afterwards only reaches the data section.
    std::string schema = opt.step_schema;
    std::transform(schema.begin(), schema.end(), schema.begin(), [](unsigned char c) { return std::toupper(c); });
    Interface_Static::SetCVal("write.step.schema", schema == "AP242" ? "AP242DIS" : schema == "AP203" ? "AP203" : "AP214");
    Interface_Static::SetCVal("write.step.unit", "MM");
    Interface_Static::SetCVal("write.step.product.name", out.stem().string().c_str());
    STEPCAFControl_Writer writer;
    writer.SetColorMode(Standard_True);
    writer.SetNameMode(Standard_True);
    try {
      if (!writer.Transfer(xdoc, STEPControl_AsIs)) throw Error("STEP transfer failed");
      // The same document exports the same file (gap log #14): the header's time is the document's last change,
      // not the clock, and its name the file's, not the folder it was written to.
      APIHeaderSection_MakeHeader header(writer.ChangeWriter().Model());
      std::string stamp = "2000-01-01T00:00:00";
      for (auto it = doc.ops.rbegin(); it != doc.ops.rend(); ++it)
        if (it->data.contains("ts") && it->data["ts"].is_string()) {
          stamp = it->data["ts"].get<std::string>();
          break;
        }
      header.SetTimeStamp(new TCollection_HAsciiString(stamp.c_str()));
      header.SetName(new TCollection_HAsciiString(out.filename().string().c_str()));
      // OCCT numbers assembly occurrences from a counter that lives as long as the process: number them in order.
      const Handle(Interface_InterfaceModel) model = writer.ChangeWriter().Model();
      int occurrence = 0;
      for (int i = 1; i <= model->NbEntities(); ++i)
        if (const auto nauo = Handle(StepRepr_NextAssemblyUsageOccurrence)::DownCast(model->Value(i)); !nauo.IsNull())
          nauo->SetId(new TCollection_HAsciiString(++occurrence));
      if (writer.Write(out.string().c_str()) != IFSelect_RetDone) throw Error("STEP write failed: " + out.string());
    } catch (const Standard_Failure& e) {
      throw Error(std::string("STEP export failed: ") + e.GetMessageString());
    }
    res.files.push_back(out);
    return res;
  }
  if (fmt == "obj") {
    write_obj(out, gather_meshes(doc, scene, bodies, opt.tolerance), opt.mtl, res);
    return res;
  }
  if (fmt == "stl") {
    auto meshes = gather_meshes(doc, scene, bodies, opt.tolerance);
    if (opt.per_body && meshes.size() > 1) {
      std::set<std::string> used;
      for (const auto& pm : meshes) {
        std::string base = safe_name(pm.name);
        std::string name = base;
        for (int i = 2; used.count(name); ++i) name = base + "_" + std::to_string(i);
        used.insert(name);
        std::filesystem::path p = out.parent_path() / (out.stem().string() + "_" + name + out.extension().string());
        write_stl_file(p, {&pm}, opt.ascii, pm.name);
        res.files.push_back(p);
      }
    } else {
      std::vector<const PlacedMesh*> ptrs;
      for (const auto& pm : meshes) ptrs.push_back(&pm);
      write_stl_file(out, ptrs, opt.ascii, out.stem().string());
      res.files.push_back(out);
    }
    return res;
  }
  if (fmt == "glb" || fmt == "gltf") {
#ifdef OPAD_HAS_GLTF
    Handle(TDocStd_Document) xdoc = build_xcaf(doc, scene, bodies, true, opt.tolerance);
    RWGltf_CafWriter writer(TCollection_AsciiString(out.string().c_str()), fmt == "glb");
    writer.ChangeCoordinateSystemConverter().SetInputLengthUnit(0.001);
    writer.ChangeCoordinateSystemConverter().SetInputCoordinateSystem(RWMesh_CoordinateSystem_Zup);
    TColStd_IndexedDataMapOfStringString meta;
    meta.Add("generator", TCollection_AsciiString(("opad/" + version_string()).c_str()));
    if (!writer.Perform(xdoc, meta, Message_ProgressRange())) throw Error("glTF write failed: " + out.string());
    res.files.push_back(out);
    return res;
#else
    throw Error("this build of OPAD has no glTF support (OCCT TKDEGLTF missing)");
#endif
  }
  throw Error("unknown export format: " + opt.format);
}

}  // namespace opad
