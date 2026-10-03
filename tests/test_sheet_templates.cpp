// Drawing templates and new drawings (TODO 11 UI-78): ISO 5457 / ISO 7200 and ASME frames and title blocks drawn from
// scratch, the title block filled in from the sheet and the part, a frame and title block from a DXF file, and a new
// drawing's standard views laid out in first and third angle at a scale that fits.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing/sheet.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

json run(Document& doc, const std::string& command, const json& args) { return commands::run(command, args, &doc); }

std::string box(Document& doc, double l, double w, double h) {
  return run(doc, "feature", {{"kind", "box"}, {"inputs", {{"length", std::to_string(l) + " mm"}, {"width", std::to_string(w) + " mm"}, {"height", std::to_string(h) + " mm"}}}})["body_ids"][0];
}

const ViewFrame& frame(const std::vector<ViewFrame>& frames, const std::string& id) {
  for (const auto& f : frames)
    if (f.id == id) return f;
  throw Error("no frame");
}

Vec2 centre(const ViewFrame& f) { return {(f.box[0] + f.box[2]) / 2, (f.box[1] + f.box[3]) / 2}; }
bool inside(const std::array<double, 4>& in, const std::array<double, 4>& room) {
  return in[0] >= room[0] - 1e-6 && in[1] >= room[1] - 1e-6 && in[2] <= room[2] + 1e-6 && in[3] <= room[3] + 1e-6;
}
bool apart(const std::array<double, 4>& a, const std::array<double, 4>& b) { return a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1]; }

std::map<std::string, std::string> texts(const Display& d, const std::string& layer) {
  std::map<std::string, std::string> out;
  for (const auto& p : d.prims)
    if (p.kind == Prim::Kind::Text && d.layers[size_t(p.layer)].name == layer) out[p.text] = p.text;
  return out;
}

}  // namespace

TEST(templates_drawn_from_scratch) {
  const json iso = make_template("iso", 420, 297);
  CHECK_EQ(iso["frame"]["left"], 20);
  CHECK_EQ(iso["frame"]["top"], 10);
  CHECK_EQ(iso["zones"]["x"], 8);  // ISO 5457: A3 has 8 x 6 fields, A4 6 x 4, A0 24 x 16
  CHECK_EQ(iso["zones"]["y"], 6);
  CHECK_EQ(make_template("iso", 297, 210)["zones"]["x"], 6);
  CHECK_EQ(make_template("iso", 210, 297)["zones"]["x"], 4);
  CHECK_EQ(make_template("iso", 1189, 841)["zones"]["x"], 24);
  CHECK_EQ(make_template("iso", 1189, 841)["zones"]["y"], 16);
  const json& tb = iso["title_block"];
  CHECK_EQ(tb["w"], 180);
  std::set<std::string> keys;
  for (const auto& f : tb["fields"]) {
    keys.insert(f["key"].get<std::string>());
    const json r = f["rect"];
    CHECK(r[0].get<double>() >= 0 && r[1].get<double>() >= 0 && r[0].get<double>() + r[2].get<double>() <= 180 + 1e-9 &&
          r[1].get<double>() + r[3].get<double>() <= tb["h"].get<double>() + 1e-9);
  }
  for (const char* k : {"owner", "number", "date", "sheet", "title", "approved", "author", "doctype", "scale", "projection"}) CHECK(keys.count(k));  // ISO 7200's mandatory fields
  // The block's lines: merged, the border 0.7 mm and the rest 0.35, none drawn twice.
  std::set<std::string> seen;
  int border = 0;
  for (const auto& l : tb["lines"]) {
    CHECK(seen.insert(l.dump()).second);
    border += l[4] == 0.7;
  }
  CHECK_EQ(border, 4);
  const json ansi = make_template("ansi", 431.8, 279.4);
  CHECK_EQ(ansi["standard"], "asme");
  CHECK_EQ(ansi["zones"]["from"], "bottom-right");
  CHECK_EQ(ansi["title_block"]["w"], 200);
  const json upright = make_template("ansi", 215.9, 279.4);  // the block shrinks onto a narrow sheet
  CHECK_NEAR(upright["title_block"]["w"].get<double>(), 215.9 - 20, 1e-6);
  CHECK_THROWS(make_template("din", 420, 297));
  CHECK_THROWS(make_template("iso", 40, 40));
  json sheet = {{"size", {{"w", 420}, {"h", 297}}}, {"template", iso}};
  const auto room = drawing_room(sheet);
  CHECK(room == (std::array<double, 4>{20, 55, 410, 287}));  // above the 40 mm block and 5 mm clear of it
  sheet.erase("template");
  CHECK(drawing_room(sheet) == (std::array<double, 4>{20, 10, 410, 287}));
}

