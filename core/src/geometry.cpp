#include "opad/geometry.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch.hpp"
#include <TopoDS_Compound.hxx>

#include <BRepBndLib.hxx>
#include <OSD_Parallel.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopoDS_Face.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepTools.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_Printer.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_FormatVersion.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_GTrsf.hxx>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace opad {

struct ShapeCache {
  std::mutex mu;
  std::unordered_map<std::string, TopoDS_Shape> shapes;
  std::unordered_map<std::string, Bnd_Box> boxes;
  std::unordered_map<std::string, Bnd_Box> tight;  // exact boxes, by key (TODO 10 B3/B10)
};

std::shared_ptr<ShapeCache> make_shape_cache() { return std::make_shared<ShapeCache>(); }

namespace {
// OCCT prints transfer statistics and warnings to std::cout by default, which would corrupt the JSON
// that opad-cli writes to stdout. Route everything to stderr and keep only alarms unless verbose.
class StderrPrinter : public Message_Printer {
 public:
  DEFINE_STANDARD_RTTI_INLINE(StderrPrinter, Message_Printer)
 protected:
  void send(const TCollection_AsciiString& theString, const Message_Gravity theGravity) const override {
    const char* tag = theGravity >= Message_Fail ? "error" : theGravity >= Message_Alarm ? "alarm" : theGravity >= Message_Warning ? "warning" : "info";
    std::fprintf(stderr, "[occt %s] %s\n", tag, theString.ToCString());
  }
};
}  // namespace

void configure_kernel_logging(bool verbose) {
  if (!verbose) {
    if (const char* e = std::getenv("OPAD_VERBOSE"); e && *e && *e != '0') verbose = true;
  }
  Handle(Message_Messenger) messenger = Message::DefaultMessenger();
  messenger->ChangePrinters().Clear();
  Handle(StderrPrinter) printer = new StderrPrinter();
  printer->SetTraceLevel(verbose ? Message_Info : Message_Alarm);
  messenger->AddPrinter(printer);
}

std::string brep_from_shape(const TopoDS_Shape& s) {
  std::ostringstream ss;
  ss.precision(17);
  // Pinned to format version 1 and without triangulation so that the text (and hence the body key)
  // depends only on the B-rep, never on tessellation state or the OCCT release that wrote it.
  BRepTools::Write(s, ss, Standard_False, Standard_False, TopTools_FormatVersion_VERSION_1);
  std::string text = ss.str();
  std::string out;
  out.reserve(text.size());
  for (char c : text)
    if (c != '\r') out.push_back(c);
  if (out.empty() || out.back() != '\n') out.push_back('\n');
  return out;
}

TopoDS_Shape shape_from_brep(const std::string& brep) {
  std::istringstream ss(brep);
  TopoDS_Shape s;
  BRep_Builder b;
  try {
    BRepTools::Read(s, ss, b);
  } catch (const Standard_Failure& e) {
    throw Error(std::string("cannot read BREP: ") + e.GetMessageString());
  }
  if (s.IsNull()) throw Error("cannot read BREP: empty shape");
  return s;
}

std::string body_key_for(const TopoDS_Shape& s, std::string* brep_out) {
  std::string brep = brep_from_shape(s);
  std::string key = sha256_hex(brep);
  if (brep_out) *brep_out = std::move(brep);
  return key;
}

TopoDS_Shape body_shape(const Document& doc, const std::string& key) {
  auto& cache = *doc.shape_cache;
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    auto it = cache.shapes.find(key);
    if (it != cache.shapes.end()) return it->second;
  }
  const BodyEntry* e = doc.body(key);
  if (!e) throw Error("body entry not found: " + key);
  TopoDS_Shape s = e->indexed.empty() ? shape_from_brep(e->brep) : shape_from_brep(std::string(e->checked_text()));
  std::lock_guard<std::mutex> lock(cache.mu);
  cache.shapes[key] = s;
  return s;
}

void cache_shape(const Document& doc, const std::string& key, const TopoDS_Shape& shape) {
  std::lock_guard<std::mutex> lock(doc.shape_cache->mu);
  doc.shape_cache->shapes[key] = shape;
}

