// AI agents' git tools (app/GitAgent.cpp) through the real headless MCP server (opad-cli mcp, stdio) against temporary
// repositories and a local bare remote: status, branches, commits refused on a protected branch while the preference
// says so and allowed after it is turned off or on a branch, merges into a protected branch refused or allowed by the
// preference, two branches that both changed an .opad merged by OPAD's driver (given to git for that command only), a
// stopped merge listed, aborted and resolved, push (the first sets the upstream; never forced; the protected-push
// preference) and a fast-forward pull with its preview, and no destructive tool in tools/list. The preferences are the
// app's QSettings: OPAD_BENCH_SETTINGS points the server at an INI file the test writes, as Preferences would.
// git's global and system config are left out; nothing outside the temporary folder is touched.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#include "Git.hpp"
#include "GitAgent.hpp"
#include "check.hpp"
#include "opad/core.hpp"
#include "opad/live.hpp"

using opad::json;

namespace {
QString bin(const QString& name) {
#ifdef _WIN32
  return QCoreApplication::applicationDirPath() + "/" + name + ".exe";
#else
  return QCoreApplication::applicationDirPath() + "/" + name;
#endif
}

QTemporaryDir* g_root = nullptr;
QString root() { return g_root->path(); }
QString settingsFile() { return root() + "/settings/opad/OPAD.ini"; }  // where the server's QSettings live (OPAD_BENCH_SETTINGS)

void setPref(const char* key, const QVariant& value) {
  QSettings s(settingsFile(), QSettings::IniFormat);
  s.setValue(QString::fromLatin1(key), value);
  s.sync();
}

QByteArray gitIn(const QString& dir, const QStringList& args) {
  git::Context c;
  c.program = git::findProgram();
  c.dir = dir;
  const git::Result r = git::run(c, args);
  if (!r.ok()) throw check::Failure(("git " + args.join(' ') + ": " + r.error()).toStdString());
  return r.out;
}

// One headless MCP server for the whole test, as an agent's client keeps one.
class Server {
 public:
  Server() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("OPAD_BENCH_SETTINGS", root() + "/settings");
    m_p.setProcessEnvironment(env);
    m_p.start(bin("opad-cli"), {"mcp"});
    if (!m_p.waitForStarted(20000)) throw check::Failure("opad-cli mcp did not start");
    const json init = request("initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", json::object()}, {"clientInfo", {{"name", "test"}, {"version", "1"}}}});
    m_instructions = init.at("result").value("instructions", "");
  }
  ~Server() {
    m_p.closeWriteChannel();
    if (!m_p.waitForFinished(10000)) {
      m_p.kill();
      m_p.waitForFinished(5000);
    }
  }
  json request(const std::string& method, const json& params = json::object()) {
    m_p.write(QByteArray::fromStdString(json{{"jsonrpc", "2.0"}, {"id", ++m_id}, {"method", method}, {"params", params}}.dump()) + '\n');
    while (!m_buffer.contains('\n')) {
      if (!m_p.waitForReadyRead(120000)) throw check::Failure("no answer from opad-cli mcp to " + method + ": " + m_p.readAllStandardError().toStdString());
      m_buffer += m_p.readAllStandardOutput();
    }
    const auto end = m_buffer.indexOf('\n');
    const json answer = json::parse(m_buffer.left(end).toStdString());
    m_buffer.remove(0, end + 1);
    return answer;
  }
  // A tool call: its structuredContent; `ok` false expects isError.
  json call(const std::string& tool, const json& args, bool ok = true) {
    const json r = request("tools/call", {{"name", tool}, {"arguments", args}}).at("result");
    if (r.value("isError", false) == ok) throw check::Failure(tool + " " + args.dump() + (ok ? " failed: " : " did not fail: ") + r.dump());
    return r.value("structuredContent", json::object());
  }
  // A refusal with this code.
  json refused(const std::string& tool, const json& args, const std::string& code) {
    const json r = call(tool, args, false);
    if (r["error"].value("code", "") != code) throw check::Failure(tool + ": expected " + code + ", got " + r.dump());
    return r["error"];
  }
  const std::string& instructions() const { return m_instructions; }
 private:
  QProcess m_p;
  QByteArray m_buffer;
  int m_id = 0;
  std::string m_instructions;
};

