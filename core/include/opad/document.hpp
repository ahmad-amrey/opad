#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
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
  // Index mode (Document::parse_index): the entry's lines in the text the document was read from, neither copied nor
  // verified when read; `brep` stays empty. text() is the BREP either way, as stored (sizes, saving, outlines).
  std::string_view indexed;
  std::string_view text() const { return indexed.empty() ? std::string_view(brep) : indexed; }
  // The BREP for using it (a shape, a copy into another store): an index-mode entry is hashed against its key on the
  // first call only (any thread) and throws on this and every later call when it does not match. Other entries were
  // verified when read.
  std::string_view checked_text() const;
  struct Check {  // 0 not hashed yet, 1 matches the key, 2 does not
    mutable std::atomic<unsigned char> state{0};
    Check() = default;
    Check(const Check& o) : state(o.state.load()) {}
    Check& operator=(const Check& o) { state = o.state.load(); return *this; }
  } check;
};

struct ShapeCache;  // opaque; defined in geometry.cpp
std::shared_ptr<ShapeCache> make_shape_cache();

class Document {
 public:
  Document();
  static Document create(const std::string& units = "mm");
  // `skip_body(key)` true leaves that body entry out, unread and unverified (a version compared with one already
  // in memory needs only the bodies it does not have).
  using BodyFilter = std::function<bool(const std::string& key)>;
  static Document load(const std::filesystem::path& path, const BodyFilter& skip_body = {});
  static Document parse(const std::string& text, const std::filesystem::path& origin = {}, const BodyFilter& skip_body = {});
  // Index mode, for reading a version rather than editing it (diff, compare, textconv, history): body entries are
  // listed with their meta but their BREP is neither copied nor hashed (git or the session that wrote it verified it).
  // The document keeps the text; a body's BREP is read from there when it is asked for and hashed the first time it is
  // used (BodyEntry::checked_text: a shape, a move into another store). Saves byte-identically.
  static Document load_index(const std::filesystem::path& path, const BodyFilter& skip_body = {});
  static Document parse_index(std::string text, const std::filesystem::path& origin = {}, const BodyFilter& skip_body = {});
  bool indexed() const { return source_ != nullptr; }

  std::string serialize() const;
  void save();                                     // to `path`
  void save_as(const std::filesystem::path& path);

  // Validates the op, fills in id/ts/by when absent, appends and returns it.
  const Op& append(json op, const std::string& author = {});
  // Adds a body entry (no-op when the key already exists). Returns the key.
  std::string add_body(const std::string& brep, json meta);
  // Viewer mode: a body that exists only as a live shape in the shape cache, with no BREP text. A document
  // holding such bodies cannot be serialised (see has_live_bodies).
  std::string add_live_body(const std::string& key, json meta);
  bool has_live_bodies() const;
  const BodyEntry* body(const std::string& key) const;
  bool has_body(const std::string& key) const { return bodies_index_.count(key) > 0; }
  std::vector<std::string> body_keys() const;
  size_t body_count() const { return bodies_.size(); }
  const std::vector<BodyEntry>& bodies() const { return bodies_; }
  // Rebuilds the body store as `keys` in that order, each kept from this store or moved out of `from` (verified when
  // that was parsed; an index-mode entry not checked yet is hashed here), then this store's other entries when
  // `keep_others`. Throws, changing nothing, when a key is in neither or a moved entry does not match its key.
  void arrange_bodies(const std::vector<std::string>& keys, Document& from, bool keep_others);

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

 private:
  static Document parse_text(std::string_view text, const std::filesystem::path& origin, const BodyFilter& skip_body, bool index);
  std::vector<BodyEntry> bodies_;
  std::unordered_map<std::string, size_t> bodies_index_;
  size_t persisted_ops_ = 0;
  std::shared_ptr<const std::string> source_;  // index mode: the text the entries' views point into
};

}  // namespace opad