TEST(title_block_filled_in) {
  Document doc = Document::create();
  const std::string body = box(doc, 60, 40, 10);
  run(doc, "rename", {{"target", body}, {"name", "Bracket"}});
  run(doc, "part_properties", {{"target", body}, {"set", {{"part_number", "OP-1002"}, {"material", "aluminium-6061"}, {"finish", "anodised"}}}});
  const json made = run(doc, "sheet", {{"size", "A4"}, {"views", "front"}, {"select", {body}}, {"by", "Ada"},
                                       {"values", {{"owner", "ACME"}, {"status", "=scale"}, {"revision", "B"}, {"finish", "=prop:finish"}}}});
  CHECK_EQ(made["template"], "iso");
  Scene s = resolve(doc);
  const Sheet& sheet = *s.sheet(made["id"].get<std::string>());
  const json v = title_values(doc, s, sheet);
  CHECK_EQ(v["title"], "Bracket");
  CHECK_EQ(v["number"], "OP-1002");
  CHECK_EQ(v["material"], "Aluminium 6061");
  CHECK_EQ(v["mass"], "64.8 g");  // 24000 mm3 at 2.7 g/cm3
  CHECK_EQ(v["author"], "Ada");
  CHECK_EQ(v["date"], sheet.def["ts"].get<std::string>().substr(0, 10));
  CHECK_EQ(v["size"], "A4");
  CHECK_EQ(v["sheet"], "1 / 1");
  CHECK_EQ(v["doctype"], "Part drawing");
  CHECK_EQ(v["owner"], "ACME");
  CHECK_EQ(v["status"], v["scale"]);
  CHECK_EQ(v["revision"], "B");
  CHECK_EQ(v["finish"], "anodised");
  CHECK_EQ(v["approved"], "");
  run(doc, "sheet", {{"size", "A4"}, {"views", "front"}});  // the drawing's second sheet
  s = resolve(doc);
  CHECK_EQ(title_values(doc, s, *s.sheet(made["id"].get<std::string>()))["sheet"], "1 / 2");
  CHECK_EQ(title_values(doc, s, s.sheets.back())["sheet"], "2 / 2");
  CHECK_EQ(title_values(doc, s, s.sheets.back())["doctype"], "Part drawing");  // the document's one root
  // On paper: labels, values, the frame's zones and the first angle symbol (the end view right of the cone).
  const Display d = sheet_display(doc, s, *s.sheet(made["id"].get<std::string>()));
  const auto shown = texts(d, "Title block");
  for (const char* t : {"Bracket", "OP-1002", "ACME", "Identification number", "Legal owner", "1 / 2", "A", "D", "1", "6"}) CHECK(shown.count(t));
  std::vector<double> circles, cone;
  for (const auto& p : d.prims) {
    if (d.layers[size_t(p.layer)].name != "Title block" || p.kind != Prim::Kind::Curve) continue;
    if (p.curve.type == Curve::Type::Arc) circles.push_back(p.curve.c[0]);
    if (p.curve.type == Curve::Type::Polyline) cone.push_back((p.curve.pts[0][0] + p.curve.pts[1][0]) / 2);
  }
  CHECK_EQ(circles.size(), 2u);
  CHECK(cone.size() == 1u && circles[0] > cone[0]);
  run(doc, "sheet_edit", {{"target", made["id"]}, {"set", {{"projection", "third"}}}});
  s = resolve(doc);
  const Display third = sheet_display(doc, s, *s.sheet(made["id"].get<std::string>()));
  circles.clear(), cone.clear();
  for (const auto& p : third.prims) {
    if (third.layers[size_t(p.layer)].name != "Title block" || p.kind != Prim::Kind::Curve) continue;
    if (p.curve.type == Curve::Type::Arc) circles.push_back(p.curve.c[0]);
    if (p.curve.type == Curve::Type::Polyline) cone.push_back((p.curve.pts[0][0] + p.curve.pts[1][0]) / 2);
  }
  CHECK(circles.size() == 2u && cone.size() == 1u && circles[0] < cone[0]);  // third angle: the end view left of the cone
}

