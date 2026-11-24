// What an AI assistant did, in the user's words (app/AgentSentences.cpp): every live tool is placed (hidden, folded or a row
// of its own) and every feature kind has a sentence; no sentence shows a tool's snake_case name; past tense comes from what
// changed (names from the scene, values in the shown unit); text an assistant supplied is quoted and capped; client names
// read as the user knows them.
#include <QCoreApplication>
#include <QRegularExpression>

#include "AgentSentences.hpp"
#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/design/feature.hpp"
#include "opad/live.hpp"

using namespace opad;
using agentsay::plain;

#define CHECK_MSG(expr, msg)                                                                                            do {                                                                                                                    if (!(expr)) throw check::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " CHECK(" #expr ") " + std::string(msg));   } while (0)
// Text a check shows: std::string of a QString or a std::string.
inline std::string str(const QString& s) { return s.toStdString(); }

namespace {
bool snake(const QString& text) {
  static const QRegularExpression re(QStringLiteral("\\b[a-z]+_[a-z_]+\\b"));
  return re.match(text).hasMatch();
}
struct Model {
  Document doc = Document::create();
  Scene before, after;
  json output;
  void run(const std::string& name, json args) {
    before = resolve(doc);
    const auto first = doc.ops.size();
    output = commands::run(name, args, &doc);
    after = resolve(doc);
    if (output.is_object()) {
      output["operation_ids"] = json::array();
      for (size_t i = first; i < doc.ops.size(); ++i) output["operation_ids"].push_back(doc.ops[i].id);
      for (const auto& id : output.value("ids", json::array()))
        if (id.is_string() && after.feature(id.get<std::string>())) output["feature_id"] = id;
    }
  }
  QString say(const std::string& name, const json& args) { return plain(agentsay::done(name, args, output, before, after, units::Display{}).text); }
};
std::string boxBody(const Scene& s) {
  for (const auto& f : s.features)
    if (f.kind == "box")
      for (const auto& b : f.result.value("bodies", json::array())) return b.at("id").get<std::string>();
  return {};
}
}  // namespace

