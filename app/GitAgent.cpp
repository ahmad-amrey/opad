#include "GitAgent.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <algorithm>
#include <filesystem>
#include <set>

#include "Git.hpp"
#include "opad/diff.hpp"
#include "opad/document.hpp"
#include "opad/merge.hpp"
#include "opad/scene.hpp"

using opad::json;

namespace gitagent {

QString defaultBranches() { return QStringLiteral("main, master"); }

QString initialBranch() {
  const QString b = QSettings().value(kInitialBranchKey, QStringLiteral("main")).toString().trimmed();
  return git::validBranchName(b) ? b : QStringLiteral("main");
}

QStringList Policy::parse(const QString& text) {
  static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
  QStringList out;
  for (const QString& name : text.split(separators, Qt::SkipEmptyParts))
    if (!out.contains(name)) out << name;
  return out;
}

Policy Policy::read() {
  const QSettings s;
  Policy p;
  p.branches = parse(s.value(kBranchesKey, defaultBranches()).toString());
  p.commits = s.value(kCommitsKey, true).toBool();
  p.merges = s.value(kMergesKey, true).toBool();
  p.pushes = s.value(kPushesKey, false).toBool();
  return p;
}

bool Policy::protects(const QString& branch) const {
  if (branch.isEmpty() || branch == "(detached)") return false;
  for (const QString& pattern : branches) {
    if (!pattern.contains('*')) {
      if (pattern == branch) return true;
      continue;
    }
    const QString re = QRegularExpression::wildcardToRegularExpression(pattern, QRegularExpression::NonPathWildcardConversion);
    if (QRegularExpression(re).match(branch).hasMatch()) return true;
  }
  return false;
}

json Policy::to_json() const {
  json names = json::array();
  for (const QString& b : branches) names.push_back(b.toStdString());
  return {{"protected_branches", names}, {"refuse_commits", commits}, {"refuse_merges", merges}, {"refuse_pushes", pushes},
          {"set_in", "OPAD Preferences > Version control > Branch protection (the user's to change)"}};
}

void useAppSettings(const QString& programDir, bool singleFile) {
  QCoreApplication::setOrganizationName("opad");
  QCoreApplication::setApplicationName("OPAD");
  QString data;
  if (singleFile) data = programDir + "/opad-data";
  if (QFile::exists(programDir + "/opad.portable")) data = programDir + "/data";
  if (!qEnvironmentVariableIsEmpty("OPAD_BENCH_SETTINGS")) data = qEnvironmentVariable("OPAD_BENCH_SETTINGS");
  if (data.isEmpty()) return;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, data);
}

Refused::Refused(std::string c, const QString& message, std::string n) : std::runtime_error(message.toStdString()), code(std::move(c)), next(std::move(n)) {}

bool writes(const std::string& name) {
  static const std::set<std::string> reads{"git_status", "git_log", "git_branches", "git_diff"};
  return !reads.count(name);
}

bool changesFiles(const std::string& name) {
  static const std::set<std::string> files{"git_switch", "git_merge", "git_merge_abort", "git_resolve", "git_pull", "git_branch_create"};
  return files.count(name) > 0;
}

namespace {
[[noreturn]] void refuse(const char* code, const QString& message, const std::string& next = {}) { throw Refused(code, message, next); }

QString text(const json& args, const char* key) {
  return args.contains(key) && args[key].is_string() ? QString::fromStdString(args[key].get<std::string>()) : QString();
}
std::string str(const QString& s) { return s.toStdString(); }
std::filesystem::path fsPath(const QString& file) { return std::filesystem::path(file.toStdU16String()); }

const QString kNoDiscard = QStringLiteral(
    " Discarding changes (git restore, checkout --, stash, reset) is not offered: if it is truly needed, it can be done with the git CLI, but only after asking "
    "the user and getting explicit confirmation.");

struct Repo {
  git::Context c;
  QString top, gitDir;
  git::Status status;
  bool merging = false, rebasing = false;
  QString branch() const { return status.branch == "(detached)" ? QString() : status.branch; }
  bool unborn() const { return status.oid == "(initial)"; }
  bool tracked() const {  // uncommitted changes of tracked files (staged or not, or in conflict)
    for (const git::Entry& e : status.entries)
      if (e.kind == '1' || e.kind == '2' || e.kind == 'u') return true;
    return false;
  }
  QStringList conflicts() const {
    QStringList out;
    for (const git::Entry& e : status.entries)
      if (e.kind == 'u') out << e.path;
    return out;
  }
};

git::RunOptions quick() {
  git::RunOptions o;
  o.timeoutMs = 30000;
  o.optionalLocks = false;
  return o;
}

git::RunOptions slow(const Call& call) {  // a merge's driver on a big document, a commit hashing one: no overall limit
  git::RunOptions o;
  o.timeoutMs = 0;
  o.cancelled = call.cancelled;
  return o;
}

git::RunOptions network(const Call& call) {
  git::RunOptions o = git::RunOptions::network();
  o.cancelled = call.cancelled;
  if (call.progress) o.progress = [p = call.progress](const QString& phase, int percent) { p(percent < 0 ? phase : QStringLiteral("%1 (%2%)").arg(phase).arg(percent)); };
  return o;
}

void readStatus(Repo& r) {
  const git::Result s = git::run(r.c, {"status", "--porcelain=v2", "--branch", "-z"}, quick());
  if (!s.ok()) refuse("git_failed", s.error());
  r.status = git::parseStatus(s.out);
  r.merging = QFileInfo::exists(r.gitDir + "/MERGE_HEAD");
  r.rebasing = QFileInfo::exists(r.gitDir + "/rebase-merge") || QFileInfo::exists(r.gitDir + "/rebase-apply");
}

Repo open(const Call& call) {
  const QString program = call.program.isEmpty() ? git::findProgram() : call.program;
  if (program.isEmpty())
    refuse("git_missing", "git was not found on this computer. The user can install it, or point OPAD at it (Version control panel > Locate git).");
  if (call.repo.isEmpty()) refuse("invalid_arguments", "Name the repository: repo is a folder or a file inside it.");
  const QFileInfo fi(call.repo);
  if (!fi.isAbsolute()) refuse("invalid_arguments", "repo must be an absolute path.");
  if (!fi.exists()) refuse("not_found", QStringLiteral("%1 does not exist.").arg(QDir::toNativeSeparators(call.repo)));
  const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
  Repo r;
  r.c = git::contextFor(program, dir, QString());  // no askpass: an agent's command never opens a sign-in dialog
  const git::Result where = git::run(r.c, {"rev-parse", "--show-toplevel", "--absolute-git-dir"}, quick());
  if (!where.ok()) {
    const QString err = QString::fromUtf8(where.err);
    if (err.contains("not a git repository"))
      refuse("not_a_repository", QStringLiteral("%1 is not in a git repository. Setting one up is the user's call (OPAD's Version control panel > Set up repository).")
                                     .arg(QDir::toNativeSeparators(dir)));
    refuse("git_failed", where.error());
  }
  const QStringList lines = QString::fromUtf8(where.out).split('\n', Qt::SkipEmptyParts);
  if (lines.size() < 2) refuse("git_failed", "git rev-parse said: " + QString::fromUtf8(where.out).trimmed());
  r.top = QDir::cleanPath(lines[0].trimmed());
  r.gitDir = QDir::cleanPath(lines[1].trimmed());
  r.c.dir = r.top;
  readStatus(r);
  return r;
}

// A revision or branch name from an agent: never an option, never a pathspec or a range of something else.
QString revision(const QString& value, const char* what) {
  static const QRegularExpression bad(QStringLiteral("[\\s\\x00-\\x1f\\x7f]"));
  if (value.isEmpty() || value.startsWith('-') || value.contains(bad) || value.size() > 250)
    refuse("invalid_arguments", QStringLiteral("%1 \"%2\" is not a branch or revision name.").arg(QString::fromLatin1(what), value));
  return value;
}

// A path from an agent, relative to the repository's top ('/' separated); absolute ones must lie inside it.
QString relative(const Repo& r, const QString& path) {
  if (path.isEmpty()) refuse("invalid_arguments", "An empty path.");
  const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path));
  QString rel = QFileInfo(cleaned).isAbsolute() ? QDir(r.top).relativeFilePath(cleaned) : cleaned;
  if (rel.isEmpty() || rel == "." || rel == ".." || rel.startsWith("../") || QFileInfo(rel).isAbsolute())
    refuse("invalid_arguments", QStringLiteral("%1 is not inside the repository %2.").arg(path, QDir::toNativeSeparators(r.top)));
  return rel;
}