bool mat_is_rigid(const Mat4& m) {
  // Columns of the 3x3 block must be mutually orthogonal and of equal length (uniform scale allowed).
  double len[3];
  for (int c = 0; c < 3; ++c) len[c] = std::sqrt(m.at(0, c) * m.at(0, c) + m.at(1, c) * m.at(1, c) + m.at(2, c) * m.at(2, c));
  if (len[0] < 1e-12) return false;
  for (int c = 0; c < 3; ++c)
    if (std::fabs(len[c] - len[0]) > 1e-6 * len[0]) return false;
  for (int a = 0; a < 3; ++a)
    for (int b = a + 1; b < 3; ++b) {
      double dot = m.at(0, a) * m.at(0, b) + m.at(1, a) * m.at(1, b) + m.at(2, a) * m.at(2, b);
      if (std::fabs(dot) > 1e-6 * len[0] * len[0]) return false;
    }
  return true;
}

Mat4 mat_from_trsf(const gp_Trsf& t) {
  Mat4 m;
  for (int r = 1; r <= 3; ++r)
    for (int c = 1; c <= 4; ++c) m.at(r - 1, c - 1) = t.Value(r, c);
  return m;
}

gp_Trsf trsf_from_mat(const Mat4& m) {
  if (!mat_is_rigid(m)) throw Error("transform is not rigid (shear or non-uniform scale)");
  gp_Trsf t;
  try {
    t.SetValues(m.at(0, 0), m.at(0, 1), m.at(0, 2), m.at(0, 3), m.at(1, 0), m.at(1, 1), m.at(1, 2), m.at(1, 3),
                m.at(2, 0), m.at(2, 1), m.at(2, 2), m.at(2, 3));
  } catch (const Standard_Failure& e) {
    throw Error(std::string("bad transform: ") + e.GetMessageString());
  }
  return t;
}

TopoDS_Shape node_world_shape(const Document& doc, const Scene& scene, const std::string& node_id) {
  if(const auto* sk=scene.sketch(node_id)) {
    TopoDS_Compound shape; BRep_Builder builder; builder.MakeCompound(shape);
    for(const auto& edge:design::sketch_edges(design::Sketch::from_json(sk->geometry),sk->frame,true)) builder.Add(shape,edge);
    return shape;
  }
  const Node* n = scene.node(node_id);
  if (!n || n->kind != Node::Kind::Body) throw Error("not a body node: " + node_id);
  TopoDS_Shape proto = body_shape(doc, n->body_key);
  Mat4 w = scene.world(node_id);
  if (w.is_identity()) return proto;
  if (mat_is_rigid(w)) return proto.Moved(TopLoc_Location(trsf_from_mat(w)));
  gp_GTrsf g;
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 3; ++c) g.SetValue(r, c, w.at(r - 1, c - 1));
    g.SetValue(r, 4, w.at(r - 1, 3));
  }
  return BRepBuilderAPI_GTransform(proto, g, Standard_True).Shape();
}

Bnd_Box body_bbox(ShapeCache& cache, const std::string& key, const TopoDS_Shape& proto) {
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    auto it = cache.boxes.find(key);
    if (it != cache.boxes.end()) return it->second;
  }
  Bnd_Box box;
  if (!proto.IsNull()) BRepBndLib::Add(proto, box, Standard_True);
  std::lock_guard<std::mutex> lock(cache.mu);
  cache.boxes[key] = box;
  return box;
}

Bnd_Box refine_body_bbox(ShapeCache& cache, const std::string& key, const TopoDS_Shape& meshed) {
  Bnd_Box box;
  if (!meshed.IsNull()) BRepBndLib::Add(meshed, box, Standard_True);
  if (box.IsVoid()) return body_bbox(cache, key, meshed);  // nothing meshed: keep what there is
  std::lock_guard<std::mutex> lock(cache.mu);
  cache.boxes[key] = box;
  return box;
}

Bnd_Box body_bbox(const Document& doc, const std::string& key) {
  auto& cache = *doc.shape_cache;
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    auto it = cache.boxes.find(key);
    if (it != cache.boxes.end()) return it->second;
  }
  return body_bbox(cache, key, body_shape(doc, key));
}

void warm_shape_cache(const Document& doc, const std::function<bool(size_t, size_t)>& progress) {
  const auto& bodies = doc.bodies();
  for (size_t i = 0; i < bodies.size(); ++i) {
    if (progress && !progress(i, bodies.size())) return;
    try {
      body_bbox(doc, bodies[i].key);  // parses the BREP via body_shape, then boxes it
    } catch (const Error&) {
    }
  }
}

Bnd_Box tight_bbox(const TopoDS_Shape& shape) {
  Bnd_Box box;
  if (shape.IsNull()) return box;
  BRepBndLib::AddOptimal(shape, box, Standard_False, Standard_False);
  // A mesh (STL, OBJ, 3MF, glTF) has no surfaces or curves to measure: its triangles are the geometry. Without this the
  // Bounding box measure said "bounding box is empty" and Properties showed no size for every mesh body.
  if (box.IsVoid()) BRepBndLib::AddOptimal(shape, box, Standard_True, Standard_False);
  return box;
}

