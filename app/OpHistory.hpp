#pragma once
// Who added each op of the open document, and in which commit (UI-64): "Added by … in 3f2a1c" in the timeline's tooltips,
// and the History page narrowed to the commits that touched an op or a node. "Show in git log" ran git log -S once per op,
// which read every version of the file (about 0.9 s a version) and timed out after about ten commits.
//
// ophistory::build reads git on a worker: the commits that changed the file (git log --topo-order), the file's blob in each
// (git cat-file --batch-check), then the op records of every blob it has not read before, up to #bodies only (the bodies
// are most of a big file): small blobs through one git cat-file --batch, big ones one git cat-file blob each, ended once
// #bodies comes. What a blob holds never changes, so its records are kept by its id in the user cache (git-ops/<oid>): a
// repository is read once, a new commit costs its own blob, and clones and branches share what was read.
// OpProvenance (OpProvenance.hpp) keeps the index of the open document; this part runs git and has no UI.
#include <QString>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Git.hpp"

namespace ophistory {

struct Record {
  std::string id, type, target;       // target: the op or node an edit, delete, rename, ... names
  std::vector<std::string> mentions;  // every other id it names (targets, nodes it creates, results it replaces)
};

// The op records of an .opad text up to #bodies, fed in pieces (a multiline record's further lines add to its mentions).
class Reader {
 public:
  void feed(std::string_view chunk);
  bool done() const { return m_section == 2; }  // #bodies came: the rest need not be read
  std::vector<Record> finish();
 private:
  void line(std::string_view l);
  std::string m_pending;
  std::vector<Record> m_records;
  int m_section = 0;  // header, ops, bodies
};

struct Provenance {
  int added = -1;     // the first commit (Index::commits) whose version has it; -1: in no commit
  int lastEdit = -1;  // the last commit that brought an edit of it
  int edits = 0;      // the commits that brought edits of it
  int deleted = -1;   // the commit that brought a delete of it
};

struct Index {
  QString top, rel, head;  // the work tree, the document in it ('/'), the commit it was read at
  std::vector<git::Commit> commits;  // that changed the document, oldest first (topological)
  std::unordered_map<std::string, Provenance> ops;
  std::unordered_map<std::string, std::vector<int>> touched;  // an op or node id -> the commits that brought ops naming it
  int blobs = 0, blobsRead = 0;  // versions, and those read from git (the others came from the cache)
  qint64 bytesRead = 0;          // what git sent of them
  const Provenance* find(const std::string& op) const;
  std::vector<int> touching(const std::vector<std::string>& ids) const;  // oldest first, each once
};

QString cacheFolder();  // <user cache>/git-ops
// Workers only: runs git. Throws std::runtime_error; an empty index when there is no commit yet or `cancelled`.
Index build(const git::Context& c, const QString& top, const QString& rel, const QString& cacheDir, const std::function<bool()>& cancelled = {},
            const std::function<void(int done, int total)>& progress = {});

}  // namespace ophistory