void documentSaved(const Call& call, const char* what) {
  if (!call.document.isEmpty() && call.documentDirty)
    refuse("unsaved_document",
           QStringLiteral("The open document %1 has unsaved changes and %2 rewrites it: save them first (the save tool), or ask the user.").arg(QFileInfo(call.document).fileName(), QString::fromLatin1(what)),
           "save");
}

void clean(const Repo& r, const char* what) {
  if (r.merging) refuse("merge_in_progress", "A merge is in progress: settle it first (git_resolve and git_commit, or git_merge_abort).", "git_status");
  if (r.rebasing) refuse("rebase_in_progress", "A rebase is in progress (started outside OPAD): the user has to finish or abort it with the git CLI.");
  if (r.tracked())
    refuse("uncommitted_changes",
           QStringLiteral("Files have uncommitted changes and %1 would carry or overwrite them: commit them first (git_commit, on a branch when this one is protected).").arg(QString::fromLatin1(what)) +
               kNoDiscard,
           "git_status");
}

[[noreturn]] void refuseProtected(const Repo& r, const char* setting, const QString& preference, const QString& doing) {
  refuse("protected_branch",
         QStringLiteral("Refused: %1 is a protected branch and the user's OPAD preference \"%2\" is on (Preferences > Version control > Branch protection; "
                        "setting %3), so agents may not %4. Work on a branch instead: git_branch_create makes one and switches to it, commit there, and "
                        "let the user merge it (or ask the user to change the preference).")
             .arg(r.branch(), preference, QString::fromLatin1(setting), doing),
         "git_branch_create");
}

[[noreturn]] void gitFailed(const git::Result& res) {
  if (res.cancelled) refuse("cancelled", "Cancelled.");
  const QString err = QString::fromUtf8(res.err + res.out);
  if (err.contains("Please tell me who you are") || err.contains("Author identity unknown") || err.contains("empty ident name"))
    refuse("identity_missing", res.error() + " Ask the user to set it (OPAD asks for it on the first commit in the Version control panel; or git config user.name and user.email).");
  refuse("git_failed", res.error());
}

git::Result must(const Repo& r, const QStringList& args, const git::RunOptions& o = quick()) {
  git::Result res = git::run(r.c, args, o);
  if (!res.ok()) gitFailed(res);
  return res;
}

const char* changeWord(char c) {
  switch (c) {
    case 'M': return "modified";
    case 'A': return "added";
    case 'D': return "deleted";
    case 'R': return "renamed";
    case 'C': return "copied";
    case 'T': return "type_changed";
    case 'U': return "unmerged";
    default: return "changed";
  }
}

json commitJson(const git::Commit& c) {
  json refs = json::array();
  for (const QString& ref : c.refs) refs.push_back(str(ref));
  json parents = json::array();
  for (const QString& p : c.parents) parents.push_back(str(p));
  return {{"hash", str(c.hash)}, {"short", str(c.shortHash)}, {"author", str(c.author)}, {"email", str(c.email)}, {"date", str(c.date)},
          {"subject", str(c.subject)}, {"parents", parents}, {"refs", refs}};
}

json statusJson(const Repo& r, const Policy& p) {
  constexpr size_t kMax = 500;
  json staged = json::array(), unstaged = json::array(), untracked = json::array(), conflicts = json::array();
  size_t nStaged = 0, nUnstaged = 0, nUntracked = 0;
  for (const git::Entry& e : r.status.entries) {
    if (e.kind == '!') continue;
    if (e.kind == '?') {
      if (nUntracked++ < kMax) untracked.push_back(str(e.path));
      continue;
    }
    if (e.kind == 'u') {
      conflicts.push_back(str(e.path));
      continue;
    }
    json item = {{"path", str(e.path)}};
    if (!e.orig.isEmpty()) item["from"] = str(e.orig);
    if (e.x != '.' && nStaged++ < kMax) {
      item["change"] = changeWord(e.x);
      staged.push_back(item);
    }
    if (e.y != '.' && nUnstaged++ < kMax) {
      item["change"] = changeWord(e.y);
      unstaged.push_back(item);
    }
  }
  const QString branch = r.branch();
  json out = {{"repo", str(QDir::toNativeSeparators(r.top))},
              {"branch", branch.isEmpty() ? json(nullptr) : json(str(branch))},
              {"detached", branch.isEmpty()},
              {"head", r.unborn() ? json(nullptr) : json(str(r.status.oid))},
              {"upstream", r.status.upstream.isEmpty() ? json(nullptr) : json(str(r.status.upstream))},
              {"ahead", r.status.ahead},
              {"behind", r.status.behind},
              {"protected", p.protects(branch)},
              {"merging", r.merging},
              {"rebasing", r.rebasing},
              {"clean", nStaged + nUnstaged + nUntracked + conflicts.size() == 0},
              {"staged", staged},
              {"unstaged", unstaged},
              {"untracked", untracked},
              {"conflicts", conflicts},
              {"policy", p.to_json()}};
  if (r.unborn()) out["no_commits_yet"] = true;
  if (nStaged > kMax) out["staged_total"] = nStaged;
  if (nUnstaged > kMax) out["unstaged_total"] = nUnstaged;
  if (nUntracked > kMax) out["untracked_total"] = nUntracked;
  return out;
}

// OPAD's driver for this merge: the repository's own config when it has one, else this installation's, for this command
// only (-c: nothing is written). Warnings when .gitattributes does not route .opad files to it.
QStringList driverOptions(const Repo& r, json& warnings) {
  QStringList options;
  const git::Result configured = git::run(r.c, {"config", "--get", "merge.opad.driver"}, quick());
  if (!configured.ok() || configured.out.trimmed().isEmpty()) {
    const git::Install in = git::Install::here();
    if (!in.cli.isEmpty() || !in.app.isEmpty())
      options << "-c" << QStringLiteral("merge.opad.name=OPAD record-aware merge") << "-c" << "merge.opad.driver=" + in.mergeDriver();
  }
  const git::Result attr = git::run(r.c, {"check-attr", "-z", "merge", "--", "x.opad"}, quick());
  const QList<QByteArray> f = attr.out.split('\0');
  if (!attr.ok() || f.size() < 3 || f[2] != "opad")
    warnings.push_back("This repository's .gitattributes does not route .opad files to OPAD's merge driver (a line '*.opad text eol=lf merge=opad diff=opad'), so git "
                       "merges them as plain text. The user can set the repository up from OPAD's Version control panel.");
  return options;
}

