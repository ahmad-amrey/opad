// Drawing commands (TODO 11 UI-76): sheet, sheet_view, sheet_item, sheet_edit, sheet_info and part_properties, for the
// CLI, MCP and Python through the command layer. Every write is one op, checked as replay will take it before it is
// appended, so a wrong pick or an unknown orientation never reaches the document.
#include <algorithm>
#include <functional>
#include <set>

#include "opad/commands.hpp"
#include "opad/design/feature.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/bom.hpp"
#include "opad/drawing/holes.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/geometry.hpp"
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

// What an item shows now, measured from its references (its result as the drawing would keep it).
json item_now(const Document& doc, const Scene& scene, const Sheet& sheet, const std::string& id, const json& def) {
  SheetItem t;
  t.id = id;
  t.sheet = sheet.id;
  t.view = def.value("view", "");
  t.kind = def.value("kind", "");
  t.type = def.value("type", "");
  t.def = def;
  const SheetView* view = t.view.empty() ? nullptr : scene.sheet_view(t.view);
  if (!t.view.empty() && (!view || view->sheet != sheet.id)) throw Error("its view is not on its sheet");
  if (view && !view->error.empty()) throw Error("its view cannot be drawn: " + view->error);
  const auto frames = view ? drawing::layout(doc, scene, sheet) : std::vector<drawing::ViewFrame>{};
  return drawing::measure_item(doc, scene, sheet, t, view ? &frame_of(frames, t.view) : nullptr);
}

}  // namespace

