// Design commands: parameters, sketches, features, regeneration. Same layer as the built-ins (F36): the CLI,
// Python and plugins get them for free; the app uses the two-phase design::plan_*/commit API directly so the
// kernel work stays off its UI thread.
#include "opad/commands.hpp"
#include "opad/agent.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/design/provenance.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_shapes.hpp"
#include "opad/scene.hpp"

#include <algorithm>
#include <optional>

namespace opad::commands {

namespace {

Document& need(Document* d) {
  if (!d) throw Error("this command needs a document: pass \"doc\"");
  return *d;
}

json parse_if_text(const json& v) {
  if (!v.is_string()) return v;
  json j = json::parse(v.get<std::string>(), nullptr, false);
  if (j.is_discarded()) throw Error("expected JSON, got: " + v.get<std::string>());
  return j;
}

const design::FeatureSpec& spec_of(const std::string& kind) {
  const design::FeatureSpec* s = design::feature_spec(kind);
  if (!s) throw Error("unknown feature kind: " + kind + " (see feature_kinds)");
  return *s;
}

// Inputs as given, with the spec's defaults for whatever was left out.
json with_defaults(const design::FeatureSpec& spec, json inputs) {
  if (!inputs.is_object()) inputs = json::object();
  for (const auto& in : spec.inputs)
    if (!inputs.contains(in.name) && !in.def.is_null()) inputs[in.name] = in.def;
  return inputs;
}

// An expression as text: a plain number (the CLI parses "15" as JSON) is the same expression written out.
std::string expression_text(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number()) return json(v.get<double>()).dump();
  throw Error("expr must be an expression such as \"20 mm\" or a number");
}

// A feature's body_name / color / parent arguments (TODO 10 B14), checked against the scene before anything is
// computed. The CLI hands every value over as text.
json body_style(const Scene& scene, const json& a) {
  json style = json::object();
  if (a.contains("body_name") && !a["body_name"].is_null()) {
    if (!a["body_name"].is_string() || a["body_name"].get<std::string>().empty()) throw Error("body_name must be a non-empty string");
    style["body_name"] = a["body_name"];
  }
  if (a.contains("color") && !a["color"].is_null()) {
    const json c = parse_if_text(a["color"]);
    if (!c.is_array() || c.size() != 3 || !std::all_of(c.begin(), c.end(), [](const json& v) { return v.is_number() && v.get<double>() >= 0 && v.get<double>() <= 1; }))
      throw Error("color must be [r,g,b] with each value in 0..1, for example [0.9, 0.1, 0.1]");
    style["color"] = c;
  }
  if (a.contains("parent")) {
    const json& p = a["parent"];
    const std::string id = p.is_string() && p.get<std::string>() != "null" ? p.get<std::string>() : "";
    if (!p.is_null() && !p.is_string()) throw Error("parent must be a component id, or null for the document root");
    if (!id.empty()) {
      const Node* n = scene.node(id);
      if (!n) throw Error("parent " + id + " does not exist; create it with the component command first");
      if (n->kind != Node::Kind::Component) throw Error("parent " + id + " is a body, not a component");
    }
    style["parent"] = id.empty() ? json(nullptr) : json(id);
  }
  return style;
}

// A sketch's or feature's component argument (TODO 11 UI-33): an existing component, or "" for the document root.
std::string component_arg(const Scene& scene, const json& a) {
  if (!a.contains("component") || a["component"].is_null()) return {};
  if (!a["component"].is_string()) throw Error("component must be a component id, or null for the document root");
  const std::string id = a["component"].get<std::string>();
  if (id.empty() || id == "null") return {};
  const Node* n = scene.node(id);
  if (!n) throw Error("component " + id + " does not exist; create it with the component command first");
  if (n->kind != Node::Kind::Component) throw Error("component " + id + " is a body, not a component");
  return id;
}

// The frame a feature's "plane" input resolves to in `scene` (TODO 10 B3), or null when it takes none or does not use it.
json plane_frame(const Document& doc, const Scene& scene, const design::FeatureSpec& spec, const json& inputs) {
  for (const auto& in : spec.inputs)
    if (in.name == "plane" && in.type == "plane" && design::input_active(in, inputs))
      return design::frame_result(design::resolve_plane(doc, scene, inputs.contains("plane") ? inputs["plane"] : in.def));
  return nullptr;
}

// Body references -> distinct body ids (a component stands for the bodies under it), as the features read them.
json picked_bodies(const Scene& scene, const json& refs) {
  json out = json::array();
  for (const auto& r : refs.is_array() ? refs : json::array()) {
    const std::string id = Ref::from_json(r).body;
    const Node* n = scene.node(id);
    if (!n) continue;
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
      if (std::find(out.begin(), out.end(), json(b)) == out.end()) out.push_back(b);
  }
  return out;
}

}  // namespace