std::string blob(const Repo& r, const QString& rev, const QString& rel) {
  if (rev.isEmpty()) return {};
  try {
    return git::show(r.c, rev, rel).toStdString();
  } catch (const std::exception&) {
    return {};  // not there
  }
}

QString nameIn(const opad::Scene& s, const std::string& target) {
  if (target.rfind("parameter:", 0) == 0) return QString::fromStdString(target.substr(10));
  if (const opad::Node* n = s.node(target)) return QString::fromStdString(n->name);
  if (const opad::Feature* f = s.feature(target)) return QString::fromStdString(f->name);
  if (const opad::SketchItem* k = s.sketch(target)) return QString::fromStdString(k->name);
  for (const opad::Param& p : s.params)
    if (p.id == target) return QString::fromStdString(p.name);
  return QString::fromStdString(target.substr(0, 8));
}

json conflictJson(const opad::MergeConflict& c, const opad::Scene& scene) {
  return {{"target", c.target}, {"field", c.field}, {"name", str(nameIn(scene, c.target))}, {"ours_op", c.ours}, {"theirs_op", c.theirs}};
}

// What merging `target` into HEAD brings (as the panel's incoming preview, VersionControl.cpp): the commits, and each
// .opad they change as OPAD's driver would merge it.
json incoming(const Repo& r, const QString& target, const QString& label, const Call& call) {
  const QString head = git::revParse(r.c, "HEAD"), theirs = git::revParse(r.c, target);
  if (theirs.isEmpty()) refuse("unknown_revision", QStringLiteral("%1 is not a branch or commit of this repository.").arg(label), "git_branches");
  const git::Result mb = head.isEmpty() ? git::Result() : git::run(r.c, {"merge-base", "HEAD", theirs}, quick());
  const QString base = mb.ok() ? QString::fromLatin1(mb.out).trimmed() : QString();
  const auto commits = git::log(r.c, head.isEmpty() ? theirs : "HEAD.." + theirs, {}, 50);
  json list = json::array();
  for (const git::Commit& c : commits) list.push_back({{"hash", str(c.hash)}, {"short", str(c.shortHash)}, {"subject", str(c.subject)}, {"author", str(c.author)}, {"date", str(c.date)}});
  json out = {{"source", str(label)}, {"head", str(head)}, {"theirs", str(theirs)}, {"base", base.isEmpty() ? json(nullptr) : json(str(base))},
              {"commits", list}, {"up_to_date", commits.empty()}, {"fast_forward", !commits.empty() && base == head}};
  if (commits.size() == 50) {
    const git::Result count = git::run(r.c, {"rev-list", "--count", head.isEmpty() ? theirs : "HEAD.." + theirs}, quick());
    if (count.ok()) out["commits_total"] = QString::fromLatin1(count.out).trimmed().toInt();
  }
  if (commits.empty()) return out;
  if (base.isEmpty() && !head.isEmpty()) {
    out["unrelated"] = true;
    out["stops"] = true;
    return out;
  }
  json documents = json::array();
  bool stops = false;
  const git::Result names = git::run(r.c, {"diff", "--name-only", "-z", base.isEmpty() ? theirs : base, theirs, "--"}, quick());
  int n = 0;
  for (const QByteArray& raw : names.out.split('\0')) {
    const QString rel = QString::fromUtf8(raw);
    if (!rel.endsWith(".opad", Qt::CaseInsensitive)) continue;
    if (++n > 10) {
      out["documents_truncated"] = true;
      break;
    }
    if (call.cancelled && call.cancelled()) refuse("cancelled", "Cancelled.");
    if (call.progress) call.progress(QStringLiteral("Reading %1").arg(QFileInfo(rel).fileName()));
    std::string b = blob(r, base, rel), o = blob(r, head, rel), t = blob(r, theirs, rel);
    json d = {{"path", str(rel)}, {"changed_here", o != b}};
    if (b.empty() || o.empty() || t.empty()) {
      d["whole_file"] = o.empty() ? "comes in new" : t.empty() ? "they delete it" : "both sides added it";
      if (!o.empty() && o != b) {
        d["conflicts"] = json::array();
        stops = true;
      }
      documents.push_back(d);
      continue;
    }
    const auto origin = fsPath(QDir(r.top).filePath(rel));
    const opad::Document baseDoc = opad::Document::parse_index(b, origin), oursDoc = opad::Document::parse_index(o, origin),
                         theirsDoc = opad::Document::parse_index(t, origin);
    d["regenerate_after"] = opad::changes_design(baseDoc, oursDoc) && opad::changes_design(baseDoc, theirsDoc);
    std::string merged;
    if (o == b) merged = t;
    else {
      const opad::FileMerge m = opad::merge_files(b, o, t);
      if (!m.error.empty()) {
        stops = true;
        d["stops"] = m.error;
        const opad::Scene scene = opad::resolve(oursDoc);
        json conflicts = json::array();
        for (const auto& c : m.conflicts)
          if (conflicts.size() < 100) conflicts.push_back(conflictJson(c, scene));
        d["conflicts"] = conflicts;
      } else {
        merged = m.text();
      }
    }
    if (!merged.empty()) {
      const opad::Document after = opad::Document::parse_index(std::move(merged), origin);
      d["summary"] = opad::semantic_diff(oursDoc, after).value("summary", "");
    } else {
      d["their_changes"] = opad::semantic_diff(baseDoc, theirsDoc).value("summary", "");
    }
    documents.push_back(d);
  }
  out["documents"] = documents;
  out["stops"] = stops;
  return out;
}

json doMerge(Repo& r, const QString& target, const QString& label, const QString& ff, const QString& message, const Call& call) {
  json warnings = json::array();
  QStringList args = driverOptions(r, warnings);
  args << "merge" << "--no-edit";
  if (ff == "no_ff") args << "--no-ff";
  else if (ff == "only") args << "--ff-only";
  if (!message.isEmpty()) args << "-m" << message;
  args << target;
  const QString before = git::revParse(r.c, "HEAD"), theirs = git::revParse(r.c, target);
  if (call.progress) call.progress(QStringLiteral("Merging %1").arg(label));
  const git::Result res = git::run(r.c, args, slow(call));
  readStatus(r);
  json out = {{"source", str(label)}, {"branch", str(r.branch())}};
  if (!warnings.empty()) out["warnings"] = warnings;
  if (res.ok()) {
    const QString after = git::revParse(r.c, "HEAD");
    out["state"] = after == before ? "up_to_date" : "merged";
    out["head"] = str(after);
    out["fast_forward"] = after == theirs && after != before;
    return out;
  }
  if (res.cancelled) refuse("cancelled", "Cancelled.");
  if (r.merging) {  // stopped on conflicts: left for git_resolve, git_commit or git_merge_abort
    json conflicts = json::array();
    for (const QString& path : r.conflicts()) conflicts.push_back(str(path));
    out["state"] = "conflicts";
    out["conflicts"] = conflicts;
    out["message"] = str(res.error());
    out["next"] = "git_resolve each conflicted file (call it without keep or choices to list an .opad's conflicts), then git_commit; or git_merge_abort.";
    return out;
  }
  if (ff == "only" && QString::fromUtf8(res.err).contains("Not possible to fast-forward"))
    refuse("not_fast_forward", QStringLiteral("%1 cannot be fast-forwarded: both sides have commits of their own.").arg(label));
  gitFailed(res);
}

