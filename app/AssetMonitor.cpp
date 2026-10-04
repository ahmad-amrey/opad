#include "AssetMonitor.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>

#include <algorithm>
#include <memory>

#include "AppDocument.hpp"
#include "Jobs.hpp"
#include "opad/assets.hpp"
#include "opad/kicad_pcb.hpp"

namespace {
namespace fs = std::filesystem;

QString qpath(const fs::path& p) { return QString::fromStdU16String(p.u16string()); }
QString qpath(const std::string& utf8) { return QDir::cleanPath(QString::fromStdString(utf8)); }

bool networkPath(const QString& p) { return p.startsWith("//") || p.startsWith("\\\\"); }

// A .gitattributes pattern against a path relative to its folder: without a slash it names a file at any depth, else a
// path from that folder; "**" spans folders. Case as git on this system sees it (core.ignorecase on Windows).
bool attributeMatch(QString pattern, const QString& rel) {
  if (pattern.startsWith('/')) pattern.remove(0, 1);
  const bool anywhere = !pattern.contains('/');
  QString rx;
  for (int i = 0; i < pattern.size(); ++i) {
    const QChar c = pattern[i];
    if (c == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
      const bool slash = i + 2 < pattern.size() && pattern[i + 2] == '/';
      rx += slash ? "(?:.*/)?" : ".*";
      i += slash ? 2 : 1;
    } else if (c == '*') {
      rx += "[^/]*";
    } else if (c == '?') {
      rx += "[^/]";
    } else {
      rx += QRegularExpression::escape(QString(c));
    }
  }
#ifdef _WIN32
  const auto options = QRegularExpression::CaseInsensitiveOption;
#else
  const auto options = QRegularExpression::NoPatternOption;
#endif
  return QRegularExpression(QRegularExpression::anchoredPattern((anywhere ? "(?:.*/)?" : "") + rx), options).match(rel).hasMatch();
}
}  // namespace

AssetMonitor::AssetMonitor(AppDocument* doc, JobRunner* jobs, QObject* parent) : QObject(parent), m_doc(doc), m_jobs(jobs) {
  m_debounce.setSingleShot(true);
  m_poll.setInterval(30000);
  connect(&m_debounce, &QTimer::timeout, this, [this] {
    if (m_job) { m_again = true; return; }
    if (m_records.empty() || !m_doc->hasDocument) return;
    auto probe = std::make_shared<opad::Document>(opad::Document::create());  // the records alone: nothing of the model
    probe->path = m_doc->doc.path;
    try {
      for (const auto& r : m_records) probe->append(r);
    } catch (const std::exception& e) {
      if (trace::enabled()) trace::log(QString("assets: records not read: %1").arg(e.what()));
      return;
    }
    const opad::AssetOptions options = AssetMonitor::options(m_doc);
    auto out = std::make_shared<std::vector<opad::json>>();
    const auto generation = m_doc->generation, version = m_version;
    QPointer<AssetMonitor> self(this);
    m_job = m_jobs->async(tr("Checking linked files"), [probe, options, out](Progress p) {
      for (const auto& s : opad::asset_status(*probe, options)) {
        if (p.cancelled()) return;
        opad::json j = s.to_json();
        if (!s.file.empty()) {
          const QFileInfo info(qpath(s.file));
          j["bytes"] = info.size();
          j["modified"] = info.lastModified().toUTC().toString(Qt::ISODate).toStdString();
          j["lfs"] = lfsStored(s.file);
          j["models"] = s.models;
          if (s.kind == "kicad_pcb") try {  // a board also changes with its 3D models: watched too
            const opad::json models = opad::kicad_models(s.file, options.kicad);
            opad::json files = opad::json::array();
            for (const auto& m : models.value("models", opad::json::array()))
              if (std::error_code ec; m.contains("file") && std::filesystem::is_regular_file(opad::path_from_utf8(m["file"].get<std::string>()), ec)) files.push_back(m["file"]);
            j["model_files"] = files;
            j["models_found"] = models.value("found", 0);
            j["models_missing"] = models.value("missing", 0);
            j["models_downloadable"] = models.value("downloadable", 0);
          } catch (const std::exception&) {
          }
        }
        out->push_back(std::move(j));
      }
    }, [self, out, generation, version](bool ok, const QString& error) {
      if (!self) return;
      self->m_job = nullptr;
      if (!ok && trace::enabled()) trace::log("assets: check failed: " + error);
      if (ok && generation == self->m_doc->generation && version == self->m_version) self->finished(*out);
      if (std::exchange(self->m_again, false)) self->check(500);
    });
  });
  auto changed = [this] { check(500); };  // an editor writes a file in several steps
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
  connect(&m_poll, &QTimer::timeout, this, [this] { check(0); });
}

