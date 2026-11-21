// The AI clients' registration (app/AgentClientConfig.cpp): the one change OPAD makes to a client's JSON configuration
// (Claude Desktop's claude_desktop_config.json) keeps every other server and key in their order, copies the file to .bak
// first, is idempotent and refuses a file it cannot read as JSON; and a registration found in a client compares with this
// copy of OPAD as connected, missing program, another OPAD / old data folder, or not registered.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <nlohmann/json.hpp>

#include "AgentClientConfig.hpp"
#include "check.hpp"

using namespace agentclients;
using ojson = nlohmann::ordered_json;

namespace {
QByteArray read(const QString& file) {
  QFile f(file);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
void write(const QString& file, const QByteArray& data) {
  QFile f(file);
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(data);
}
}  // namespace

TEST(desktop_config_merge_keeps_everything_else) {
  QTemporaryDir dir;
  const QString file = dir.filePath("Claude/claude_desktop_config.json");
  QDir().mkpath(dir.filePath("Claude"));
  const QByteArray original = R"({
  "globalShortcut": "Ctrl+Space",
  "mcpServers": {
    "filesystem": {"command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "C:/work"]},
    "opad": {"command": "C:/old/opad-cli.exe", "args": ["mcp", "--live", "--discovery", "C:/old/agent"], "env": {"X": "1"}}
  },
  "zLast": [1, 2, 3]
})";
  write(file, original);
  const Entry mine{"C:/Program Files/OPAD/opad-cli.exe", serverArgs("D:/Data/opad/OPAD/agent"), true};
  bool changed = false;
  QString error;
  CHECK(writeServer(file, mine, &error, &changed) && changed && error.isEmpty());
  CHECK(read(file + ".bak") == original);  // the file as it was, copied first
  const ojson after = ojson::parse(read(file).toStdString());
  CHECK(after["globalShortcut"] == "Ctrl+Space" && after["zLast"].size() == 3);
  CHECK(after["mcpServers"]["filesystem"]["args"][1] == "@modelcontextprotocol/server-filesystem");
  CHECK(after["mcpServers"]["opad"]["command"] == "C:/Program Files/OPAD/opad-cli.exe");
  CHECK(after["mcpServers"]["opad"]["env"]["X"] == "1");  // a key of the user's on the entry stays
  std::vector<std::string> order;
  for (const auto& [key, value] : after.items()) order.push_back(key);
  CHECK((order == std::vector<std::string>{"globalShortcut", "mcpServers", "zLast"}));  // key order kept
  const auto found = readServer(file);
  CHECK(found && found->command == mine.command && found->args == mine.args);

  // Idempotent: the same again writes nothing (and leaves the .bak of the first change).
  const QByteArray once = read(file);
  CHECK(writeServer(file, mine, &error, &changed) && !changed && read(file) == once && read(file + ".bak") == original);

  // Removed: only "opad" goes, the other server and keys stay.
  CHECK(writeServer(file, std::nullopt, &error, &changed) && changed);
  const ojson removed = ojson::parse(read(file).toStdString());
  CHECK(!removed["mcpServers"].contains("opad") && removed["mcpServers"].contains("filesystem") && removed["globalShortcut"] == "Ctrl+Space");
  CHECK(!readServer(file));
  CHECK(writeServer(file, std::nullopt, &error, &changed) && !changed);  // nothing to remove: nothing written
}

TEST(desktop_config_created_when_missing_and_broken_file_refused) {
  QTemporaryDir dir;
  const QString file = dir.filePath("Claude/claude_desktop_config.json");
  const Entry mine{"/Applications/OPAD.app/Contents/MacOS/opad-cli", serverArgs("/Volumes/Data/opad/OPAD/agent"), true};
  bool changed = false;
  CHECK(writeServer(file, mine, nullptr, &changed) && changed && !QFile::exists(file + ".bak"));  // nothing to back up
  CHECK(readServer(file).has_value());
  const QByteArray broken = "{ \"mcpServers\": { oops";
  write(file, broken);
  QString error;
  CHECK(!writeServer(file, mine, &error, &changed) && !changed && !error.isEmpty());
  CHECK(read(file) == broken);  // left alone
  CHECK(!readServer(file, &error) && !error.isEmpty());
}

TEST(registrations_compare_with_this_copy) {
  QTemporaryDir dir;
  const QString exe = dir.filePath("opad-cli.exe"), other = dir.filePath("other/opad-cli.exe");
  write(exe, "x");
  QDir().mkpath(dir.filePath("other"));
  write(other, "x");
  const QString discovery = dir.filePath("agent");
  CHECK(classify(std::nullopt, exe, discovery) == State::NotRegistered);
  CHECK(classify(Entry{exe, serverArgs(discovery), true}, exe, discovery) == State::Registered);
  CHECK(classify(Entry{QDir::toNativeSeparators(exe), {"mcp", "--live", "--discovery", QDir::toNativeSeparators(discovery)}, true}, exe, discovery) == State::Registered);
  CHECK(classify(Entry{exe, serverArgs(discovery), false}, exe, discovery) == State::Disabled);
  CHECK(classify(Entry{other, serverArgs(discovery), true}, exe, discovery) == State::Stale);             // another OPAD
  CHECK(classify(Entry{exe, serverArgs(dir.filePath("opad-data/opad/agent")), true}, exe, discovery) == State::Stale);  // an old folder
  CHECK(classify(Entry{dir.filePath("gone/opad-cli.exe"), serverArgs(discovery), true}, exe, discovery) == State::Missing);  // moved away
}

CHECK_MAIN()