// ---------------------------------------------------------------- the tools
json status(const Call& call) {
  Repo r = open(call);
  return statusJson(r, call.policy);
}

json log(const json& args, const Call& call) {
  Repo r = open(call);
  const int limit = args.value("limit", 20), skip = args.value("skip", 0);
  QString range = text(args, "branch");
  if (!range.isEmpty()) {
    revision(range, "branch");
    if (git::revParse(r.c, range).isEmpty()) refuse("unknown_revision", QStringLiteral("%1 is not a branch or commit of this repository.").arg(range), "git_branches");
  }
  const QString path = text(args, "path").isEmpty() ? QString() : relative(r, text(args, "path"));
  const auto commits = git::log(r.c, range, path, limit, skip);
  json list = json::array();
  for (const auto& c : commits) list.push_back(commitJson(c));
  json out = {{"commits", list}, {"branch", range.isEmpty() ? str(r.branch().isEmpty() ? QStringLiteral("HEAD") : r.branch()) : str(range)}};
  if (!path.isEmpty()) out["path"] = str(path);
  if (int(commits.size()) == limit) out["next_skip"] = skip + limit;
  return out;
}

json branches(const Call& call) {
  Repo r = open(call);
  json list = json::array();
  for (const git::Branch& b : git::branches(r.c)) {
    json item = {{"name", str(b.name)}, {"remote", b.remote}, {"current", b.head}, {"head", str(b.oid.left(12))}, {"subject", str(b.subject)}, {"date", str(b.date)}};
    if (!b.remote) {
      item["protected"] = call.policy.protects(b.name);
      if (!b.upstream.isEmpty()) {
        item["upstream"] = str(b.upstream);
        item["ahead"] = b.ahead;
        item["behind"] = b.behind;
        if (b.gone) item["upstream_gone"] = true;
      }
    }
    list.push_back(item);
  }
  return {{"current", r.branch().isEmpty() ? json(nullptr) : json(str(r.branch()))}, {"branches", list}, {"remotes", [&] {
             json remotes = json::array();
             for (const QString& name : git::remotes(r.c)) remotes.push_back(str(name));
             return remotes;
           }()},
          {"policy", call.policy.to_json()}};
}

QString remoteOf(const Repo& r, const QString& given) {
  const QStringList remotes = git::remotes(r.c);
  if (!given.isEmpty()) {
    revision(given, "remote");
    if (!remotes.contains(given)) refuse("unknown_remote", QStringLiteral("This repository has no remote %1 (it has: %2).").arg(given, remotes.isEmpty() ? QStringLiteral("none") : remotes.join(", ")));
    return given;
  }
  if (!r.status.upstream.isEmpty()) {
    QString remote = r.status.upstream.section('/', 0, 0);
    for (const QString& name : remotes)  // a remote's name may hold a '/'
      if (r.status.upstream.startsWith(name + '/')) remote = name;
    return remote;
  }
  if (remotes.contains("origin")) return QStringLiteral("origin");
  if (remotes.size() == 1) return remotes.front();
  if (remotes.isEmpty())
    refuse("no_remote", "This repository has no remote. Adding one is the user's call (OPAD's Version control panel asks for it on the first Push, or git remote add).");
  refuse("invalid_arguments", QStringLiteral("Several remotes (%1): name one with remote.").arg(remotes.join(", ")));
}

json fetch(const json& args, const Call& call) {
  Repo r = open(call);
  const QString given = text(args, "remote");
  QStringList fetchArgs{"fetch", "--progress"};
  if (given.isEmpty()) fetchArgs << "--all";
  else fetchArgs << remoteOf(r, given);
  must(r, fetchArgs, network(call));
  readStatus(r);
  return {{"fetched", given.isEmpty() ? "all remotes" : str(given)}, {"branch", str(r.branch())}, {"upstream", str(r.status.upstream)}, {"ahead", r.status.ahead}, {"behind", r.status.behind}};
}

json branchCreate(const json& args, const Call& call) {
  Repo r = open(call);
  const QString name = text(args, "name");
  if (!git::validBranchName(name)) refuse("invalid_arguments", git::explain("is not a valid branch name") + " (\"" + name + "\")");
  if (!git::revParse(r.c, "refs/heads/" + name).isEmpty()) refuse("branch_exists", QStringLiteral("A branch %1 exists already: git_switch to it, or choose another name.").arg(name), "git_switch");
  // Making a protected branch at a commit of the agent's choosing puts commits on it as a commit or merge would: refused
  // while either protection is on. Starting a branch from a protected one (from: main) is fine.
  if ((call.policy.commits || call.policy.merges) && call.policy.protects(name)) {
    const bool commits = call.policy.commits;
    refuse("protected_branch",
           QStringLiteral("Refused: %1 is a protected branch and the user's OPAD preference \"%2\" is on (Preferences > Version control > Branch protection; setting %3), so "
                          "agents may not create it: a protected branch made at a commit of the agent's choosing would hold commits no commit or merge put there. "
                          "Nothing was changed. Choose another name (git_branch_create makes a branch from %1 too: from: %1), or ask the user to create it.")
               .arg(name, commits ? QStringLiteral("Refuse commits to a protected branch") : QStringLiteral("Refuse merges into a protected branch"),
                    QString::fromLatin1(commits ? kCommitsKey : kMergesKey)),
           "git_branch_create");
  }
  const bool go = args.value("switch", true);
  QString from = text(args, "from"), at;
  if (!from.isEmpty()) {
    revision(from, "from");
    at = git::revParse(r.c, from);
    if (at.isEmpty()) refuse("unknown_revision", QStringLiteral("%1 is not a branch or commit of this repository.").arg(from), "git_branches");
  }
  const QString head = git::revParse(r.c, "HEAD");
  const bool here = at.isEmpty() || at == head;
  if (!go) {
    if (r.unborn() && at.isEmpty()) refuse("no_commits", "Commit first: a branch starts at a commit.");
    must(r, {"branch", name, here ? QStringLiteral("HEAD") : at});
  } else if (here) {  // the files stay as they are: nothing to carry or overwrite
    if (r.merging) refuse("merge_in_progress", "A merge is in progress: settle it first (git_resolve and git_commit, or git_merge_abort).");
    must(r, {"switch", "-q", "-c", name});
  } else {
    clean(r, "switching to a branch at another commit");
    documentSaved(call, "switching");
    must(r, {"switch", "-q", "-c", name, at});
  }
  readStatus(r);
  return {{"created", str(name)}, {"at", r.unborn() ? json(nullptr) : json(str(git::revParse(r.c, "refs/heads/" + name)))}, {"switched", go}, {"branch", str(r.branch())},
          {"protected", call.policy.protects(name)}};
}

json changedFiles(const Repo& r, const QString& before, const QString& after) {
  json out = {{"count", 0}, {"documents", json::array()}};
  if (before.isEmpty() || after.isEmpty() || before == after) return out;
  const git::Result names = git::run(r.c, {"diff", "--name-only", "-z", before, after, "--"}, quick());
  int count = 0;
  for (const QByteArray& raw : names.out.split('\0')) {
    if (raw.isEmpty()) continue;
    ++count;
    const QString rel = QString::fromUtf8(raw);
    if (rel.endsWith(".opad", Qt::CaseInsensitive) && out["documents"].size() < 100) out["documents"].push_back(str(rel));
  }
  out["count"] = count;
  return out;
}

