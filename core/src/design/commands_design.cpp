// Design commands: parameters, sketches, features, regeneration. Same layer as the built-ins (F36): the CLI,
// Python and plugins get them for free; the app uses the two-phase design::plan_*/commit API directly so the
// kernel work stays off its UI thread.
#include "opad/commands.hpp"
#include "opad/agent.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/scene.hpp"

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

std::string title_case(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
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
          ops.push_back(design::make_param_op(name, a.at("expr").get<std::string>(), a.value("comment", "")));
        } else {
          json set = json::object();
          if (a.contains("expr")) set["expr"] = a["expr"];
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
        j["constraints"] = k->geometry.value("constraints", json::array()).size();
        if (!k->error.empty()) j["error"] = k->error;
        out.push_back(j);
      } else if (e.op->type == "feature") {
        const Feature* f = s.feature(e.op->id);
        json j = {{"id", f->id}, {"type", "feature"}, {"kind", f->kind}, {"name", f->name}, {"inputs", f->inputs}};
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

  reg("sketch", "Create a sketch on a plane from its geometry (points, entities, constraints; see docs/design.md)",
      {{"doc", "path"}, {"name", "string"}, {"plane", "object - {\"base\":\"xy|xz|yz\"} | {\"face\":ref} | {\"feature\":plane id}"}, {"geometry", "object"}, {"by", "string"}}, true,
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const Scene s = resolve(doc);
        json plane = a.contains("plane") ? parse_if_text(a["plane"]) : json{{"base", "xy"}};
        plane["frame"] = design::resolve_plane(doc, s, plane).to_json();
        const json geometry = design::Sketch::from_json(parse_if_text(a.value("geometry", json::object()))).to_json();
        const std::string name = a.contains("name") ? a["name"].get<std::string>() : design::next_name(s, "Sketch");
        return design::apply_ops(doc, {design::make_sketch_op(name, plane, geometry)}, a.value("by", ""));
      });

  reg("drawing_to_sketch", "Project chosen drawing layers into one editable sketch, in the drawing's own plane and origin (where its import placed it) unless plane names another", {{"layers","array of {id, construction}"},{"plane","xy|xz|yz - optional; default: the drawing's own frame"},{"name","string"},{"tolerance","number, mm"}},true,[](Document* d,const json& a) {
    auto& doc=need(d); const auto scene=resolve(doc); std::vector<design::DrawingLayer> layers;
    for(const auto& layer:a.at("layers")) layers.push_back({layer.at("id").get<std::string>(),layer.value("construction",false)});
    const bool own=!a.contains("plane"); const std::string base=a.value("plane","xy");
    const auto frame=own?design::drawing_frame(scene,layers):design::base_frame(base);
    const auto sketch=design::drawing_sketch(doc,scene,layers,frame,a.value("tolerance",0.01));
    const json plane=own?json{{"frame",frame.to_json()}}:json{{"base",base},{"frame",frame.to_json()}};
    return design::apply_ops(doc,{design::make_sketch_op(a.value("name","Converted drawing"),plane,sketch.to_json())},a.value("by",""));
  });

  reg("sketch_edit", "Replace a sketch's geometry (and optionally its name); features built on it are regenerated",
      {{"doc", "path"}, {"target", "uuid - sketch op id"}, {"geometry", "object"}, {"plane", "object"}, {"name", "string"}, {"by", "string"}}, true, [](Document* d, const json& a) {
        Document& doc = need(d);
        json set = json::object();
        if (a.contains("geometry")) set["geometry"] = design::Sketch::from_json(parse_if_text(a["geometry"])).to_json();
        if (a.contains("name")) set["name"] = a["name"];
        if (a.contains("plane")) {auto plane=parse_if_text(a["plane"]);plane["frame"]=design::resolve_plane(doc,resolve(doc),plane).to_json();set["plane"]=std::move(plane);}
        if (set.empty()) throw Error("sketch_edit: nothing to change");
        return design::apply_ops(doc, {design::make_edit_op(a.at("target").get<std::string>(), set)}, a.value("by", ""));
      });

  reg("feature", "Add a feature (extrude, fillet, shell, ...) to the design history; see feature_kinds for the inputs",
      {{"doc", "path"}, {"kind", "string"}, {"inputs", "object - values are numbers, expressions (\"width/2\"), choices or references"}, {"name", "string"}, {"by", "string"}}, true,
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const std::string kind = a.at("kind").get<std::string>();
        const design::FeatureSpec& spec = spec_of(kind);
        const json inputs = with_defaults(spec, parse_if_text(a.value("inputs", json::object())));
        agent::validate_input(agent::feature_schema(kind),inputs,"inputs");
        const std::string name = a.contains("name") ? a["name"].get<std::string>() : design::next_name(resolve(doc), title_case(spec.label.substr(0, spec.label.find(' '))));
        return design::apply_ops(doc, {design::make_feature_op(kind, name, inputs)}, a.value("by", ""));
      });

  reg("feature_edit", "Change a feature's inputs, name or suppression; later features are regenerated",
      {{"doc", "path"}, {"target", "uuid - feature op id"}, {"inputs", "object - only the inputs that change"}, {"name", "string"}, {"suppressed", "bool"}, {"by", "string"}}, true,
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
        if (set.empty()) throw Error("feature_edit: nothing to change");
        return design::apply_ops(doc, {design::make_edit_op(target, set)}, a.value("by", ""));
      });

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