Server* g_server = nullptr;
Server& mcp() { return *g_server; }

std::string s(const QString& t) { return t.toStdString(); }

json box(const QString& doc, int x) {
  return mcp().call("feature", {{"doc", s(doc)}, {"kind", "box"}, {"inputs", {{"x", std::to_string(x) + " mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}}}});
}

size_t ops(const QString& doc, const char* type) {
  size_t n = 0;
  for (const auto& op : opad::Document::load(doc.toStdU16String()).ops) n += op.type == type;
  return n;
}

// A repository with OPAD's attributes (as Set up repository writes them, the driver config left out) and one document.
QString repository(const QString& name) {
  const QString dir = root() + "/" + name;
  QDir().mkpath(dir);
  gitIn(dir, {"init", "-q", "-b", "main"});
  QFile attributes(dir + "/.gitattributes");
  if (!attributes.open(QIODevice::WriteOnly)) throw check::Failure("attributes");
  attributes.write("*.opad text eol=lf merge=opad diff=opad\n");
  attributes.close();
  mcp().call("new", {{"doc", s(dir + "/model.opad")}});
  return dir;
}

void defaults() {
  setPref("git/initialBranch", "main");
  setPref("git/protectedBranches", "main, master");
  setPref("git/protectCommits", true);
  setPref("git/protectMerges", true);
  setPref("git/protectPushes", false);
}
}  // namespace

TEST(tools_listed_none_destructive) {
  const json tools = mcp().request("tools/list").at("result").at("tools");
  std::set<std::string> git;
  for (const auto& t : tools) {
    const std::string name = t.at("name");
    if (name.rfind("git_", 0) != 0) continue;
    git.insert(name);
    for (const char* word : {"force", "reset", "rebase", "clean", "stash", "delete", "drop", "restore", "checkout", "amend", "gc", "prune"}) {
      CHECK(name.find(word) == std::string::npos);
      CHECK(!t["inputSchema"]["properties"].contains(word));
    }
    CHECK(t["inputSchema"]["required"].dump().find("repo") != std::string::npos);  // headless: the repository is named
    CHECK(!t["annotations"].value("destructiveHint", true));
  }
  const std::set<std::string> expected{"git_status", "git_log", "git_branches", "git_diff", "git_init", "git_fetch", "git_branch_create", "git_switch",
                                       "git_commit", "git_merge", "git_merge_abort", "git_resolve", "git_pull", "git_push", "git_tag"};
  CHECK(git == expected);
  // The live server lists the same tools, on the bound document's repository (no repo), writes with a request_id.
  std::set<std::string> live;
  for (const auto& t : opad::agent::live_tools())
    if (opad::agent::git_tool(t.at("name"))) {
      live.insert(t.at("name").get<std::string>());
      CHECK(!t["inputSchema"]["properties"].contains("repo"));
    }
  CHECK(live == expected);
  CHECK(opad::agent::live_schema("git_commit")["required"].dump().find("request_id") != std::string::npos);
  CHECK(opad::agent::live_schema("git_status")["required"].empty());
  // A merge or pull preview and a resolve that only lists change nothing: no request_id needed (live), and a merge preview
  // or a listing is a read (no edit permission, no receipt); a pull preview still fetches.
  for (const char* name : {"git_merge", "git_pull", "git_resolve"}) CHECK(opad::agent::live_schema(name)["required"].dump().find("request_id") == std::string::npos);
  opad::agent::validate_input(opad::agent::live_schema("git_merge"), {{"source", "develop"}, {"preview", true}});
  opad::agent::validate_input(opad::agent::live_schema("git_pull"), {{"preview", true}});
  opad::agent::validate_input(opad::agent::live_schema("git_resolve"), {{"path", "model.opad"}});
  CHECK(!gitagent::writes("git_merge", {{"source", "develop"}, {"preview", true}}) && gitagent::receipted("git_merge", {{"source", "develop"}}));
  CHECK(!gitagent::writes("git_resolve", {{"path", "model.opad"}}) && gitagent::receipted("git_resolve", {{"path", "model.opad"}, {"keep", "ours"}}));
  CHECK(gitagent::writes("git_pull", {{"preview", true}}) && !gitagent::receipted("git_pull", {{"preview", true}}) && gitagent::receipted("git_pull"));
  for (const char* name : {"git_commit", "git_switch", "git_branch_create", "git_tag", "git_fetch", "git_push", "git_merge_abort", "git_init"})
    CHECK(opad::agent::live_schema(name)["required"].dump().find("request_id") != std::string::npos && gitagent::receipted(name));
  // The instructions and the push tool say what is not offered and how it may still happen.
  CHECK(mcp().instructions().find("explicit confirmation") != std::string::npos);
  for (const auto& t : tools)
    if (t["name"] == "git_push") CHECK(t["description"].get<std::string>().find("only after asking the user") != std::string::npos);
  std::printf("git tools listed: %zu, none destructive PASS\n", git.size());
}

TEST(status_commit_and_protection) {
  defaults();
  const QString repo = repository("commits"), doc = repo + "/model.opad";
  json st = mcp().call("git_status", {{"repo", s(repo)}});
  CHECK(st["branch"] == "main" && st["protected"] == true && st["no_commits_yet"] == true && st["untracked"].size() == 2);
  CHECK(st["policy"]["refuse_commits"] == true && st["policy"]["protected_branches"] == json({"main", "master"}));
  // Refused on main while the preference is on: the sentence names it and says how to work instead.
  const json no = mcp().refused("git_commit", {{"repo", s(repo)}, {"message", "first"}, {"all", true}}, "protected_branch");
  CHECK(no["message"].get<std::string>().find("Refuse commits to a protected branch") != std::string::npos);
  CHECK(no["message"].get<std::string>().find("git_branch_create") != std::string::npos && no["next"] == "git_branch_create");
  CHECK(mcp().call("git_status", {{"repo", s(repo)}})["staged"].empty());  // nothing was staged by the refusal
  // The preference off (as Preferences writes it): allowed on main, read again at the next call.
  setPref("git/protectCommits", false);
  json c = mcp().call("git_commit", {{"repo", s(repo)}, {"message", "first"}, {"all", true}});
  CHECK(c["state"] == "committed" && c["branch"] == "main" && c["files"].size() == 2);
  setPref("git/protectCommits", true);
  mcp().refused("git_commit", {{"repo", s(repo)}, {"message", "again"}, {"all", true}}, "protected_branch");
  // On a branch of its own: allowed; exactly the paths named are committed; an empty commit is refused.
  json b = mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "agent/box"}});
  CHECK(b["switched"] == true && b["branch"] == "agent/box" && b["protected"] == false);
  mcp().refused("git_branch_create", {{"repo", s(repo)}, {"name", "agent/box"}}, "branch_exists");
  box(doc, 0);
  QFile other(repo + "/notes.txt");
  CHECK(other.open(QIODevice::WriteOnly));
  other.write("not for this commit\n");
  other.close();
  c = mcp().call("git_commit", {{"repo", s(repo)}, {"message", "A box"}, {"paths", {"model.opad"}}});
  CHECK(c["branch"] == "agent/box" && c["files"].size() == 1 && c["files"][0]["path"] == "model.opad");
  st = mcp().call("git_status", {{"repo", s(repo)}});
  CHECK(st["untracked"] == json({"notes.txt"}) && st["protected"] == false);
  mcp().refused("git_commit", {{"repo", s(repo)}, {"message", "nothing"}, {"paths", {"model.opad"}}}, "nothing_to_commit");
  mcp().refused("git_commit", {{"repo", s(repo)}, {"message", "outside"}, {"paths", {s(root() + "/elsewhere.txt")}}}, "invalid_arguments");
  // Log and branches.
  const json log = mcp().call("git_log", {{"repo", s(repo)}, {"limit", 5}});
  CHECK(log["commits"].size() == 2 && log["commits"][0]["subject"] == "A box" && log["commits"][1]["subject"] == "first");
  CHECK(mcp().call("git_log", {{"repo", s(repo)}, {"branch", "main"}})["commits"].size() == 1);
  mcp().refused("git_log", {{"repo", s(repo)}, {"branch", "--all"}}, "invalid_arguments");  // never an option
  const json br = mcp().call("git_branches", {{"repo", s(repo)}});
  CHECK(br["current"] == "agent/box" && br["branches"].size() == 2);
  for (const auto& x : br["branches"]) CHECK(x["protected"] == (x["name"] == "main"));
  // A switch refuses uncommitted changes rather than discard them.
  box(doc, 10);
  const json dirty = mcp().refused("git_switch", {{"repo", s(repo)}, {"branch", "main"}}, "uncommitted_changes");
  CHECK(dirty["message"].get<std::string>().find("explicit confirmation") != std::string::npos);
  CHECK(ops(doc, "feature") == 2);  // still there
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "Second box"}, {"paths", {"model.opad"}}});
  const json sw = mcp().call("git_switch", {{"repo", s(repo)}, {"branch", "main"}});
  CHECK(sw["state"] == "switched" && sw["branch"] == "main" && sw["files_changed"]["documents"] == json({"model.opad"}) && ops(doc, "feature") == 0);
  // agent/box has commits main lacks: not merged, nothing to tidy.
  const json unmerged = mcp().call("git_branches", {{"repo", s(repo)}});
  CHECK(unmerged["merged_into_current"].empty());
  for (const auto& x : unmerged["branches"]) CHECK(x["merged"] == false);
  std::printf("status, branches, commits refused on main and allowed off it or with the preference off PASS\n");
}