json switchTo(const json& args, const Call& call) {
  Repo r = open(call);
  const QString branch = revision(text(args, "branch"), "branch");
  QStringList sw{"switch", "-q"};
  QString local = branch;
  if (!git::revParse(r.c, "refs/heads/" + branch).isEmpty()) {
    sw << branch;
  } else if (!git::revParse(r.c, "refs/remotes/" + branch).isEmpty()) {  // its local branch, following it
    local = branch.section('/', 1, -1);
    if (!git::revParse(r.c, "refs/heads/" + local).isEmpty()) sw << local;
    else sw << "-c" << local << "--track" << branch;
  } else {
    refuse("unknown_branch", QStringLiteral("There is no branch %1 (git_branches lists them; git_branch_create makes one).").arg(branch), "git_branches");
  }
  if (local == r.branch()) return {{"state", "unchanged"}, {"branch", str(local)}};
  clean(r, "switching");
  documentSaved(call, "switching");
  const QString before = git::revParse(r.c, "HEAD");
  must(r, sw);
  readStatus(r);
  return {{"state", "switched"}, {"branch", str(r.branch())}, {"head", str(r.status.oid)}, {"protected", call.policy.protects(r.branch())},
          {"files_changed", changedFiles(r, before, r.status.oid)}};
}

json commit(const json& args, const Call& call) {
  Repo r = open(call);
  const QString message = text(args, "message").trimmed();
  if (message.isEmpty()) refuse("invalid_arguments", "The commit message is empty.");
  const bool all = args.value("all", false);
  const bool hasPaths = args.contains("paths");
  if (all == hasPaths) refuse("invalid_arguments", all ? "Give paths or all=true, not both." : "Name the paths to commit, or pass all=true for every change.");
  if (r.branch().isEmpty()) refuse("detached_head", "HEAD names no branch: commits there are easily lost. git_branch_create makes a branch here first.", "git_branch_create");
  if (r.rebasing) refuse("rebase_in_progress", "A rebase is in progress (started outside OPAD): the user has to finish or abort it with the git CLI.");
  if (call.policy.commits && call.policy.protects(r.branch())) refuseProtected(r, kCommitsKey, "Refuse commits to a protected branch", "commit to it");
  QStringList rels;
  if (hasPaths)
    for (const auto& p : args["paths"]) rels << relative(r, QString::fromStdString(p.get<std::string>()));
  rels.removeDuplicates();
  if (!call.document.isEmpty() && call.documentDirty) {
    const QString doc = relative(r, call.document);
    if (all || rels.contains(doc))
      refuse("unsaved_document", QStringLiteral("The open document %1 has unsaved changes: save them first (the save tool), then commit.").arg(QFileInfo(call.document).fileName()), "save");
  }
  if (!r.conflicts().isEmpty() && (all || std::any_of(rels.begin(), rels.end(), [&](const QString& p) { return r.conflicts().contains(p); }) || r.merging))
    refuse("conflicts", QStringLiteral("Files are still in conflict (%1): settle them with git_resolve first, or git_merge_abort.").arg(r.conflicts().join(", ")), "git_resolve");
  if (call.progress) call.progress(QStringLiteral("Adding the files"));
  const git::RunOptions o = slow(call);
  if (all) must(r, {"add", "-A"}, o);
  else must(r, QStringList{"--literal-pathspecs", "add", "--"} + rels, o);
  if (!r.merging) {  // an empty commit is refused; a merge commit may change nothing and is still the merge
    const QStringList quiet = all ? QStringList{"diff", "--cached", "--quiet"} : QStringList{"--literal-pathspecs", "diff", "--cached", "--quiet", "--"} + rels;
    const git::Result d = git::run(r.c, quiet, o);
    if (d.ok()) refuse("nothing_to_commit", all ? QStringLiteral("Nothing to commit: every file is as committed.") : QStringLiteral("Nothing to commit: the named files are as committed."));
    if (d.code != 1) gitFailed(d);
  }
  if (call.progress) call.progress(QStringLiteral("Writing the commit"));
  git::RunOptions write = o;
  write.input = message.toUtf8();
  if (r.merging || all) must(r, {"commit", "-q", "-F", "-"}, write);
  else must(r, QStringList{"--literal-pathspecs", "commit", "-q", "-F", "-", "--"} + rels, write);
  readStatus(r);
  json files = json::array();
  const git::Result shown = git::run(r.c, {"show", "--name-status", "-z", "--format=", "HEAD"}, quick());
  const QList<QByteArray> parts = shown.out.split('\0');
  for (int i = 0; i + 1 < parts.size() && files.size() < 200; i += 2) {
    QByteArray change = parts[i];
    if (change.isEmpty()) break;
    json f = {{"change", changeWord(change[0])}, {"path", str(QString::fromUtf8(parts[i + 1]))}};
    if ((change[0] == 'R' || change[0] == 'C') && i + 2 < parts.size()) {
      f["from"] = f["path"];
      f["path"] = str(QString::fromUtf8(parts[i + 2]));
      ++i;
    }
    files.push_back(f);
  }
  return {{"state", "committed"}, {"hash", str(r.status.oid)}, {"short", str(r.status.oid.left(7))}, {"branch", str(r.branch())},
          {"subject", str(message.section('\n', 0, 0))}, {"files", files}, {"ahead", r.status.ahead}};
}

json merge(const json& args, const Call& call) {
  Repo r = open(call);
  const QString source = revision(text(args, "source"), "source");
  if (r.branch().isEmpty()) refuse("detached_head", "HEAD names no branch: switch to the branch to merge into first (git_switch).", "git_switch");
  if (r.unborn()) refuse("no_commits", "This branch has no commits yet: commit first.");
  if (args.value("preview", false)) {
    json out = incoming(r, source, source, call);
    out["state"] = "preview";
    out["into"] = str(r.branch());
    out["protected"] = call.policy.protects(r.branch());
    return out;
  }
  if (call.policy.merges && call.policy.protects(r.branch())) refuseProtected(r, kMergesKey, "Refuse merges into a protected branch", "merge into it");
  clean(r, "a merge");
  documentSaved(call, "a merge");
  const json preview = incoming(r, source, source, call);
  if (preview.value("up_to_date", false)) return {{"state", "up_to_date"}, {"source", str(source)}, {"branch", str(r.branch())}};
  json out = doMerge(r, source, source, text(args, "ff").isEmpty() ? QStringLiteral("auto") : text(args, "ff"), text(args, "message"), call);
  out["preview"] = preview;
  return out;
}

json mergeAbort(const Call& call) {
  Repo r = open(call);
  if (!r.merging) refuse("no_merge", "No merge is in progress.");
  documentSaved(call, "aborting the merge");
  must(r, {"merge", "--abort"});
  readStatus(r);
  return {{"state", "aborted"}, {"branch", str(r.branch())}, {"head", str(r.status.oid)}};
}

void writeFile(const Repo& r, const QString& rel, const std::string& bytes) {
  QSaveFile out(QDir(r.top).filePath(rel));
  if (!out.open(QIODevice::WriteOnly) || out.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !out.commit())
    refuse("write_failed", QStringLiteral("Could not write %1: %2").arg(rel, out.errorString()));
}