TEST(new_drawing_views_laid_out) {
  Document doc = Document::create();
  const std::string body = box(doc, 60, 40, 10);
  // First angle (ISO): the top view under the front view, the view from the left right of it, the isometric bottom right.
  const json made = run(doc, "sheet", {{"size", "A3"}, {"views", {"front", "top", "left", "iso"}}, {"hidden", true}});
  CHECK_EQ(made["views"].size(), 4u);
  CHECK_EQ(made["views"][1]["kind"], "projected");
  CHECK_EQ(made["views"][3]["kind"], "base");
  CHECK_EQ(made["scale"], "2:1");  // 60 + 40 wide, 10 + 40 high: 2:1 fits the A3 room, 5:1 does not
  Scene s = resolve(doc);
  const Sheet& sheet = *s.sheet(made["id"].get<std::string>());
  CHECK_EQ(view_orientation(s, *s.sheet_view(made["views"][1]["id"].get<std::string>())), "top");
  CHECK_EQ(view_orientation(s, *s.sheet_view(made["views"][2]["id"].get<std::string>())), "left");
  CHECK_EQ(view_orientation(s, *s.sheet_view(made["views"][3]["id"].get<std::string>())), "iso");
  CHECK_EQ(s.sheet_view(made["views"][3]["id"].get<std::string>())->def["style"]["hidden"], false);
  const auto frames = layout(doc, s, sheet);
  const auto room = drawing_room(sheet.def);
  std::vector<ViewFrame> f;
  for (const auto& v : made["views"]) f.push_back(frame(frames, v["id"].get<std::string>()));
  for (size_t i = 0; i < f.size(); ++i) {
    CHECK(f[i].error.empty());
    CHECK(inside(f[i].box, room));
    for (size_t j = 0; j < i; ++j) CHECK(apart(f[i].box, f[j].box));
  }
  CHECK_NEAR(centre(f[1])[0], centre(f[0])[0], 1e-6);
  CHECK(centre(f[1])[1] < centre(f[0])[1]);
  CHECK_NEAR(centre(f[2])[1], centre(f[0])[1], 1e-6);
  CHECK(centre(f[2])[0] > centre(f[0])[0]);
  CHECK(centre(f[3])[0] > centre(f[0])[0] && centre(f[3])[1] < centre(f[0])[1]);
  // The group is centred in the room.
  const double left = std::min({f[0].box[0], f[1].box[0]}), right = std::max(f[2].box[2], f[3].box[2]);
  CHECK(std::fabs((left - room[0]) - (room[2] - right)) < 1);
  // Third angle (ASME): the top view above, the view from the right ("side") right of the front, the isometric top right.
  const json asme = run(doc, "sheet", {{"standard", "asme"}, {"size", "ANSI-B"}, {"views", "front,top,side,iso"}});
  CHECK_EQ(asme["template"], "ansi");
  s = resolve(doc);
  const auto g = layout(doc, s, *s.sheet(asme["id"].get<std::string>()));
  const ViewFrame &front = frame(g, asme["views"][0]["id"]), &top = frame(g, asme["views"][1]["id"]), &side = frame(g, asme["views"][2]["id"]),
                  &iso = frame(g, asme["views"][3]["id"]);
  CHECK(centre(top)[1] > centre(front)[1]);
  CHECK_EQ(view_orientation(s, *s.sheet_view(asme["views"][2]["id"].get<std::string>())), "right");
  CHECK(centre(side)[0] > centre(front)[0]);
  CHECK(centre(iso)[0] > centre(front)[0] && centre(iso)[1] > centre(front)[1]);
  // A big part comes out reduced; a given scale is kept; a front and an iso view sit side by side.
  Document large = Document::create();
  box(large, 900, 500, 300);
  const json reduced = run(large, "sheet", {{"size", "A3"}, {"views", "front,top,side"}});
  CHECK_EQ(reduced["scale"], "1:5");
  const json fixed = run(large, "sheet", {{"size", "A3"}, {"views", "front,top"}, {"scale", "1:10"}});
  CHECK_EQ(fixed["scale"], "1:10");
  const json pair = run(doc, "sheet", {{"size", "A4"}, {"views", "front,iso"}});
  s = resolve(doc);
  const auto two = layout(doc, s, *s.sheet(pair["id"].get<std::string>()));
  CHECK_NEAR(centre(two[0])[1], centre(two[1])[1], 1e-6);
  CHECK(centre(two[1])[0] > centre(two[0])[0]);
  CHECK_THROWS(run(doc, "sheet", {{"views", "front,diagonal"}}));
  CHECK_THROWS(run(doc, "sheet", {{"scale", "auto"}}));
  CHECK_THROWS(run(doc, "sheet", {{"template", "din"}}));
  // No template: the plain ISO frame of earlier sheets.
  const json plain = run(doc, "sheet", {{"template", "none"}});
  s = resolve(doc);
  CHECK(!s.sheet(plain["id"].get<std::string>())->def.contains("template"));
}

