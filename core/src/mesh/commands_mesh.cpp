// The read-only mesh_check command (agent_context.cpp registers it; opad/mesh_edit.hpp): what keeps each mesh body from
// printing, before Repair mesh is run on it.
#include "opad/geometry.hpp"
#include "opad/mesh_edit.hpp"
#include "opad/scene.hpp"
#include "opad/util.hpp"

namespace opad::meshops {

json check_command(const Document& doc, const Scene& scene, const json& args, const Cancel& cancel) {
  std::vector<std::string> ids;
  auto take = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!n) throw Error("no body or component " + id);
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
      if (std::find(ids.begin(), ids.end(), b) == ids.end()) ids.push_back(b);
  };
  const json& bodies = args.contains("bodies") ? args["bodies"] : json();
  if (bodies.is_string()) take(bodies.get<std::string>());
  else if (bodies.is_array())
    for (const auto& b : bodies) take(b.get<std::string>());
  else
    for (const auto& id : scene.all_bodies())
      if (const Node* n = scene.node(id); n && n->representation == "mesh") ids.push_back(id);
  CheckOptions opt;
  opt.self_intersections = args.value("self_intersections", true);
  opt.limit = static_cast<size_t>(std::max<long long>(1, args.value("limit", 1000LL)));
  json items = json::array();
  size_t printable = 0;
  for (const auto& id : ids) {
    const Node* n = scene.node(id);
    json item = {{"id", id}, {"name", n->name}};
    if (n->representation != "mesh") {
      item["skipped"] = "not a mesh (a solid's faces are checked by validate)";
      items.push_back(std::move(item));
      continue;
    }
    const Report r = check(from_shape(node_world_shape(doc, scene, id)), opt, cancel);
    item.update(r.to_json());
    printable += r.printable();
    if (!r.printable()) item["next"] = "feature mesh_repair on this body (weld, clean, orient, close holes); crossings left after it need the parts joined (combine join) or moved apart";
    items.push_back(std::move(item));
  }
  return {{"bodies", items}, {"printable", printable}, {"checked", items.size()}};
}

}  // namespace opad::meshops