json resolve(const json& args, const Call& call) {
  Repo r = open(call);
  if (!r.merging) refuse("no_merge", "No merge is in progress.");
  const QString rel = relative(r, text(args, "path"));
  if (!r.conflicts().contains(rel)) refuse("not_conflicted", QStringLiteral("%1 is not in conflict (in conflict: %2).").arg(rel, r.conflicts().isEmpty() ? QStringLiteral("nothing") : r.conflicts().join(", ")));
  const QString keep = text(args, "keep");
  const bool hasChoices = args.contains("choices"), whole = args.value("whole_file", false);
  const bool listing = keep.isEmpty() && !hasChoices;
  if (!listing) documentSaved(call, "resolving");
  if (whole && keep.isEmpty()) refuse("invalid_arguments", "whole_file needs keep: ours or theirs.");
  if (hasChoices && !keep.isEmpty()) refuse("invalid_arguments", "Give keep or choices, not both.");
  std::string base = blob(r, ":1", rel), ours = blob(r, ":2", rel), theirs = blob(r, ":3", rel);
  const bool document = rel.endsWith(".opad", Qt::CaseInsensitive) && !base.empty() && !ours.empty() && !theirs.empty();
  std::string written;
  if (document && !whole) {
    if (call.progress) call.progress(QStringLiteral("Merging %1").arg(QFileInfo(rel).fileName()));
    opad::FileMerge m = opad::merge_files(base, ours, theirs, true);
    if (!m.error.empty()) {
      if (listing)
        return {{"path", str(rel)}, {"mergeable", false}, {"reason", m.error}, {"next", "Keep one side's whole file: git_resolve with keep and whole_file=true."}};
      refuse("not_mergeable", QStringLiteral("The two versions of %1 cannot be merged (%2): keep one side's whole file (keep with whole_file=true).").arg(rel, QString::fromStdString(m.error)));
    }
    const auto origin = fsPath(QDir(r.top).filePath(rel));
    if (listing) {
      const opad::Document oursDoc = opad::Document::parse_index(ours, origin), theirsDoc = opad::Document::parse_index(theirs, origin), baseDoc = opad::Document::parse_index(base, origin);
      const opad::Scene scene = opad::resolve(oursDoc);
      json list = json::array();
      for (size_t i = 0; i < m.conflicts.size() && i < 1000; ++i) {
        const auto& c = m.conflicts[i];
        json item = conflictJson(c, scene);
        item["index"] = i;
        if (const opad::Op* op = oursDoc.find_op(c.ours)) item["ours"] = {{"type", op->type}, {"by", op->data.value("by", "")}, {"effect", opad::op_effect(op->data, c.target, c.field)}};
        if (const opad::Op* op = theirsDoc.find_op(c.theirs)) item["theirs"] = {{"type", op->type}, {"by", op->data.value("by", "")}, {"effect", opad::op_effect(op->data, c.target, c.field)}};
        list.push_back(item);
      }
      return {{"path", str(rel)}, {"mergeable", true}, {"conflicts", list}, {"conflicts_total", m.conflicts.size()},
              {"regenerate_after", opad::changes_design(baseDoc, oursDoc) && opad::changes_design(baseDoc, theirsDoc)},
              {"next", "git_resolve with keep (ours|theirs for every conflict) or choices (one per index); both sides' other changes are kept."}};
    }
    std::vector<bool> mine;
    if (hasChoices) {
      if (args["choices"].size() != m.conflicts.size())
        refuse("invalid_arguments", QStringLiteral("choices has %1 entries for %2 conflicts (list them: git_resolve without keep or choices).").arg(args["choices"].size()).arg(m.conflicts.size()));
      for (const auto& c : args["choices"]) mine.push_back(c == "ours");
    } else {
      mine.assign(m.conflicts.size(), keep == "ours");
    }
    written = m.conflicts.empty() ? m.text() : opad::resolve_merge(m.text(), m.conflicts, mine, "Agent");
  } else {
    if (listing)
      return {{"path", str(rel)}, {"mergeable", false}, {"ours_exists", !ours.empty()}, {"theirs_exists", !theirs.empty()},
              {"next", "git_resolve with keep: ours or theirs takes that side's whole version."}};
    if (hasChoices) refuse("invalid_arguments", "choices are for an .opad document both sides changed; this file takes keep: ours or theirs.");
    written = keep == "ours" ? ours : theirs;
    if (written.empty())
      refuse("side_deleted", QStringLiteral("The %1 side deleted %2: settling that is the user's call (the Version control panel, or the git CLI after asking).").arg(keep, rel));
  }
  writeFile(r, rel, written);
  must(r, {"add", "--", rel});
  readStatus(r);
  json left = json::array();
  for (const QString& p : r.conflicts()) left.push_back(str(p));
  return {{"state", "resolved"}, {"path", str(rel)}, {"kept", keep.isEmpty() ? "choices" : str(keep)}, {"remaining_conflicts", left},
          {"next", left.empty() ? "git_commit (it commits the merge as a whole)." : "git_resolve the remaining files."}};
}

json pull(const json& args, const Call& call) {
  Repo r = open(call);
  if (r.branch().isEmpty()) refuse("detached_head", "HEAD names no branch.", "git_switch");
  if (r.status.upstream.isEmpty())
    refuse("no_upstream", QStringLiteral("Branch %1 follows no remote branch yet: git_push sets one up.").arg(r.branch()), "git_push");
  const bool preview = args.value("preview", false), ffOnly = args.value("ff_only", true);
  if (r.merging) refuse("merge_in_progress", "A merge is in progress: settle it first (git_resolve and git_commit, or git_merge_abort).");
  const QString remote = remoteOf(r, {});
  if (call.progress) call.progress(QStringLiteral("Fetching from %1").arg(remote));
  must(r, {"fetch", "--progress", remote}, network(call));
  readStatus(r);
  json in = incoming(r, "@{u}", r.status.upstream, call);
  if (preview) {
    in["state"] = "preview";
    in["into"] = str(r.branch());
    return in;
  }
  if (in.value("up_to_date", false)) return {{"state", "up_to_date"}, {"branch", str(r.branch())}, {"upstream", str(r.status.upstream)}};
  const bool ff = in.value("fast_forward", false);
  if (!ff && ffOnly)
    refuse("not_fast_forward",
           QStringLiteral("%1 and %2 both have commits of their own, so this is no fast-forward. git_pull with ff_only=false merges them (a merge commit), or merge on a branch.")
               .arg(r.branch(), r.status.upstream),
           "git_pull");
  if (!ff && call.policy.merges && call.policy.protects(r.branch())) refuseProtected(r, kMergesKey, "Refuse merges into a protected branch", "merge into it");
  clean(r, "a pull");
  documentSaved(call, "a pull");
  json out = doMerge(r, "@{u}", r.status.upstream, ffOnly ? QStringLiteral("only") : QStringLiteral("auto"), {}, call);
  out["preview"] = in;
  return out;
}

