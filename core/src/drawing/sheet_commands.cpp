// Drawing commands (TODO 11 UI-76): sheet, sheet_view, sheet_item, sheet_edit, sheet_info and part_properties, for the
// CLI, MCP and Python through the command layer. Every write is one op, checked as replay will take it before it is
// appended, so a wrong pick or an unknown orientation never reaches the document.
#include <algorithm>
#include <functional>
#include <set>

#include "opad/commands.hpp"
#include "opad/design/feature.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/materials.hpp"
#include "opad/render.hpp"
#include "opad/scene.hpp"

namespace opad::commands {
namespace {

Document& need_doc(Document* d) {
  if (!d) throw Error("this command needs a document: pass \"doc\"");
  return *d;
}

std::vector<std::string> strings(const json& v) {
  std::vector<std::string> out;
  if (v.is_array()) {
    for (const auto& e : v) out.push_back(e.get<std::string>());
  } else if (v.is_string()) {
    const std::string s = v.get<std::string>();
    for (size_t start = 0; start <= s.size();) {
      const size_t comma = std::min(s.find(',', start), s.size());
      std::string item = s.substr(start, comma - start);
      item.erase(0, item.find_first_not_of(' '));
      item.erase(item.find_last_not_of(' ') + 1);
      if (!item.empty()) out.push_back(item);
      start = comma + 1;
    }
  }
  return out;
}

const Sheet& need_sheet(const Scene& scene, const std::string& id) {
  const Sheet* sheet = scene.sheet(id);
  if (!sheet) throw Error("sheet " + id + " does not exist (sheet_info lists the sheets)");
  return *sheet;
}

const drawing::ViewFrame& frame_of(const std::vector<drawing::ViewFrame>& frames, const std::string& id) {
  for (const auto& f : frames)
    if (f.id == id) return f;
  throw Error("view " + id + " is not on its sheet");
}

// A reference as a sheet item keeps it: with the hint that lets it follow a topology change, and its aspect. A centre
// reference is its circle's edge with the aspect "center".
json reference(const Document& doc, const Scene& scene, const json& r, std::string aspect) {
  Ref ref = Ref::from_json(r);
  if (aspect.empty() && r.is_object()) aspect = r.value("aspect", "");
  if (ref.kind == Ref::Kind::Center) {
    ref.kind = Ref::Kind::Edge;
    if (aspect.empty()) aspect = "center";
  }
  if (ref.kind != Ref::Kind::Point && !scene.node(ref.body)) throw Error("reference body " + ref.body + " does not exist");
  json j = design::make_ref(doc, scene, ref);
  if (!aspect.empty()) {
    static const std::set<std::string> aspects = {"start", "end", "mid", "center"};
    if (!aspects.count(aspect)) throw Error("aspect is start, end, mid or center, not '" + aspect + "'");
    j["aspect"] = aspect;
  }
  return j;
}

json references(const Document& doc, const Scene& scene, const json& refs, const json& aspects) {
  if (!refs.is_array() || refs.empty()) throw Error("refs: a list of references");
  json out = json::array();
  for (size_t i = 0; i < refs.size(); ++i)
    out.push_back(reference(doc, scene, refs[i], aspects.is_array() && i < aspects.size() ? aspects[i].get<std::string>() : std::string()));
  return out;
}

// The view as replay will take it: its orientation through the parents, side, sources and style. Throws.
void check_view(const Scene& scene, const std::string& id, const json& def) {
  SheetView v;
  v.id = id;
  v.sheet = def.at("sheet").get<std::string>();
  v.parent = def.value("parent", "");
  v.kind = def.value("kind", "");
  v.def = def;
  need_sheet(scene, v.sheet);
  if (v.kind != "base" && v.kind != "projected") throw Error("kind is base or projected");
  if (v.kind == "projected") {
    const SheetView* p = scene.sheet_view(v.parent);
    if (!p) throw Error("parent view " + v.parent + " does not exist");
    if (p->sheet != v.sheet) throw Error("the parent view is on another sheet");
    if (!p->error.empty()) throw Error("the parent view cannot be drawn: " + p->error);
    for (const SheetView* q = p; q; q = scene.sheet_view(q->parent))
      if (q->id == id) throw Error("a view cannot be projected from itself");
  }
  if (def.contains("scale") && def["scale"] != "sheet") drawing::parse_scale(def["scale"].get<std::string>());
  drawing::view_spec(scene, v);
}

// A dimension's value now, and where its text goes by default: beside what it measures.
json dimension_result(const Document& doc, const Scene& scene, const Sheet& sheet, const std::string& id, const json& def) {
  SheetItem t;
  t.id = id;
  t.sheet = sheet.id;
  t.view = def.value("view", "");
  t.kind = "dimension";
  t.type = def.value("type", "");
  t.def = def;
  const SheetView* view = scene.sheet_view(t.view);
  if (!view || view->sheet != sheet.id) throw Error("a dimension needs a view on its sheet");
  if (!view->error.empty()) throw Error("its view cannot be drawn: " + view->error);
  const auto frames = drawing::layout(doc, scene, sheet);
  return drawing::evaluate_item(doc, scene, sheet, t, frame_of(frames, t.view));
}

json default_place(const json& result, const std::string& type) {
  const double x = result["anchor"][0].get<double>(), y = result["anchor"][1].get<double>();
  if (type == "vertical") return {x + 8, y};
  if (type == "radius" || type == "diameter") return {x + 8, y + 8};
  return {x, y + 8};
}

}  // namespace

void register_sheet_commands(const std::function<void(const CommandInfo&, Handler)>& add) {
  add({"sheet", "Add a drawing sheet",
       {{"doc", "path"}, {"name", "string"}, {"drawing", "string - the drawing it belongs to"},
        {"size", "A4|A3|A2|A1|A0|ANSI-A|ANSI-B|ANSI-C|ANSI-D|ANSI-E - default A3"}, {"orientation", "landscape|portrait"},
        {"width", "number - mm, a custom size with height"}, {"height", "number"}, {"standard", "iso|asme"},
        {"projection", "first|third - angle; default by standard"}, {"scale", "string - the views' scale, 1:2 (default 1:1)"},
        {"values", "object - title block fields"}, {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        json op = {{"op", "sheet"}};
        op["name"] = a.value("name", "Sheet " + std::to_string(scene.sheets.size() + 1));
        op["drawing"] = a.value("drawing", scene.sheets.empty() ? std::string("Drawing 1") : scene.sheets.back().drawing);
        if (a.contains("width") || a.contains("height")) {
          const double w = a.value("width", 0.0), h = a.value("height", 0.0);
          if (!(w > 0 && h > 0 && w < 1e5 && h < 1e5)) throw Error("sheet: width and height are the paper's size in mm");
          op["size"] = {{"w", w}, {"h", h}};
        } else {
          const std::string o = a.value("orientation", "landscape");
          if (o != "landscape" && o != "portrait") throw Error("sheet: orientation is landscape or portrait");
          op["size"] = drawing::paper_size(a.value("size", "A3"), o == "landscape");
        }
        const std::string standard = a.value("standard", "iso");
        if (standard != "iso" && standard != "asme") throw Error("sheet: standard is iso or asme");
        const std::string projection = a.value("projection", standard == "asme" ? "third" : "first");
        if (projection != "first" && projection != "third") throw Error("sheet: projection is first or third (angle)");
        op["standard"] = standard;
        op["projection"] = projection;
        op["scale"] = drawing::scale_text(drawing::parse_scale(a.value("scale", "1:1")));
        if (a.contains("values")) {
          if (!a["values"].is_object()) throw Error("sheet: values is an object of title block fields");
          op["values"] = a["values"];
        }
        const std::string id = doc.append(op, a.value("by", "")).id;
        return json{{"id", id}, {"size", op["size"]}, {"scale", op["scale"]}, {"projection", projection}};
      });

  add({"sheet_view", "Add a base view, or one projected from parent (first/third angle as the sheet says); returns its paper frame",
       {{"doc", "path"}, {"sheet", "uuid"}, {"kind", "base|projected"}, {"name", "string"},
        {"orient", "string - front (default), top, right, iso, ... or a view bookmark id"}, {"dir", "[x,y,z] - towards the viewer"},
        {"up", "[x,y,z]"}, {"select", "array|csv - nodes (default all)"}, {"hide", "array|csv"}, {"at", "[x,y] - paper mm of its centre"},
        {"scale", "string - sheet (default), 1:5 or auto"}, {"parent", "uuid"},
        {"side", "left|right|top|bottom|top-left|top-right|bottom-left|bottom-right"}, {"gap", "number - mm between frames (20)"},
        {"hidden", "bool - hidden lines"}, {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        const Sheet& sheet = need_sheet(scene, a.at("sheet").get<std::string>());
        const std::string kind = a.value("kind", a.contains("parent") ? "projected" : "base");
        json op = {{"op", "sheet_view"}, {"sheet", sheet.id}};
        if (a.contains("name")) op["name"] = a["name"];
        op["kind"] = kind;
        if (kind == "base") {
          json orient;
          if (a.contains("dir")) {
            orient["dir"] = a["dir"];
            if (a.contains("up")) orient["up"] = a["up"];
          } else if (const std::string o = a.value("orient", "front"); is_uuid(o)) {
            orient["view"] = o;
          } else {
            Camera::preset(o);  // throws for a name it does not know
            orient["preset"] = o;
          }
          op["orient"] = orient;
          json source = json::object();
          if (const auto nodes = strings(a.value("select", json())); !nodes.empty()) source["nodes"] = nodes;
          if (const auto hide = strings(a.value("hide", json())); !hide.empty()) source["hide"] = hide;
          if (!source.empty()) op["source"] = source;
          op["at"] = a.contains("at") ? a["at"] : json::array({sheet.width / 2, sheet.height / 2});
          if (const std::string scale = a.value("scale", "sheet"); scale == "auto") {
            SheetView probe;
            probe.sheet = sheet.id;
            probe.kind = kind;
            probe.def = op;
            const auto e = drawing::view_extent(doc, scene, drawing::view_spec(scene, probe));
            op["scale"] = drawing::scale_text(drawing::fit_scale(e[2] - e[0], e[3] - e[1], 0.4 * sheet.width, 0.4 * sheet.height));
          } else if (scale != "sheet") {
            op["scale"] = drawing::scale_text(drawing::parse_scale(scale));
          }
        } else if (kind == "projected") {
          op["parent"] = a.at("parent");
          op["side"] = a.value("side", "right");
          if (a.contains("gap")) op["gap"] = a["gap"];
        }
        if (a.contains("hidden")) op["style"] = {{"hidden", a["hidden"].get<bool>()}};
        check_view(scene, "", op);
        const std::string id = doc.append(op, a.value("by", "")).id;
        const Scene after = resolve(doc);
        json frame = frame_of(drawing::layout(doc, after, need_sheet(after, sheet.id)), id).to_json();
        frame.erase("id");
        return json{{"id", id}, {"frame", frame}};
      });

  add({"sheet_item", "Add a dimension (measured in its view, its value kept) or a note to a sheet",
       {{"doc", "path"}, {"sheet", "uuid"}, {"view", "uuid"}, {"kind", "dimension|note"},
        {"type", "horizontal|vertical|aligned|radius|diameter|angle"},
        {"refs", "array - two vertices/edges, one edge, or one circle or cylinder"}, {"aspects", "array - per ref"},
        {"place", "[x,y] - text, paper mm from the view's centre"}, {"text", "string - a note; <> is a dimension's value"},
        {"at", "[x,y] - a note, paper mm"}, {"precision", "int - decimals (2)"}, {"tolerance", "object - {type: sym|dev, plus, minus}"},
        {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        const Sheet& sheet = need_sheet(scene, a.at("sheet").get<std::string>());
        const std::string kind = a.value("kind", a.contains("refs") ? "dimension" : "note");
        json op = {{"op", "sheet_item"}, {"sheet", sheet.id}};
        if (a.contains("view")) {
          const SheetView* view = scene.sheet_view(a["view"].get<std::string>());
          if (!view || view->sheet != sheet.id) throw Error("sheet_item: view " + a["view"].get<std::string>() + " is not on this sheet");
          op["view"] = view->id;
        }
        op["kind"] = kind;
        if (kind == "dimension") {
          const std::string type = a.value("type", "aligned");
          static const std::set<std::string> types = {"horizontal", "vertical", "aligned", "radius", "diameter", "angle"};
          if (!types.count(type)) throw Error("sheet_item: type is horizontal, vertical, aligned, radius, diameter or angle");
          op["type"] = type;
          op["refs"] = references(doc, scene, a.at("refs"), a.value("aspects", json()));
          if (a.contains("place")) op["place"] = {{"text", a["place"]}};
          for (const char* k : {"precision", "text"})
            if (a.contains(k)) op[k] = a[k];
          if (a.contains("tolerance")) op["tol"] = a["tolerance"];
          const json result = dimension_result(doc, scene, sheet, "", op);
          if (!op.contains("place")) op["place"] = {{"text", default_place(result, type)}};
          op["result"] = {{"value", result["value"]}, {"shown", result["shown"]}};
        } else if (kind == "note") {
          const std::string text = a.value("text", "");
          if (text.empty()) throw Error("sheet_item: a note needs its text");
          op["text"] = text;
          op["at"] = a.contains("at") ? a["at"] : op.contains("view") ? json::array({0, 0}) : json::array({sheet.width / 2, sheet.height / 2});
        } else {
          throw Error("sheet_item: kind is dimension or note");
        }
        const std::string id = doc.append(op, a.value("by", "")).id;
        json out = {{"id", id}};
        if (op.contains("result")) out["result"] = op["result"];
        return out;
      });

  add({"sheet_edit", "Change a sheet, view or item: set fields as sheet_info shows them (null removes); a dimension is measured again",
       {{"doc", "path"}, {"target", "uuid"}, {"set", "object"}, {"by", "string"}}, true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        const std::string id = a.at("target").get<std::string>();
        const Op* target = doc.find_op(id);
        if (!target || !drawing::is_sheet_record(target->type)) throw Error("sheet_edit: target is a sheet, view or item op id");
        if (std::find(scene.deleted_ops.begin(), scene.deleted_ops.end(), id) != scene.deleted_ops.end()) throw Error("sheet_edit: it was deleted");
        json set = a.at("set");
        if (!set.is_object() || set.empty()) throw Error("sheet_edit: set is an object of fields to change");
        json def;
        if (const Sheet* s = scene.sheet(id)) def = s->def;
        else if (const SheetView* v = scene.sheet_view(id)) def = v->def;
        else if (const SheetItem* t = scene.sheet_item(id)) def = t->def;
        else throw Error("sheet_edit: it is not on a sheet any more");
        if (set.contains("size") && set["size"].is_string())
          set["size"] = drawing::paper_size(set["size"].get<std::string>(), def["size"].value("w", 0.0) >= def["size"].value("h", 0.0));
        if (set.contains("refs")) set["refs"] = references(doc, scene, set["refs"], set.value("aspects", json()));
        set.erase("aspects");
        if (set.contains("place") && set["place"].is_array()) set["place"] = {{"text", set["place"]}};
        json after = def;
        for (const auto& [k, v] : set.items()) {
          if (v.is_null()) after.erase(k);
          else after[k] = v;
        }
        Document::validate_op(after);
        if (target->type == "sheet") {
          drawing::parse_scale(after.value("scale", "1:1"));
          const auto one_of = [&](const char* key, const char* fallback, const std::set<std::string>& known) {
            if (!known.count(after.value(key, fallback))) throw Error(std::string("sheet_edit: ") + key + " is not one this build knows");
          };
          one_of("standard", "iso", {"iso", "asme"});
          one_of("projection", after.value("standard", "iso") == "asme" ? "third" : "first", {"first", "third"});
          one_of("units", "mm", {"mm", "in"});
        } else if (target->type == "sheet_view") {
          check_view(scene, id, after);
        } else {
          const std::string kind = after.value("kind", "");
          if (kind != "dimension" && kind != "note") throw Error("sheet_edit: a " + kind + " needs a newer OPAD");
          const Sheet& sheet = need_sheet(scene, after.at("sheet").get<std::string>());
          if (after.contains("view"))
            if (const SheetView* v = scene.sheet_view(after["view"].get<std::string>()); !v || v->sheet != sheet.id)
              throw Error("sheet_edit: its view is not on its sheet");
          static const std::set<std::string> touches = {"refs", "type", "view", "precision", "prefix", "suffix", "text", "tol", "obtuse"};
          bool again = false;
          for (const auto& [k, v] : set.items()) again = again || touches.count(k);
          if (kind == "dimension" && again) {
            const json result = dimension_result(doc, scene, sheet, id, after);
            set["result"] = {{"value", result["value"]}, {"shown", result["shown"]}};
          }
        }
        const std::string edit = doc.append({{"op", "edit"}, {"target", id}, {"set", set}}, a.value("by", "")).id;
        json out = {{"id", edit}};
        if (set.contains("result")) out["result"] = set["result"];
        return out;
      });

  add({"sheet_info", "Sheets; with sheet: its views (paper frames) and items (dimension value made with and now)",
       {{"doc", "path"}, {"sheet", "uuid"}, {"project", "bool - project each view"}}, false},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        const auto summary = [](const Sheet& s) {
          json j = {{"id", s.id}, {"name", s.name}, {"drawing", s.drawing}, {"size", {{"w", s.width}, {"h", s.height}}}, {"standard", s.standard},
                    {"projection", s.projection}, {"scale", drawing::scale_text(s.scale)}, {"views", s.views.size()}, {"items", s.items.size()}};
          if (s.def.contains("values")) j["values"] = s.def["values"];
          return j;
        };
        if (!a.contains("sheet")) {
          json sheets = json::array();
          for (const auto& s : scene.sheets) sheets.push_back(summary(s));
          return json{{"sheets", sheets}};
        }
        const Sheet& sheet = need_sheet(scene, a["sheet"].get<std::string>());
        const auto frames = drawing::layout(doc, scene, sheet);
        json views = json::array(), items = json::array(), unresolved = json::array();
        for (const auto& id : sheet.views) {
          const SheetView& v = *scene.sheet_view(id);
          json j = v.def;
          for (const char* k : {"op", "ts", "by"}) j.erase(k);
          json frame = frame_of(frames, id).to_json();
          frame.erase("id");
          j["frame"] = frame;
          if (!v.error.empty()) j["error"] = v.error;
          if (a.value("project", false) && frame_of(frames, id).error.empty()) {
            const auto g = drawing::project(doc, scene, drawing::view_spec(scene, v));
            j["projection"] = {{"tier", drawing::quality_name(g->tier)}, {"counts", g->counts()}, {"bounds", g->bounds}, {"fingerprint", g->fingerprint},
                               {"ms", g->stats.value("ms", 0)}};
          }
          views.push_back(j);
        }
        for (const auto& id : sheet.items) {
          const SheetItem& t = *scene.sheet_item(id);
          json j = t.def;
          for (const char* k : {"op", "ts", "by"}) j.erase(k);
          if (!t.error.empty()) j["error"] = t.error;
          if (t.kind == "dimension" && t.error.empty()) {
            try {
              const json now = drawing::evaluate_item(doc, scene, sheet, t, frame_of(frames, t.view));
              j["current"] = {{"value", now["value"]}, {"shown", now["shown"]}};
              if (now.contains("rehinted")) j["current"]["rehinted"] = now["rehinted"];
              j["changed"] = std::fabs(now["value"].get<double>() - t.def.value("result", json::object()).value("value", 0.0)) > 1e-9;
            } catch (const std::exception& e) {
              j["error"] = e.what();
              j["dangling"] = true;
            }
          }
          if (j.contains("error")) unresolved.push_back(id);
          items.push_back(j);
        }
        return json{{"sheet", summary(sheet)}, {"views", views}, {"items", items}, {"unresolved", unresolved}};
      });

  add({"part_properties", "Part properties of nodes: part_number, description, material, density g/cm3, mass g, vendor, notes, bom include|exclude|purchased; null removes",
       {{"doc", "path"}, {"target", "uuid"}, {"targets", "array of uuids"}, {"set", "object"}, {"appearance", "bool - colour as the material"}, {"by", "string"}}, true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        const json set = a.at("set");
        if (!set.is_object() || set.empty()) throw Error("part_properties: set is an object of properties");
        if (set.contains("bom") && !set["bom"].is_null()) {
          static const std::set<std::string> bom = {"include", "exclude", "purchased"};
          if (!set["bom"].is_string() || !bom.count(set["bom"].get<std::string>())) throw Error("part_properties: bom is include, exclude or purchased");
        }
        for (const char* k : {"part_number", "description", "material", "vendor", "notes"})
          if (set.contains(k) && !set[k].is_null() && !set[k].is_string()) throw Error(std::string("part_properties: ") + k + " is text");
        for (const char* k : {"density", "mass"})
          if (set.contains(k) && !set[k].is_null() && !(set[k].is_number() && property_number(set[k]) > 0))
            throw Error(std::string("part_properties: ") + k + (std::string(k) == "density" ? " is a positive number, g/cm3" : " is a positive number, g"));
        std::vector<std::string> targets = a.contains("targets") ? strings(a["targets"]) : std::vector<std::string>{a.at("target").get<std::string>()};
        if (targets.empty()) throw Error("part_properties: give target or targets");
        for (const auto& t : targets)
          if (!scene.node(t)) throw Error("part_properties: node " + t + " does not exist");
        const Material* material = set.contains("material") && set["material"].is_string() ? find_material(set["material"].get<std::string>()) : nullptr;
        if (a.value("appearance", false) && !material) throw Error("part_properties: appearance needs a material from the library (materials lists them)");
        json ids = json::array();
        for (const auto& t : targets) {
          ids.push_back(doc.append({{"op", "properties"}, {"target", t}, {"set", set}}, a.value("by", "")).id);
          if (a.value("appearance", false)) doc.append({{"op", "appearance"}, {"target", t}, {"color", material->color}, {"opacity", material->opacity}}, a.value("by", ""));
        }
        json out = {{"id", ids.front()}};
        if (a.contains("targets")) out["ids"] = ids;
        if (material) out["material"] = material->to_json();
        return out;
      });

  add({"materials", "Material library: id, name, density g/cm3, colour; match: the one a name stands for", {{"match", "string"}}, false},
      [](Document*, const json& a) {
        if (a.contains("match")) {
          const Material* m = find_material(a["match"].get<std::string>());
          return json{{"match", m ? m->to_json() : json(nullptr)}};
        }
        json list = json::array();
        for (const auto& m : materials()) list.push_back(m.to_json());
        return json{{"materials", list}};
      });
}

}  // namespace opad::commands