void register_sheet_commands(const std::function<void(const CommandInfo&, Handler)>& add) {
  add({"sheet", "Add a drawing sheet with a template; views: laid out at a scale that fits",
       {{"doc", "path"}, {"name", "string"}, {"drawing", "string - the drawing it belongs to"},
        {"size", "A4|A3|A2|A1|A0|ANSI-A|ANSI-B|ANSI-C|ANSI-D|ANSI-E - default A3"}, {"orientation", "landscape|portrait"},
        {"width", "number - mm, a custom size with height"}, {"height", "number"}, {"standard", "iso|asme"},
        {"projection", "first|third - angle; default by standard"}, {"scale", "string - 1:2 (default 1:1; auto with views)"},
        {"template", "iso|ansi|none|object"}, {"template_file", "path - DXF|DWG"}, {"template_brep", "string"},
        {"values", "object - title block fields"}, {"views", "array|csv - front,top,side,iso"}, {"select", "array|csv"}, {"hide", "array|csv"},
        {"hidden", "bool"}, {"tangent", "show|thin|hide"}, {"centermarks", "bool - centre marks and lines of holes"}, {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        json op = {{"op", "sheet"}};
        op["name"] = a.value("name", "Sheet " + std::to_string(scene.sheets.size() + 1));
        op["drawing"] = a.value("drawing", scene.sheets.empty() ? std::string("Drawing 1") : scene.sheets.back().drawing);
        const std::string standard = a.value("standard", "iso");
        if (standard != "iso" && standard != "asme") throw Error("sheet: standard is iso or asme");
        json tmpl;
        if (a.contains("template_file")) tmpl = drawing::template_from_file(doc, path_from_utf8(a["template_file"].get<std::string>()));
        if (a.contains("template_brep")) {  // a template file read beforehand (read_template_file): its geometry is stored here
          if (!a.value("template", json()).is_object()) throw Error("sheet: template_brep goes with a template object");
          tmpl = a["template"];
          tmpl["geometry"] = drawing::store_template_geometry(doc, tmpl, a["template_brep"].get<std::string>());
        }
        if (a.contains("width") || a.contains("height")) {
          const double w = a.value("width", 0.0), h = a.value("height", 0.0);
          if (!(w > 0 && h > 0 && w < 1e5 && h < 1e5)) throw Error("sheet: width and height are the paper's size in mm");
          op["size"] = {{"w", w}, {"h", h}};
        } else if (tmpl.contains("size") && !a.contains("size")) {
          op["size"] = tmpl["size"];
        } else {
          const std::string o = a.value("orientation", "landscape");
          if (o != "landscape" && o != "portrait") throw Error("sheet: orientation is landscape or portrait");
          op["size"] = drawing::paper_size(a.value("size", "A3"), o == "landscape");
        }
        if (tmpl.is_null()) {
          const json t = a.value("template", json(standard == "asme" ? "ansi" : "iso"));
          if (t.is_object()) tmpl = t;
          else if (t.is_string() && t != "none") tmpl = drawing::make_template(t.get<std::string>(), op["size"]["w"].get<double>(), op["size"]["h"].get<double>());
          else if (!t.is_string()) throw Error("sheet: template is iso, ansi, none or a template object");
        }
        if (tmpl.is_object()) tmpl.erase("size");
        if (!tmpl.is_null()) op["template"] = tmpl;
        const std::string projection = a.value("projection", standard == "asme" ? "third" : "first");
        if (projection != "first" && projection != "third") throw Error("sheet: projection is first or third (angle)");
        op["standard"] = standard;
        op["projection"] = projection;
        if (a.contains("values")) {
          if (!a["values"].is_object()) throw Error("sheet: values is an object of title block fields");
          op["values"] = a["values"];
        }
        const std::vector<std::string> views = strings(a.value("views", json()));
        const std::string scale = a.value("scale", views.empty() ? "1:1" : "auto");
        json plan;
        if (!views.empty()) {
          json source = json::object(), style = json::object();
          if (const auto nodes = strings(a.value("select", json())); !nodes.empty()) source["nodes"] = nodes;
          if (const auto hide = strings(a.value("hide", json())); !hide.empty()) source["hide"] = hide;
          if (a.contains("hidden")) style["hidden"] = a["hidden"].get<bool>();
          if (a.value("centermarks", false)) style["centermarks"] = true;
          if (a.contains("tangent")) {
            const std::string t = a["tangent"].get<std::string>();
            if (t != "show" && t != "thin" && t != "hide") throw Error("sheet: tangent is show, thin or hide");
            style["tangent"] = t;
          }
          plan = drawing::plan_views(doc, scene, op, views, source, scale, style);
          op["scale"] = plan["scale"];
        } else {
          if (scale == "auto") throw Error("sheet: scale auto needs views to fit");
          op["scale"] = drawing::scale_text(drawing::parse_scale(scale));
        }
        const std::string id = doc.append(op, a.value("by", "")).id;
        json out = {{"id", id}, {"size", op["size"]}, {"scale", op["scale"]}, {"projection", projection}};
        if (op.contains("template")) out["template"] = op["template"].value("id", "");
        if (!plan.is_null()) {
          json made = json::array();
          std::string base;
          for (const auto& v : plan["views"]) {
            json record = v["record"];
            record["sheet"] = id;
            if (record.value("parent", "") == "base") record["parent"] = base;
            const std::string view = doc.append(record, a.value("by", "")).id;
            if (base.empty()) base = view;
            made.push_back({{"id", view}, {"view", v["view"]}, {"kind", record["kind"]}});
          }
          out["views"] = made;
        }
        return out;
      });

  add({"sheet_view", "Add a base view, or one projected from parent (first/third angle as the sheet says); returns its paper frame",
       {{"doc", "path"}, {"sheet", "uuid"}, {"kind", "base|projected"}, {"name", "string"},
        {"orient", "string - front (default), top, right, iso, ... or a view bookmark id"}, {"dir", "[x,y,z] - towards the viewer"},
        {"up", "[x,y,z]"}, {"select", "array|csv - nodes (default all)"}, {"hide", "array|csv"}, {"at", "[x,y] - paper mm of its centre"},
        {"scale", "string - sheet (default), 1:5 or auto"}, {"parent", "uuid"},
        {"side", "left|right|top|bottom|top-left|top-right|bottom-left|bottom-right"}, {"gap", "number - mm between frames (20)"},
        {"hidden", "bool - hidden lines"}, {"centermarks", "bool"}, {"by", "string"}},
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
        if (a.contains("hidden")) op["style"]["hidden"] = a["hidden"].get<bool>();
        if (a.value("centermarks", false)) op["style"]["centermarks"] = true;
        check_view(scene, "", op);
        const std::string id = doc.append(op, a.value("by", "")).id;
        const Scene after = resolve(doc);
        json frame = frame_of(drawing::layout(doc, after, need_sheet(after, sheet.id)), id).to_json();
        frame.erase("id");
        return json{{"id", id}, {"frame", frame}};
      });

  add({"sheet_item",
       "Add an annotation to a sheet, measured in its view with its value kept: dimension, note (refs: a leader), centermark, centerline, hole_callout, "
       "hole_table, datum, fcf (feature control frame), surface (texture), dimension_set (ordinate/baseline/chain from refs[0])",
       {{"doc", "path"}, {"sheet", "uuid"}, {"view", "uuid"},
        {"kind", "dimension|note|centermark|centerline|hole_callout|hole_table|datum|fcf|surface|dimension_set"},
        {"type", "horizontal|vertical|aligned|radius|diameter|angle; sets: ordinate|baseline|chain"},
        {"refs", "array - what it measures or points at"}, {"aspects", "array - per ref: start|end|mid|center"},
        {"place", "[x,y] - text or symbol, paper mm from the view's centre"}, {"text", "string - a note; <> is a dimension's value"},
        {"at", "[x,y] - a note (paper mm), a hole table's top left (sheet mm)"}, {"precision", "int - decimals (2)"},
        {"tolerance", "object - {type: sym|dev|limits|fit, plus, minus, fit: H7}"}, {"letter", "string - datum A-Z"},
        {"characteristic", "string - fcf: position, flatness, perpendicularity, ..."}, {"value", "number|string - fcf tolerance, surface requirement"},
        {"zone", "diameter"}, {"material", "M|L|S"}, {"datums", "array - fcf datum letters, B(M) with a modifier"},
        {"process", "any|removal|no_removal"}, {"axis", "horizontal|vertical - sets"}, {"extend", "number - centre marks and lines, mm"},
        {"op", "object - a record planned beforehand"}, {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        json op;
        if (a.contains("op")) {  // planned on a worker (the app): checked as replay takes it
          op = a["op"];
          if (!op.is_object() || op.value("op", "") != "sheet_item") throw Error("sheet_item: op is a sheet_item record");
          const Sheet& sheet = need_sheet(scene, op.value("sheet", ""));
          if (op.contains("view"))
            if (const SheetView* v = scene.sheet_view(op["view"].get<std::string>()); !v || v->sheet != sheet.id) throw Error("sheet_item: its view is not on its sheet");
          if (op.value("kind", "") != "note" && !drawing::known_item(op.value("kind", ""), op.value("type", ""))) throw Error("sheet_item: a kind this build does not know");
        } else {
          op = drawing::plan_item(doc, scene, a);
        }
        const std::string id = doc.append(op, a.value("by", "")).id;
        json out = {{"id", id}};
        if (op.contains("result")) out["result"] = op["result"];
        return out;
      });

  add({"sheet_datum_dimensions", "Dimension a view's features (default: its holes) from its datum symbols: ordinate, baseline or chain sets",
       {{"doc", "path"}, {"sheet", "uuid"}, {"view", "uuid"}, {"type", "ordinate|baseline|chain"}, {"datums", "array - letters (default all)"},
        {"refs", "array - features"}, {"precision", "int"}, {"ops", "array - sets planned beforehand"}, {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        json planned;
        if (a.contains("ops")) {  // planned on a worker (the app): sets of a sheet that exists
          const Scene scene = resolve(doc);
          for (const auto& op : a["ops"])
            if (!op.is_object() || op.value("op", "") != "sheet_item" || op.value("kind", "") != "dimension_set" || !scene.sheet(op.value("sheet", "")))
              throw Error("sheet_datum_dimensions: ops are dimension_set records of a sheet");
          planned = {{"ops", a["ops"]}};
        } else {
          planned = drawing::datum_dimensions(doc, resolve(doc), a);
        }
        json ids = json::array(), results = json::array();
        for (const auto& op : planned["ops"]) {
          ids.push_back(doc.append(op, a.value("by", "")).id);
          results.push_back(op.value("result", json::object()));
        }
        return json{{"ids", ids}, {"results", results}};
      });

  add({"holes", "Holes of a body: diameter, depth or through, counterbore, countersink, drill point, the faces they are made of",
       {{"doc", "path"}, {"target", "uuid - a body"}}, false},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        const std::string id = a.at("target").get<std::string>();
        const Node* n = scene.node(id);
        if (!n || n->kind != Node::Kind::Body) throw Error("holes: target is a body");
        json list = json::array();
        for (const auto& h : drawing::find_holes(node_world_shape(doc, scene, id))) list.push_back(h.to_json());
        return json{{"holes", list}};
      });

  add({"sheet_edit", "Change a sheet, view or item: set fields as sheet_info shows them (null removes); a dimension is measured again; sheets also take template, template_file",
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
          set["size"] = drawing::paper_size(set["size"].get<std::string>(), set.value("orientation", def["size"].value("w", 0.0) >= def["size"].value("h", 0.0) ? "landscape" : "portrait") == "landscape");
        set.erase("orientation");
        if (target->type == "sheet") {  // a built-in template is drawn for its paper: made again for a new one
          if (set.contains("template_file")) {
            json t = drawing::template_from_file(doc, path_from_utf8(set["template_file"].get<std::string>()));
            t.erase("size");
            set["template"] = t;
            set.erase("template_file");
          }
          if (set.contains("template_brep")) {
            if (!set.value("template", json()).is_object()) throw Error("sheet_edit: template_brep goes with a template object");
            set["template"]["geometry"] = drawing::store_template_geometry(doc, set["template"], set["template_brep"].get<std::string>());
            set["template"].erase("size");
            set.erase("template_brep");
          }
          const json size = set.value("size", def.value("size", json::object()));
          const json was = def.value("template", json());
          std::string remake = set.contains("template") && set["template"].is_string() ? set["template"].get<std::string>() : "";
          if (remake.empty() && set.contains("size") && !set.contains("template") && was.is_object() && (was.value("id", "") == "iso" || was.value("id", "") == "ansi"))
            remake = was["id"].get<std::string>();
          if (remake == "none") set["template"] = nullptr;
          else if (!remake.empty()) {
            set["template"] = drawing::make_template(remake, size.value("w", 0.0), size.value("h", 0.0));
            if (was.is_object() && was.contains("fields") && (was.value("id", "") == "iso" || was.value("id", "") == "ansi"))
              set["template"]["fields"] = was["fields"];  // placed on the sheet by hand: kept
          }
        }
        if (set.contains("refs") && target->type == "sheet_item")  // re-attached: references as sheet_item keeps them
          set["refs"] = drawing::item_references(doc, scene, set["refs"], set.value("aspects", json()));
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
          if (kind != "note" && !drawing::known_item(kind, after.value("type", ""))) throw Error("sheet_edit: a " + kind + " needs a newer OPAD");
          if (set.contains("tol")) drawing::check_tolerance(set["tol"]);
          const Sheet& sheet = need_sheet(scene, after.at("sheet").get<std::string>());
          if (after.contains("view"))
            if (const SheetView* v = scene.sheet_view(after["view"].get<std::string>()); !v || v->sheet != sheet.id)
              throw Error("sheet_edit: its view is not on its sheet");
          static const std::set<std::string> touches = {"refs", "type", "view", "precision", "prefix", "suffix", "text", "tol", "obtuse", "axis"};
          bool again = false;
          for (const auto& [k, v] : set.items()) again = again || touches.count(k);
          if (again)
            if (const json result = drawing::item_result(after, item_now(doc, scene, sheet, id, after)); !result.is_null()) set["result"] = result;
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
          if (t.error.empty() && (t.kind != "note" || t.def.contains("refs"))) {
            try {
              const drawing::ViewFrame* f = t.view.empty() ? nullptr : &frame_of(frames, t.view);
              const json now = drawing::measure_item(doc, scene, sheet, t, f);
              const json current = drawing::item_result(t.def, now);
              if (!current.is_null()) {
                j["current"] = current;
                if (now.contains("rehinted")) j["current"]["rehinted"] = now["rehinted"];
                const json made = t.def.value("result", json::object());
                j["changed"] = t.kind == "dimension" ? std::fabs(now["value"].get<double>() - made.value("value", 0.0)) > 1e-9 : current.value("shown", json()) != made.value("shown", json());
              }
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
       {{"doc", "path"}, {"target", "uuid"}, {"targets", "array of uuids"}, {"set", "object"}, {"appearance", "bool - colour as the material"},
        {"document", "bool - the document's own"}, {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene scene = resolve(doc);
        const json set = a.at("set");
        if (!set.is_object() || set.empty()) throw Error("part_properties: set is an object of properties");
        if (a.value("document", false)) {  // title block fields every drawing of the document fills in
          if (!is_uuid(doc.header.uuid)) throw Error("part_properties: this document has no id to hold its properties");
          for (const auto& [k, v] : set.items())
            if (k.empty() || !(v.is_string() || v.is_number() || v.is_null())) throw Error("part_properties: '" + k + "' is text, a number or null");
          return json{{"id", doc.append({{"op", "properties"}, {"target", doc.header.uuid}, {"set", set}}, a.value("by", "")).id}};
        }
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

  add({"bom", "Bill of materials: parts with quantities, properties and masses; json or csv",
       {{"doc", "path"}, {"mode", "parts|top|indented"}, {"root", "uuid"}, {"format", "json|csv"}, {"out", "path"}, {"mass", "bool"},
        {"mass_unit", "g|kg|lb"}, {"match_shapes", "bool"}, {"references", "bool - mesh/drawing bodies"}, {"separator", "string"}},
       false},
      [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        drawing::BomOptions o;
        o.mode = a.value("mode", "parts");
        o.root = a.value("root", "");
        o.mass = a.value("mass", true);
        o.mass_unit = a.value("mass_unit", "g");
        o.match_shapes = a.value("match_shapes", true);
        o.references = a.value("references", false);
        const std::string format = a.value("format", "json"), out = a.value("out", ""), separator = a.value("separator", ",");
        if (format != "json" && format != "csv") throw Error("bom: format is json or csv");
        if (separator != "," && separator != ";" && separator != "\t") throw Error("bom: separator is a comma, a semicolon or a tab");
        const json b = drawing::bom(doc, resolve(doc), o);
        const std::string text = format == "csv" ? drawing::bom_csv(b, separator[0]) : b.dump(2) + "\n";
        if (out.empty()) return format == "csv" ? json{{"csv", text}, {"totals", b["totals"]}} : b;
        write_text_file(path_from_utf8(out), text);
        return json{{"out", out}, {"rows", b["rows"].size()}, {"totals", b["totals"]}};
      });
}

}  // namespace opad::commands