json push(const json& args, const Call& call) {
  Repo r = open(call);
  const QString branch = r.branch();
  if (branch.isEmpty()) refuse("detached_head", "HEAD names no branch: switch to a branch, or make one here, to push.", "git_branch_create");
  if (r.unborn()) refuse("no_commits", "Nothing to push yet: commit first.", "git_commit");
  if (call.policy.pushes && call.policy.protects(branch)) refuseProtected(r, kPushesKey, "Refuse pushes of a protected branch", "push it");
  const QString given = text(args, "remote");
  const QString remote = remoteOf(r, given);
  QStringList pushArgs{"push", "--progress"};
  QString destination = branch;
  const bool first = r.status.upstream.isEmpty() || (!given.isEmpty() && !r.status.upstream.startsWith(given + '/'));
  if (!first) destination = r.status.upstream.mid(remote.size() + 1);
  if (first) pushArgs << "-u";
  pushArgs << remote << QStringLiteral("refs/heads/%1:refs/heads/%2").arg(branch, destination);  // never a + (forced) refspec
  json tags = json::array();
  for (const auto& t : args.value("tags", json::array())) {
    const QString name = QString::fromStdString(t.get<std::string>());
    if (name.startsWith('-') || git::revParse(r.c, "refs/tags/" + name).isEmpty())
      refuse("unknown_tag", QStringLiteral("There is no tag %1 here (git_tag makes one).").arg(name), "git_tag");
    pushArgs << QStringLiteral("refs/tags/%1:refs/tags/%1").arg(name);  // a tag the remote has elsewhere is rejected, never replaced
    tags.push_back(str(name));
  }
  if (!args.value("allow_large", false)) {
    if (call.progress) call.progress(QStringLiteral("Checking what goes up"));
    const QStringList warnings = git::pushWarnings(r.c);
    if (!warnings.isEmpty())
      refuse("large_files", warnings.join(' ') + " Ask the user, then git_push with allow_large=true if they agree.");
  }
  if (call.progress) call.progress(QStringLiteral("Pushing %1").arg(branch));
  const git::Result res = git::run(r.c, pushArgs, network(call));
  if (!res.ok()) {
    if (res.cancelled) refuse("cancelled", "Cancelled.");
    const QString err = QString::fromUtf8(res.err);
    if (err.contains("[rejected]") || err.contains("non-fast-forward") || err.contains("fetch first"))
      refuse("rejected", res.error() + " git_pull (or merge the remote branch) first. Force pushing is not offered: if it is truly needed, it can be done with the git CLI, but only after asking the user and getting explicit confirmation.",
             "git_pull");
    gitFailed(res);
  }
  readStatus(r);
  json out = {{"state", "pushed"}, {"branch", str(branch)}, {"remote", str(remote)}, {"upstream", str(r.status.upstream)}, {"upstream_set", first},
              {"ahead", r.status.ahead}, {"behind", r.status.behind}};
  if (!tags.empty()) out["tags"] = tags;
  return out;
}

// A new repository as the panel's Set up repository makes it (git::setUp, the dialog's defaults), on the initial branch the
// user's setting names unless `branch` does; then, when asked, the first commit of `paths` with OPAD's two files.
json init(const json& args, const Call& call) {
  const QString program = call.program.isEmpty() ? git::findProgram() : call.program;
  if (program.isEmpty())
    refuse("git_missing", "git was not found on this computer. The user can install it, or point OPAD at it (Version control panel > Locate git).");
  const QFileInfo given(call.repo);
  if (call.repo.isEmpty() || !given.isAbsolute()) refuse("invalid_arguments", "Name the folder by its absolute path.");
  if (given.isFile()) refuse("invalid_arguments", QStringLiteral("%1 is a file: name its folder.").arg(QDir::toNativeSeparators(call.repo)));
  if (!given.exists() && !QDir().mkpath(call.repo)) refuse("write_failed", QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(call.repo)));
  const QString folder = QDir::cleanPath(QFileInfo(call.repo).absoluteFilePath());
  if (!call.document.isEmpty() && QDir(folder).relativeFilePath(QFileInfo(call.document).absoluteFilePath()).startsWith(".."))
    refuse("invalid_arguments", QStringLiteral("The open document %1 is not inside %2: its repository must hold it.").arg(QDir::toNativeSeparators(call.document), QDir::toNativeSeparators(folder)));
  git::Context c = git::contextFor(program, folder, QString());
  const git::Result inside = git::run(c, {"rev-parse", "--show-toplevel"}, quick());
  if (inside.ok())
    refuse("already_a_repository",
           QStringLiteral("%1 is inside the git repository %2 already: nothing was changed. git_status reads it; OPAD's Version control panel (Set up repository) can add OPAD's merge settings to it.")
               .arg(QDir::toNativeSeparators(folder), QDir::toNativeSeparators(QString::fromUtf8(inside.out).trimmed())),
           "git_status");
  if (!QString::fromUtf8(inside.err).contains("not a git repository")) refuse("git_failed", inside.error());
  const QString branch = text(args, "branch").isEmpty() ? initialBranch() : text(args, "branch");
  if (!git::validBranchName(branch)) refuse("invalid_arguments", git::explain("is not a valid branch name") + " (\"" + branch + "\")");
  QStringList rels;
  for (const auto& p : args.value("paths", json::array())) {
    const QString path = QString::fromStdString(p.get<std::string>());
    const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString rel = QFileInfo(cleaned).isAbsolute() ? QDir(folder).relativeFilePath(cleaned) : cleaned;
    if (rel.isEmpty() || rel == "." || rel.startsWith("../") || rel == ".." || QFileInfo(rel).isAbsolute())
      refuse("invalid_arguments", QStringLiteral("%1 is not inside %2.").arg(path, QDir::toNativeSeparators(folder)));
    if (!QFileInfo::exists(QDir(folder).filePath(rel))) refuse("not_found", QStringLiteral("%1 does not exist.").arg(rel));
    rels << rel;
  }
  if (!rels.isEmpty()) {
    if (call.policy.commits && call.policy.protects(branch))
      refuse("protected_branch",
             QStringLiteral("Refused: the initial branch %1 is a protected branch and the user's OPAD preference \"Refuse commits to a protected branch\" is on "
                            "(Preferences > Version control > Branch protection; setting %2), so agents may not make its first commit. Nothing was changed. "
                            "git_init without paths makes the repository and leaves the first commit to the user, or ask the user to change the preference.")
                 .arg(branch, QString::fromLatin1(kCommitsKey)),
             "git_init");
    if (!call.document.isEmpty() && call.documentDirty && rels.contains(QDir(folder).relativeFilePath(QFileInfo(call.document).absoluteFilePath())))
      refuse("unsaved_document", QStringLiteral("The open document %1 has unsaved changes: save them first (the save tool).").arg(QFileInfo(call.document).fileName()), "save");
  }
  git::SetupOptions o;  // as the Set up repository dialog's defaults: attributes, .gitignore, LFS when installed, this OPAD as the driver
  o.branch = branch;
  git::RunOptions ro;
  ro.cancelled = call.cancelled;
  if (call.progress) ro.progress = [p = call.progress](const QString& phase, int) { p(phase); };
  try {
    git::setUp(c, folder, git::Install::here(), o, ro);
  } catch (const std::exception& e) {
    refuse("git_failed", QString::fromUtf8(e.what()));
  }
  Call opened = call;
  opened.repo = folder;
  Repo r = open(opened);
  const git::Result driver = git::run(r.c, {"config", "--get", "merge.opad.driver"}, quick());
  json out = {{"state", "initialized"}, {"repo", str(QDir::toNativeSeparators(r.top))}, {"branch", str(branch)},
              {"attributes", QFileInfo::exists(r.top + "/.gitattributes")}, {"ignore", QFileInfo::exists(r.top + "/.gitignore")},
              {"lfs", git::run(r.c, {"lfs", "version"}, quick()).ok()}, {"driver", str(QString::fromUtf8(driver.out).trimmed())},
              {"protected", call.policy.protects(branch)}};
  if (rels.isEmpty()) return out;
  for (const char* own : {".gitattributes", ".gitignore"})
    if (QFileInfo::exists(r.top + "/" + own) && !rels.contains(QString::fromLatin1(own))) rels << QString::fromLatin1(own);
  QString message = text(args, "message").trimmed();
  if (message.isEmpty()) {
    const QStringList named = rels.filter(QRegularExpression(QStringLiteral("^[^.]")));  // not OPAD's dot files
    message = named.isEmpty() ? QStringLiteral("Initial commit") : QStringLiteral("Add %1").arg(named.join(", "));
  }
  const git::RunOptions slowly = slow(call);
  must(r, QStringList{"--literal-pathspecs", "add", "--"} + rels, slowly);
  git::RunOptions write = slowly;
  write.input = message.toUtf8();
  must(r, {"commit", "-q", "-F", "-"}, write);
  readStatus(r);
  json files = json::array();
  for (const QString& rel : rels) files.push_back(str(rel));
  out["commit"] = {{"hash", str(r.status.oid)}, {"short", str(r.status.oid.left(7))}, {"subject", str(message.section('\n', 0, 0))}, {"files", files}};
  return out;
}

