#pragma once
#include <functional>
#include <string>
#include <vector>

#include "document.hpp"
#include "scene.hpp"

namespace opad {

struct ImportOptions {
  bool heal = true;      // run ShapeFix on bodies that fail BRepCheck
  std::string author;    // recorded on the op ("by")
  std::string parent;    // uuid of the component to import under; empty = root
  std::function<bool(double, const std::string&)> progress;  // return false to cancel
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
};

struct ExportResult {
  std::vector<std::filesystem::path> files;
  int bodies = 0;
  json to_json() const;
};

ExportResult export_selection(const Document& doc, const Scene& scene, const std::filesystem::path& out,
                              const ExportOptions& opt);

// Expands a selection (node ids, or empty for all) into body node ids.
std::vector<std::string> select_bodies(const Scene& scene, const std::vector<std::string>& select);

}  // namespace opad
