// What each sketch tool asks for (app/SketchSteps.hpp, TODO 11 UI-25): every tool the panel, the ribbon or the code can
// start has its steps, so no prompt is blank, and every step follows the verb style.
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "check.hpp"
#include "SketchSteps.hpp"

namespace {
std::string source(const char* name) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "app" / name;
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
std::set<std::string> matches(const std::string& text, const std::regex& pattern) {
  std::set<std::string> out;
  for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) out.insert((*it)[1].str());
  return out;
}
}  // namespace

TEST(every_tool_has_named_steps_in_the_verb_style) {
  std::set<std::string> ids;
  for (const auto& tool : sketchsteps::tools()) {
    CHECK(ids.insert(tool.id).second);  // one entry each
    CHECK(*tool.name);
    CHECK(!tool.steps.empty());
    for (const char* step : tool.steps) {
      if (!sketchsteps::styled(step)) throw check::Failure(std::string(tool.id) + ": \"" + step + "\" does not start with Click, Pick, Select, Set, Choose, Press or Apply");
    }
    const std::string note = tool.note;
    CHECK(note.empty() || ((std::islower(static_cast<unsigned char>(note[0])) || note.rfind("Alt", 0) == 0) && note.back() != '.'));
  }
  CHECK(ids.size() >= 70);
  CHECK(sketchsteps::find("c:symmetric")->steps.size() == 3);
  CHECK(sketchsteps::find("nothing") == nullptr);
  CHECK(!sketchsteps::styled("Ready") && !sketchsteps::styled("Click the centre.") && !sketchsteps::styled("click the centre") && !sketchsteps::styled("Click "));
}

TEST(every_tool_the_app_starts_has_steps) {
  std::set<std::string> started;
  // The sketch panel's registry (its tool list and the ribbon's More tools menus) ...
  for (const auto& id : matches(source("SketchPanel.cpp"), std::regex(R"re(\{QObject::tr\("[^"]*"\),"([a-z0-9_:]+)",QObject::tr\()re"))) started.insert(id);
  CHECK(started.size() >= 60);
  // ... the ribbon's own tool buttons and every tool the code starts by name.
  for (const auto& id : matches(source("MainWindow.cpp"), std::regex(R"re(\{"([a-z0-9_:]+)", tr\("[^"]*"\), "[A-Za-z0-9]+"\})re"))) started.insert(id);
  for (const char* file : {"SketchEditor.cpp", "SketchTools.cpp", "SketchWorkflow.cpp", "SketchModify.cpp", "SketchPrimitives.cpp", "SketchImages.cpp", "SketchPanel.cpp", "DesignController.cpp", "MainWindow.cpp"})
    for (const auto& id : matches(source(file), std::regex(R"re(setTool\("([a-z0-9_:]+)"\))re"))) started.insert(id);
  for (const auto& id : started)
    if (!sketchsteps::find(id)) throw check::Failure("the sketch tool \"" + id + "\" has no steps in app/SketchSteps.hpp: its prompt would be blank");
}

CHECK_MAIN()