void warm_tight_bboxes(const Document& doc, const std::vector<std::string>& keys, const std::function<bool()>& cancelled) {
  auto& cache = *doc.shape_cache;
  std::vector<std::string> missing;
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    for (const auto& k : keys)
      if (!cache.tight.count(k) && std::find(missing.begin(), missing.end(), k) == missing.end()) missing.push_back(k);
  }
  if (missing.empty()) return;
  // Independent read-only shapes: measured side by side (the Engine's 252 shapes took minutes one after another).
  std::vector<Bnd_Box> boxes(missing.size());
  std::vector<char> done(missing.size(), 0);
  OSD_Parallel::For(0, static_cast<int>(missing.size()), [&](int i) {
    if (cancelled && cancelled()) return;
    try {
      boxes[static_cast<size_t>(i)] = tight_bbox(body_shape(doc, missing[static_cast<size_t>(i)]));
      done[static_cast<size_t>(i)] = 1;
    } catch (const std::exception&) {
    }
  });
  if (cancelled && cancelled()) throw Error("cancelled");
  std::lock_guard<std::mutex> lock(cache.mu);
  for (size_t i = 0; i < missing.size(); ++i)
    if (done[i]) cache.tight[missing[i]] = boxes[i];
}

Bnd_Box node_tight_bbox(const Document& doc, const Scene& scene, const std::string& node_id, bool exact) {
  const Node* n = scene.node(node_id);
  if (!n || n->kind != Node::Kind::Body) throw Error("not a body node: " + node_id);
  const Mat4 w = scene.world(node_id);
  // A turned body is measured where it is (its own box turned would not be tight), unless a union of many only needs
  // the corners of its own tight box turned: never smaller than the body, and one measurement per shape.
  const bool shift_only = w.m[0] == 1 && w.m[5] == 1 && w.m[10] == 1 && w.m[1] == 0 && w.m[2] == 0 && w.m[4] == 0 && w.m[6] == 0 &&
                          w.m[8] == 0 && w.m[9] == 0 && w.m[12] == 0 && w.m[13] == 0 && w.m[14] == 0 && w.m[15] == 1;
  if (!shift_only && exact) return tight_bbox(node_world_shape(doc, scene, node_id));
  auto& cache = *doc.shape_cache;
  Bnd_Box local;
  bool cached = false;
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    if (auto it = cache.tight.find(n->body_key); it != cache.tight.end()) {
      local = it->second;
      cached = true;
    }
  }
  if (!cached) {
    local = tight_bbox(body_shape(doc, n->body_key));
    std::lock_guard<std::mutex> lock(cache.mu);
    cache.tight[n->body_key] = local;
  }
  if (local.IsVoid()) return local;
  double x0, y0, z0, x1, y1, z1;
  local.Get(x0, y0, z0, x1, y1, z1);
  Bnd_Box out;
  if (shift_only) {
    out.Update(x0 + w.m[3], y0 + w.m[7], z0 + w.m[11], x1 + w.m[3], y1 + w.m[7], z1 + w.m[11]);
    return out;
  }
  for (int i = 0; i < 8; ++i) {
    const Vec3 p = w.apply({(i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0});
    out.Update(p[0], p[1], p[2]);
  }
  return out;
}