TEST(merge_protection_and_driver) {
  defaults();
  const QString repo = repository("merges"), doc = repo + "/model.opad";
  setPref("git/protectCommits", false);
  mcp().call("param", {{"doc", s(doc)}, {"name", "w"}, {"expr", "5 mm"}});
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "base"}, {"all", true}});
  setPref("git/protectCommits", true);
  // Two branches change the same document in different ways.
  mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "left"}});
  box(doc, 0);
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "left box"}, {"all", true}});
  mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "right"}, {"from", "main"}});
  box(doc, 20);
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "right box"}, {"all", true}});
  mcp().call("git_switch", {{"repo", s(repo)}, {"branch", "main"}});
  // Into a protected branch: refused while the preference is on (a preview is still allowed).
  const json no = mcp().refused("git_merge", {{"repo", s(repo)}, {"source", "left"}}, "protected_branch");
  CHECK(no["message"].get<std::string>().find("Refuse merges into a protected branch") != std::string::npos);
  const json preview = mcp().call("git_merge", {{"repo", s(repo)}, {"source", "left"}, {"preview", true}});
  CHECK(preview["state"] == "preview" && preview["fast_forward"] == true && preview["commits"].size() == 1 && preview["protected"] == true);
  CHECK(preview["documents"].size() == 1 && preview["documents"][0]["path"] == "model.opad" && !preview["documents"][0]["summary"].get<std::string>().empty());
  // Into a branch that is not protected: allowed with the preference on. left gets right's box through OPAD's driver.
  mcp().call("git_switch", {{"repo", s(repo)}, {"branch", "left"}});
  const json ahead = mcp().call("git_merge", {{"repo", s(repo)}, {"source", "right"}, {"preview", true}});
  CHECK(ahead["fast_forward"] == false && ahead["stops"] == false && ahead["documents"][0]["changed_here"] == true);
  const json m = mcp().call("git_merge", {{"repo", s(repo)}, {"source", "right"}});
  CHECK(m["state"] == "merged" && m["fast_forward"] == false);
  CHECK(!m.contains("warnings"));  // the attributes route .opad to the driver
  CHECK(ops(doc, "feature") == 2);  // both boxes: merged record by record, no conflict markers
  CHECK(QString::fromUtf8(gitIn(repo, {"log", "-1", "--format=%P"})).trimmed().split(' ').size() == 2);
  CHECK(!QString::fromUtf8(gitIn(repo, {"config", "--local", "--list"})).contains("merge.opad"));  // the driver was given for the command only
  // The preference off: merging into main is allowed.
  mcp().call("git_switch", {{"repo", s(repo)}, {"branch", "main"}});
  setPref("git/protectMerges", false);
  const json ff = mcp().call("git_merge", {{"repo", s(repo)}, {"source", "left"}});
  CHECK(ff["state"] == "merged" && ff["fast_forward"] == true && ops(doc, "feature") == 2);
  setPref("git/protectMerges", true);
  // Branch hygiene without deleting: left and right are in main now, reported as merged (main itself is the current one).
  const json tidy = mcp().call("git_branches", {{"repo", s(repo)}});
  if (tidy["merged_into_current"] != json({"left", "right"})) throw check::Failure("merged branches: " + tidy.dump());
  for (const auto& x : tidy["branches"])
    if (x["remote"] == false) CHECK(x["merged"] == (x["name"] != "main"));
  std::printf("merges into main refused and allowed by the preference, both branches' boxes merged by the driver PASS\n");
}

