#pragma once
// Linked files (opad/assets.hpp) while their document is open (UI-68): which nodes belong to which asset, and what each
// file is now. The files are watched (QFileSystemWatcher on each file and its folder: an editor that saves through a
// temporary file drops the file's own watch; the folder of a missing one, to see it come back; a network folder, which
// may not notify, is also looked at every 30 s). A change is debounced (500 ms), then checked on a worker: located,
// trust-checked and hashed by opad::asset_status (the hash is remembered per path, size and time), with the file's size,
// time and whether git LFS stores it. The states go into AppDocument::assetStates, which the browser badges and Properties
// read; a file that changed on disk since it was last looked at is reported once (filesChanged). A KiCad board's 3D models
// are watched with it (its look changes with them) and counted (models_found, models_missing, models_downloadable: missing
// ones of KiCad's library); a file not trusted yet is not watched (nor is a share for a missing one).
#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "opad/assets.hpp"

class AppDocument;
class Job;
class JobRunner;

class AssetMonitor : public QObject {
  Q_OBJECT
 public:
  AssetMonitor(AppDocument* doc, JobRunner* jobs, QObject* parent = nullptr);
  // After every change of the document: its linked imports and their nodes are read again (O(ops + nodes), only when it
  // links files); a change of what they record (a sync, an undo) or a new document has the files looked at again.
  void documentChanged(bool replaced);
  void check(int delayMs = 0);  // look at the files again (a running look is followed by another)
  bool checking() const { return m_job || m_again; }
  int looks() const { return m_looks; }  // looks at the files started so far (benches)

  struct Asset {
    opad::json asset;            // as its edits leave it
    std::string name;            // the import's source file name
    std::string root;            // its top node
    int bodies = 0, missing = 0; // body nodes; those without their shape (the file not read, or gone from it)
    int stale = 0;               // shown from a file that is not the version synced (load_assets marks them)
  };
  const std::map<std::string, Asset>& assets() const { return m_assets; }  // by import op id; linked, project or embedded
  const Asset* asset(const std::string& import) const;
  // The linked import a node belongs to (component or body; empty when it is no linked file's), and whether it is that
  // file's top node (else one of its parts: read-only).
  std::string importOf(const std::string& node) const;
  bool isRoot(const std::string& node) const { return m_roots.count(node) > 0; }
  const std::set<std::string>& roots() const { return m_roots; }  // every linked file's top nodes
  const std::set<std::string>& stale() const { return m_stale; }  // body nodes shown from a file not the version synced
  const opad::json* state(const std::string& import) const;  // its AppDocument::assetStates entry
  std::vector<std::string> changed() const;                    // the imports whose file changed since their last sync
  QString file(const std::string& import) const;               // where its file is found, else where it was recorded
  QStringList watched() const;                                 // files and folders (benches)
  void setSyncing(const std::string& import, bool on);         // a sync or embed is planned: shown as such meanwhile
  bool syncing(const std::string& import) const { return m_syncing.count(import) > 0; }
  void synced(const std::string& import, const std::string& sha256);  // a sync was committed: in sync with that file now

  // How linked files are read for `doc`: AppDocument::assetOptions, and the folders of the files read in this session (the
  // user trusted them once, or linked them just now).
  static opad::AssetOptions options(const AppDocument* doc);
  // Whether git LFS stores `file`: the .gitattributes from its work tree's root down to its folder give it filter=lfs.
  static bool lfsStored(const std::filesystem::path& file);

 signals:
  void statesChanged();
  void filesChanged(const std::vector<std::string>& imports);  // found changed on disk (or at the load), once each

 private:
  void rescan();
  void watch();
  void finished(const std::vector<opad::json>& states);
  AppDocument* m_doc;
  JobRunner* m_jobs;
  QFileSystemWatcher m_watcher;
  QTimer m_debounce, m_poll;
  Job* m_job = nullptr;
  bool m_again = false;
  int m_looks = 0;
  std::map<std::string, Asset> m_assets;
  std::vector<std::string> m_order;  // its imports in the log's order
  std::unordered_map<std::string, std::string> m_nodes;  // node -> its linked import
  std::set<std::string> m_roots, m_stale;
  std::vector<opad::json> m_records;   // the log's asset records (imports without nodes, their edits and tombstones)
  std::string m_signature;             // of m_records: a change has the files looked at again
  std::map<std::string, std::string> m_seen;  // import -> the file's hash when it was last looked at
  std::set<std::string> m_syncing;
  unsigned long long m_version = 0;   // of the records: a look at older ones is dropped (another follows)
};