// Debounced: a file event waits until the files have been quiet for 500 ms; a look already due now stays due.
void AssetMonitor::check(int delayMs) {
  if (m_records.empty()) return;
  if (m_job) m_again = true;
  else if (!m_debounce.isActive() || m_debounce.interval() > 0) m_debounce.start(delayMs);
}

const AssetMonitor::Asset* AssetMonitor::asset(const std::string& import) const {
  const auto it = m_assets.find(import);
  return it == m_assets.end() ? nullptr : &it->second;
}

std::string AssetMonitor::importOf(const std::string& node) const {
  const auto it = m_nodes.find(node);
  return it == m_nodes.end() ? std::string() : it->second;
}

const opad::json* AssetMonitor::state(const std::string& import) const {
  for (const auto& s : m_doc->assetStates)
    if (s.value("import", "") == import) return &s;
  return nullptr;
}

std::vector<std::string> AssetMonitor::changed() const {
  std::vector<std::string> out;
  for (const std::string& import : m_order)  // in the log's order
    if (const opad::json* s = state(import); s && s->value("state", "") == "changed" && m_assets.at(import).asset.value("storage", "linked") != "embedded") out.push_back(import);
  return out;
}

QString AssetMonitor::file(const std::string& import) const {
  if (const opad::json* s = state(import); s && s->contains("file")) return qpath((*s)["file"].get<std::string>());
  const Asset* a = asset(import);
  if (!a) return {};
  if (const std::string rel = a->asset.value("path", ""); !rel.empty() && !m_doc->doc.path.empty())
    return QDir::cleanPath(qpath(fs::absolute(m_doc->doc.path).parent_path()) + "/" + QString::fromStdString(rel));
  return qpath(a->asset.value("abs", std::string()));
}

QStringList AssetMonitor::watched() const { return m_watcher.files() + m_watcher.directories(); }

void AssetMonitor::setSyncing(const std::string& import, bool on) {
  if (on ? !m_syncing.insert(import).second : !m_syncing.erase(import)) return;
  emit statesChanged();
}

void AssetMonitor::synced(const std::string& import, const std::string& sha256) {
  if (sha256.empty()) return;
  m_seen[import] = sha256;
  for (auto& s : m_doc->assetStates)
    if (s.value("import", "") == import) {
      s["state"] = "ok";
      s["sha256"] = sha256;
      s.erase("reason");
      s.erase("unbound");
    }
  emit statesChanged();
}

opad::AssetOptions AssetMonitor::options(const AppDocument* doc) {
  opad::AssetOptions o = AppDocument::assetOptions();
  for (const auto& s : doc->assetStates)  // read in this session (trusted once, or linked just now): still trusted
    if ((s.value("state", "") == "ok" || s.value("state", "") == "changed") && s.contains("file"))
      o.trusted.push_back(opad::path_from_utf8(s["file"].get<std::string>()).parent_path());
  return o;
}