TEST(conflicts_listed_aborted_resolved) {
  defaults();
  const QString repo = repository("conflicts"), doc = repo + "/model.opad";
  setPref("git/protectedBranches", "release/*");  // main is not protected here; patterns are
  mcp().call("param", {{"doc", s(doc)}, {"name", "w"}, {"expr", "5 mm"}});
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "base"}, {"all", true}});
  mcp().refused("git_branch_create", {{"repo", s(repo)}, {"name", "release/1"}, {"switch", false}}, "protected_branch");  // a protected name
  setPref("git/protectCommits", false);
  setPref("git/protectMerges", false);
  mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "release/1"}, {"switch", false}});
  setPref("git/protectCommits", true);
  setPref("git/protectMerges", true);
  CHECK(mcp().call("git_branches", {{"repo", s(repo)}})["branches"][1]["protected"] == true);
  mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "theirs"}});
  mcp().call("param", {{"doc", s(doc)}, {"name", "w"}, {"expr", "20 mm"}});
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "w 20"}, {"all", true}});
  mcp().call("git_switch", {{"repo", s(repo)}, {"branch", "main"}});
  mcp().call("param", {{"doc", s(doc)}, {"name", "w"}, {"expr", "10 mm"}});
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "w 10"}, {"all", true}});
  const json preview = mcp().call("git_merge", {{"repo", s(repo)}, {"source", "theirs"}, {"preview", true}});
  if (!(preview["stops"] == true && preview["documents"][0]["conflicts"].size() == 1 && preview["documents"][0]["conflicts"][0]["name"] == "w"))
    throw check::Failure("the preview of a conflicting merge: " + preview.dump());
  json m = mcp().call("git_merge", {{"repo", s(repo)}, {"source", "theirs"}});
  CHECK(m["state"] == "conflicts" && m["conflicts"] == json({"model.opad"}));
  mcp().refused("git_commit", {{"repo", s(repo)}, {"message", "too early"}, {"all", true}}, "conflicts");
  mcp().refused("git_switch", {{"repo", s(repo)}, {"branch", "theirs"}}, "merge_in_progress");
  const json listed = mcp().call("git_resolve", {{"repo", s(repo)}, {"path", "model.opad"}});
  CHECK(listed["mergeable"] == true && listed["conflicts"].size() == 1 && listed["conflicts"][0]["index"] == 0);
  CHECK(mcp().call("git_merge_abort", {{"repo", s(repo)}})["state"] == "aborted");
  CHECK(mcp().call("git_status", {{"repo", s(repo)}})["merging"] == false);
  mcp().refused("git_merge_abort", {{"repo", s(repo)}}, "no_merge");
  m = mcp().call("git_merge", {{"repo", s(repo)}, {"source", "theirs"}});
  CHECK(m["state"] == "conflicts");
  const json r = mcp().call("git_resolve", {{"repo", s(repo)}, {"path", "model.opad"}, {"choices", {"theirs"}}});
  CHECK(r["state"] == "resolved" && r["remaining_conflicts"].empty());
  const json done = mcp().call("git_commit", {{"repo", s(repo)}, {"message", "merge theirs"}, {"all", true}});
  CHECK(done["state"] == "committed" && mcp().call("git_status", {{"repo", s(repo)}})["merging"] == false);
  const json params = mcp().call("params", {{"doc", s(doc)}});
  CHECK(params.dump().find("20 mm") != std::string::npos);
  std::printf("a stopped merge listed, aborted, resolved for theirs and committed PASS\n");
}

