// The `opad` Python module (F34): one pybind11 binding over core, shipped as a pip package and embedded in the app.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "opad/core.hpp"
#include "opad/geometry.hpp"

namespace py = pybind11;
using namespace pybind11::literals;
using opad::json;

namespace {

py::object to_py(const json& j) {
  switch (j.type()) {
    case json::value_t::null: return py::none();
    case json::value_t::boolean: return py::bool_(j.get<bool>());
    case json::value_t::number_integer: return py::int_(j.get<long long>());
    case json::value_t::number_unsigned: return py::int_(j.get<unsigned long long>());
    case json::value_t::number_float: return py::float_(j.get<double>());
    case json::value_t::string: return py::str(j.get<std::string>());
    case json::value_t::array: {
      py::list l;
      for (const auto& e : j) l.append(to_py(e));
      return l;
    }
    case json::value_t::object: {
      py::dict d;
      for (auto it = j.begin(); it != j.end(); ++it) d[py::str(it.key())] = to_py(it.value());
      return d;
    }
    default: return py::none();
  }
}

json to_json(py::handle h) {
  if (h.is_none()) return nullptr;
  if (py::isinstance<py::bool_>(h)) return h.cast<bool>();
  if (py::isinstance<py::int_>(h)) return h.cast<long long>();
  if (py::isinstance<py::float_>(h)) return h.cast<double>();
  if (py::isinstance<py::str>(h)) return h.cast<std::string>();
  if (py::isinstance<py::bytes>(h)) return h.cast<std::string>();
  if (py::isinstance<py::dict>(h)) {
    json o = json::object();
    for (auto item : h.cast<py::dict>()) o[py::str(item.first).cast<std::string>()] = to_json(item.second);
    return o;
  }
  if (py::isinstance<py::sequence>(h)) {
    json a = json::array();
    for (auto item : h.cast<py::sequence>()) a.push_back(to_json(item));
    return a;
  }
  if (py::hasattr(h, "__fspath__")) return py::str(h).cast<std::string>();
  throw opad::Error("cannot convert Python value to JSON: " + py::str(py::type::of(h)).cast<std::string>());
}

json kwargs_json(const py::kwargs& kw) {
  json a = json::object();
  for (auto item : kw) a[py::str(item.first).cast<std::string>()] = to_json(item.second);
  return a;
}

// An .opad file with its linked files read where they are (opad/assets.hpp): those in its project, or any with trust_assets.
opad::Document open_document(const std::string& path, bool trust_assets) {
  opad::Document d = opad::Document::load(path);
  if (opad::has_assets(d)) {
    opad::AssetOptions o;
    o.trust_all = trust_assets;
    o.derive = opad::derive_asset;
    opad::load_assets(d, o);
  }
  return d;
}

py::object run_on(opad::Document* d, const std::string& name, const py::kwargs& kw) {
  return to_py(opad::commands::run(name, kwargs_json(kw), d));
}

}  // namespace