// The asset records of the log in its order (imports without their nodes, the edits of their asset, the tombstones of
// those), the asset each import's edits leave, and which nodes are whose.
void AssetMonitor::rescan() {
  m_assets.clear();
  m_order.clear();
  m_nodes.clear();
  m_roots.clear();
  m_stale.clear();
  m_records.clear();
  m_signature.clear();
  if (!m_doc->hasDocument || m_doc->browse) return;
  std::set<std::string> kept;
  for (const auto& o : m_doc->doc.ops) {
    const std::string target = o.data.value("target", "");
    if (o.type == "import" && o.data.contains("asset") && o.data["asset"].is_object()) {
      m_records.push_back({{"op", "import"}, {"id", o.id}, {"source", o.data.value("source", "")}, {"asset", o.data["asset"]}, {"nodes", opad::json::array()}});
      kept.insert(o.id);
    } else if (o.type == "edit" && kept.count(target) && o.data.contains("set") && o.data["set"].contains("asset")) {
      m_records.push_back({{"op", "edit"}, {"id", o.id}, {"target", target}, {"set", {{"asset", o.data["set"]["asset"]}}}});
      kept.insert(o.id);
    } else if (o.type == "delete" && kept.count(target)) {
      m_records.push_back({{"op", "delete"}, {"id", o.id}, {"target", target}});
      kept.insert(o.id);
    }
  }
  if (m_records.empty()) return;
  const std::set<std::string> deleted(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end());
  for (const auto& r : m_records) {
    const std::string id = r.value("id", "");
    if (deleted.count(id)) continue;
    if (r["op"] == "import") {
      Asset& a = m_assets[id];
      m_order.push_back(id);
      a.asset = r["asset"];
      a.name = r.value("source", "");
    } else if (r["op"] == "edit") {
      if (const auto it = m_assets.find(r.value("target", "")); it != m_assets.end()) it->second.asset = r["set"]["asset"];
    }
  }
  for (const auto& [id, a] : m_assets) m_signature += id + a.asset.dump() + "\n";
  for (const auto& [id, n] : m_doc->scene.nodes) {
    const auto it = m_assets.find(n.source_op);
    if (it == m_assets.end()) continue;
    const opad::Node* parent = n.parent.empty() ? nullptr : m_doc->scene.node(n.parent);
    if (!parent || parent->source_op != n.source_op) {
      if (it->second.root.empty()) it->second.root = id;
      if (n.linked) m_roots.insert(id);
    }
    if (n.linked) m_nodes[id] = n.source_op;
    if (n.kind == opad::Node::Kind::Body) {
      ++it->second.bodies;
      it->second.missing += n.body_missing;
      if (const opad::BodyEntry* b = n.linked && !n.body_missing ? m_doc->doc.body(n.body_key) : nullptr; b && b->meta.value("stale", false)) {
        m_stale.insert(id);
        ++it->second.stale;
      }
    }
  }
}

void AssetMonitor::documentChanged(bool replaced) {
  trace::Scope scope("AssetMonitor::documentChanged");
  const std::string before = m_signature;
  const std::set<std::string> staleBefore = m_stale;
  rescan();
  if (replaced) {
    m_seen.clear();
    m_syncing.clear();
    m_again = false;
    m_debounce.stop();
  }
  for (const auto& [import, a] : m_assets)  // what a load found, else the file synced (linked just now)
    if (!m_seen.count(import)) {
      const opad::json* s = state(import);
      m_seen[import] = s && s->contains("sha256") ? (*s)["sha256"].get<std::string>() : a.asset.value("sha256", std::string());
    }
  if (!replaced && m_signature == before) {
    if (m_stale != staleBefore) emit statesChanged();  // a file read since (loadAssets)
    return;
  }
  ++m_version;
  watch();
  emit statesChanged();
  if (replaced)  // a load says what it found; the size, time and LFS come with the first look
    if (const auto found = changed(); !found.empty()) emit filesChanged(found);
  check(0);
}

void AssetMonitor::finished(const std::vector<opad::json>& states) {
  std::vector<std::string> fresh;
  bool reread = false;
  for (const auto& s : states) {
    const std::string import = s.value("import", ""), sha = s.value("sha256", "");
    std::string now = s.value("state", "");
    const Asset* a = asset(import);
    opad::json next = s;
    next.erase("models");
    // A file read through another (kicad-cli's STEP of a board) is as synced while its source and models are: the other one
    // is made again when needed, and kicad-cli writes its time into it (the load compared the parts).
    if (now == "changed" && a && a->asset.contains("derived") && sha == a->asset.value("sha256", "") &&
        s.value("models", "") == a->asset.value("models_sha256", s.value("models", std::string()))) {
      now = "ok";
      next["state"] = now;
      next.erase("reason");
    }
    if (trace::enabled()) trace::log(QString("assets: %1 %2 %3").arg(QString::fromStdString(s.value("name", "")), QString::fromStdString(now), QString::fromStdString(next.value("reason", ""))));
    opad::json* known = nullptr;
    for (auto& k : m_doc->assetStates)
      if (k.value("import", "") == import) known = &k;
    const std::string was = known ? known->value("state", "") : std::string();
    if (known && was == now && !next.contains("reason") && known->contains("reason")) next["reason"] = (*known)["reason"];  // what the load said
    if (known && known->contains("unbound") && !next.contains("unbound")) next["unbound"] = (*known)["unbound"];
    if (now == "changed" && !sha.empty() && m_seen[import] != sha) fresh.push_back(import);
    if (!sha.empty()) m_seen[import] = sha;
    reread = reread || ((was == "missing" || was == "error") && (now == "ok" || now == "changed") && a && a->missing > 0);
    if (known) *known = std::move(next);
    else m_doc->assetStates.push_back(std::move(next));
  }
  watch();
  emit statesChanged();
  if (!fresh.empty()) emit filesChanged(fresh);
  // A file that is back: the parts it left missing are read (a worker job; a file outside the project waits for the user).
  if (reread && !m_doc->designBusy && !m_doc->loading) m_doc->loadAssets(m_jobs, false);
}