TEST(protected_branch_never_created) {
  defaults();
  const QString repo = repository("creates");
  setPref("git/protectCommits", false);
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "base"}, {"all", true}});
  setPref("git/protectCommits", true);
  mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "develop"}});
  // A protected name made by an agent at a commit of its choosing would hold its commits: refused while commits or merges
  // into protected branches are, nothing made, the preference named.
  const json no = mcp().refused("git_branch_create", {{"repo", s(repo)}, {"name", "master"}, {"from", "develop"}}, "protected_branch");
  CHECK(no["message"].get<std::string>().find("Refuse commits to a protected branch") != std::string::npos && no["next"] == "git_branch_create");
  CHECK(QString::fromUtf8(gitIn(repo, {"branch", "--list", "master"})).trimmed().isEmpty());
  CHECK(mcp().call("git_status", {{"repo", s(repo)}})["branch"] == "develop");
  setPref("git/protectedBranches", "main, master, release/*");
  setPref("git/protectCommits", false);  // merges into them still refused: still no creating them
  const json merges = mcp().refused("git_branch_create", {{"repo", s(repo)}, {"name", "release/2"}, {"switch", false}}, "protected_branch");
  CHECK(merges["message"].get<std::string>().find("Refuse merges into a protected branch") != std::string::npos);
  // A branch from a protected one is fine.
  const json from = mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "feature/x"}, {"from", "main"}});
  CHECK(from["branch"] == "feature/x" && from["protected"] == false);
  // Both preferences off: the user allows it.
  setPref("git/protectMerges", false);
  CHECK(mcp().call("git_branch_create", {{"repo", s(repo)}, {"name", "master"}, {"switch", false}})["created"] == "master");
  defaults();
  std::printf("protected branches never created by an agent while protected, branching from them allowed PASS\n");
}

