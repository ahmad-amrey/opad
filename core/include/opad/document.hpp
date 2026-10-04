#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.hpp"
#include "util.hpp"

namespace opad {

constexpr int kFormatVersion = 2; // multiline sketch records; version 1 remains readable

struct Header {
  int format = kFormatVersion;
  std::string uuid;
  std::string units = "mm";
  std::string created;
  std::string generator;
  json to_json() const;
  static Header from_json(const json& j);
};

// One line of the operation log. `raw` is the exact text persisted in the file (empty until first save);
// on save, persisted ops are written back verbatim so earlier lines never change (F7).
struct Op {
  std::string id;
  std::string type;
  json data;
  std::string raw;
};

// One immutable, content-addressed body-store entry (F3/F4). key = SHA-256 hex of `brep`.
struct BodyEntry {
  std::string key;
  json meta;         // name, color, units, source, ...
  std::string brep;  // OCCT ASCII BREP, LF line endings, trailing newline
};

struct ShapeCache;  // opaque; defined in geometry.cpp
std::shared_ptr<ShapeCache> make_shape_cache();

class Document {
 public:
  Document();
  static Document create(const std::string& units = "mm");
  static Document load(const std::filesystem::path& path);
  static Document parse(const std::string& text, const std::filesystem::path& origin = {});

  std::string serialize() const;
  void save();                                     // to `path`
  void save_as(const std::filesystem::path& path);

  // Validates the op, fills in id/ts/by when absent, appends and returns it.
  const Op& append(json op, const std::string& author = {});
  // Adds a body entry (no-op when the key already exists). Returns the key.
  std::string add_body(const std::string& brep, json meta);
  // The same with the key hashed already (sha256_hex(brep), by a worker): the UI thread neither hashes nor copies the text.
  std::string add_body(std::string&& brep, json meta, const std::string& key);
  // Viewer mode: a body that exists only as a live shape in the shape cache, with no BREP text. A document
  // holding such bodies cannot be serialised (see has_live_bodies).
  std::string add_live_body(const std::string& key, json meta);
  bool has_live_bodies() const;
  const BodyEntry* body(const std::string& key) const;
  bool has_body(const std::string& key) const { return bodies_index_.count(key) > 0; }
  std::vector<std::string> body_keys() const;
  size_t body_count() const { return bodies_.size(); }
  const std::vector<BodyEntry>& bodies() const { return bodies_; }

  // Removes body entries that no live (non-tombstoned) op references. Returns removed keys.
  std::vector<std::string> gc();

  // Undo support: removes every op after the first `count` (returning them, persisted text intact) and puts
  // them back. A restored op serialises byte-identically, so undo + redo + save leaves the file unchanged.
  std::vector<Op> truncate_ops(size_t count);
  void restore_ops(std::vector<Op> removed);

  const Op* find_op(const std::string& id) const;
  bool is_deleted(const std::string& op_id) const;

  Header header;
  std::vector<Op> ops;
  std::filesystem::path path;
  bool dirty = false;
  std::shared_ptr<ShapeCache> shape_cache;  // lazily filled by geometry.hpp helpers

  static void validate_op(const json& op);
  static const std::vector<std::string>& op_types();
  // False for an op type of a newer build. Such an op loads as an opaque record (UI-65): saved back byte for byte,
  // reported unresolved and never applied by replay, refused as an edit target; gc keeps the body keys it mentions.
  static bool known_type(const std::string& type);

 private:
  std::vector<BodyEntry> bodies_;
  std::unordered_map<std::string, size_t> bodies_index_;
  size_t persisted_ops_ = 0;
};

}  // namespace opad
