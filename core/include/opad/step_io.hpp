#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "document.hpp"
#include "scene.hpp"

namespace opad {

struct ImportOptions {
  bool heal = true;      // run ShapeFix on bodies that fail BRepCheck
  bool viewer = false;   // viewer mode: keep the reader's shapes live in the shape cache; no healing, no BREP
                         // text, no content hashing. Much faster, but such a document cannot be saved.
  std::string author;    // recorded on the op ("by")
  std::string parent;    // uuid of the component to import under; empty = root
  std::function<bool(double, const std::string&)> progress;  // return false to cancel
  // Drawings (DXF, SVG, DWG): where the drawing's own XY plane and origin go, stored as the imported root component's
  // placement, so a sketch converted from it keeps exactly that plane and origin. `center_drawing` first moves the
  // drawing's bounding-box centre to its origin (a drawing opened on its own is centred on the grid).
  Mat4 placement;
  bool center_drawing = false;
  // DXF read as a drawing template (UI-78): model space attributes (ATTDEF, ATTRIB) and texts that are a placeholder ({title},
  // <DWG_NO>) are returned here where they stand instead of drawn: {tag, sample, at [x, y] mm, height, halign 0-2, valign
  // 0 baseline-3 top, angle, w}.
  std::vector<json>* text_fields = nullptr;
};

struct ImportResult {
  std::string op_id;
  int components = 0;
  int bodies = 0;        // body nodes created
  int new_entries = 0;   // body-store entries added (instances of existing keys are free)
  int healed = 0;
  std::vector<std::string> warnings;
  json to_json() const;
};

ImportResult import_step(Document& doc, const std::filesystem::path& step, const ImportOptions& opt = {});

// Browse mode (F1): a transient, never-saved document with the STEP imported.
Document browse_step(const std::filesystem::path& step, const ImportOptions& opt = {});

// Viewer mode -> an editable document, in place of a second read of the file: every live body (ImportOptions::viewer)
// gets its BREP text and content key, healed like a full import, and the ops are rewritten to the new keys, so hidden
// layers, colours and other view changes carry over. Live keys whose shape came through unchanged are in `renamed`
// (a view can keep what it shows for them); `healed` lists new keys whose shape the healing changed.
struct EditableKeys {
  std::map<std::string, std::string> renamed;  // live key -> content key
  std::set<std::string> healed;
};
Document make_editable(const Document& viewer, EditableKeys* changed = nullptr, const std::function<bool(double)>& progress = {});

// Viewer mode remembers slow reads (viewer_cache.cpp): the shapes of a viewer document's import, with any display meshes
// made since, kept under the user cache and keyed by the file's path, size and time and the options that shape the read.
// load: false when nothing usable is kept (then read the file); store: best effort, never throws for cache trouble.
bool viewer_cache_load(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
void viewer_cache_store(const Document& doc, const std::filesystem::path& file, const ImportOptions& opt, const std::function<bool()>& cancelled = {});

// Imports a shape given as OCCT ASCII BREP text (the bridge for build123d/CadQuery/OCP users): solids in a
// compound become separate bodies under a component named `name`.
ImportResult import_brep(Document& doc, const std::string& brep, const std::string& name, const ImportOptions& opt = {});

struct ExportOptions {
  std::string format = "step";        // step | obj | stl | glb | <plugin format>
  std::vector<std::string> select;    // node ids; empty = whole document
  std::string step_schema = "AP214";  // AP214 | AP242
  double tolerance = 0.1;             // linear deflection (mm) for meshes
  bool ascii = false;                 // STL text instead of binary
  bool per_body = false;              // STL: one file per body
  bool mtl = true;                    // OBJ: write a material library for colours
  // 2D formats (dxf, svg, dwg; pdf and png where a painter is installed, drawing::can_paint): solids and meshes export as
  // a hidden-line view (TODO 11 UI-87), described as the projection reads it ({"view": "front"} or {"dir": [..], "up": [..]},
  // "hidden" (default false), "tangent", "quality"); drawings and sketches are left out of a view. Null: drawings and sketches
  // as drawn, and solids as seen from the top (their XY plane, where drawings lie).
  json view;
  int decimals = 6;                   // 2D coordinates
  int dpi = 300;                      // PNG
  std::string sheet;                  // 2D: a drawing sheet (id or name) as drawn, or "drawing:<name>": its sheets (PDF pages)
  std::function<bool(double, const std::string&)> progress;  // 2D views: return false to cancel
};

struct ExportResult {
  std::vector<std::filesystem::path> files;
  int bodies = 0;
  json details = json::object();  // 2D: entities and layers written, the view's tier
  json to_json() const;
};

ExportResult export_selection(const Document& doc, const Scene& scene, const std::filesystem::path& out,
                              const ExportOptions& opt);

// Expands a selection (node ids, or empty for all) into body node ids.
std::vector<std::string> select_bodies(const Scene& scene, const std::vector<std::string>& select);

}  // namespace opad
