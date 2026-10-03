#pragma once
// Linked assets (TODO 11 UI-67): a file imported by reference rather than by copy, for a STEP or a board one designs
// around without editing it. Its import op carries an `asset` object:
//   {"v":1, "kind":"step|iges|brep|mesh|drawing|kicad_pcb", "path":"<relative to the document, forward slashes>",
//    "abs":"<absolute fallback>", "sha256":"<the file's>", "size":N, "storage":"linked|project|embedded",
//    "builder":{"name":"opad","version":1,"options":{...how it was read}}, "synced":"<time>",
//    "models_sha256":"<a KiCad board: its 3D models' names and contents as found>",
//    "derived":{"kind":"step","path","abs","sha256","builder":{"name":"kicad-cli",...}}}
// `derived`: the shapes come from another file made from the source by a converter (a board exported by kicad-cli); the
// source is what is watched and synced, the derived file what is read; AssetOptions::derive makes it again.
// and its bodies never enter the body store: they are read from the file whenever the document opens (load_assets, on the
// load worker), as the viewer reads files, under keys derived from each body's geometry ("opad-asset/2|<digest>": topology
// counts, vertices, edge and face types and middle points), so a part a new version of the file leaves alone keeps its key.
// Node ids are derived from the import op and each node's place in the file (names, a KiCad footprint's uuid), so a sync,
// which is an `edit` of the import op with the new hash and nodes plus the regeneration of what depends on it, keeps them
// and every rename, colour, placement or reference made since. Old builds open such a document: the asset's components
// and bodies are listed, the bodies missing (docs/format.md).
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "design/feature.hpp"
#include "step_io.hpp"

namespace opad {

struct AssetOptions {
  std::vector<std::filesystem::path> trusted;  // folders read without asking, besides the document's project
  bool trust_all = false;                      // the caller vouches for every path (the user's own recovery snapshot)
  bool cache = true;                           // remember every read by content (the viewer cache): fast reopening,
                                               // and a file changed since still shows the version synced
  KicadOptions kicad;                          // this machine's KiCad model folders (a board's own options come from its op)
  std::function<bool(double, const std::string&)> progress;  // as ImportOptions::progress; false cancels
  // Makes an asset's derived file again from its source (the asset object says how: derived.builder) and returns where it
  // is: when it is missing here, and on sync. Runs on the worker that reads the assets. Without it such an asset reads the
  // derived file it finds, else it is missing.
  std::function<std::filesystem::path(const json& asset, const std::filesystem::path& source)> derive;
};

// One linked asset as this machine finds it.
struct AssetState {
  std::string import_id, name, kind, storage;
  std::string path;            // as recorded: relative to the document, else absolute
  std::filesystem::path file;  // where it was found; empty when missing
  // ok | changed (not the file last synced: sync takes it) | missing | untrusted (outside the document's project: ask
  // before reading it) | error (unreadable) | embedded (no longer linked)
  std::string state;
  std::string reason;
  std::string sha256;          // of the file found
  std::string models;          // a KiCad board: the digest of its 3D models as found (names and contents)
  std::filesystem::path derived;  // an asset read through a derived file: where that is (empty: missing)
  std::string derived_sha256;
  int bodies = 0, unbound = 0; // body nodes; those the file did not give (gone since the sync, or the file was not read)
  json to_json() const;
};

// Imports `file` linked: read as the viewer reads it (no healing, BREP text or shape hashing), its bodies registered as
// external, its op given the asset object. `opt` as import_file takes it (KiCad options, placement, parent).
ImportResult link_file(Document& doc, const std::filesystem::path& file, const ImportOptions& opt = {});
// Links `file` read through `derived`, a file a converter made from it (`builder`: {"name","version","options"}, how to make
// it again): the source is watched and synced, the derived file read (kicad-cli's STEP of a board, UI-73).
ImportResult link_derived(Document& doc, const std::filesystem::path& file, const std::filesystem::path& derived, const json& builder,
                          const ImportOptions& opt = {});

// Whether the document links any file (an import op with a linked or project asset).
bool has_assets(const Document& doc);
// The asset object of an import op as its edits leave it; null when it is no asset.
json asset_of(const Document& doc, const std::string& import_id);
// Where an asset's file is on this machine, or empty: its path beside the document, the project's copy (assets/<name>),
// then the absolute path. A network path (\\server\share) is not even looked at unless opt.trusted names it.
std::filesystem::path locate_asset(const Document& doc, const json& asset, const AssetOptions& opt = {});
// Whether `file` may be read for `doc` without asking: inside the document's folder or the git work tree it is in, inside
// one of opt.trusted, or opt.trust_all. A document not saved yet holds only what was linked in this session: trusted.
bool asset_trusted(const Document& doc, const std::filesystem::path& file, const AssetOptions& opt = {});
// A file's SHA-256, remembered in the user cache per path, size and time (a big STEP is hashed once).
std::string file_sha256(const std::filesystem::path& file);

// Every asset of the document: located, trust-checked and hashed; nothing is read.
std::vector<AssetState> asset_status(const Document& doc, const AssetOptions& opt = {});
// Reads every asset whose bodies are not registered yet and registers them (a worker's job: it reads files). A missing,
// untrusted or unreadable file leaves its bodies missing and the rest of the document as it is.
std::vector<AssetState> load_assets(Document& doc, const AssetOptions& opt = {});

// Sync: reads the asset again (from `file` when given: it moved) and plans the edit of its import op with the new hash,
// nodes and bodies, ids kept per place in the file and keys kept for bodies whose geometry did not change, and the
// regeneration of what uses them. Empty when the file is the one synced. Commit with design::commit. plan.report:
// {"import","sha256","up_to_date","added","removed","changed","kept","regenerated","errors"}.
design::Plan plan_asset_sync(const Document& doc, const std::string& import_id, const AssetOptions& opt = {},
                             const std::filesystem::path& file = {});
// Embed: the asset's bodies become ordinary body-store entries (healed like a full import, content keys), editable; the
// asset object stays with storage "embedded" (where it came from) and the file is no longer read.
design::Plan plan_asset_embed(const Document& doc, const std::string& import_id, const std::function<bool()>& cancel = {});
// Pack: copies the file into the assets/ folder beside the document (a KiCad board with its project file and the models in
// its folder, as assets/<board>/; a derived file into assets/.opad/) and appends the edit pointing the asset there (storage
// "project").
// {"import","path","copied"}.
json pack_asset(Document& doc, const std::string& import_id, const std::string& author = {});

// Linked imports and their edits not saved yet get their path relative to `dir`, where the document is being saved.
void rebase_asset_paths(Document& doc, const std::filesystem::path& dir);

}  // namespace opad