PYBIND11_MODULE(opad, m) {
  opad::configure_kernel_logging();
  m.doc() = "OPAD: git-native STEP viewer core. Every method maps to a command of the shared command layer.";
  m.attr("__version__") = opad::version_string();
  m.attr("FORMAT_VERSION") = opad::kFormatVersion;

  py::register_exception<opad::Error>(m, "OpadError");

  py::class_<opad::Document>(m, "Document", "An .opad document: header, append-only op log, content-addressed body store.")
      .def_static("create", [](const std::string& units) { return opad::Document::create(units); }, "units"_a = "mm")
      .def_static("open", &open_document, "path"_a, "trust_assets"_a = false,
                  "Open an .opad file; its linked files are read from its project (any path with trust_assets).")
      .def_static("browse", [](const std::string& step) { return opad::browse_step(step); }, "step"_a,
                  "Browse mode: a transient document with the STEP imported (F1).")
      .def_property_readonly("path", [](const opad::Document& d) { return d.path.string(); })
      .def_property_readonly("uuid", [](const opad::Document& d) { return d.header.uuid; })
      .def_property_readonly("units", [](const opad::Document& d) { return d.header.units; })
      .def_property_readonly("dirty", [](const opad::Document& d) { return d.dirty; })
      .def_property_readonly("op_count", [](const opad::Document& d) { return d.ops.size(); })
      .def_property_readonly("body_keys", [](const opad::Document& d) { return d.body_keys(); })
      .def("save", [](opad::Document& d) { d.save(); })
      .def("save_as", [](opad::Document& d, const std::string& p) { d.save_as(p); }, "path"_a)
      .def("serialize", &opad::Document::serialize, "The exact .opad text.")
      .def("run", [](opad::Document& d, const std::string& name, const py::kwargs& kw) { return run_on(&d, name, kw); },
           "name"_a, "Run any command of the command layer on this document.")
      .def("info", [](opad::Document& d, const py::kwargs& kw) { return run_on(&d, "info", kw); })
      .def("tree", [](opad::Document& d, const py::kwargs& kw) { return run_on(&d, "tree", kw); })
      .def("ops", [](opad::Document& d, const py::kwargs& kw) { return run_on(&d, "ops", kw); })
      .def("annotations", [](opad::Document& d, const py::kwargs& kw) { return run_on(&d, "annotations", kw); })
      .def("properties", [](opad::Document& d, const std::string& node) { return to_py(opad::commands::run("properties", json{{"node", node}}, &d)); }, "node"_a)
      .def("inspect", [](opad::Document& d, const std::string& ref) { return to_py(opad::commands::run("inspect", json{{"ref", ref}}, &d)); }, "ref"_a,
           "Inspect a reference: uuid | uuid/face/N | uuid/edge/N | uuid/vertex/N | point/x,y,z")
      .def("measure", [](opad::Document& d, const std::string& kind, const std::vector<std::string>& refs, bool pin, const std::string& by) {
             return to_py(opad::commands::run("measure", json{{"kind", kind}, {"refs", refs}, {"pin", pin}, {"by", by}}, &d));
           }, "kind"_a, "refs"_a, "pin"_a = false, "by"_a = "")
      .def("append", [](opad::Document& d, py::object op, const std::string& by) { return to_py(opad::commands::run("append", json{{"op", to_json(op)}, {"by", by}}, &d)); },
           "op"_a, "by"_a = "", "Validate and append an op given as a dict.")
      .def("import_step", [](opad::Document& d, const std::string& file, const std::string& by, const std::string& parent, bool heal) {
             return to_py(opad::commands::run("import", json{{"file", file}, {"by", by}, {"parent", parent}, {"heal", heal}}, &d));
           }, "file"_a, "by"_a = "", "parent"_a = "", "heal"_a = true)
      .def("import_file", [](opad::Document& d, const std::string& file, const std::string& by, const std::string& parent) {
             return to_py(opad::commands::run("import", json{{"file", file}, {"by", by}, {"parent", parent}}, &d));
           }, "file"_a, "by"_a = "", "parent"_a = "", "Import STEP, DXF, SVG, DWG via converter, STL or OBJ.")
      .def("import_brep", [](opad::Document& d, const std::string& brep, const std::string& name, const std::string& by, const std::string& parent) {
             return to_py(opad::commands::run("import_brep", json{{"brep", brep}, {"name", name}, {"by", by}, {"parent", parent}}, &d));
           }, "brep"_a, "name"_a = "Body", "by"_a = "", "parent"_a = "",
           "Import OCCT ASCII BREP text (e.g. from OCP's BRepTools.Write_s / build123d's export_brep).")
      .def("export", [](opad::Document& d, const std::string& format, const std::string& out, const py::kwargs& kw) {
             json a = kwargs_json(kw);
             a["format"] = format;
             a["out"] = out;
             return to_py(opad::commands::run("export", a, &d));
           }, "format"_a, "out"_a, "Export step|obj|stl|glb; kwargs: select=[uuids], schema, tolerance, ascii, per_body, mtl")
      .def("render", [](opad::Document& d, const std::string& out, const py::kwargs& kw) {
             json a = kwargs_json(kw);
             a["out"] = out;
             return to_py(opad::commands::run("render", a, &d));
           }, "out"_a, "Headless PNG; kwargs: view, width, height, select, edges, camera")
      .def("annotate", [](opad::Document& d, const std::string& anchor, const std::string& text, const std::string& by) {
             return to_py(opad::commands::run("annotate", json{{"anchor", anchor}, {"text", text}, {"by", by}}, &d));
           }, "anchor"_a, "text"_a, "by"_a = "")
      .def("delete", [](opad::Document& d, const std::string& target, const std::string& by) { return to_py(opad::commands::run("delete", json{{"target", target}, {"by", by}}, &d)); },
           "target"_a, "by"_a = "", "Tombstone an op.")
      .def("rename", [](opad::Document& d, const std::string& target, const std::string& name) { return to_py(opad::commands::run("rename", json{{"target", target}, {"name", name}}, &d)); }, "target"_a, "name"_a)
      .def("gc", [](opad::Document& d) { return d.gc(); }, "Drop body entries no live op references.")
      .def("body_brep", [](opad::Document& d, const std::string& key) {
             const opad::BodyEntry* e = d.body(key);
             if (!e) throw opad::Error("unknown body key: " + key);
             return e->brep;
           }, "key"_a, "ASCII BREP text of a body-store entry (feed to OCP's BRepTools.Read_s).")
      .def("node_brep", [](opad::Document& d, const std::string& node) {
             opad::Scene s = opad::resolve(d);
             return opad::brep_from_shape(opad::node_world_shape(d, s, node));
           }, "node"_a, "ASCII BREP of a body node placed in world coordinates.")
      .def("__repr__", [](const opad::Document& d) {
        return "<opad.Document " + (d.path.empty() ? std::string("(unsaved)") : d.path.string()) + " ops=" + std::to_string(d.ops.size()) + " bodies=" + std::to_string(d.body_count()) + ">";
      });

  m.def("open", &open_document, "path"_a, "trust_assets"_a = false);
  m.def("browse", [](const std::string& p) { return opad::browse_step(p); }, "step"_a);
  m.def("run", [](const std::string& name, const py::kwargs& kw) { return to_py(opad::commands::run(name, kwargs_json(kw))); }, "name"_a,
        "Run a command by name with keyword arguments (doc=path loads/saves the document).");
  m.def("commands", []() { return to_py(opad::commands::run("commands", json::object())); });
  m.def("diff", [](const std::string& a, const std::string& b, const py::kwargs& kw) {
    json args = kwargs_json(kw);
    args["a"] = a;
    args["b"] = b;
    return to_py(opad::commands::run("diff", args));
  }, "a"_a, "b"_a, "Diff two documents; kwargs: image=path renders a geometric diff.");
  m.def("load_plugin", [](const std::string& p) {
    opad::LoadedPlugin lp = opad::load_plugin(p);
    return py::dict("name"_a = lp.name, "version"_a = lp.version, "path"_a = lp.path.string());
  }, "path"_a);
  m.def("cache_dir", []() { return opad::cache_dir().string(); });
  m.def("new_uuid", &opad::new_uuid);
}
