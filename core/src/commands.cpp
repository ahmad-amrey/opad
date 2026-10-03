#include "opad/commands.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#include "opad/cache.hpp"
#include "opad/diff.hpp"
#include "opad/explode.hpp"
#include "opad/inspect.hpp"
#include "opad/mesh.hpp"
#include "opad/render.hpp"
#include "opad/scene.hpp"
#include "opad/design/feature.hpp"
#include "opad/step_io.hpp"
#include "opad/drawing_io.hpp"

namespace opad::commands {

void register_design_commands(const std::function<void(const CommandInfo&, Handler)>& add);  // design/commands_design.cpp
void register_agent_commands(const std::function<void(const CommandInfo&, Handler)>& add);

namespace {

struct Registry {
  std::vector<CommandInfo> infos;
  std::map<std::string, Handler> handlers;
  std::map<std::string, ExportFn> exporters;
  std::recursive_mutex mu;
};

Registry& raw_registry() {
  static Registry r;
  return r;
}

void register_builtins();

Registry& registry() {
  static std::once_flag once;
  std::call_once(once, register_builtins);
  return raw_registry();
}

std::vector<std::string> str_list(const json& v) {
  std::vector<std::string> out;
  if (v.is_null()) return out;
  if (v.is_string()) {
    std::string s = v.get<std::string>();
    size_t start = 0;
    while (start <= s.size()) {
      size_t comma = s.find(',', start);
      std::string item = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      while (!item.empty() && item.front() == ' ') item.erase(item.begin());
      while (!item.empty() && item.back() == ' ') item.pop_back();
      if (!item.empty()) out.push_back(item);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    return out;
  }
  if (v.is_array())
    for (const auto& e : v) out.push_back(e.get<std::string>());
  return out;
}

Document& need(Document* d) {
  if (!d) throw Error("this command needs a document: pass \"doc\"");
  return *d;
}

// A file OPAD reads but does not write as a document (STEP, STL, DXF, ...): commands see it read-only.
bool is_foreign_path(const std::filesystem::path& p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
  const auto& known = importable_extensions();
  return std::find(known.begin(), known.end(), e) != known.end();
}

}  // namespace

// The screenshot options render and viewport_image share (TODO 10 B9), all off unless asked for.
void apply_picture_options(RenderOptions& o, const json& a) {
  o.edge_lines = a.value("edge_lines", false);
  const std::string shading = a.value("shading", "flat");
  if (shading != "flat" && shading != "smooth") throw Error("shading is flat or smooth");
  o.smooth = shading == "smooth";
  for (const auto& r : a.value("highlight", json::array())) {
    const Ref ref = Ref::from_json(r);
    if (ref.kind != Ref::Kind::Face && ref.kind != Ref::Kind::Edge && ref.kind != Ref::Kind::Center) throw Error("highlight takes faces and edges");
    o.highlight.push_back(ref);
  }
  static const std::set<std::string> presets = {"iso", "top", "bottom", "front", "back", "left", "right", "iso-back"};
  for (const auto& v : str_list(a.value("views", json()))) {
    if (!presets.count(v)) throw Error("views are iso, top, bottom, front, back, left, right or iso-back, not " + v);
    o.views.push_back(v);
  }
  if (o.views.size() > 9) throw Error("at most 9 views in one image");
}

namespace {

RenderOptions render_options(const json& a) {
  RenderOptions o;
  o.width = a.value("width", 1280);
  o.height = a.value("height", 720);
  if (a.contains("size") && a["size"].is_string()) {
    int w = 0, h = 0;
    if (std::sscanf(a["size"].get<std::string>().c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { o.width = w; o.height = h; }
  }
  if (a.contains("view") && a["view"].is_string()) o.camera = Camera::preset(a["view"].get<std::string>());
  if (a.contains("camera") && a["camera"].is_object()) {
    o.camera = Camera::from_json(a["camera"]);
    o.fit = a.value("fit", !(o.camera.absolute || o.camera.scale > 0));
  }
  o.edges = a.value("edges", true);
  o.tolerance = a.value("tolerance", 0.2);
  o.select = str_list(a.value("select", json()));
  o.ignore_visibility = a.value("ignore_visibility", false);
  if (a.contains("background") && a["background"].is_array() && a["background"].size() == 3)
    o.background = {a["background"][0].get<float>(), a["background"][1].get<float>(), a["background"][2].get<float>()};
  o.supersample = a.value("supersample", 2);
  apply_picture_options(o, a);
  return o;
}

json op_with_target(const std::string& type, const json& a) {
  json op;
  op["op"] = type;
  op["target"] = a.at("target");
  return op;
}

// `target`, or a `targets` array (TODO 10 B15). Several targets are several ops of the same kind, so the op format,
// replay and merges stay as they are.
std::vector<json> targets_of(const std::string& command, const json& a) {
  if (!a.contains("targets")) return {a.at("target")};
  if (a.contains("target")) throw Error(command + ": give target or targets, not both");
  const json& t = a["targets"];
  if (!t.is_array() || t.empty()) throw Error(command + ": targets must be a non-empty array of node ids");
  std::vector<json> out;
  for (const auto& id : t) {
    if (!id.is_string()) throw Error(command + ": targets must be node id strings, got " + id.dump());
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

// Appends one op per target: `make(target, index, count)` builds it. The result keeps "id" (the first op) and lists
// every op in "ids" when there are several.
json append_per_target(Document& doc, const std::string& command, const json& a, const std::function<json(const json&, size_t, size_t)>& make) {
  const std::vector<json> targets = targets_of(command, a);
  json j;
  json ids = json::array();
  for (size_t i = 0; i < targets.size(); ++i) {
    json op = {{"op", command}, {"target", targets[i]}};  // the field order of the single-target form
    const json fields = make(targets[i], i, targets.size());
    for (const auto& [k, v] : fields.items()) op[k] = v;
    ids.push_back(doc.append(op, a.value("by", "")).id);
  }
  j["id"] = ids.front();
  if (a.contains("targets")) j["ids"] = ids;
  return j;
}

// TODO 11 UI-37: a locked node, or one under a locked component, is not moved (a transform, a reparent): refused naming
// it before anything is appended.
void refuse_locked(const Document& doc, const std::vector<json>& targets, const char* what) {
  if (!design::has_locks(doc)) return;
  const Scene s = resolve(doc);
  for (const auto& t : targets)
    if (const Node* n = t.is_string() ? s.node(t.get<std::string>()) : nullptr; n && s.effectively_locked(n->id))
      throw Error("\"" + n->name + "\" is locked: unlock it before " + what + " it");
}

void register_builtins() {
  auto& r = raw_registry();
  auto reg = [&](const char* name, const char* desc, json args, bool mutates, Handler h) {
    r.infos.push_back({name, desc, std::move(args), mutates});
    r.handlers[name] = std::move(h);
  };

  reg("version", "OPAD version", json::object(), false, [](Document*, const json&) {
    json j;
    j["version"] = version_string();
    j["format"] = kFormatVersion;
    return j;
  });

  reg("commands", "List every command with its arguments (the MCP adapter maps these 1:1)", json::object(), false,
      [](Document*, const json&) {
        json out = json::array();
        for (const auto& c : list()) out.push_back({{"name", c.name}, {"description", c.description}, {"args", c.args}, {"mutates", c.mutates}});
        return out;
      });

  reg("new", "Create an empty document", {{"doc", "path - .opad file to create"}, {"units", "string - mm (default)"}}, true,
      [](Document*, const json& a) {
        Document d = Document::create(a.value("units", "mm"));
        d.save_as(path_from_utf8(a.at("doc").get<std::string>()));
        json j;
        j["doc"] = d.path.string();
        j["uuid"] = d.header.uuid;
        return j;
      });

  reg("info", "Header, units, op count, bodies, bounding box", {{"doc", "path - .opad or .step"}}, false,
      [](Document* d, const json&) { return document_info(need(d), resolve(need(d))); });

  reg("ops", "The op log, optionally filtered",
      {{"doc", "path"}, {"type", "string|array - op types to include"}, {"since", "uuid - only ops after this op"},
       {"live_only", "bool - drop tombstoned ops"}, {"target", "uuid - only ops touching this node/op"}},
      false, [](Document* d, const json& a) {
        Document& doc = need(d);
        Scene s = resolve(doc);
        std::vector<std::string> types = str_list(a.value("type", json()));
        std::string since = a.value("since", ""), target = a.value("target", "");
        bool live_only = a.value("live_only", false);
        bool started = since.empty();
        json out = json::array();
        for (const auto& o : doc.ops) {
          if (!started) { if (o.id == since) started = true; continue; }
          if (!types.empty() && std::find(types.begin(), types.end(), o.type) == types.end()) continue;
          bool deleted = std::find(s.deleted_ops.begin(), s.deleted_ops.end(), o.id) != s.deleted_ops.end();
          if (live_only && deleted) continue;
          if (!target.empty()) {
            bool hit = o.data.value("target", "") == target;
            if (!hit && o.data.contains("anchor")) hit = o.data["anchor"].dump().find(target) != std::string::npos;
            if (!hit && o.data.contains("refs")) hit = o.data["refs"].dump().find(target) != std::string::npos;
            if (!hit && o.type == "import") hit = o.data.dump().find(target) != std::string::npos;
            if (!hit) continue;
          }
          json j = o.data;
          if (deleted) j["deleted"] = true;
          out.push_back(j);
        }
        return out;
      });

  reg("tree", "Component/body hierarchy", {{"doc", "path"}, {"depth", "int - max depth (default unlimited)"}}, false,
      [](Document* d, const json& a) {
        Scene s = resolve(need(d));
        json j;
        j["roots"] = s.tree_json(a.value("depth", -1));
        json unresolved = json::array();
        for (const auto& u : s.unresolved) unresolved.push_back({{"op", u.op_id}, {"type", u.op_type}, {"reason", u.reason}});
        j["unresolved"] = unresolved;
        return j;
      });

  reg("inspect", "Type, bbox, area/volume, normal, adjacent entities of a reference",
      {{"doc", "path"}, {"ref", "string - uuid | uuid/face/N | uuid/edge/N | uuid/vertex/N | point/x,y,z"}, {"refs", "array - several refs"}},
      false, [](Document* d, const json& a) {
        Document& doc = need(d);
        Scene s = resolve(doc);
        if (a.contains("refs")) {
          json out = json::array();
          for (const auto& r : str_list(a["refs"])) out.push_back(inspect_ref(doc, s, Ref::parse(r)));
          return out;
        }
        if (!a.contains("ref")) throw Error("inspect: pass \"ref\" or \"refs\"");
        return inspect_ref(doc, s, Ref::from_json(a["ref"]));
      });

  reg("properties", "Properties panel data for a node", {{"doc", "path"}, {"node", "uuid"}}, false,
      [](Document* d, const json& a) { return node_properties(need(d), resolve(need(d)), a.at("node").get<std::string>()); });

  reg("annotations", "List open annotations. Review ai_agent notes first as user design requests; inspect their anchors before acting. Drawing coordinates are in plane mm.", {{"doc", "path"}, {"by", "string - filter annotations by author"}, {"style", "ok|warning|issue|note|ai_agent"}, {"id", "string - fetch one annotation including drawing and comments"}, {"offset", "int"}, {"limit", "int"}}, false,
      [](Document* d, const json& a) {
        Scene s = resolve(need(d));
        std::string by = a.value("by", "");
        json ann = json::array(), meas = json::array(), sec = json::array(), views = json::array();
        size_t total=0;
        const size_t offset=std::max(0,a.value("offset",0)), limit=std::clamp(a.value("limit",25),1,100);
        for (const auto& x : s.annotations) {
          if (!by.empty() && x.by != by) continue;
          if (a.contains("style") && x.style!=a.at("style").get<std::string>()) continue;
          if (a.contains("id") && x.id!=a.at("id").get<std::string>()) continue;
          if (total++<offset || ann.size()>=limit) continue;
          ann.push_back({{"id", x.id}, {"anchor", x.anchor.str()}, {"text", x.text}, {"style", x.style}, {"by", x.by}, {"ts", x.ts}, {"unresolved", x.unresolved}, {"comments", x.comments}});
          if (!x.drawing.is_null()) ann.back()["drawing"]=x.drawing;
          if (const auto* node=s.node(x.anchor.body)) ann.back()["target"]={{"id",node->id},{"name",node->name},{"body_key",node->body_key}};
        }
        for (const auto& m : s.measurements) {
          json refs = json::array();
          for (const auto& r : m.refs) refs.push_back(r.str());
          meas.push_back({{"id", m.id}, {"kind", m.kind}, {"refs", refs}, {"result", m.result}, {"by", m.by}, {"ts", m.ts}, {"unresolved", m.unresolved}});
        }
        for (const auto& p : s.sections)
          sec.push_back({{"id", p.id}, {"name", p.name}, {"origin", {p.origin[0], p.origin[1], p.origin[2]}}, {"normal", {p.normal[0], p.normal[1], p.normal[2]}}, {"enabled", p.enabled}});
        for (const auto& v : s.views) {
          views.push_back({{"id", v.id}, {"name", v.name}, {"camera", v.camera}});
          if (!v.explode.is_null()) views.back()["explode"] = v.explode;
        }
        json j;
        j["annotations"] = ann;
        j["total"]=total;
        j["next_offset"]=offset+ann.size()<total ? json(offset+ann.size()) : json(nullptr);
        j["measurements"] = meas;
        j["sections"] = sec;
        j["views"] = views;
        return j;
      });

  reg("measure", "Distance, angle, radius or bbox between references; optionally pinned as a measurement op. A read unless pinned; queries measures several at once",
      {{"doc", "path"}, {"kind", "distance|angle|radius|bbox"}, {"refs", "array - references"},
       {"queries", "array - several measurements [{kind, refs}], answered in order as results (a failed one carries error)"},
       {"pin", "bool - append a measurement op (each, with queries)"}, {"explode", "uuid|object - measure in an exploded view: a view op id or an explode spec"}, {"by", "string"}},
      true, [](Document* d, const json& a) {
        Document& doc = need(d);
        Scene s = resolve(doc);  // once for every query (gap log #4)
        if (a.contains("explode")) {
          if (a.value("pin", false)) throw Error("measure: pinned measurements use the assembled model; pin without explode");
          s = exploded_scene(doc, s, a["explode"]);
        }
        auto one = [&](const std::string& kind, const json& refArgs) {
          std::vector<Ref> refs;
          for (const auto& r : str_list(refArgs)) refs.push_back(Ref::parse(r));
          json res;
          if (kind == "distance") {
            if (refs.size() != 2) throw Error("distance needs exactly two refs");
            res = measure_distance(doc, s, refs[0], refs[1]);
          } else if (kind == "angle") {
            if (refs.size() != 2) throw Error("angle needs exactly two refs");
            res = measure_angle(doc, s, refs[0], refs[1]);
          } else if (kind == "radius" || kind == "diameter") {
            if (refs.size() != 1) throw Error("radius needs exactly one ref");
            res = measure_radius(doc, s, refs[0]);
          } else if (kind == "bbox") {
            res = measure_bbox(doc, s, refs);
          } else {
            throw Error("unknown measurement kind: " + kind);
          }
          if (a.value("pin", false)) {
            json op;
            op["op"] = "measurement";
            op["kind"] = kind;
            json rj = json::array();
            for (const auto& r : refs) rj.push_back(r.to_json());
            op["refs"] = rj;
            op["result"] = res;
            res["pinned_op"] = doc.append(op, a.value("by", "")).id;
          }
          return res;
        };
        if (!a.contains("queries")) {
          if (!a.contains("refs")) throw Error("measure: pass refs (with kind) or queries");
          return one(a.value("kind", "distance"), a["refs"]);
        }
        json results = json::array();
        for (const auto& q : a["queries"]) {
          const std::string kind = q.value("kind", "distance");
          try {
            results.push_back(one(kind, q.value("refs", json())));
          } catch (const Standard_Failure& e) {
            results.push_back({{"kind", kind}, {"refs", q.value("refs", json())}, {"error", std::string("the modelling kernel failed: ") + e.GetMessageString()}});
          } catch (const std::exception& e) {
            results.push_back({{"kind", kind}, {"refs", q.value("refs", json())}, {"error", e.what()}});
          }
        }
        return json{{"results", results}};
      });

  reg("append", "Validate and append one op or a list of ops", {{"doc", "path"}, {"op", "object - the op"}, {"ops", "array - several ops"}, {"by", "string - author"}},
      true, [](Document* d, const json& a) {
        Document& doc = need(d);
        json ids = json::array();
        if (a.contains("ops")) for (const auto& o : a["ops"]) ids.push_back(doc.append(o, a.value("by", "")).id);
        if (a.contains("op")) ids.push_back(doc.append(a["op"], a.value("by", "")).id);
        if (ids.empty()) throw Error("append: pass \"op\" or \"ops\"");
        json j;
        j["appended"] = ids;
        return j;
      });

  reg("import", "Import STEP, IGES, BREP, STL, 3MF, OBJ, PLY, glTF, VRML, DXF, DWG (converter) or SVG into the document",
      {{"doc", "path"}, {"file", "path - .step/.iges/.brep/.stl/.3mf/.obj/.ply/.gltf/.glb/.wrl/.dxf/.dwg/.svg"}, {"by", "string"}, {"parent", "uuid - component to import under"}, {"heal", "bool - default true"},
       {"placement", "[16] - drawings: where the drawing's XY plane and origin go (row-major 4x4, mm)"}, {"plane", "object - drawings: place on this plane instead, {\"base\":\"xz\"} or {\"face\":ref}, its origin at the plane's"},
       {"center", "bool - drawings: centre the drawing on its origin (default false)"}},
      true, [](Document* d, const json& a) {
        ImportOptions o;
        o.author = a.value("by", "");
        o.parent = a.value("parent", "");
        o.heal = a.value("heal", true);
        if (a.contains("placement")) o.placement = Mat4::from_json(a["placement"]);
        if (a.contains("plane")) {  // resolved now, stored as the placement: replay never needs the plane again
          const Frame f = design::resolve_plane(need(d), resolve(need(d)), a["plane"]);
          const Vec3 n = f.normal();
          Mat4 m;
          for (int r = 0; r < 3; ++r) { m.at(r, 0) = f.x[r]; m.at(r, 1) = f.y[r]; m.at(r, 2) = n[r]; m.at(r, 3) = f.origin[r]; }
          o.placement = m * o.placement;
        }
        o.center_drawing = a.value("center", false);
        return import_file(need(d), path_from_utf8(a.at("file").get<std::string>()), o).to_json();
      });

  reg("import_brep", "Import a shape given as OCCT ASCII BREP text (build123d/CadQuery/OCP bridge)",
      {{"doc", "path"}, {"brep", "string - BREP text"}, {"file", "path - .brep file (alternative to brep)"}, {"name", "string"}, {"by", "string"}, {"parent", "uuid"}},
      true, [](Document* d, const json& a) {
        ImportOptions o;
        o.author = a.value("by", "");
        o.parent = a.value("parent", "");
        std::string text = a.value("brep", "");
        if (text.empty() && a.contains("file")) text = read_text_file(path_from_utf8(a["file"].get<std::string>()));
        if (text.empty()) throw Error("import_brep: pass \"brep\" text or \"file\"");
        return import_brep(need(d), text, a.value("name", "Body"), o).to_json();
      });

  reg("export", "Export selected objects (or everything) to step|obj|stl|glb|dxf|svg|dwg or a plugin format",
      {{"doc", "path"}, {"format", "step|obj|stl|glb|dxf|svg|dwg|..."}, {"out", "path"}, {"select", "array|csv - node uuids"}, {"schema", "AP214|AP242"},
       {"tolerance", "number - mesh deflection mm"}, {"ascii", "bool - STL text"}, {"per_body", "bool - STL one file per body"}, {"mtl", "bool - OBJ materials"}},
      false, [](Document* d, const json& a) {
        Document& doc = need(d);
        std::string fmt = a.value("format", "step");
        if (has_exporter(fmt)) return run_exporter(fmt, doc, a);
        ExportOptions o;
        o.format = fmt;
        o.select = str_list(a.value("select", json()));
        o.step_schema = a.value("schema", "AP214");
        o.tolerance = a.value("tolerance", 0.1);
        o.ascii = a.value("ascii", false);
        o.per_body = a.value("per_body", false);
        o.mtl = a.value("mtl", true);
        std::string out = a.value("out", "");
        if (out.empty()) throw Error("export: \"out\" path required");
        if (fmt == "svg" || fmt == "dxf" || fmt == "dwg") return export_drawing(doc, resolve(doc), path_from_utf8(out), o).to_json();
        return export_selection(doc, resolve(doc), path_from_utf8(out), o).to_json();
      });

  reg("render", "Headless screenshot (PNG). views puts several fitted views in one labelled grid; edge_lines draws the model's edges; highlight tints faces and edges; shading smooth uses vertex normals",
      {{"doc", "path"}, {"out", "path - .png"}, {"view", "iso|top|bottom|front|back|left|right"}, {"camera", "object - {eye,target,up,projection,scale}"},
       {"width", "int"}, {"height", "int"}, {"select", "array|csv - node uuids"}, {"edges", "bool - silhouette outlines (default true)"}, {"background", "[r,g,b] 0..1"}, {"tolerance", "number"},
       {"views", "array|csv - e.g. iso,front,top,right: one labelled grid"}, {"edge_lines", "bool - the model's edges as lines"}, {"highlight", "array - face/edge references to tint"},
       {"shading", "flat|smooth"}, {"explode", "uuid|object - an exploded view: a view op id or an explode spec"}},
      false, [](Document* d, const json& a) {
        Document& doc = need(d);
        RenderOptions o = render_options(a);
        Image img = render_scene(doc, a.contains("explode") ? exploded_scene(doc, resolve(doc), a["explode"]) : resolve(doc), o);
        std::string out = a.value("out", "");
        if (out.empty()) throw Error("render: \"out\" path required");
        write_png(path_from_utf8(out), img);
        json j;
        j["out"] = out;
        j["width"] = img.width;
        j["height"] = img.height;
        return j;
      });

  reg("diff", "Added/removed/changed ops between two documents, optionally with a geometric diff image",
      {{"a", "path"}, {"b", "path"}, {"image", "path - optional .png"}, {"view", "string"}, {"width", "int"}, {"height", "int"}}, false,
      [](Document*, const json& a) {
        Document da = Document::load(path_from_utf8(a.at("a").get<std::string>()));
        Document db = Document::load(path_from_utf8(a.at("b").get<std::string>()));
        json j = diff_documents(da, db);
        if (a.contains("image") && a["image"].is_string()) {
          RenderOptions o = render_options(a);
          write_png(a["image"].get<std::string>(), render_diff(da, db, o));
          j["image"] = a["image"];
        }
        return j;
      });

  reg("gc", "Remove body entries no live op references", {{"doc", "path"}}, true, [](Document* d, const json&) {
    json j;
    j["removed"] = need(d).gc();
    return j;
  });

  reg("mesh", "Tessellated geometry of bodies in world coordinates (for plugins and tools)",
      {{"doc", "path"}, {"select", "array|csv"}, {"tolerance", "number"}}, false, [](Document* d, const json& a) {
        Document& doc = need(d);
        Scene s = resolve(doc);
        double tol = a.value("tolerance", 0.1);
        json out = json::array();
        for (const auto& id : select_bodies(s, str_list(a.value("select", json())))) {
          const Node* n = s.node(id);
          if (n->body_missing) continue;
          Mesh m = tessellate_body(doc, n->body_key, tol);
          Mat4 w = s.world(id);
          json pos = json::array(), nrm = json::array();
          for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
            Vec3 p = w.apply({m.positions[i], m.positions[i + 1], m.positions[i + 2]});
            Vec3 dn = w.apply_dir({m.normals[i], m.normals[i + 1], m.normals[i + 2]});
            pos.push_back(p[0]); pos.push_back(p[1]); pos.push_back(p[2]);
            nrm.push_back(dn[0]); nrm.push_back(dn[1]); nrm.push_back(dn[2]);
          }
          json b;
          b["id"] = id;
          b["name"] = n->name;
          b["color"] = {n->color[0], n->color[1], n->color[2]};
          b["positions"] = pos;
          b["normals"] = nrm;
          b["indices"] = m.indices;
          out.push_back(b);
        }
        return out;
      });

  reg("annotate", "Add an anchored note or hand drawing. ai_agent notes are requests for the agent to review first.",
      {{"doc", "path"}, {"anchor", "string - reference"}, {"text", "string"}, {"style", "ok|warning|issue|note|ai_agent - default note"}, {"drawing", "object - plane and strokes"}, {"reply_to", "uuid"}, {"by", "string"}}, true, [](Document* d, const json& a) {
        json op;
        op["op"] = "annotation";
        op["anchor"] = Ref::from_json(a.at("anchor")).to_json();
        op["text"] = a.at("text");
        if (a.contains("style")) op["style"] = a["style"];
        if (a.contains("drawing")) op["drawing"] = a["drawing"];
        if (a.contains("reply_to")) {
          const auto* parent = need(d).find_op(a.at("reply_to").get<std::string>());
          if (!parent || parent->type != "annotation" || parent->data.contains("reply_to")) throw Error("comment parent must be a top-level annotation");
          op["reply_to"] = parent->id;
        }
        json j;
        j["id"] = need(d).append(op, a.value("by", "")).id;
        return j;
      });

  reg("delete_annotation", "Delete a note or hand drawing from the review UI and agent queue. Undo can restore it; append-only history remains.",
      {{"doc","path"},{"target","uuid - annotation op id"},{"by","string"}}, true, [](Document* d,const json& a) {
        auto& doc=need(d);const auto id=a.at("target").get<std::string>();const auto* target=doc.find_op(id);
        if(!target || target->type!="annotation") throw Error("delete_annotation: target must be an annotation");
        const auto& op=doc.append({{"op","delete"},{"target",id},{"reason","annotation_deleted"}},a.value("by",""));
        return json{{"id",op.id}};
      });

  reg("delete", "Tombstone an earlier op (annotation resolved, rename undone, import removed...)",
      {{"doc", "path"}, {"target", "uuid - op id"}, {"by", "string"}}, true, [](Document* d, const json& a) {
        // Through the design engine: tombstoning (or restoring) a sketch or feature changes what the later
        // features produce, and that is recomputed in the same step.
        json op = op_with_target("delete", a);
        op["id"] = new_uuid();
        json j = design::apply_ops(need(d), {op}, a.value("by", ""));
        j["id"] = op["id"];
        return j;
      });

  reg("rename", "Rename a node, or several: with targets the name is numbered (\"Board screw {n}\" puts the number where {n} is; without {n} it is appended)",
      {{"doc", "path"}, {"target", "uuid"}, {"targets", "array of uuids - instead of target"}, {"name", "string"}}, true, [](Document* d, const json& a) {
    const std::string name = a.at("name").get<std::string>();
    return append_per_target(need(d), "rename", a, [&](const json&, size_t i, size_t n) {
      return json{{"name", a.contains("targets") ? design::numbered_name(name, i + 1, n) : name}};
    });
  });

  reg("appearance", "Set colour/opacity/visibility/lock of a node, or of several (targets)",
      {{"doc", "path"}, {"target", "uuid"}, {"targets", "array of uuids - instead of target"}, {"color", "[r,g,b]"}, {"opacity", "number"}, {"visible", "bool"}, {"locked", "bool"}}, true,
      [](Document* d, const json& a) {
        return append_per_target(need(d), "appearance", a, [&](const json&, size_t, size_t) {
          json op = json::object();
          for (const char* k : {"color", "opacity", "visible", "locked"}) if (a.contains(k)) op[k] = a[k];
          return op;
        });
      });

  reg("transform", "Set the local placement of a node", {{"doc", "path"}, {"target", "uuid"}, {"matrix", "[16] row-major"}}, true,
      [](Document* d, const json& a) {
        json op = op_with_target("transform", a);
        op["matrix"] = Mat4::from_json(a.at("matrix")).to_json();
        refuse_locked(need(d), {op["target"]}, "moving");
        json j;
        j["id"] = need(d).append(op, a.value("by", "")).id;
        return j;
      });

  reg("reparent", "Move a node, or several (targets, kept in that order), under another component (null = root)",
      {{"doc", "path"}, {"target", "uuid"}, {"targets", "array of uuids - instead of target"}, {"parent", "uuid|null"}, {"index", "int"},
       {"keep_place", "bool - stay put in the world (adds transforms)"}}, true,
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const std::vector<json> targets = targets_of("reparent", a);
        refuse_locked(doc, targets, "moving");
        const json parent = a.contains("parent") ? a["parent"] : json(nullptr);
        std::vector<std::pair<std::string, Mat4>> kept;  // node -> its local under the new parent, from the scene before
        if (a.value("keep_place", false)) {
          const Scene s = resolve(doc);
          const std::string into = parent.is_string() ? parent.get<std::string>() : std::string();
          const Node* p = into.empty() ? nullptr : s.node(into);
          if (into.empty() || (p && p->kind == Node::Kind::Component)) {
            const std::vector<std::string> above = into.empty() ? std::vector<std::string>() : s.path_to(into);
            const Mat4 back = p ? s.world(into).inverse() : Mat4{};
            for (const auto& t : targets)
              if (const Node* n = t.is_string() ? s.node(t.get<std::string>()) : nullptr;
                  n && std::find(above.begin(), above.end(), n->id) == above.end()) {  // a cycle is not replayed: nothing moves
                const Mat4 local = back * s.world(n->id);
                if (!(local * n->local.inverse()).is_identity(1e-9)) kept.emplace_back(n->id, local);
              }
          }
        }
        json j = append_per_target(doc, "reparent", a, [&](const json&, size_t i, size_t) {
          json op = {{"parent", parent}};
          if (a.contains("index")) op["index"] = a["index"].get<int>() < 0 ? a["index"].get<int>() : a["index"].get<int>() + static_cast<int>(i);
          return op;
        });
        for (const auto& [id, local] : kept) doc.append({{"op", "transform"}, {"target", id}, {"matrix", local.to_json()}}, a.value("by", ""));
        if (!kept.empty()) j["transformed"] = kept.size();
        return j;
      });

  reg("section", "Add a named section plane", {{"doc", "path"}, {"name", "string"}, {"origin", "[x,y,z]"}, {"normal", "[x,y,z]"}}, true,
      [](Document* d, const json& a) {
        json op;
        op["op"] = "section";
        op["name"] = a.at("name");
        op["origin"] = a.at("origin");
        op["normal"] = a.at("normal");
        json j;
        j["id"] = need(d).append(op, a.value("by", "")).id;
        return j;
      });

  reg("view", "Add a named camera bookmark", {{"doc", "path"}, {"name", "string"}, {"camera", "object"}, {"explode", "object - an exploded view (see the explode command)"}}, true,
      [](Document* d, const json& a) {
    json op;
    op["op"] = "view";
    op["name"] = a.at("name");
    op["camera"] = a.contains("camera") ? a["camera"] : Camera::preset(a.value("preset", "iso")).to_json();
    if (a.contains("explode")) op["explode"] = ExplodeSpec::from_json(a["explode"]).to_json();
    json j;
    j["id"] = need(d).append(op, a.value("by", "")).id;
    return j;
  });

  reg("explode", "Exploded view: what moves together (units, by level) and where, at t. view: start from a view's explode; name: save as a new view; update: save into view",
      {{"doc", "path"}, {"view", "uuid - a view op"}, {"root", "uuid - component (default all)"}, {"levels", "int - split depth: 1 = the root's children whole, 0 = all"},
       {"mode", "radial|axis|stack"}, {"axis", "[x,y,z] - for axis and stack (default +Z)"}, {"spacing", "number - distance factor"},
       {"keep", "array|csv - components moving as one unit"}, {"split", "array|csv - components whose parts split beyond levels"},
       {"groups", "array - node id lists, each moving as one unit"}, {"offsets", "object - manual moves {unit id: [x,y,z]}"},
       {"attach_small", "bool - small parts ride on what they touch"}, {"fasteners", "bool - radial: screws, pins and bolts leave along their axis"},
       {"small_ratio", "number - small: diagonal share of the parent (0.05)"},
       {"small_size", "number - small: diagonal in mm"}, {"stages", "levels|together|units"}, {"t", "number - 0 assembled .. 1 exploded"},
       {"name", "string - save as a new view"}, {"camera", "object - the new view's camera"}, {"update", "bool - save into view"}, {"by", "string"}},
      true, [](Document* d, const json& a) {
        Document& doc = need(d);
        const Scene s = resolve(doc);
        const std::string view = a.value("view", "");
        json spec_json = view.empty() ? ExplodeSpec{}.to_json() : view_explode(s, view).to_json();
        for (const char* k : {"root", "levels", "mode", "axis", "spacing", "groups", "offsets", "attach_small", "fasteners", "small_ratio", "small_size", "stages", "t"})
          if (a.contains(k)) spec_json[k] = a[k];
        for (const char* k : {"keep", "split"})
          if (a.contains(k)) spec_json[k] = str_list(a[k]);
        const ExplodeSpec spec = ExplodeSpec::from_json(spec_json);
        if (const Node* r = s.node(spec.root); !spec.root.empty() && (!r || r->kind != Node::Kind::Component))
          throw Error("explode: root " + spec.root + " is not a component of the document");
        if (a.contains("name") && a.value("update", false)) throw Error("explode: name saves a new view, update saves into view: give one");
        json warnings = json::array();
        auto known = [&](const std::string& id, const std::string& what) {
          if (!s.node(id)) warnings.push_back(what + " " + id + " is not in the document");
        };
        for (const auto& id : spec.keep) known(id, "keep");
        for (const auto& id : spec.split) known(id, "split");
        for (const auto& g : spec.groups)
          for (const auto& id : g) known(id, "group member");
        const std::vector<ExplodeUnit> units = explode_units(doc, s, spec);
        const std::vector<Vec3> moves = explode_unit_offsets(units, spec, spec.t);
        json list = json::array(), offsets = json::object();
        int stages = 0;
        for (size_t i = 0; i < units.size(); ++i) {
          const ExplodeUnit& u = units[i];
          stages = std::max(stages, u.level);
          list.push_back({{"id", u.id}, {"name", u.name}, {"level", u.level}, {"parent", u.parent < 0 ? json(nullptr) : json(units[static_cast<size_t>(u.parent)].id)},
                          {"bodies", u.bodies}, {"centre", u.centre}, {"dir", u.dir}, {"distance", u.distance}, {"t0", u.t0}, {"t1", u.t1}, {"offset", moves[i]}});
          if (moves[i] != Vec3{0, 0, 0})
            for (const auto& b : u.bodies) offsets[b] = moves[i];
        }
        for (const auto& [id, v] : spec.offsets)
          if (std::none_of(units.begin(), units.end(), [&](const ExplodeUnit& u) { return u.id == id; })) warnings.push_back("offset " + id + " moves no unit");
        const std::string root = explode_root(s, spec);
        json j;
        j["root"] = root.empty() ? json(nullptr) : json(root);
        j["depth"] = explode_depth(s, spec);
        j["stages"] = stages;
        j["explode"] = spec.to_json();
        j["units"] = list;
        j["offsets"] = offsets;
        if (!warnings.empty()) j["warnings"] = warnings;
        if (a.contains("name")) {
          const json op = {{"op", "view"}, {"name", a["name"]}, {"camera", a.contains("camera") ? a["camera"] : Camera::preset("iso").to_json()}, {"explode", spec.to_json()}};
          j["id"] = doc.append(op, a.value("by", "")).id;
        } else if (a.value("update", false)) {
          if (view.empty()) throw Error("explode: update saves into view: pass view");
          j["id"] = doc.append({{"op", "edit"}, {"target", view}, {"set", {{"explode", spec.to_json()}}}}, a.value("by", "")).id;
        }
        return j;
      });

  reg("cache", "Inspect or clear the user cache", {{"action", "info|clear"}}, false, [](Document*, const json& a) {
    json j;
    if (a.value("action", "info") == "clear") cache_clear();
    j["dir"] = cache_dir().string();
    j["bytes"] = cache_size_bytes();
    return j;
  });

  // F25: the running app publishes its selection to <cache>/selection.json; agents read it here.
  reg("selection", "Current GUI selection (uuids + descriptors) as published by the running app", json::object(), false,
      [](Document*, const json&) {
        json j;
        std::filesystem::path p = cache_dir() / "selection.json";
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
          j["available"] = false;
          j["selection"] = json::array();
          return j;
        }
        j = json::parse(read_text_file(p), nullptr, false);
        if (j.is_discarded()) j = json{{"available", false}, {"selection", json::array()}};
        else j["available"] = true;
        j["file"] = p.string();
        return j;
      });

  register_design_commands([&](const CommandInfo& info, Handler h) {
    r.infos.push_back(info);
    r.handlers[info.name] = std::move(h);
  });
  register_agent_commands([&](const CommandInfo& info,Handler h){r.infos.push_back(info);r.handlers[info.name]=std::move(h);});
}

}  // namespace

void register_command(const CommandInfo& info, Handler h) {
  auto& r = registry();
  std::lock_guard<std::recursive_mutex> lock(r.mu);
  auto it = std::find_if(r.infos.begin(), r.infos.end(), [&](const CommandInfo& c) { return c.name == info.name; });
  if (it != r.infos.end()) *it = info;
  else r.infos.push_back(info);
  r.handlers[info.name] = std::move(h);
}

std::vector<CommandInfo> list() {
  auto& r = registry();
  std::lock_guard<std::recursive_mutex> lock(r.mu);
  return r.infos;
}

bool exists(const std::string& name) {
  auto& r = registry();
  std::lock_guard<std::recursive_mutex> lock(r.mu);
  return r.handlers.count(name) > 0;
}

json run(const std::string& name, const json& args, Document* live) {
  auto& r = registry();
  Handler h;
  CommandInfo info;
  {
    std::lock_guard<std::recursive_mutex> lock(r.mu);
    auto it = r.handlers.find(name);
    if (it == r.handlers.end()) throw Error("unknown command: " + name);
    h = it->second;
    info = *std::find_if(r.infos.begin(), r.infos.end(), [&](const CommandInfo& c) { return c.name == name; });
  }
  Document loaded;
  Document* doc = live;
  const auto identify = [&](const Document* d) {  // scripted builds: ids from this command (gap log #15)
    if (!deterministic_ids()) return;
    json key = args;
    key.erase("doc");
    // Not the path: the same script writes the same document wherever it writes it (a new document's id comes from
    // the seed alone; use a seed per document).
    set_id_context(name + "|" + key.dump() + "|" + (d ? d->header.uuid + ":" + std::to_string(d->ops.size()) : std::string()));
  };
  bool save_after = false, transient = false;
  if (!doc && name != "new" && args.contains("doc") && args["doc"].is_string()) {
    std::filesystem::path p = path_from_utf8(args["doc"].get<std::string>());
    if (is_foreign_path(p)) {
      loaded = Document::create();
      import_file(loaded, p);
      loaded.dirty = false;
      transient = true;
    } else {
      loaded = Document::load(p);
      save_after = info.mutates && args.value("save", true);
    }
    doc = &loaded;
  }
  identify(doc);
  json out;
  try {
    out = h(doc, args);
  } catch (const Standard_Failure& e) {
    // A kernel exception is not a std::exception: the CLI died on one ("terminate called", gap log #4).
    throw Error(std::string("the modelling kernel failed: ") + e.GetMessageString());
  }
  if (save_after && doc->dirty) doc->save();
  if (transient && info.mutates && out.is_object()) out["transient"] = true;
  return out;
}

void register_exporter(const std::string& format, ExportFn fn) {
  auto& r = registry();
  std::lock_guard<std::recursive_mutex> lock(r.mu);
  r.exporters[format] = std::move(fn);
}

bool has_exporter(const std::string& format) {
  auto& r = registry();
  std::lock_guard<std::recursive_mutex> lock(r.mu);
  return r.exporters.count(format) > 0;
}

json run_exporter(const std::string& format, const Document& doc, const json& args) {
  ExportFn fn;
  {
    auto& r = registry();
    std::lock_guard<std::recursive_mutex> lock(r.mu);
    auto it = r.exporters.find(format);
    if (it == r.exporters.end()) throw Error("no exporter for format: " + format);
    fn = it->second;
  }
  return fn(doc, args);
}

std::vector<std::string> exporter_formats() {
  std::vector<std::string> out = {"step", "obj", "stl", "glb", "dxf", "svg", "dwg"};
  auto& r = registry();
  std::lock_guard<std::recursive_mutex> lock(r.mu);
  for (const auto& [k, v] : r.exporters) out.push_back(k);
  return out;
}

}  // namespace opad::commands
