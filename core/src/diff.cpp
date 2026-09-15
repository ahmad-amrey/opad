#include "opad/diff.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "opad/scene.hpp"

namespace opad {

static json op_summary(const Op& o) {
  json j;
  j["id"] = o.id;
  j["op"] = o.type;
  j["ts"] = o.data.value("ts", "");
  j["by"] = o.data.value("by", "");
  for (const char* k : {"target", "name", "text", "kind", "source"})
    if (o.data.contains(k) && o.data[k].is_string()) j[k] = o.data[k];
  return j;
}

json diff_documents(const Document& a, const Document& b) {
  std::map<std::string, const Op*> am, bm;
  for (const auto& o : a.ops) am[o.id] = &o;
  for (const auto& o : b.ops) bm[o.id] = &o;
  json added = json::array(), removed = json::array(), changed = json::array();
  for (const auto& o : b.ops)
    if (!am.count(o.id)) added.push_back(op_summary(o));
  for (const auto& o : a.ops) {
    auto it = bm.find(o.id);
    if (it == bm.end()) removed.push_back(op_summary(o));
    else if (it->second->data != o.data) {
      json c = op_summary(o);
      c["before"] = o.data;
      c["after"] = it->second->data;
      changed.push_back(c);
    }
  }
  const std::vector<std::string> akeys = a.body_keys(), bkeys = b.body_keys();
  std::set<std::string> ak(akeys.begin(), akeys.end()), bk(bkeys.begin(), bkeys.end());
  json badd = json::array(), brem = json::array();
  for (const auto& k : bk) if (!ak.count(k)) badd.push_back(k);
  for (const auto& k : ak) if (!bk.count(k)) brem.push_back(k);

  Scene sa = resolve(a), sb = resolve(b);
  auto signatures = [](const Scene& s, const Document& d) {
    std::map<std::string, std::string> sig;  // node id -> signature
    for (const auto& id : s.all_bodies()) {
      const Node* n = s.node(id);
      Mat4 w = s.world(id);
      char buf[512];
      std::string t;
      for (double v : w.m) { std::snprintf(buf, sizeof buf, "%.6f,", v); t += buf; }
      sig[id] = n->body_key + "|" + t;
    }
    (void)d;
    return sig;
  };
  auto siga = signatures(sa, a), sigb = signatures(sb, b);
  int geom_added = 0, geom_removed = 0, geom_moved = 0;
  std::multiset<std::string> sa_sigs, sb_sigs;
  for (auto& [id, s] : siga) sa_sigs.insert(s);
  for (auto& [id, s] : sigb) sb_sigs.insert(s);
  for (auto& [id, s] : sigb) if (!sa_sigs.count(s)) ++geom_added;
  for (auto& [id, s] : siga) if (!sb_sigs.count(s)) ++geom_removed;
  for (auto& [id, s] : siga)
    if (sigb.count(id) && sigb[id] != s) ++geom_moved;

  json j;
  j["a"] = a.path.string();
  j["b"] = b.path.string();
  j["same_document"] = a.header.uuid == b.header.uuid;
  j["ops"] = {{"added", added}, {"removed", removed}, {"changed", changed}};
  j["bodies"] = {{"added", badd}, {"removed", brem}};
  j["geometry"] = {{"added_or_moved", geom_added}, {"removed_or_moved", geom_removed}, {"moved", geom_moved}};
  j["summary"] = std::to_string(added.size()) + " op(s) added, " + std::to_string(removed.size()) + " removed, " +
                 std::to_string(changed.size()) + " changed; " + std::to_string(badd.size()) + " body entr(ies) added, " +
                 std::to_string(brem.size()) + " removed";
  return j;
}

Image render_diff(const Document& a, const Document& b, const RenderOptions& opt) {
  Scene sa = resolve(a), sb = resolve(b);
  auto sig_of = [](const Scene& s, const std::string& id) {
    Mat4 w = s.world(id);
    char buf[64];
    std::string t = s.node(id)->body_key + "|";
    for (double v : w.m) { std::snprintf(buf, sizeof buf, "%.6f,", v); t += buf; }
    return t;
  };
  std::multiset<std::string> in_a, in_b;
  for (const auto& id : sa.all_bodies()) in_a.insert(sig_of(sa, id));
  for (const auto& id : sb.all_bodies()) in_b.insert(sig_of(sb, id));

  std::vector<Mesh> meshes;
  meshes.reserve(sa.nodes.size() + sb.nodes.size());
  std::vector<RenderItem> items;
  int id = 1;
  auto add = [&](const Document& d, const Scene& s, const std::string& nid, std::array<float, 3> color, float opacity) {
    const Node* n = s.node(nid);
    if (!n || n->body_missing) return;
    meshes.push_back(tessellate_body(d, n->body_key, opt.tolerance));
    RenderItem it;
    it.mesh = &meshes.back();
    it.world = s.world(nid);
    it.color = color;
    it.opacity = opacity;
    it.id = id++;
    items.push_back(it);
  };
  std::multiset<std::string> drawn_common;
  for (const auto& nid : sa.all_bodies()) {
    std::string s = sig_of(sa, nid);
    if (in_b.count(s)) { add(a, sa, nid, {0.72f, 0.72f, 0.75f}, 1.0f); drawn_common.insert(s); }
    else add(a, sa, nid, {0.85f, 0.22f, 0.18f}, 0.85f);
  }
  for (const auto& nid : sb.all_bodies()) {
    std::string s = sig_of(sb, nid);
    if (!in_a.count(s)) add(b, sb, nid, {0.18f, 0.68f, 0.30f}, 0.85f);
  }
  return render_items(items, opt);
}

}  // namespace opad