TEST(template_follows_the_paper) {
  Document doc = Document::create();
  box(doc, 60, 40, 10);
  const std::string id = run(doc, "sheet", {{"size", "A3"}})["id"];
  run(doc, "sheet_edit", {{"target", id}, {"set", {{"size", "A2"}}}});
  Scene s = resolve(doc);
  CHECK_EQ(s.sheet(id)->def["template"]["zones"]["x"], 12);
  run(doc, "sheet_edit", {{"target", id}, {"set", {{"size", "A4"}, {"orientation", "portrait"}}}});
  s = resolve(doc);
  CHECK_EQ(s.sheet(id)->width, 210);
  CHECK_EQ(s.sheet(id)->def["template"]["zones"]["x"], 4);
  run(doc, "sheet_edit", {{"target", id}, {"set", {{"template", "ansi"}}}});
  CHECK_EQ(resolve(doc).sheet(id)->def["template"]["id"], "ansi");
  run(doc, "sheet_edit", {{"target", id}, {"set", {{"template", "none"}}}});
  CHECK(!resolve(doc).sheet(id)->def.contains("template"));
  CHECK_THROWS(run(doc, "sheet_edit", {{"target", id}, {"set", {{"template", "din"}}}}));
}

TEST(template_from_a_dxf_file) {
  // A company frame drawn as a DXF (written by the DXF writer): a border on A4 landscape and a block with a line of text.
  Display company;
  const int ink = company.layer({"Border", kInk, LineType::Continuous, 0.5});
  company.polyline(ink, {{10, 10}, {287, 10}, {287, 200}, {10, 200}}, true);
  company.polyline(ink, {{187, 10}, {287, 10}, {287, 40}, {187, 40}}, true);
  company.line(ink, {187, 25}, {287, 25});
  const auto dir = std::filesystem::temp_directory_path() / new_uuid();
  std::filesystem::create_directories(dir);
  write_text_file(dir / "company.dxf", dxf_text(company));
  Display far = company;  // the same drawn far from the origin
  for (auto& p : far.prims)
    for (auto& q : p.curve.pts) q = {q[0] + 5000, q[1] + 3000};
  write_text_file(dir / "far.dxf", dxf_text(far));

  Document doc = Document::create();
  box(doc, 60, 40, 10);
  const json made = run(doc, "sheet", {{"template_file", (dir / "company.dxf").string()}, {"views", "front"}});
  CHECK_EQ(made["template"], "file");
  CHECK_EQ(made["size"]["preset"], "A4");
  CHECK_EQ(made["size"]["w"], 297);
  Scene s = resolve(doc);
  const Sheet& sheet = *s.sheet(made["id"].get<std::string>());
  const std::string key = sheet.def["template"]["geometry"];
  CHECK(doc.has_body(key) && !sheet.def["template"].contains("at"));
  CHECK(doc.gc().empty());  // the sheet keeps its template's geometry
  CHECK(doc.has_body(key));
  const Display d = sheet_display(doc, s, sheet);
  int lines = 0;
  for (const auto& p : d.prims)
    if (d.layers[size_t(p.layer)].name == "Template") {
      ++lines;
      for (const auto& q : p.curve.pts) CHECK(q[0] >= 9.9 && q[0] <= 287.1 && q[1] >= 9.9 && q[1] <= 200.1);
    }
  CHECK(lines >= 9);  // the border's and the block's sides and the line through it
  CHECK(Document::parse(doc.serialize()).has_body(key));
  // Drawn elsewhere: moved onto its paper, centred.
  const json moved = run(doc, "sheet", {{"template_file", (dir / "far.dxf").string()}});
  s = resolve(doc);
  const json at = s.sheet(moved["id"].get<std::string>())->def["template"]["at"];
  CHECK_NEAR(at[0].get<double>(), -5000, 1e-6);  // its border back at 10 mm from the paper's edges
  CHECK_NEAR(at[1].get<double>(), -3000, 1e-6);
  // And onto an existing sheet.
  const std::string plainSheet = run(doc, "sheet", {{"size", "A4"}})["id"];
  run(doc, "sheet_edit", {{"target", plainSheet}, {"set", {{"template_file", (dir / "company.dxf").string()}}}});
  CHECK_EQ(resolve(doc).sheet(plainSheet)->def["template"]["id"], "file");
  CHECK_THROWS(run(doc, "sheet", {{"template_file", (dir / "missing.dxf").string()}}));
  // Read beforehand (the app reads on a worker), stored by the command.
  std::string brep;
  const json read = read_template_file(dir / "company.dxf", brep);
  CHECK(!read.contains("geometry") && !brep.empty());
  const json given = run(doc, "sheet", {{"template", read}, {"template_brep", brep}});
  CHECK_EQ(resolve(doc).sheet(given["id"].get<std::string>())->def["template"]["geometry"], key);  // the same content, the same key
  CHECK_EQ(given["size"]["preset"], "A4");
  CHECK_THROWS(run(doc, "sheet", {{"template", read}, {"template_brep", "not a shape"}}));
  CHECK_THROWS(run(doc, "sheet", {{"template_brep", brep}}));
  std::error_code e;
  std::filesystem::remove_all(dir, e);
}

CHECK_MAIN()