// Called while the registry is being built, so it registers through `add` rather than register_command().
void register_design_commands(const std::function<void(const CommandInfo&, Handler)>& add) {
  auto reg = [&](const char* name, const char* desc, json args, bool mutates, Handler h) { add({name, desc, std::move(args), mutates}, std::move(h)); };

  reg("params", "User parameters with their values", {{"doc", "path"}}, false, [](Document* d, const json&) {
    const Scene s = resolve(need(d));
    json out = json::array();
    for (const auto& p : s.params) {
      json j = {{"id", p.id}, {"name", p.name}, {"expr", p.expr}, {"value", p.value}, {"shown", p.shown}};
      if (!p.comment.empty()) j["comment"] = p.comment;
      if (!p.error.empty()) j["error"] = p.error;
      j["used_by"] = design::param_users(need(d), p.name);
      out.push_back(j);
    }
    return out;
  });

  reg("param", "Create a user parameter, or change it when the name exists (everything using it is regenerated)",
      {{"doc", "path"}, {"name", "string"}, {"expr", "string - e.g. \"20 mm\", \"width / 2\", \"30 deg\""}, {"comment", "string"}, {"rename", "string - new name; every expression using it follows"},
       {"by", "string"}},
      true, [](Document* d, const json& a) {
        Document& doc = need(d);
        const std::string name = a.at("name").get<std::string>();
        const Scene s = resolve(doc);
        const Param* existing = s.param(name);
        std::vector<json> ops;
        if (!existing) {
          ops.push_back(design::make_param_op(name, expression_text(a.at("expr")), a.value("comment", "")));
        } else {
          json set = json::object();
          if (a.contains("expr")) set["expr"] = expression_text(a["expr"]);
          if (a.contains("comment")) set["comment"] = a["comment"];
          if (!set.empty()) ops.push_back(design::make_edit_op(existing->id, set));
          if (a.contains("rename") && a["rename"].get<std::string>() != name) {
            const std::string to = a["rename"].get<std::string>();
            if (s.param(to)) throw Error("a parameter called \"" + to + "\" already exists");
            for (auto& op : design::rename_param_ops(doc, name, to)) ops.push_back(std::move(op));
          }
        }
        if (ops.empty()) throw Error("param: nothing to change");
        return design::apply_ops(doc, std::move(ops), a.value("by", ""));
      });

  reg("param_delete", "Delete a user parameter nothing uses", {{"doc", "path"}, {"name", "string"}, {"by", "string"}}, true, [](Document* d, const json& a) {
    Document& doc = need(d);
    const std::string name = a.at("name").get<std::string>();
    const Scene s = resolve(doc);
    const Param* p = s.param(name);
    if (!p) throw Error("unknown parameter \"" + name + "\"");
    const auto users = design::param_users(doc, name);
    if (!users.empty()) throw Error("\"" + name + "\" is used by " + users.front() + (users.size() > 1 ? " and " + std::to_string(users.size() - 1) + " more" : ""));
    return design::apply_ops(doc, {json{{"op", "delete"}, {"target", p->id}}}, a.value("by", ""));
  });

  reg("feature_kinds", "Every feature kind with the inputs it takes", json::object(), false, [](Document*, const json&) { return design::feature_specs_json(); });

  reg("features", "The design history: sketches and features in order, with their state", {{"doc", "path"}}, false, [](Document* d, const json&) {
    Document& doc = need(d);
    const Scene s = resolve(doc);
    json out = json::array();
    for (const auto& e : effective_ops(doc)) {
      if (e.op->type == "sketch") {
        const SketchItem* k = s.sketch(e.op->id);
        json j = {{"id", e.op->id}, {"type", "sketch"}, {"name", k->name}, {"plane", k->plane}, {"dof", k->dof}, {"visible", k->visible}};
        j["entities"] = k->geometry.value("entities", json::array()).size();
        j["constraints"] = k->geometry.value("constraints", json::array()).size() + k->geometry.value("more_constraints", json::array()).size();
        if (!k->component.empty()) j["component"] = k->component;
        if (!k->error.empty()) j["error"] = k->error;
        out.push_back(j);
      } else if (e.op->type == "feature") {
        const Feature* f = s.feature(e.op->id);
        json j = {{"id", f->id}, {"type", "feature"}, {"kind", f->kind}, {"name", f->name}, {"inputs", f->inputs}};
        if (!f->component.empty()) j["component"] = f->component;
        if (f->suppressed) j["suppressed"] = true;
        if (!f->error.empty()) j["error"] = f->error;
        json bodies = json::array();
        for (const auto& b : f->result.value("bodies", json::array())) bodies.push_back(b.value("id", ""));
        j["bodies"] = bodies;
        if (f->result.contains("removed")) j["removed"] = f->result["removed"];
        out.push_back(j);
      }
    }
    return out;
  });

  reg("sketch", "Create a sketch on a plane from its geometry: points, entities, constraints and high-level shapes (rectangles, rounded rectangles, arcs by three points or a radius, paths with fillets and tangent arcs, slots, offsets, text), which become ordinary curves; the result's id_map lists what each shape made. The agent guide (MCP resource opad://guide/agent) describes the format",
      {{"doc", "path"}, {"name", "string"}, {"plane", "object - {\"base\":\"xy|xz|yz\"} | {\"face\":ref} | {\"feature\":plane id}"}, {"geometry", "object"},
       {"component", "uuid|null - component it is made in"}, {"by", "string"}}, true,
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const Scene s = resolve(doc);
        const std::string component = component_arg(s, a);
        json plane = a.contains("plane") ? parse_if_text(a["plane"]) : json{{"base", "xy"}};
        const Frame frame = design::resolve_plane(doc, s, plane);
        plane["frame"] = frame.to_json();
        json id_map;
        const json geometry = design::expand_sketch_shapes(parse_if_text(a.value("geometry", json::object())), &id_map);
        const std::string name = a.contains("name") ? a["name"].get<std::string>() : design::next_name(s, "Sketch");
        json op = design::make_sketch_op(name, plane, geometry);
        if (!component.empty()) op["component"] = component;
        json out = design::apply_ops(doc, {op}, a.value("by", ""));
        out["sketch_id"] = out["ids"][0];
        out["frame"] = design::frame_result(frame);
        if (!id_map.empty()) out["id_map"] = id_map;
        return out;
      });

  reg("drawing_to_sketch", "Project chosen drawing layers into one editable sketch, in the drawing's own plane and origin (where its import placed it) unless plane names another", {{"layers","array of {id, construction}"},{"plane","xy|xz|yz - optional; default: the drawing's own frame"},{"name","string"},{"tolerance","number, mm"}},true,[](Document* d,const json& a) {
    auto& doc=need(d); const auto scene=resolve(doc); std::vector<design::DrawingLayer> layers;
    for(const auto& layer:a.at("layers")) layers.push_back({layer.at("id").get<std::string>(),layer.value("construction",false)});
    const bool own=!a.contains("plane"); const std::string base=a.value("plane","xy");
    const auto frame=own?design::drawing_frame(scene,layers):design::base_frame(base);
    const auto sketch=design::drawing_sketch(doc,scene,layers,frame,a.value("tolerance",0.01));
    const json plane=own?json{{"frame",frame.to_json()}}:json{{"base",base},{"frame",frame.to_json()}};
    json out=design::apply_ops(doc,{design::make_sketch_op(a.value("name","Converted drawing"),plane,sketch.to_json())},a.value("by",""));
    out["sketch_id"]=out["ids"][0];out["frame"]=design::frame_result(frame);return out;
  });

  reg("sketch_edit", "Replace a sketch's geometry (and optionally its name); features built on it are regenerated",
      {{"doc", "path"}, {"target", "uuid - sketch op id"}, {"geometry", "object"}, {"plane", "object"}, {"name", "string"}, {"by", "string"}}, true, [](Document* d, const json& a) {
        Document& doc = need(d);
        json set = json::object();
        json id_map;
        if (a.contains("geometry")) set["geometry"] = design::expand_sketch_shapes(parse_if_text(a["geometry"]), &id_map);
        if (a.contains("name")) set["name"] = a["name"];
        json frame;
        if (a.contains("plane")) {
          auto plane = parse_if_text(a["plane"]);
          const Scene s = resolve(doc);
          const auto resolved = design::resolve_plane(doc, s, plane);
          plane["frame"] = resolved.to_json();
          frame = design::frame_result(resolved);
          const SketchItem* sketch = s.sketch(a.at("target").get<std::string>());
          set["plane"] = sketch ? design::plane_as_made(*sketch, std::move(plane)) : std::move(plane);  // its component moved since (UI-33)
        }
        if (set.empty()) throw Error("sketch_edit: nothing to change");
        json out = design::apply_ops(doc, {design::make_edit_op(a.at("target").get<std::string>(), set)}, a.value("by", ""));
        out["sketch_id"] = a.at("target");
        if (!frame.is_null()) out["frame"] = frame;
        if (!id_map.empty()) out["id_map"] = id_map;
        return out;
      });

  reg("feature", "Add a feature (extrude, fillet, shell, ...) to the design history; see feature_kinds for the inputs. "
      "The bodies it makes are named after it (numbered when there are several); body_name, color and parent name, colour and place them "
      "in the same step. Result: feature_id, body_ids (the bodies it made or changed), all_body_ids for mirror and patterns (the picked bodies too)",
      {{"doc", "path"}, {"kind", "string"}, {"inputs", "object - values are numbers, expressions (\"width/2\"), choices or references"}, {"name", "string"},
       {"body_name", "string - name for the new bodies (default: the feature's name); several are numbered \"<name> 1\", \"<name> 2\", or \"{n}\" marks where the number goes"},
       {"color", "[r,g,b] - colour of the new bodies, each 0..1"}, {"parent", "uuid|null - component the new bodies go into (null: the document root)"},
       {"component", "uuid|null - component it is made in (its new bodies go there)"},
       {"suppress_if", "string - expression over the parameters; while it is true (nonzero) the feature is suppressed"}, {"by", "string"}}, true,
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const std::string kind = a.at("kind").get<std::string>();
        const design::FeatureSpec& spec = spec_of(kind);
        const json inputs = with_defaults(spec, parse_if_text(a.value("inputs", json::object())));
        agent::validate_input(agent::feature_schema(kind),inputs,"inputs");
        const bool styled = a.contains("body_name") || a.contains("color") || a.contains("parent");
        const bool copies = kind == "mirror" || kind == "pattern_rect" || kind == "pattern_circ";
        const Scene scene_before = resolve(doc);
        const Scene* scene = &scene_before;
        const json frame = plane_frame(doc, *scene, spec, inputs);
        const json style = styled ? body_style(*scene, a) : json::object();
        const std::string component = component_arg(*scene, a);
        const std::string name = a.contains("name") ? a["name"].get<std::string>() : design::next_name(*scene, design::name_prefix(spec));
        json feature_op = design::make_feature_op(kind, name, inputs);
        if (!component.empty()) feature_op["component"] = component;
        if (a.contains("suppress_if") && a["suppress_if"].is_string() && !a["suppress_if"].get<std::string>().empty()) feature_op["suppress_if"] = a["suppress_if"];  // gap log #9
        design::Plan plan = design::plan_ops(doc, {feature_op});
        const std::string op = plan.ops.front()["id"].get<std::string>();
        json bodies = json::array();
        for (const auto& b : plan.ops.front()["result"].value("bodies", json::array())) bodies.push_back(b["id"]);
        json placed = json::array();  // a linked file Move moved as one: its top nodes
        for (const auto& p : plan.ops.front()["result"].value("placements", json::array())) placed.push_back(p["id"]);
        const size_t before = plan.ops.size();
        const size_t made = design::style_new_bodies(plan, op, style);
        json styling = json::array();
        for (size_t i = before; i < plan.ops.size(); ++i) {
          plan.ops[i]["id"] = new_uuid();
          styling.push_back(plan.ops[i]["id"]);
        }
        json out = design::commit(doc, std::move(plan), a.value("by", ""));
        out["feature_id"] = op;
        out["body_ids"] = bodies;
        if (!placed.empty()) out["placed_ids"] = placed;
        if (copies) {
          json all = picked_bodies(*scene, inputs.value("bodies", json::array()));
          for (const auto& b : bodies)
            if (std::find(all.begin(), all.end(), b) == all.end()) all.push_back(b);
          out["all_body_ids"] = all;
        }
        if (!styling.empty()) out["style_ids"] = styling;
        if (!frame.is_null()) out["frame"] = frame;
        if (styled && made == 0) out["warnings"] = json::array({"body_name, color and parent apply to the bodies a feature makes; this one made none (it changed existing bodies), so they were not used"});
        return out;
      });

  reg("feature_edit", "Change a feature's inputs, name or suppression; later features are regenerated",
      {{"doc", "path"}, {"target", "uuid - feature op id"}, {"inputs", "object - only the inputs that change"}, {"name", "string"}, {"suppressed", "bool"},
       {"suppress_if", "string - expression over the parameters, suppressed while true (nonzero); \"\" removes it"}, {"by", "string"}}, true,
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const std::string target = a.at("target").get<std::string>();
        const Scene s = resolve(doc);
        const Feature* f = s.feature(target);
        if (!f) throw Error("not a feature: " + target);
        json set = json::object();
        if (a.contains("inputs")) {
          json inputs = f->inputs;
          const json changes = parse_if_text(a["inputs"]);
          for (const auto& [k, v] : changes.items()) inputs[k] = v;
          agent::validate_input(agent::feature_schema(f->kind),inputs,"inputs");
          set["inputs"] = inputs;
        }
        if (a.contains("name")) set["name"] = a["name"];
        if (a.contains("suppressed")) set["suppressed"] = a["suppressed"];
        if (a.contains("suppress_if")) set["suppress_if"] = a["suppress_if"].get<std::string>().empty() ? json() : a["suppress_if"];
        if (set.empty()) throw Error("feature_edit: nothing to change");
        json out = design::apply_ops(doc, {design::make_edit_op(target, set)}, a.value("by", ""));
        out["feature_id"] = target;
        // The frame its plane resolves to where the feature is in the history (TODO 10 B3).
        if (const design::FeatureSpec* spec = design::feature_spec(f->kind)) {
          const json inputs = set.contains("inputs") ? set["inputs"] : f->inputs;
          for (const auto& in : spec->inputs)
            if (in.name == "plane" && in.type == "plane" && design::input_active(in, inputs)) {
              out["frame"] = plane_frame(doc, resolve(doc, target), *spec, inputs);
              break;
            }
        }
        return out;
      });

  // TODO 11 UI-94: face provenance from the body key chain; one implementation for the CLI, MCP and the app's worker.
  // TODO 11 UI-97: then the groups the geometry shows, on imported bodies too.
  reg("related", "The feature that made each picked face or edge (boss, pocket, hole, fillet, pattern, ...) and every face each such feature made on those bodies, from the design history; "
      "then recognised groups holding the picks (hole, fillet, chamfer, boss, pocket, wall, tangent, loop, similar), also on imported bodies",
      {{"doc", "path"}, {"refs", "array"},
       {"kinds", json{{"type", "array"}, {"items", {{"type", "string"}, {"enum", {"feature", "import", "body", "hole", "fillet", "chamfer", "boss", "pocket", "wall", "tangent", "loop", "similar"}}}}}},
       {"limit", "int - face refs per candidate (500)"}},
      false, [](Document* d, const json& a) { return design::related(need(d), a); });

  reg("regenerate", "Recompute whatever in the design history is out of date (after a merge or a hand edit)", {{"doc", "path"}, {"force", "bool - recompute everything"}, {"by", "string"}}, true,
      [](Document* d, const json& a) {
        Document& doc = need(d);
        return design::commit(doc, design::plan_regenerate(doc, a.value("force", false)), a.value("by", ""));
      });

  reg("component", "Create an empty component", {{"doc", "path"}, {"name", "string"}, {"parent", "uuid"}, {"by", "string"}}, true, [](Document* d, const json& a) {
    Document& doc = need(d);
    json op = {{"op", "import"}, {"source", ""}, {"units", "mm"}};
    const std::string id = new_uuid();
    op["nodes"] = json::array({json{{"type", "component"}, {"id", id}, {"name", a.value("name", "Component")}}});
    if (a.contains("parent") && a["parent"].is_string() && !a["parent"].get<std::string>().empty()) op["parent"] = a["parent"];
    json j;
    j["op"] = doc.append(op, a.value("by", "")).id;
    j["id"] = id;
    j["component_id"] = id;
    j["operation_ids"] = json::array({j["op"]});
    return j;
  });
}

}  // namespace opad::commands