Bnd_Box node_world_bbox(const Document& doc, const Scene& scene, const std::string& node_id) {
  if(scene.sketch(node_id)) { Bnd_Box b; BRepBndLib::Add(node_world_shape(doc,scene,node_id),b); return b; }
  const Node* n = scene.node(node_id);
  if (!n || n->kind != Node::Kind::Body) throw Error("not a body node: " + node_id);
  Bnd_Box local = body_bbox(doc, n->body_key);
  if (local.IsVoid()) return local;
  Mat4 w = scene.world(node_id);
  if (w.is_identity()) return local;
  double x0, y0, z0, x1, y1, z1;
  local.Get(x0, y0, z0, x1, y1, z1);
  Bnd_Box out;
  for (int i = 0; i < 8; ++i) {
    Vec3 p = w.apply({(i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0});
    out.Update(p[0], p[1], p[2]);
  }
  return out;
}

static TopAbs_ShapeEnum abs_of(Ref::Kind k) {
  switch (k) {
    case Ref::Kind::Face: return TopAbs_FACE;
    case Ref::Kind::Edge: return TopAbs_EDGE;
    case Ref::Kind::Vertex: return TopAbs_VERTEX;
    default: return TopAbs_SHAPE;
  }
}

bool is_mesh_shape(const TopoDS_Shape& shape) {
  TopExp_Explorer faces(shape,TopAbs_FACE);
  return faces.More() && BRep_Tool::Surface(TopoDS::Face(faces.Current())).IsNull();
}

static TopoDS_Shape mesh_subshape(const TopoDS_Shape& shape, Ref::Kind kind, int index) {
  if(index<0) throw Error("mesh sub-shape index out of range");
  for(TopExp_Explorer faces(shape,TopAbs_FACE);faces.More();faces.Next()) {
    TopLoc_Location location;
    const auto mesh=BRep_Tool::Triangulation(TopoDS::Face(faces.Current()),location);
    if(mesh.IsNull()) continue;
    const int count=kind==Ref::Kind::Vertex ? mesh->NbNodes() : mesh->NbTriangles()*(kind==Ref::Kind::Edge?3:1);
    if(index>=count) { index-=count; continue; }
    auto point=[&](int i) { return mesh->Node(i).Transformed(location.Transformation()); };
    if(kind==Ref::Kind::Vertex) return BRepBuilderAPI_MakeVertex(point(index+1)).Vertex();
    int a,b,c; mesh->Triangle(kind==Ref::Kind::Edge?index/3+1:index+1).Get(a,b,c);
    const int v[]={a,b,c};
    if(kind==Ref::Kind::Edge) return BRepBuilderAPI_MakeEdge(point(v[index%3]),point(v[(index+1)%3])).Edge();
    BRepBuilderAPI_MakePolygon wire(point(a),point(b),point(c),true);
    TopoDS_Face face=BRepBuilderAPI_MakeFace(wire.Wire());
    // Preserve a tiny triangulation for shaded hover/selection without meshing.
    Handle(Poly_Triangulation) triangle=new Poly_Triangulation(3,1,false);
    triangle->SetNode(1,point(a)); triangle->SetNode(2,point(b)); triangle->SetNode(3,point(c));
    triangle->SetTriangle(1,Poly_Triangle(1,2,3)); BRep_Builder().UpdateFace(face,triangle);
    return face;
  }
  throw Error("mesh sub-shape index out of range");
}

TopoDS_Shape subshape(const TopoDS_Shape& proto, Ref::Kind kind, int index) {
  if (kind == Ref::Kind::Body) return proto;
  if (kind == Ref::Kind::Center && is_mesh_shape(proto)) {
    for(const auto& c:mesh_circles(proto)) if(c.index==index) return BRepBuilderAPI_MakeVertex(c.circle.Location()).Vertex();
    throw Error("mesh circle center no longer exists");
  }
  if (kind == Ref::Kind::Center) {
    BRepAdaptor_Curve curve(TopoDS::Edge(subshape(proto, Ref::Kind::Edge, index)));
    if (curve.GetType() != GeomAbs_Circle) throw Error("center requires a circular edge");
    return BRepBuilderAPI_MakeVertex(curve.Circle().Location()).Vertex();
  }
  if (is_mesh_shape(proto)) return mesh_subshape(proto,kind,index);
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(proto, abs_of(kind), map);
  if (index < 0 || index >= map.Extent())
    throw Error(std::string(Ref::kind_name(kind)) + " index " + std::to_string(index) + " out of range (0.." +
                std::to_string(map.Extent() - 1) + ")");
  return map(index + 1);
}

int subshape_count(const TopoDS_Shape& proto, Ref::Kind kind) {
  if (kind == Ref::Kind::Body) return 1;
  if(is_mesh_shape(proto)) {
    int count=0;
    for(TopExp_Explorer faces(proto,TopAbs_FACE);faces.More();faces.Next()) {
      TopLoc_Location location; const auto mesh=BRep_Tool::Triangulation(TopoDS::Face(faces.Current()),location);
      if(!mesh.IsNull()) count+=kind==Ref::Kind::Vertex ? mesh->NbNodes() : mesh->NbTriangles()*(kind==Ref::Kind::Edge?3:1);
    }
    return count;
  }
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(proto, abs_of(kind), map);
  return map.Extent();
}

int subshape_index(const TopoDS_Shape& proto, const TopoDS_Shape& sub) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(proto, sub.ShapeType(), map);
  int i = map.FindIndex(sub);
  return i > 0 ? i - 1 : -1;
}

}  // namespace opad
