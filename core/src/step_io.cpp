#include "opad/step_io.hpp"

#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_StepModelType.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#if __has_include(<RWGltf_CafWriter.hxx>)
#include <RWGltf_CafWriter.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#define OPAD_HAS_GLTF 1
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#include "opad/geometry.hpp"
#include "opad/mesh.hpp"

namespace opad {

json ImportResult::to_json() const {
  json j;
  j["op"] = op_id;
  j["components"] = components;
  j["bodies"] = bodies;
  j["new_entries"] = new_entries;
  j["healed"] = healed;
  j["warnings"] = warnings;
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
  int live_counter = 0;        // viewer mode: sequential keys for live (never hashed) bodies

  bool label_color(const TDF_Label& l, Quantity_Color& c) {
    if (l.IsNull()) return false;
    return ct->GetColor(l, XCAFDoc_ColorSurf, c) || ct->GetColor(l, XCAFDoc_ColorGen, c) ||
           ct->GetColor(l, XCAFDoc_ColorCurv, c);
  }

  void progress(double frac, const std::string& what) {
    if (opt.progress && !opt.progress(frac, what)) throw Error("import cancelled");
  }
  double fraction() const { return total > 0 ? std::min(0.99, static_cast<double>(visited) / total) : -1.0; }

  std::string store(TopoDS_Shape proto, const std::string& name, const Quantity_Color* color) {
    const void* ts = proto.TShape().get();
    auto it = key_by_tshape.find(ts);
    if (it != key_by_tshape.end() && proto.Location().IsIdentity()) return it->second;
    if (opt.viewer) {
      // Viewer mode skips everything that only serves persistence: the validity check and healing, the BREP
      // text and its SHA-256 key, and the later re-parse of that text. The reader's shape is cached as is.
      std::string key = sha256_hex("live:" + std::to_string(++live_counter));  // keys must look like content hashes
      json meta;
      meta["name"] = name;
      if (color) meta["color"] = {color->Red(), color->Green(), color->Blue()};
      meta["units"] = "mm";
      meta["source"] = source;
      ++res.new_entries;
      doc.add_live_body(key, meta);
      cache_shape(doc, key, proto);
      key_by_tshape[ts] = key;
      return key;
    }
    if (opt.heal) {
      BRepCheck_Analyzer ana(proto);
      if (!ana.IsValid()) {
        ShapeFix_Shape fix(proto);
        fix.Perform();
        proto = fix.Shape();
        ++res.healed;
        BRepCheck_Analyzer again(proto);
        if (!again.IsValid()) res.warnings.push_back("body '" + name + "' still has invalid topology after healing");
      }
    }
    std::string brep;
    std::string key = body_key_for(proto, &brep);
    json meta;
    meta["name"] = name;
    if (color) meta["color"] = {color->Red(), color->Green(), color->Blue()};
    meta["units"] = "mm";
    meta["source"] = source;
    if (!doc.has_body(key)) ++res.new_entries;
    doc.add_body(brep, meta);
    key_by_tshape[ts] = key;
    return key;
  }

  json body_node(const TopoDS_Shape& placed, const std::string& name, const Quantity_Color* color,
                 const TopLoc_Location& outer) {
    TopLoc_Location loc = outer * placed.Location();
    TopoDS_Shape proto = placed.Located(TopLoc_Location());
    json n;
    n["type"] = "body";
    n["id"] = new_uuid();
    n["name"] = name;
    n["key"] = store(proto, name, color);
    if (!loc.IsIdentity()) n["transform"] = mat_from_trsf(loc.Transformation()).to_json();
    if (color) n["color"] = {color->Red(), color->Green(), color->Blue()};
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

    if (st->IsAssembly(ref)) {
      json node;
      node["type"] = "component";
      node["id"] = new_uuid();
      node["name"] = name;
      if (!loc.IsIdentity()) node["transform"] = mat_from_trsf(loc.Transformation()).to_json();
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
    if (parts.size() == 1) return body_node(parts[0], name, has_col ? &col : nullptr, loc);

    json node;
    node["type"] = "component";
    node["id"] = new_uuid();
    node["name"] = name;
    if (!loc.IsIdentity()) node["transform"] = mat_from_trsf(loc.Transformation()).to_json();
    json children = json::array();
    int i = 1;
    for (const auto& p : parts) {
      Quantity_Color pc;
      const Quantity_Color* pcol = has_col ? &col : nullptr;
      TDF_Label sub;
      if (st->FindSubShape(ref, p, sub) && label_color(sub, pc)) pcol = &pc;
      std::string pname = name + "[" + std::to_string(i++) + "]";
      if (!sub.IsNull() && !label_name(sub).empty()) pname = label_name(sub);
      children.push_back(body_node(p, pname, pcol, TopLoc_Location()));
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
    status = reader.ReadFile(step.string().c_str());
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

  Importer imp{doc, opt, XCAFDoc_DocumentTool::ShapeTool(xdoc->Main()), XCAFDoc_DocumentTool::ColorTool(xdoc->Main()), {},
               step.filename().string(), {}, 0};
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
};

std::vector<PlacedMesh> gather_meshes(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies,
                                      double tol) {
  std::vector<PlacedMesh> out;
  for (const auto& id : bodies) {
    const Node* n = scene.node(id);
    if (!n || n->body_missing) continue;
    PlacedMesh pm{id, n->name, tessellate_body(doc, n->body_key, tol), n->color, n->has_color};
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
    if (with_mtl) f << "usemtl m" << mi++ << "\n";
    for (size_t i = 0; i + 2 < pm.mesh.indices.size(); i += 3) {
      size_t a = offset + pm.mesh.indices[i], b = offset + pm.mesh.indices[i + 1], c = offset + pm.mesh.indices[i + 2];
      f << "f " << a << "//" << a << " " << b << "//" << b << " " << c << "//" << c << "\n";
    }
    offset += pm.mesh.positions.size() / 3;
  }
  res.files.push_back(out);
  if (with_mtl) {
    std::ofstream m(mtl, std::ios::binary);
    int i = 0;
    for (const auto& pm : meshes) {
      m << "newmtl m" << i++ << "\n";
      std::snprintf(buf, sizeof buf, "Kd %.4f %.4f %.4f\n", pm.color[0], pm.color[1], pm.color[2]);
      m << buf << "Ka 0.1 0.1 0.1\nKs 0.2 0.2 0.2\nNs 20\n\n";
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

// Builds an XCAF document mirroring the selected part of the scene (names, colours, placements).
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

  std::function<TopoDS_Shape(const std::string&)> build = [&](const std::string& id) -> TopoDS_Shape {
    const Node* n = scene.node(id);
    if (!n || !keep.count(id)) return TopoDS_Shape();
    TopoDS_Shape proto;
    if (n->kind == Node::Kind::Body) {
      if (n->body_missing) return TopoDS_Shape();
      proto = body_shape(doc, n->body_key);
      if (with_mesh) BRepMesh_IncrementalMesh(proto, tol, Standard_False, 20.0 * M_PI / 180.0, Standard_True);
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
  for (const auto& b : built) {
    TDF_Label proto_label, inst_label;
    if (st->FindShape(b.proto, proto_label, Standard_False) && !proto_label.IsNull()) {
      TDataStd_Name::Set(proto_label, b.node->name.c_str());
      if (b.node->kind == Node::Kind::Body && b.node->has_color)
        ct->SetColor(proto_label, Quantity_Color(b.node->color[0], b.node->color[1], b.node->color[2], Quantity_TOC_RGB),
                     XCAFDoc_ColorSurf);
    }
    if (st->FindShape(b.placed, inst_label, Standard_True) && !inst_label.IsNull() && inst_label != proto_label)
      TDataStd_Name::Set(inst_label, b.node->name.c_str());
  }
  return xdoc;
}

}  // namespace

ExportResult export_selection(const Document& doc, const Scene& scene, const std::filesystem::path& out,
                              const ExportOptions& opt) {
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
