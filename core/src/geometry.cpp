#include "opad/geometry.hpp"

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
  TopoDS_Shape s = shape_from_brep(e->brep);
  std::lock_guard<std::mutex> lock(cache.mu);
  cache.shapes[key] = s;
  return s;
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

static TopAbs_ShapeEnum abs_of(Ref::Kind k) {
  switch (k) {
    case Ref::Kind::Face: return TopAbs_FACE;
    case Ref::Kind::Edge: return TopAbs_EDGE;
    case Ref::Kind::Vertex: return TopAbs_VERTEX;
    default: return TopAbs_SHAPE;
  }
}

TopoDS_Shape subshape(const TopoDS_Shape& proto, Ref::Kind kind, int index) {
  if (kind == Ref::Kind::Body) return proto;
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(proto, abs_of(kind), map);
  if (index < 0 || index >= map.Extent())
    throw Error(std::string(Ref::kind_name(kind)) + " index " + std::to_string(index) + " out of range (0.." +
                std::to_string(map.Extent() - 1) + ")");
  return map(index + 1);
}

int subshape_count(const TopoDS_Shape& proto, Ref::Kind kind) {
  if (kind == Ref::Kind::Body) return 1;
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