json tag(const json& args, const Call& call) {
  Repo r = open(call);
  if (r.unborn()) refuse("no_commits", "Nothing to tag yet: commit first.", "git_commit");
  const QString name = text(args, "name");
  if (name.startsWith('-') || !git::run(r.c, {"check-ref-format", "refs/tags/" + name}, quick()).ok())
    refuse("invalid_arguments", QStringLiteral("\"%1\" is not a name git takes for a tag.").arg(name));
  if (const QString at = git::revParse(r.c, "refs/tags/" + name); !at.isEmpty())
    refuse("tag_exists", QStringLiteral("The tag %1 exists already (at %2). Tags are never moved or replaced here: choose another name.").arg(name, at.left(7)));
  const QString message = text(args, "message").trimmed();
  if (message.isEmpty()) {
    must(r, {"tag", name});
  } else {
    git::RunOptions o = quick();
    o.input = message.toUtf8();
    must(r, {"tag", "-a", name, "-F", "-"}, o);
  }
  return {{"state", "tagged"}, {"tag", str(name)}, {"target", str(git::revParse(r.c, "HEAD"))}, {"annotated", !message.isEmpty()}, {"branch", str(r.branch())}};
}

json diff(const json& args, const Call& call) {
  Repo r = open(call);
  QString from = text(args, "from"), to = text(args, "to");
  if (from.isEmpty()) {
    if (r.unborn()) refuse("no_commits", "There is no commit to compare with yet.");
    from = QStringLiteral("HEAD");
  }
  auto resolveRev = [&r](const QString& rev, const char* what) {
    revision(rev, what);
    const QString hash = git::revParse(r.c, rev);
    if (hash.isEmpty()) refuse("unknown_revision", QStringLiteral("%1 is not a branch or commit of this repository.").arg(rev), "git_log");
    return hash;
  };
  const QString a = resolveRev(from, "from"), b = to.isEmpty() ? QString() : resolveRev(to, "to");
  const QString only = text(args, "path").isEmpty() ? QString() : relative(r, text(args, "path"));
  QStringList cmd{"diff", "--numstat", "-z", "-M", a};
  if (!b.isEmpty()) cmd << b;
  cmd << "--";
  if (!only.isEmpty()) cmd << only;
  const QList<QByteArray> parts = must(r, cmd).out.split('\0');
  json files = json::array();
  int total = 0;
  QList<QPair<QString, QString>> documents;  // (path before, path after)
  for (int i = 0; i < parts.size();) {
    const QByteArray record = parts[i];
    if (record.isEmpty()) {
      ++i;
      continue;
    }
    const QList<QByteArray> cols = record.split('\t');
    if (cols.size() < 3) {
      ++i;
      continue;
    }
    QString before, after = QString::fromUtf8(cols[2]);
    if (after.isEmpty() && i + 2 < parts.size()) {  // a rename: its two paths follow
      before = QString::fromUtf8(parts[i + 1]);
      after = QString::fromUtf8(parts[i + 2]);
      i += 3;
    } else {
      ++i;
    }
    ++total;
    if (files.size() < 500) {
      json f = {{"path", str(after)}};
      if (!before.isEmpty()) f["from"] = str(before);
      if (cols[0] == "-") f["binary"] = true;
      else {
        f["added"] = cols[0].toInt();
        f["deleted"] = cols[1].toInt();
      }
      files.push_back(f);
    }
    if (after.endsWith(".opad", Qt::CaseInsensitive)) documents.append({before.isEmpty() ? after : before, after});
  }
  json docs = json::array();
  for (const auto& [was, now] : documents) {
    if (docs.size() >= 10) break;
    if (call.cancelled && call.cancelled()) refuse("cancelled", "Cancelled.");
    std::string left = blob(r, a, was), right;
    if (b.isEmpty()) {
      QFile f(QDir(r.top).filePath(now));
      if (f.open(QIODevice::ReadOnly)) right = f.readAll().toStdString();
    } else {
      right = blob(r, b, now);
    }
    json d = {{"path", str(now)}};
    if (left.empty() || right.empty()) {
      d["change"] = left.empty() ? "added" : "deleted";
      docs.push_back(d);
      continue;
    }
    const auto origin = fsPath(QDir(r.top).filePath(now));
    const json semantic = opad::semantic_diff(opad::Document::parse_index(std::move(left), origin), opad::Document::parse_index(std::move(right), origin));
    d["summary"] = semantic.value("summary", "");
    d["relation"] = semantic.value("relation", "");
    json changes = semantic.value("changes", json::array());
    d["changes_total"] = changes.size();
    if (changes.size() > 100) changes.erase(changes.begin() + 100, changes.end());
    d["changes"] = changes;
    docs.push_back(d);
  }
  json out = {{"from", str(from)}, {"to", to.isEmpty() ? std::string("work tree") : str(to)}, {"files", files}, {"files_total", total}, {"documents", docs}};
  if (documents.size() > 10) out["documents_truncated"] = true;
  if (b.isEmpty() && call.documentDirty) out["unsaved_changes_not_included"] = true;
  return out;
}
}  // namespace

json run(const std::string& name, const json& args, const Call& call) {
  if (name == "git_status") return status(call);
  if (name == "git_log") return log(args, call);
  if (name == "git_branches") return branches(call);
  if (name == "git_fetch") return fetch(args, call);
  if (name == "git_branch_create") return branchCreate(args, call);
  if (name == "git_switch") return switchTo(args, call);
  if (name == "git_commit") return commit(args, call);
  if (name == "git_merge") return merge(args, call);
  if (name == "git_merge_abort") return mergeAbort(call);
  if (name == "git_resolve") return resolve(args, call);
  if (name == "git_pull") return pull(args, call);
  if (name == "git_push") return push(args, call);
  if (name == "git_init") return init(args, call);
  if (name == "git_tag") return tag(args, call);
  if (name == "git_diff") return diff(args, call);
  refuse("unknown_tool", QStringLiteral("There is no git tool %1.").arg(QString::fromStdString(name)));
}

}  // namespace gitagent
