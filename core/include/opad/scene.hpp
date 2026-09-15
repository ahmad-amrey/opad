#pragma once
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "document.hpp"

namespace opad {

// The resolved state obtained by replaying the op log (skipping tombstoned ops).
struct Node {
  enum class Kind { Component, Body };
  std::string id;
  Kind kind = Kind::Component;
  std::string name;
  std::string parent;  // empty = document root
  std::vector<std::string> children;
  std::string body_key;  // Body only
  bool body_missing = false;  // Body whose key is not in the store (F8)
  Mat4 local;
  bool has_color = false;
  std::array<double, 3> color{0.75, 0.75, 0.78};
  double opacity = 1.0;
  bool visible = true;
  bool locked = false;
  std::string source_op;  // the import op that created it
  std::vector<std::string> modified_by;  // ops that touched this node after import
};

struct Annotation {
  std::string id;  // == op id
  Ref anchor;
  std::string text, by, ts;
  bool unresolved = false;
};

struct Measurement {
  std::string id;
  std::string kind;  // distance | angle | radius | diameter | bbox
  std::vector<Ref> refs;
  json result;
  std::string by, ts;
  bool unresolved = false;
};

struct SectionPlane {
  std::string id, name;
  Vec3 origin{0, 0, 0}, normal{0, 0, 1};
  bool enabled = true;
};

struct ViewBookmark {
  std::string id, name;
  json camera;
};

struct Unresolved {
  std::string op_id, op_type, reason;
};

struct Scene {
  std::vector<std::string> roots;
  std::unordered_map<std::string, Node> nodes;
  std::vector<Annotation> annotations;
  std::vector<Measurement> measurements;
  std::vector<SectionPlane> sections;
  std::vector<ViewBookmark> views;
  std::vector<Unresolved> unresolved;
  std::vector<std::string> deleted_ops;  // ids of tombstoned ops
  std::unordered_map<std::string, int> instance_count;  // body key -> number of body nodes

  const Node* node(const std::string& id) const;
  Mat4 world(const std::string& id) const;
  bool effectively_visible(const std::string& id) const;
  std::vector<std::string> bodies_under(const std::string& id) const;  // depth-first
  std::vector<std::string> all_bodies() const;
  std::vector<std::string> path_to(const std::string& id) const;  // root..id
  json tree_json(int max_depth = -1) const;
};

Scene resolve(const Document& doc);

}  // namespace opad