TEST(push_pull_never_forced) {
  defaults();
  const QString repo = repository("pushes"), doc = repo + "/model.opad", remote = root() + "/remote.git", other = root() + "/other";
  gitIn(root(), {"init", "-q", "--bare", "-b", "main", remote});
  gitIn(repo, {"remote", "add", "origin", remote});
  setPref("git/protectCommits", false);
  box(doc, 0);
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "one"}, {"all", true}});
  // The first push sets the upstream; pushing main is allowed while the push preference is off (its default).
  json p = mcp().call("git_push", {{"repo", s(repo)}});
  CHECK(p["state"] == "pushed" && p["upstream_set"] == true && p["upstream"] == "origin/main");
  setPref("git/protectPushes", true);
  box(doc, 10);
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "two"}, {"all", true}});
  const json no = mcp().refused("git_push", {{"repo", s(repo)}}, "protected_branch");
  CHECK(no["message"].get<std::string>().find("Refuse pushes of a protected branch") != std::string::npos);
  setPref("git/protectPushes", false);
  p = mcp().call("git_push", {{"repo", s(repo)}});
  CHECK(p["upstream_set"] == false && p["ahead"] == 0);
  // Someone else pushes; pull previews it, then fast-forwards (into protected main: a fast-forward is no merge into it).
  gitIn(root(), {"clone", "-q", remote, other});
  box(other + "/model.opad", 20);
  gitIn(other, {"commit", "-q", "-am", "theirs"});
  gitIn(other, {"push", "-q"});
  const json preview = mcp().call("git_pull", {{"repo", s(repo)}, {"preview", true}});
  CHECK(preview["state"] == "preview" && preview["fast_forward"] == true && preview["commits"].size() == 1 && preview["commits"][0]["subject"] == "theirs");
  CHECK(ops(doc, "feature") == 2);  // a preview changes nothing
  const json pulled = mcp().call("git_pull", {{"repo", s(repo)}});
  CHECK(pulled["state"] == "merged" && pulled["fast_forward"] == true && ops(doc, "feature") == 3);
  CHECK(mcp().call("git_pull", {{"repo", s(repo)}})["state"] == "up_to_date");
  // The remote moves on while this clone commits too: the push is rejected, never forced, and pull is fast-forward only.
  box(other + "/model.opad", 30);
  gitIn(other, {"commit", "-q", "-am", "theirs again"});
  gitIn(other, {"push", "-q"});
  box(doc, 40);
  mcp().call("git_commit", {{"repo", s(repo)}, {"message", "mine"}, {"all", true}});
  const json rejected = mcp().refused("git_push", {{"repo", s(repo)}}, "rejected");
  CHECK(rejected["message"].get<std::string>().find("Force pushing is not offered") != std::string::npos && rejected["next"] == "git_pull");
  mcp().refused("git_pull", {{"repo", s(repo)}}, "not_fast_forward");
  mcp().refused("git_pull", {{"repo", s(repo)}, {"ff_only", false}}, "protected_branch");  // a merge commit into protected main
  CHECK(QString::fromUtf8(gitIn(remote, {"log", "-1", "--format=%s", "main"})).trimmed() == "theirs again");  // the remote kept its commit
  setPref("git/protectMerges", false);
  const json merged = mcp().call("git_pull", {{"repo", s(repo)}, {"ff_only", false}});
  CHECK(merged["state"] == "merged" && merged["fast_forward"] == false && ops(doc, "feature") == 5);
  CHECK(mcp().call("git_push", {{"repo", s(repo)}})["state"] == "pushed");
  // A tag goes up with tags; an existing one is never moved.
  CHECK(mcp().call("git_tag", {{"repo", s(repo)}, {"name", "v1"}})["state"] == "tagged");
  mcp().refused("git_push", {{"repo", s(repo)}, {"tags", {"v9"}}}, "unknown_tag");
  CHECK(mcp().call("git_push", {{"repo", s(repo)}, {"tags", {"v1"}}})["tags"] == json({"v1"}));
  CHECK(QString::fromUtf8(gitIn(remote, {"tag"})).trimmed() == "v1");
  std::printf("push (upstream set, protected-push preference), pull preview and fast-forward, a rejected push never forced PASS\n");
}