// One watch per file and per folder: the folder sees a file saved through a temporary one (its own watch goes then) and a
// missing file coming back.
void AssetMonitor::watch() {
  QStringList want;
  auto add = [&want](const QString& path) {
    if (!path.isEmpty() && !want.contains(path)) want << path;
  };
  const QString docDir = m_doc->doc.path.empty() ? QString() : qpath(fs::absolute(m_doc->doc.path).parent_path());
  for (const auto& [import, a] : m_assets) {
    if (a.asset.value("storage", "linked") == "embedded") continue;
    const opad::json* s = state(import);
    if (s && s->value("state", "") == "untrusted") continue;  // not even watched (a share would get the user's credentials)
    if (const QString found = s && s->contains("file") ? qpath((*s)["file"].get<std::string>()) : QString(); !found.isEmpty()) {  // found by the last look
      add(found);
      add(QFileInfo(found).absolutePath());
      for (const auto& model : s->value("model_files", opad::json::array()))  // a board's 3D models and their folders
        if (const QString m = qpath(model.get<std::string>()); !m.isEmpty() && !networkPath(m)) {  // a share is not looked at
          add(m);
          add(QFileInfo(m).absolutePath());
        }
      continue;
    }
    QStringList places;  // where it may come back
    if (const std::string rel = a.asset.value("path", ""); !rel.empty() && !docDir.isEmpty()) places << QDir::cleanPath(docDir + "/" + QString::fromStdString(rel));
    if (const std::string abs = a.asset.value("abs", ""); !abs.empty()) places << qpath(abs);
    for (const QString& p : places)
      if (const QString dir = QFileInfo(p).absolutePath(); !networkPath(dir) && QFileInfo(dir).isDir()) add(dir);  // a share is not looked at
  }
  QStringList drop;
  for (const QString& p : watched())
    if (!want.contains(p)) drop << p;
  if (!drop.isEmpty()) m_watcher.removePaths(drop);
  QStringList fresh;
  for (const QString& p : want)
    if (!watched().contains(p)) fresh << p;
  if (!fresh.isEmpty()) m_watcher.addPaths(fresh);
  if (trace::enabled() && (!drop.isEmpty() || !fresh.isEmpty())) trace::log("assets: watching " + watched().join(", "));
  const bool remote = std::any_of(want.begin(), want.end(), networkPath);  // may not notify: looked at every 30 s
  if (remote && !m_poll.isActive()) m_poll.start();
  if (!remote) m_poll.stop();
}

bool AssetMonitor::lfsStored(const fs::path& file) {
  std::error_code ec;
  const fs::path abs = fs::absolute(file, ec).lexically_normal();
  std::vector<fs::path> dirs;  // the file's folder up to the work tree's root
  bool tree = false;
  for (fs::path p = abs.parent_path(); !p.empty(); p = p.parent_path()) {
    dirs.push_back(p);
    if (fs::exists(p / ".git", ec)) { tree = true; break; }
    if (p == p.parent_path()) break;
  }
  if (!tree) return false;
  bool lfs = false;
  for (auto dir = dirs.rbegin(); dir != dirs.rend(); ++dir) {  // the root's first: a deeper file overrides it
    QFile attributes(qpath(*dir / ".gitattributes"));
    if (!attributes.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
    const QString rel = QString::fromStdU16String(abs.lexically_relative(*dir).generic_u16string());
    static const QRegularExpression space("\\s+");
    for (const QByteArray& raw : attributes.readAll().split('\n')) {
      const QString line = QString::fromUtf8(raw).trimmed();
      if (line.isEmpty() || line.startsWith('#')) continue;
      const QStringList parts = line.split(space, Qt::SkipEmptyParts);
      if (parts.size() < 2 || !attributeMatch(parts[0], rel)) continue;
      for (int i = 1; i < parts.size(); ++i) {
        if (parts[i] == "filter=lfs") lfs = true;
        else if (parts[i].startsWith("filter=") || parts[i] == "-filter" || parts[i] == "!filter") lfs = false;
      }
    }
  }
  return lfs;
}
