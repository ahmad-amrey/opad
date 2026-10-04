// The extension seams of UI-119 that parallel work adds to without editing shared lines: area icon tables
// (OPAD_ICON_TABLE), translation fragments (app/i18n/<code>/*.json), the bench registry (OPAD_BENCH), the feature-area
// registry (OPAD_AREA) and the ribbon layout areas add to.
#include <QAction>
#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Ribbon.hpp"
#include "check.hpp"

OPAD_ICON_TABLE(seams, {"seamsSquare", R"(<rect x="4" y="4" width="16" height="16"/>)"},
                {"seamsDot", R"(<circle cx="12" cy="12" r="3" fill="currentColor"/>)"});
OPAD_ICON_TABLE(seamsAgain, {"seamsDot", R"(<circle cx="12" cy="12" r="3" fill="currentColor"/>)"}, {"open", R"(<path d="M0 0h24"/>)"});

OPAD_BENCH(OPAD_BENCH_SEAMS_TEST_B, second) { return true; }
OPAD_BENCH(OPAD_BENCH_SEAMS_TEST_A, first) { return true; }
OPAD_BENCH(OPAD_BENCH_SEAMS_TEST_A, again) { return false; }

namespace {
struct SeamsAreaB : AreaController {
  using AreaController::AreaController;
};
struct SeamsAreaA : AreaController {
  using AreaController::AreaController;
};
struct SeamsAreaAgain : AreaController {
  using AreaController::AreaController;
};
}  // namespace
OPAD_AREA(SeamsAreaB)
OPAD_AREA(SeamsAreaA)
[[maybe_unused]] static const bool seamsAreaAgain = areas::add("SeamsAreaA", [](AreaServices& s) -> AreaController* { return new SeamsAreaAgain(s); });
[[maybe_unused]] static const bool seamsAreaOff = areas::add("SeamsAreaOff", [](AreaServices&) -> AreaController* { return nullptr; });

namespace {
void write(const QString& path, const char* text) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(text);
}
QStringList codes(const QString& dir) {
  QStringList out;
  for (const i18n::Language& l : i18n::languages(dir)) out << l.code;
  return out;
}
}  // namespace

TEST(icon_tables) {
  CHECK(icons::has("seamsSquare") && icons::has("seamsDot") && icons::has("open") && !icons::has("seamsNone"));
  CHECK(icons::clashes() == QStringList{"open"});  // the same markup twice is no clash; a taken name keeps its first
  const QImage square = icons::pixmap("seamsSquare", Qt::black, 24).toImage();
  CHECK(square.pixelColor(4, 12).alpha() > 0 && square.pixelColor(12, 12).alpha() == 0);  // the outline, hollow
  const QImage open = icons::pixmap("open", Qt::black, 24).toImage();
  CHECK(open.pixelColor(0, 0).alpha() == 0 && open.pixelColor(3, 15).alpha() > 0);  // still the folder
}

TEST(fragments_merge_after_their_language) {
  QTemporaryDir built, local;
  write(built.filePath("ar.json"), R"({"@name": "عربي", "@rtl": true, "Open": "فتح", "Save": "حفظ", "Close": ""})");
  write(built.filePath("ar/b-area.json"), R"({"Save": "احفظ", "Close": "إغلاق", "@name": "x"})");
  write(built.filePath("ar/a-area.json"), R"({"Save": "first", "Draw": "ارسم"})");
  write(built.filePath("de.json"), R"({"@name": "Deutsch", "Open": "Öffnen"})");
  write(local.filePath("ar/a-area.json"), R"({"Open": "افتح"})");
  const QHash<QString, QString> t = i18n::table("ar", {built.path(), local.path()});
  CHECK(t.value("Save") == QString::fromUtf8("احفظ"));  // fragments after ar.json, by name: b-area after a-area
  CHECK(t.value("Close") == QString::fromUtf8("إغلاق") && t.value("Draw") == QString::fromUtf8("ارسم"));
  CHECK(t.value("Open") == QString::fromUtf8("افتح"));  // the app dir's fragments after the built-in ones
  CHECK(t.size() == 4 && !t.contains("@name"));
  CHECK(i18n::table("de", {built.path()}).size() == 1);
  CHECK(codes(built.path()) == QStringList({"en", "ar", "de"}));  // a fragment folder is not a language
}