TEST(every_live_tool_is_placed_and_says_no_tool_name) {
  Model m;
  m.run("feature", {{"kind", "box"}, {"name", "Bracket"}, {"inputs", {{"length", "40 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}});
  for (const auto& tool : agent::live_tools()) {
    const auto name = tool.at("name").get<std::string>();
    const auto kind = agentsay::kindOf(name, json::object());
    if (kind == agentsay::Kind::Hidden) continue;
    const auto said = agentsay::done(name, json::object(), json::object(), m.after, m.after, units::Display{});
    const QString text = plain(said.text);
    CHECK_MSG(!text.isEmpty(), name);
    CHECK_MSG(!snake(text), name + ": " + text.toStdString());
    CHECK_MSG(!text.contains(QString::fromStdString(name)) || !QString::fromStdString(name).contains('_'), name);
    if (kind != agentsay::Kind::Fold) CHECK_MSG(!plain(agentsay::running(name, json::object(), m.after, units::Display{})).isEmpty(), name);
  }
  for (const char* h : {"live_state", "wait_for_idle", "request_status", "live_diagnostics", "feature_schema"}) CHECK(agentsay::kindOf(h, json::object()) == agentsay::Kind::Hidden);
  for (const char* f : {"tree", "entity_details", "query_entities", "git_status"}) CHECK(agentsay::kindOf(f, json::object()) == agentsay::Kind::Fold);
  CHECK(agentsay::kindOf("context", {{"section", "ai_agent_notes"}}) == agentsay::Kind::Note);
  CHECK(agentsay::kindOf("viewport_image", json::object()) == agentsay::Kind::Look);
}

TEST(every_feature_kind_has_a_sentence) {
  Model m;
  m.run("feature", {{"kind", "box"}, {"name", "Bracket"}, {"inputs", {{"length", "40 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}});
  for (const auto& spec : design::feature_specs()) {
    Feature f;
    f.id = "x";
    f.kind = spec.kind;
    f.name = spec.label + " 1";
    f.inputs = json::object();
    const auto said = agentsay::feature({{"kind", spec.kind}}, f, m.after, m.after, units::Display{});
    const QString text = plain(said.text);
    CHECK_MSG(!text.isEmpty() && !snake(text), spec.kind + ": " + text.toStdString());
    CHECK_MSG(plain(said.second).startsWith(QString::fromStdString(spec.label)), spec.kind);
  }
}

TEST(past_tense_comes_from_the_scene) {
  Model m;
  m.run("feature", {{"kind", "box"}, {"name", "Bracket"}, {"inputs", {{"length", "40 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}});
  CHECK_MSG(m.say("feature", {{"kind", "box"}}) == QStringLiteral("Added a 40 × 20 × 10 mm box"), str(m.say("feature", {{"kind", "box"}})));
  const auto body = boxBody(m.after);
  CHECK(!body.empty());
  // The body's name is the user's (renamed after the box was made), the radius in the shown unit.
  m.run("rename", {{"target", body}, {"name", "Bracket"}});
  CHECK_MSG(m.say("rename", {{"target", body}, {"name", "Bracket"}}).startsWith("Renamed ") && m.say("rename", {{"target", body}, {"name", "Bracket"}}).endsWith(" to Bracket"), str(m.say("rename", {{"target", body}, {"name", "Bracket"}})));
  json edges = json::array();
  for (int i = 1; i <= 4; ++i) edges.push_back(body + "/edge/" + std::to_string(i));
  m.run("feature", {{"kind", "fillet"}, {"inputs", {{"edges", edges}, {"radius", "2 mm"}}}});
  const auto said = agentsay::done("feature", {{"kind", "fillet"}}, m.output, m.before, m.after, units::Display{});
  CHECK_MSG(plain(said.text) == QStringLiteral("Filleted 4 edges of Bracket, 2 mm"), str(plain(said.text)));
  CHECK(plain(said.second).startsWith("Fillet"));
  CHECK(std::find(said.subjects.begin(), said.subjects.end(), body) != said.subjects.end());
  CHECK(!said.op.empty());
  units::Display inches;
  inches.length = "in";
  const auto inInches = agentsay::done("feature", {{"kind", "fillet"}}, m.output, m.before, m.after, inches);
  CHECK_MSG(plain(inInches.text).endsWith(" in"), str(plain(inInches.text)));
}

TEST(agent_text_is_quoted_and_capped) {
  const QString long_text(200, QLatin1Char('x'));
  const QString q = plain(agentsay::quote(long_text));
  CHECK(q.startsWith(QStringLiteral("“")) && q.endsWith(QStringLiteral("”")));
  CHECK(q.size() <= 62);
  CHECK(plain(agentsay::quote("  two\nlines ")) == QStringLiteral("“two lines”"));
}

TEST(counts_use_whole_phrases) {
  CHECK(agentsay::lookups(1) == "1 lookup");
  CHECK(agentsay::lookups(2) == "2 lookups");
  CHECK(agentsay::lookups(12) == "12 lookups");
  CHECK(agentsay::changesCount(1) == "1 change");
  CHECK(agentsay::changesCount(4) == "4 changes");
}

TEST(failures_hide_what_the_assistant_retries) {
  for (const char* code : {"busy", "stale_revision", "invalid_arguments", "unknown_preview", "edit_session_busy", "cancelled"})
    CHECK_MSG(agentsay::failure(code, "x", "Adding a fillet…").isEmpty(), code);
  const QString solid = plain(agentsay::failure("invalid_operation", "Generated body failed solid validation; document unchanged.", "Shelling Bracket…"));
  CHECK(solid.contains("Shelling Bracket"));
  CHECK(solid.contains("valid solid") && solid.contains("Nothing changed"));
}

TEST(client_names) {
  using agentnames::display;
  CHECK(display("claude-code").full == "Claude Code" && display("claude-code").brief == "Claude");
  CHECK(display("claude-ai").full == "Claude Desktop");
  CHECK(display("Claude Desktop").brief == "Claude");
  CHECK(display("codex-mcp-client").full == "Codex");
  CHECK(display("cursor-vscode").full == "Cursor");
  CHECK(display("my_tool-mcp-client").full == "My Tool");
  CHECK(display("MCP client").full == "AI assistant");
  CHECK(display("").brief == "AI assistant");
  CHECK(display(QString(100, QLatin1Char('a'))).full.size() == 40);
}

TEST(ai_steps_name_their_assistant) {
  using agentsay::author;
  using agentsay::authorLine;
  const QDateTime now = QDateTime::currentDateTime();
  const std::string today = now.toUTC().toString(Qt::ISODate).toStdString();
  const json ai = {{"by", "Agent"}, {"ai_client", "Claude"}, {"ts", today}};
  CHECK(str(author(ai)) == "Claude (AI)");
  CHECK_MSG(str(authorLine(ai, now)) == "Claude (AI) · " + now.toString("HH:mm").toStdString(), str(authorLine(ai, now)));
  CHECK(str(author({{"by", "Agent"}})) == "AI (AI)");  // an older step: no name saved
  const json older = {{"by", "Agent"}, {"ai_client", "Codex"}, {"ts", "2024-03-05T09:30:00Z"}};
  CHECK_MSG(str(authorLine(older, now)).rfind("Codex (AI) · 2024-03-0", 0) == 0, str(authorLine(older, now)));  // another day: the date too
  const json person = {{"by", "Ahmad"}, {"ts", "2024-03-05T09:30:00Z"}};
  CHECK(str(author(person)) == "Ahmad");
  CHECK_MSG(str(authorLine(person, now)).rfind("Ahmad · 2024-03-0", 0) == 0, str(authorLine(person, now)));  // as it always read
  CHECK(str(authorLine({{"by", "Agent"}, {"ai_client", "Claude"}}, now)) == "Claude (AI)");  // no time saved
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return check::run_all(argc, argv);
}