TEST(init_tag_diff) {
  defaults();
  // A new repository as Set up repository makes it: the initial branch, OPAD's attributes and ignore file, this OPAD as the driver.
  const QString fresh = root() + "/fresh/project";
  QDir().mkpath(fresh);  // outside a repository git_status points at git_init (agents may make one)
  const json none = mcp().refused("git_status", {{"repo", s(fresh)}}, "not_a_repository");
  CHECK(none["next"] == "git_init" && none["message"].get<std::string>().find("git_init") != std::string::npos);
  CHECK(none["message"].get<std::string>().find("user's call") == std::string::npos);
  const json made = mcp().call("git_init", {{"repo", s(fresh)}});
  CHECK(made["state"] == "initialized" && made["branch"] == "main" && made["attributes"] == true && made["ignore"] == true && made["protected"] == true);
  CHECK(made["driver"].get<std::string>().find("merge-driver") != std::string::npos);
  {  // the diff driver is set up without git's textconv cache (its notes ref shows in git log --all as a commit)
    git::Context c;
    c.program = git::findProgram();
    c.dir = fresh;
    CHECK(git::run(c, {"config", "--local", "--get", "diff.opad.textconv"}).ok());
    CHECK(!git::run(c, {"config", "--local", "--get", "diff.opad.cachetextconv"}).ok());
  }
  QFile attributes(fresh + "/.gitattributes");
  CHECK(attributes.open(QIODevice::ReadOnly) && attributes.readAll().contains("*.opad text eol=lf merge=opad diff=opad"));
  mcp().refused("git_init", {{"repo", s(fresh)}}, "already_a_repository");
  QDir().mkpath(fresh + "/sub");
  mcp().refused("git_init", {{"repo", s(fresh + "/sub")}}, "already_a_repository");
  // A first commit on a protected initial branch is refused before anything is made; on another branch it is made.
  const QString second = root() + "/second", doc = second + "/model.opad";
  QDir().mkpath(second);
  mcp().call("new", {{"doc", s(doc)}});
  const json no = mcp().refused("git_init", {{"repo", s(second)}, {"paths", {"model.opad"}}}, "protected_branch");
  CHECK(no["message"].get<std::string>().find("Nothing was changed") != std::string::npos && !QFileInfo::exists(second + "/.git"));
  setPref("git/initialBranch", "trunk");
  const json first = mcp().call("git_init", {{"repo", s(second)}, {"paths", {"model.opad"}}});
  CHECK(first["branch"] == "trunk" && first["protected"] == false && first["commit"]["files"].size() == 3 && first["commit"]["subject"] == "Add model.opad");
  setPref("git/initialBranch", "main");
  // Tags: lightweight, annotated, never moved.
  CHECK(mcp().call("git_tag", {{"repo", s(second)}, {"name", "v1"}})["annotated"] == false);
  mcp().refused("git_tag", {{"repo", s(second)}, {"name", "v1"}}, "tag_exists");
  mcp().refused("git_tag", {{"repo", s(second)}, {"name", "bad name"}}, "invalid_arguments");
  CHECK(mcp().call("git_tag", {{"repo", s(second)}, {"name", "v2"}, {"message", "Second release"}})["annotated"] == true);
  CHECK(QString::fromUtf8(gitIn(second, {"cat-file", "-t", "v2"})).trimmed() == "tag");
  // The diff: line counts and OPAD's semantic diff, of the work tree and between commits.
  box(doc, 0);
  json d = mcp().call("git_diff", {{"repo", s(second)}});
  CHECK(d["to"] == "work tree" && d["files"].size() == 1 && d["files"][0]["path"] == "model.opad" && d["files"][0]["added"].get<int>() > 0);
  CHECK(d["documents"].size() == 1 && !d["documents"][0]["summary"].get<std::string>().empty() && d["documents"][0]["changes_total"].get<int>() > 0);
  mcp().call("git_commit", {{"repo", s(second)}, {"message", "A box"}, {"all", true}});
  d = mcp().call("git_diff", {{"repo", s(second)}, {"from", "v1"}, {"to", "HEAD"}, {"path", "model.opad"}});
  CHECK(d["files"].size() == 1 && d["documents"][0]["summary"] == mcp().call("git_diff", {{"repo", s(second)}, {"from", "HEAD~1"}, {"to", "HEAD"}})["documents"][0]["summary"]);
  mcp().refused("git_diff", {{"repo", s(second)}, {"from", "nowhere"}}, "unknown_revision");
  std::printf("init as Set up repository (refused inside a work tree, first commit on a protected branch refused), tags never moved, diffs PASS\n");
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir dir;
  g_root = &dir;
  if (git::findProgram().isEmpty()) {
    std::printf("git not found: skipped\n");
    return 0;
  }
  // git's own settings stay out of it: no global or system config, a fixed identity.
  qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(dir.filePath("gitconfig")));
  qputenv("GIT_CONFIG_NOSYSTEM", "1");
  for (const char* k : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"}) qputenv(k, "OPAD Test");
  for (const char* k : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"}) qputenv(k, "test@example.com");
  qputenv("OPAD_CACHE_DIR", QFile::encodeName(dir.filePath("cache")));
  Server server;
  g_server = &server;
  return check::run_all(argc, argv);
}