TEST(embedded_translations) {
  // The generated qrc holds every JSON file of app/i18n, fragments included, at the same path under :/i18n.
  int files = 0;
  for (QDirIterator it(OPAD_I18N_SOURCE, {"*.json"}, QDir::Files, QDirIterator::Subdirectories); it.hasNext(); ++files)
    CHECK(QFile::exists(":/i18n/" + QDir(OPAD_I18N_SOURCE).relativeFilePath(it.next())));
  CHECK(files >= 1);
  const QStringList built = codes(":/i18n");
  CHECK(built.front() == "en" && built.contains("ar"));
  for (const QString& code : built) CHECK(!code.contains('/') && (code == "en" || QFile::exists(":/i18n/" + code + ".json")));
  CHECK(i18n::table("ar", {":/i18n"}).size() > 1000);
}

TEST(bench_registry) {
  CHECK(bench::variables() == QStringList({"OPAD_BENCH_SEAMS_TEST_A", "OPAD_BENCH_SEAMS_TEST_B"}));
  CHECK(bench::clashes() == QStringList{"OPAD_BENCH_SEAMS_TEST_A"});  // two files claim one switch: the first keeps it
  CHECK(bench::pending().isEmpty());
  qputenv("OPAD_BENCH_SEAMS_TEST_B", "1");
  CHECK(bench::pending() == "OPAD_BENCH_SEAMS_TEST_B");
  qputenv("OPAD_BENCH_SEAMS_TEST_A", "x");
  CHECK(bench::pending() == "OPAD_BENCH_SEAMS_TEST_A");  // by name when several are set
  qunsetenv("OPAD_BENCH_SEAMS_TEST_A");
  qunsetenv("OPAD_BENCH_SEAMS_TEST_B");
  CHECK(bench::pending().isEmpty());
}

TEST(area_registry) {
  CHECK(areas::names() == QStringList({"SeamsAreaA", "SeamsAreaB", "SeamsAreaOff"}));
  CHECK(areas::clashes() == QStringList{"SeamsAreaA"});  // two files claim one name: the first keeps it
  AreaServices services(nullptr);
  const std::vector<AreaController*> made = areas::create(services);
  CHECK(made.size() == 2);  // SeamsAreaOff declined this run
  CHECK(made[0]->objectName() == "SeamsAreaA" && dynamic_cast<SeamsAreaA*>(made[0]) && made[1]->objectName() == "SeamsAreaB");
  CHECK(&made[0]->services() == &services && made[0]->maybeClose());
  for (AreaController* area : made) delete area;
}

TEST(ribbon_layout) {
  QAction a("a"), b("b"), c("c");
  RibbonLayout layout;
  layout.addWorkspace("review", {"Review", "eye", "Ctrl+1", "", ""});
  layout.addWorkspace("design", {"Design", "component", "Ctrl+2", "", ""});
  CHECK(layout.addWorkspace("review", {"Other", "", "", "", ""}).workspace.name == "Review");  // an id once
  const Workspace probe{"Probe", "dot", "Ctrl+9", "What it is for", "ops: none"};  // the positional form every area uses
  CHECK(probe.description == "What it is for" && probe.ops == "ops: none" && probe.command.isEmpty() && !probe.contextual);
  CHECK(layout.addTab("review", "review.view", "View", {{&a}}) && layout.addTab("design", "design.solid", "Solid"));
  CHECK(!layout.addTab("drawings", "drawings.sheet", "Sheet"));  // no such workspace
  CHECK(layout.addGroup("review.view", {&b, &c}) && !layout.addGroup("review.none", {&a}));
  layout.addWorkspace("drawings", {"Drawings", "drawing", "Ctrl+3", "", ""});
  layout.addTab("drawings", "drawings.sheet", "Sheet", {{&c}});
  CHECK(layout.index("review") == 0 && layout.index("design") == 1 && layout.index("drawings") == 2 && layout.index("none") == -1);
  const QList<RibbonLayout::Group>& groups = layout.tab("review.view")->groups;  // untitled groups (UI-120 b: titled ones too)
  CHECK(groups.size() == 2 && groups[0].actions() == QList<QAction*>{&a} && groups[1].actions() == QList<QAction*>({&b, &c}) && groups[1].title.isEmpty());
  CHECK(layout.workspace("design")->tabs.size() == 1 && layout.tab("drawings.sheet")->title == "Sheet" && !layout.tab("sheet"));
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return check::run_all(argc, argv);
}
