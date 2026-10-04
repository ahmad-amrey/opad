// Command help (UI-106): every command id the app registers has an English record and its Arabic translation, the
// registry's lookups, search and tooltips, and the rich card's states, timing and layout (offscreen). The animated
// clips (UI-107): the library, rendering, translations, the loader's checks and templates, the player.
#include "CommandHelp.hpp"
#include "CommandPalette.hpp"
#include "GuidedTool.hpp"
#include "HelpClip.hpp"
#include "HelpReference.hpp"
#include "HelpWindows.hpp"
#include "I18n.hpp"
#include "Motion.hpp"
#include "RichTip.hpp"
#include "Theme.hpp"
#include "check.hpp"
#include "opad/design/feature.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <QToolButton>
#include <QVBoxLayout>
#include <set>

namespace {
QString source(const QString& path) {
  QFile f(QStringLiteral(OPAD_SOURCE_DIR) + "/" + path);
  if (!f.open(QIODevice::ReadOnly)) throw check::Failure("cannot read " + path.toStdString());
  return QString::fromUtf8(f.readAll());
}

// Command ids registered by MainWindow (MainWindow*.cpp): literal addAction ids (and the Help menu's help("help.about",
// ...) helper of the IP branch), CommandInfo ids, the generated families, the sketch tool tables and the feature kinds of
// the core spec table. The bench OPAD_BENCH_RICHTIP checks the live list of the running app.
std::set<QString> registeredIds() {
  std::set<QString> ids;
  QString main;
  for (const QString& file : QDir(QStringLiteral(OPAD_SOURCE_DIR) + "/app").entryList({"MainWindow*.cpp"}, QDir::Files, QDir::Name)) main += source("app/" + file);
  for (const auto& m : QRegularExpression(R"(\b(?:addAction|help)\("([a-z]+\.[A-Za-z0-9_.]+)\")").globalMatch(main)) ids.insert(m.captured(1));
  const QRegularExpression info(R"(\.id\s*=\s*"([a-z]+\.[A-Za-z0-9_.]+)\")");
  for (const auto& m : info.globalMatch(main)) ids.insert(m.captured(1));
  // An area's records: info.id = "..." or CommandInfo name{"...", ...}.
  const QRegularExpression braced(R"(\bCommandInfo\s+\w+\s*\{\s*"([a-z]+\.[A-Za-z0-9_.]+)\")");
  for (const QString& file : QDir(QStringLiteral(OPAD_SOURCE_DIR) + "/app").entryList({"*Area.cpp"}, QDir::Files, QDir::Name)) {
    const QString text = source("app/" + file);
    for (const auto& m : info.globalMatch(text)) ids.insert(m.captured(1));
    for (const auto& m : braced.globalMatch(text)) ids.insert(m.captured(1));
  }
  for (const auto& m : QRegularExpression(R"re(\{"([a-z0-9_:]+)", tr\(")re").globalMatch(main)) ids.insert("sketch." + m.captured(1).replace(':', '.'));
  for (const auto& m : QRegularExpression(R"re(QObject::tr\("[^"]+"\),"([a-z0-9_:]+)")re").globalMatch(source("app/SketchPanel.cpp")))
    ids.insert("sketch." + m.captured(1).replace(':', '.'));
  for (const char* v : {"top", "front", "right", "iso", "bottom", "back", "left"}) ids.insert(QString("view.") + v);
  for (const char* v : {"fusion", "solidworks", "onshape", "blender"}) ids.insert(QString("nav.") + v);
  // The view navigation staples (ViewNavigation.cpp, UI-47): its add("...") helper and one CommandInfo of its own.
  for (const auto& m : QRegularExpression(R"re(\b(?:add\(|CommandInfo\s+\w+\s*\{)"([a-z0-9]+\.[A-Za-z0-9_.]+)")re").globalMatch(source("app/ViewNavigation.cpp")))
    ids.insert(m.captured(1));
  for (const char* v : {"bodies", "faces", "edges", "vertices"}) ids.insert(QString("select.") + v);
  for (const char* v : {"view.extensions", "view.tracking", "view.gridSnap", "view.orthoSnap", "view.polarSnap", "sketch.selectionOptions", "sketch.constraints", "sketch.snaps"}) ids.insert(v);
  for (const char* v : {"Create", "Modify", "Constrain", "Reference", "Files"}) ids.insert(QString("sketch.more") + v);
  ids.erase("sketch.more");  // the prefix of the generated menu ids
  for (const auto& spec : opad::design::feature_specs()) ids.insert("design." + QString::fromStdString(spec.kind));
  return ids;
}

// The app's Arabic as i18n::install sets it up while it lives: ar.json and its fragments from the embedded qrc.
class Arabic : public QTranslator {
 public:
  Arabic() : m_table(i18n::table("ar", {":/i18n"})) { QCoreApplication::installTranslator(this); }
  ~Arabic() override { QCoreApplication::removeTranslator(this); }
  QString translate(const char*, const char* source, const char*, int) const override { return m_table.value(QString::fromUtf8(source)); }
  bool isEmpty() const override { return m_table.isEmpty(); }
 private:
  QHash<QString, QString> m_table;
};

int sentences(const QString& text) { return static_cast<int>(text.count(QRegularExpression(R"([.!?](\s|$))"))); }
QString clean(QString s) { return s.remove('&').remove(QString::fromUtf8("…")).remove("...").trimmed(); }
}  // namespace

TEST(every_registered_command_has_help) {
  help::load("en");
  const auto ids = registeredIds();
  CHECK(ids.size() > 200 && ids.count("help.reference"));
  // Commands the TODO 11 tracks added before their help was written: the records come with the help and ribbon pass of wave 3
  // (t7b), which empties this list.
  const QStringList pending{"assets.autoSync", "assembly.activate", "assembly.activateNew", "assembly.activateRoot", "assembly.activeHistory", "assembly.activeVisibility",
                            "assembly.explode", "assembly.explodeGroup", "assembly.explodeKeep", "assembly.explodeOff", "assembly.explodePlay",
                            "assembly.explodeSave", "assembly.explodeSplit", "assembly.explodeUngroup", "design.componentFromSelection", "design.remove_faces",
                            "file.clone", "file.documentProperties", "file.exportBom", "inspect.area", "inspect.material", "inspect.partProperties", "timeline.designOnly",
                            "timeline.historyList", "timeline.names", "timeline.rollForward", "vcs.backgroundFetch", "vcs.compare", "vcs.unsavedChanges"};
  QStringList missing;
  for (const QString& id : ids) if (!help::find(id) && !pending.contains(id)) missing << id;
  if (!missing.isEmpty()) throw check::Failure("no help for " + missing.join(", ").toStdString());
}

// The other way round: a record is for a command the app has. The licence, ODA and view cube commands come with the IP
// branch (UI-13/14, t8-ip); their help is here ahead of it, and the Tool guide lists only what the build has.
TEST(every_record_has_a_command) {
  help::load("en");
  const auto ids = registeredIds();
  const QStringList ahead{"help.licenses", "help.aboutqt", "files.useOda", "view.cubeEdgesCorners"};
  QStringList stale;
  for (const CommandHelp& h : help::all()) if (!ids.count(h.id) && !ahead.contains(h.id)) stale << h.id;
  if (!stale.isEmpty()) throw check::Failure("help for no command: " + stale.join(", ").toStdString());
}

TEST(records_are_complete) {
  help::load("en");
  CHECK(!help::all().isEmpty());
  for (const CommandHelp& h : help::all()) {
    const std::string id = h.id.toStdString();
    if (h.title.isEmpty() || h.summary.isEmpty() || h.details.isEmpty() || h.keywords.isEmpty() || h.clip.isEmpty()) throw check::Failure(id + ": empty field");
    if (sentences(h.summary) != 1) throw check::Failure(id + ": the summary is not one sentence");
    if (sentences(h.details) < 2 || sentences(h.details) > 4) throw check::Failure(id + ": details need 2-4 sentences");
    if (h.title.contains('&')) throw check::Failure(id + ": mnemonic in the title");
  }
}

TEST(arabic_covers_every_record) {
  help::load("ar");
  CHECK_EQ(help::language(), QString("ar"));
  // Titles of commands whose label is already translated in app/i18n/ar.json use that translation (one name per command).
  const QJsonObject labels = QJsonDocument::fromJson(source("app/i18n/ar.json").toUtf8()).object();
  QMultiHash<QString, QString> label;  // "Save screens&hot…" and "Save screenshot" both clean to one key
  for (auto it = labels.begin(); it != labels.end(); ++it) label.insert(clean(it.key()), clean(it.value().toString()));
  help::load("en");
  QHash<QString, QString> english;
  QHash<QString, QStringList> texts;  // summary, details, requires
  for (const CommandHelp& h : help::all()) {
    english.insert(h.id, h.title);
    texts.insert(h.id, {h.summary, h.details, h.requirement});
  }
  // {name} placeholders (filled by help::requirement, or meant literally) survive the translation.
  auto placeholders = [](const QString& text) {
    QStringList out;
    for (const auto& m : QRegularExpression(R"(\{[A-Za-z_]+\})").globalMatch(text)) out << m.captured(0);
    out.sort();
    return out;
  };
  help::load("ar");
  for (const CommandHelp& h : help::all()) {
    const std::string id = h.id.toStdString();
    if (!h.translated) throw check::Failure(id + ": not translated in app/help/commands.ar.json");
    if (!h.title.contains(QRegularExpression("[\\x{0600}-\\x{06FF}]"))) throw check::Failure(id + ": the Arabic title has no Arabic");
    if (label.contains(english.value(h.id)) && !label.values(english.value(h.id)).contains(h.title)) throw check::Failure(id + ": title differs from ar.json");
    const QStringList original = texts.value(h.id), translated{h.summary, h.details, h.requirement};
    for (int i = 0; i < 3; ++i)
      if (placeholders(original[i]) != placeholders(translated[i])) throw check::Failure(id + ": the translation changes the {placeholders}");
  }
  help::load("en");
}

// The help area's own tr() strings have their Arabic in the area's fragment app/i18n/ar/help.json (or ar.json), embedded
// with the app's translations and merged by i18n::install; without the translator they stay English.
TEST(help_area_strings_are_translated) {
  const QHash<QString, QString> arabic = i18n::table("ar", {QStringLiteral(OPAD_SOURCE_DIR) + "/app/i18n"});
  const QJsonObject fragment = QJsonDocument::fromJson(source("app/i18n/ar/help.json").toUtf8()).object();
  CHECK(!fragment.isEmpty() && i18n::table("ar", {":/i18n"}) == arabic);  // the fragment is embedded, as it is in the tree
  QStringList strings;
  for (const char* file : {"app/RichTip.cpp", "app/RichTip.hpp", "app/CommandHelp.cpp", "app/HelpBench.cpp", "app/HelpClip.cpp", "app/HelpClip.hpp", "app/HelpReference.cpp",
                           "app/HelpArea.cpp", "app/HelpWindows.cpp", "app/CommandPalette.cpp"})
    for (const auto& m : QRegularExpression(R"re(\b(?:tr\(|translate\("help", )"((?:[^"\\]|\\.)*)"\))re").globalMatch(source(file))) strings << m.captured(1);
  CHECK(strings.size() >= 2);
  for (const QString& s : strings)
    if (arabic.value(s).isEmpty()) throw check::Failure("no Arabic for \"" + s.toStdString() + "\" in app/i18n/ar/help.json");
  {
    Arabic on;
    for (const QString& s : strings) CHECK_EQ(RichTip::tr(s.toUtf8().constData()), arabic.value(s));
  }
  for (const QString& s : strings) CHECK_EQ(RichTip::tr(s.toUtf8().constData()), s);
}

TEST(lookups_search_and_tooltips) {
  help::load("en");
  const CommandHelp* extrude = help::find("design.extrude");
  CHECK(extrude && extrude->title == "Extrude" && extrude->clip == "design.extrude");
  CHECK(!help::find("no.such.command"));
  CHECK(help::matches(*help::find("design.offset_face"), "push pull"));
  CHECK(!help::matches(*extrude, "zebra"));
  CommandHelp templ;
  templ.requirement = "Select {n} bodies";
  CHECK_EQ(help::requirement(templ, {{"n", 2}}), QString("Select 2 bodies"));
  QAction a("E&xtrude");
  a.setObjectName("design.extrude");
  a.setShortcut(QKeySequence("E"));
  const QString tip = help::tooltip(&a);
  CHECK(tip.startsWith("<qt>") && tip.contains("<b>Extrude</b>") && tip.contains(extrude->summary.toHtmlEscaped()));
  help::load("ar");
  CHECK(help::matches(*help::find("design.extrude"), QString::fromUtf8("بثق")));
  CHECK(help::matches(*help::find("design.extrude"), "pull"));  // English keywords still find it
  CHECK(help::matches(*help::find("sketch.fillet"), "sketch fillet"));  // and the English name
  help::load("en");
}

TEST(rich_tip_states_and_layout) {
  help::load("en");
  QWidget window;
  auto* layout = new QVBoxLayout(&window);
  auto* extrude = new QAction("Extrude", &window);
  extrude->setObjectName("design.extrude");
  extrude->setShortcut(QKeySequence("E"));
  extrude->setData("extrude");
  auto* button = new QToolButton(&window);
  button->setDefaultAction(extrude);
  auto* other = new QToolButton(&window);
  layout->addWidget(button);
  layout->addWidget(other);
  window.resize(300, 200);
  window.show();
  RichTip::attach(button, "design.extrude");
  RichTip* tip = RichTip::instance();
  auto move = [](QWidget* w) {
    const QPointF at(w->width() / 2.0, w->height() / 2.0);
    QMouseEvent e(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
  };
  move(button);
  QTest::qWait(250);
  CHECK(tip->state() == RichTip::State::Hidden);
  QTest::qWait(350);
  CHECK(tip->state() == RichTip::State::Compact && tip->isVisible() && tip->commandId() == "design.extrude");
  CHECK(tip->width() <= 372 && !tip->showsRequirement() && !tip->clip());
  const int compact = tip->height();
  QTest::qWait(1400);
  CHECK(tip->state() == RichTip::State::Expanded && tip->height() > compact);
  CHECK(!tip->grab().toImage().isNull());
  extrude->setEnabled(false);
  tip->showFor(button, RichTip::State::Compact);
  CHECK(tip->showsRequirement());
  extrude->setEnabled(true);
  RichTip::setClipFactory([](const QString& clip, QWidget* parent) { auto* w = new QWidget(parent); w->setObjectName(clip); return w; });
  tip->hideTip();
  tip->showFor(button, RichTip::State::Expanded);
  CHECK(tip->clip() && tip->clip()->objectName() == "design.extrude" && tip->clip()->size() == RichTip::kClip);
  RichTip::setClipFactory(nullptr);
  move(other);  // off the button: the card stays 300 ms for the pointer to reach it, then hides
  CHECK(tip->state() == RichTip::State::Expanded);
  QTest::qWait(450);
  CHECK(tip->state() == RichTip::State::Hidden && !tip->isVisible());
  // Right to left: the card mirrors, the icon on the right.
  QApplication::setLayoutDirection(Qt::RightToLeft);
  tip->showFor(button, RichTip::State::Compact);
  CHECK(tip->layoutDirection() == Qt::RightToLeft);
  QApplication::setLayoutDirection(Qt::LeftToRight);
  tip->hideTip();
  RichTip::detach(button);
}

// The palette's recent commands (UI-106): newest first, each once, at most kRecent; the palette itself and undo/redo are
// not remembered.
TEST(palette_recent_commands) {
  CHECK_EQ(palette::remember({}, "view.fit"), QStringList({"view.fit"}));
  CHECK_EQ(palette::remember({"view.fit", "view.home"}, "view.home"), QStringList({"view.home", "view.fit"}));
  CHECK_EQ(palette::remember({"a", "b", "c"}, "d", 3), QStringList({"d", "a", "b"}));
  CHECK_EQ(palette::remember({"a"}, QString()), QStringList({"a"}));
  QSettings().remove("palette/recent");
  for (const char* id : {"design.extrude", "tools.commands", "edit.undo", "edit.redo", "view.fit"}) palette::noteRun(id);
  CHECK_EQ(palette::recent(), QStringList({"view.fit", "design.extrude"}));
  for (int i = 0; i < 20; ++i) palette::noteRun(QString("x.%1").arg(i));
  CHECK_EQ(palette::recent().size(), palette::kRecent);
  CHECK_EQ(palette::recent().first(), QString("x.19"));
  QSettings().remove("palette/recent");
}

// The Help menu's windows (UI-108): key caps, the mouse of each navigation preset as Viewport::setNavPreset binds it,
// the cheat sheet's groups (keyed commands only, the sketch's first while sketching, then the mouse and every tool's
// keys), the problem report's text and the Getting started lessons (each clip and command there).
TEST(help_menu_contents) {
  help::load("en");
  CHECK_EQ(help::keyCaps("Ctrl+Shift+U"), QStringList({"Ctrl", "Shift", "U"}));
  CHECK_EQ(help::keyCaps("Ctrl++"), QStringList({"Ctrl", "+"}));
  CHECK_EQ(help::keyCaps("F1"), QStringList({"F1"}));
  CHECK_EQ(help::keyAlternates("Ctrl+Y / Ctrl+Shift+Z"), QStringList({"Ctrl+Y", "Ctrl+Shift+Z"}));
  CHECK_EQ(help::keyAlternates("Ctrl+/"), QStringList({"Ctrl+/"}));
  {  // the cheat sheet draws each alternate's caps, a plain "/" between them
    ShortcutSheet sheet;
    sheet.setGroups({help::KeyGroup{"Edit", {help::KeyRow{"Redo", "Ctrl+Y / Ctrl+Shift+Z"}, help::KeyRow{"Menu", "Menu key / Shift+F10"}}}});
    QStringList caps, plain;
    for (QLabel* l : sheet.findChildren<QLabel*>()) (l->objectName() == "keycap" ? caps : plain) << l->text();
    CHECK_EQ(caps, QStringList({"Ctrl", "Y", "Ctrl", "Shift", "Z", "Menu key", "Shift", "F10"}));
    CHECK(plain.count("/") == 2);
  }
  auto mouse = [](const QString& preset, int row) { return help::mouseRows(preset).value(row).keys; };
  CHECK_EQ(mouse("fusion", 0), QString("Shift+Middle drag"));
  CHECK_EQ(mouse("fusion", 1), QString("Middle drag"));
  CHECK_EQ(mouse("solidworks", 0), QString("Middle drag"));
  CHECK_EQ(mouse("solidworks", 1), QString("Ctrl+Middle drag"));
  CHECK_EQ(mouse("onshape", 0), QString("Right drag"));
  CHECK_EQ(mouse("blender", 1), QString("Shift+Middle drag"));
  QAction fit("Fit"), line("Line"), box("Box");
  fit.setObjectName("view.fit");
  fit.setShortcut(QKeySequence("F"));
  fit.setProperty("commandGroup", "View");
  line.setObjectName("sketch.line");
  line.setShortcut(QKeySequence("L"));
  line.setProperty("commandGroup", "Sketch");
  box.setObjectName("design.box");  // no key: not on the sheet
  auto titles = [](const QList<help::KeyGroup>& groups) {
    QStringList out;
    for (const auto& g : groups) out << g.title;
    return out;
  };
  const auto groups = help::keyGroups({&fit, &line, &box}, false, "fusion");
  CHECK_EQ(titles(groups), QStringList({"View", "Sketch", "Mouse", "Without the mouse", "In every tool"}));
  CHECK(groups[0].rows.size() == 1 && groups[0].rows[0].label == help::find("view.fit")->title && groups[0].rows[0].keys == "F");
  CHECK_EQ(titles(help::keyGroups({&fit, &line, &box}, true, "fusion")), QStringList({"Sketch", "View", "Mouse", "Without the mouse", "In every tool"}));
  CHECK_EQ(help::problemReport("  It broke \n", {"OPAD 1", "Qt 6"}), QString("What happened:\nIt broke\n\nOPAD and this computer:\n- OPAD 1\n- Qt 6\n"));
  CHECK(help::problemReport("", {}).contains("(not described)"));
  for (const char* preset : {"fusion", "solidworks", "onshape", "blender"}) {
    const auto lessons = GettingStarted::lessons(preset);
    CHECK_EQ(lessons.size(), qsizetype(6));
    CHECK_EQ(lessons[0].clip, QString("nav.") + preset);
    for (const auto& l : lessons) {
      if (!clips::has(l.clip)) throw check::Failure("no clip " + l.clip.toStdString());
      if (!l.command.isEmpty() && !help::find(l.command)) throw check::Failure("no help for " + l.command.toStdString());
      CHECK(!l.title.isEmpty() && l.text.count('.') >= 2);
    }
  }
  CHECK(GettingStarted::lessons("onshape")[0].text.contains("Right drag"));
}

// Menu entries that are commands with help show their card beside the menu; other entries (no id, a submenu) none.
TEST(rich_tip_menu_entries) {
  help::load("en");
  QMenu menu;
  QAction* extrude = menu.addAction("Extrude");
  extrude->setObjectName("design.extrude");
  QAction* plain = menu.addAction("Recent file");
  QMenu* sub = menu.addMenu("More");
  sub->menuAction()->setObjectName("design.fillet");  // a submenu entry is no command even with an id
  menu.popup(QPoint(100, 100));
  CHECK(RichTip::commandEntry(&menu, menu.actionGeometry(extrude).center()) == extrude);
  CHECK(!RichTip::commandEntry(&menu, menu.actionGeometry(plain).center()) && !RichTip::commandEntry(&menu, menu.actionGeometry(sub->menuAction()).center()));
  auto moveTo = [&menu](QAction* a) {
    const QPointF at = QRectF(menu.actionGeometry(a)).center();
    QMouseEvent e(QEvent::MouseMove, at, menu.mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&menu, &e);
  };
  RichTip* tip = RichTip::instance();
  RichTip::setMenuCards(false);
  moveTo(extrude);
  QTest::qWait(600);
  CHECK(tip->state() == RichTip::State::Hidden);  // off: menus are left alone
  RichTip::setMenuCards(true);
  moveTo(plain);
  moveTo(extrude);
  QTest::qWaitFor([tip] { return tip->state() == RichTip::State::Compact; }, 5000);  // its timer, late on a busy machine
  CHECK(tip->state() == RichTip::State::Compact && tip->entry() == extrude && tip->target() == &menu && tip->commandId() == "design.extrude");
  CHECK(tip->geometry().left() + RichTip::kMargin > menu.geometry().right());
  moveTo(plain);
  QTest::qWaitFor([tip] { return tip->state() == RichTip::State::Hidden; }, 5000);
  CHECK(tip->state() == RichTip::State::Hidden);
  moveTo(extrude);
  tip->showFor(&menu, RichTip::State::Expanded, extrude);
  CHECK(tip->state() == RichTip::State::Expanded && tip->entry() == extrude);
  menu.hide();
  CHECK(tip->state() == RichTip::State::Hidden && !tip->entry());
  RichTip::setMenuCards(false);
}

// UI-107: the clips. The library loads without a problem and has the clips the help promises, with contiguous steps.
TEST(clips_load_cleanly) {
  clips::load();
  if (!clips::problems().isEmpty()) throw check::Failure(clips::problems().join("; ").toStdString());
  CHECK(clips::ids().size() >= 30);
  for (const char* id : {"sketch.line", "sketch.rect", "sketch.circle", "sketch.arc3", "sketch.slot", "sketch.polygon", "sketch.offset", "sketch.trim", "sketch.fillet",
                         "sketch.dimension", "sketch.c.horizontal", "sketch.c.coincident", "design.extrude", "design.revolve", "design.fillet", "design.chamfer", "design.shell",
                         "design.hole", "design.pattern_rect", "design.mirror", "inspect.section", "inspect.distance", "inspect.angle", "inspect.radius", "assembly.explode",
                         "component.activate", "select.smart", "vcs.compare", "vcs.commit", "insert.canvas", "drawing.baseView"})
    if (!clips::has(id)) throw check::Failure(std::string("no clip ") + id);
  for (const QString& id : clips::ids()) {
    const auto steps = clips::steps(id);
    CHECK(!steps.isEmpty());
    double at = 0;
    for (const auto& s : steps) {
      if (std::abs(s.from - at) > 1e-9 || s.to <= s.from) throw check::Failure(id.toStdString() + ": steps leave a gap");
      at = s.to;
    }
    if (std::abs(at - clips::duration(id)) > 1e-9) throw check::Failure(id.toStdString() + ": the last step does not end the clip");
    CHECK(clips::stepAt(id, 0) == 0 && clips::stepAt(id, clips::duration(id)) == steps.size() - 1);
    CHECK(clips::stillTime(id) > 0 && clips::stillTime(id) <= clips::duration(id));
  }
  // Every command id with a clip of its own has a help record (future commands name theirs here first).
  for (const QString& id : clips::ids())
    if (!help::find(id) && !QStringList{"assembly.explode", "component.activate", "select.smart", "vcs.compare", "vcs.commit", "insert.canvas", "drawing.baseView"}.contains(id))
      throw check::Failure(id.toStdString() + ": a clip for no command");
}

// Every clip renders at its start, middle and end; it moves; the still frame is steady; right to left mirrors the
// chrome (caption bar, cards) and nothing else changes size.
TEST(clips_render_and_move) {
  clips::load();
  const Tokens dark = theme::tokens(true), light = theme::tokens(false);
  for (const QString& id : clips::ids()) {
    clips::Options o;
    o.tokens = &dark;
    const double d = clips::duration(id);
    const QImage start = clips::frame(id, 0, {288, 162}, 1, o), mid = clips::frame(id, d / 2, {288, 162}, 1, o), end = clips::frame(id, d, {288, 162}, 1, o);
    if (start.isNull() || mid.isNull() || end.isNull()) throw check::Failure(id.toStdString() + ": no frame");
    if (start == mid && mid == end) throw check::Failure(id.toStdString() + ": does not move");
    o.tokens = &light;
    if (clips::frame(id, d / 2, {288, 162}, 1, o) == mid) throw check::Failure(id.toStdString() + ": ignores the theme");
    o.tokens = &dark;
    o.rtl = true;
    if (clips::frame(id, d / 2, {288, 162}, 1, o) == mid) throw check::Failure(id.toStdString() + ": the caption bar is not mirrored");
    o.rtl = false;
    o.still = true;
    CHECK(clips::frame(id, clips::stillTime(id), {288, 162}, 1, o) == clips::frame(id, clips::stillTime(id), {288, 162}, 1, o));
  }
  // An unknown clip paints the empty background.
  CHECK(!clips::frame("no.such.clip", 1, {50, 30}).isNull());
}

// Every caption, label and card text of every clip is translated into Arabic (app/i18n/ar.json or the help area's
// app/i18n/ar/help.json); with the app's translator they translate at run time.
TEST(clip_texts_are_translated) {
  const QHash<QString, QString> arabic = i18n::table("ar", {QStringLiteral(OPAD_SOURCE_DIR) + "/app/i18n"});
  const QJsonObject fragment = QJsonDocument::fromJson(source("app/i18n/ar/help.json").toUtf8()).object();
  clips::load();
  QStringList missing;
  for (const QString& id : clips::ids())
    for (const QString& text : clips::texts(id)) if (arabic.value(text).isEmpty()) missing << id + ": " + text;
  if (!missing.isEmpty()) throw check::Failure("no Arabic for " + missing.join(" | ").toStdString());
  {
    Arabic on;
    CHECK_EQ(i18n::t("Click the start point"), fragment.value("Click the start point").toString());
  }
  CHECK_EQ(i18n::t("Click the start point"), QString("Click the start point"));
}

// The loader names what is wrong, expands templates with arithmetic on their parameters, and keeps clip fields over
// template fields.
TEST(clip_loader_checks_and_templates) {
  QTemporaryDir dir;
  QFile f(dir.filePath("clips.json"));
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(R"({"templates": {
    "mark": {"params": {"at": null, "t": 1}, "items": [{"el": "snap", "kind": "endpoint", "at": "$at", "keys": [["$t-0.5", {"opacity": 0}], ["$t+0.25", {"opacity": 1}]]}]},
    "whole": {"params": {"caption": null}, "duration": 3, "items": [{"el": "grid"}], "steps": [{"to": 3, "caption": "$caption"}]}},
   "clips": [
    {"id": "ok", "duration": 2, "extent": [-5, -5, 5, 5], "items": [{"use": "mark", "args": {"at": [1, 1]}}, {"el": "cursor", "keys": [[0, {"pos": [0, 0]}], [1, {"pos": [2, 2], "click": 1}]]}],
     "steps": [{"to": 1, "caption": "One"}, {"to": 2, "caption": "Two"}]},
    {"id": "whole", "template": "whole", "args": {"caption": "From the template"}, "duration": 4, "items": [{"el": "label", "at": [0, 0], "text": "Extra"}]},
    {"id": "bad", "items": [{"el": "nope"}, {"el": "poly", "colour": "red"}, {"use": "missing"}, {"el": "poly", "color": "purple"}, {"use": "mark"},
                            {"el": "cursor", "keys": [[2, {"pos": [0, 0]}], [1, {"pos": [1, 1]}]]}, {"el": "cursor", "badge": "no-such-icon"}]}]})");
  f.close();
  clips::load(f.fileName());
  const QString problems = clips::problems().join("\n");
  for (const char* expected : {"bad: unknown element nope", "bad: poly: unknown property colour", "bad: unknown template missing", "bad: poly.color: unknown colour purple",
                               "bad: template mark needs at", "bad: cursor: keys must be [time, {...}] in time order", "bad: cursor: no icon no-such-icon", "bad: no steps"})
    if (!problems.contains(expected)) throw check::Failure("missing problem \"" + std::string(expected) + "\" in:\n" + problems.toStdString());
  CHECK(!problems.contains("ok:") && !problems.contains("whole:"));
  CHECK(clips::has("ok") && clips::duration("ok") == 2 && clips::steps("ok").size() == 2 && clips::stepAt("ok", 1.5) == 1);
  CHECK(clips::duration("whole") == 4 && clips::steps("whole").value(0).caption == "From the template");
  CHECK(clips::texts("whole").contains("From the template") && clips::texts("whole").contains("Extra"));
  // The click is a ripple while playing and a numbered badge in the still frame.
  clips::Options still;
  still.still = true;
  CHECK(clips::frame("ok", 1.1, {288, 162}) != clips::frame("ok", 1.1, {288, 162}, 1, still));
  clips::load();
}

// A camera turn that follows a drag in screen places ("dragged": the navigation presets' orbit) turns the other way right to
// left, where the drag is mirrored; the model itself is never mirrored.
TEST(clip_dragged_turn_follows_the_mirrored_drag) {
  QTemporaryDir dir;
  QFile f(dir.filePath("clips.json"));
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(R"({"clips": [
    {"id": "drag", "duration": 1, "view": "iso", "extent": [-12, -12, 12, 12], "steps": [{"to": 1, "caption": "One"}],
     "items": [{"el": "camera", "dragged": true, "keys": [[0, {"az": -45}], [1, {"az": -95}]]}, {"el": "poly", "points": [[0, 0, 0], [10, 0, 0], [10, 4, 0]]}]},
    {"id": "turn", "duration": 1, "view": "iso", "extent": [-12, -12, 12, 12], "steps": [{"to": 1, "caption": "One"}],
     "items": [{"el": "camera", "keys": [[0, {"az": -45}], [1, {"az": -95}]]}, {"el": "poly", "points": [[0, 0, 0], [10, 0, 0], [10, 4, 0]]}]},
    {"id": "back", "duration": 1, "view": "iso", "extent": [-12, -12, 12, 12], "steps": [{"to": 1, "caption": "One"}],
     "items": [{"el": "camera", "keys": [[0, {"az": 5}]]}, {"el": "poly", "points": [[0, 0, 0], [10, 0, 0], [10, 4, 0]]}]}]})");
  f.close();
  clips::load(f.fileName());
  CHECK(clips::problems().isEmpty());
  clips::Options ltr, rtl;
  ltr.caption = rtl.caption = false;
  rtl.rtl = true;
  const QSize size(288, 162);
  CHECK(clips::frame("turn", 1, size, 1, rtl) == clips::frame("turn", 1, size, 1, ltr));  // a turn of its own: as written
  CHECK(clips::frame("drag", 1, size, 1, ltr) == clips::frame("turn", 1, size, 1, ltr));
  CHECK(clips::frame("drag", 1, size, 1, rtl) == clips::frame("back", 1, size, 1, ltr));  // -45 - 50 mirrored: -45 + 50
  CHECK(clips::frame("drag", 0, size, 1, rtl) == clips::frame("drag", 0, size, 1, ltr));
  clips::load();
}

// UI-124: reduced motion (ui/reduceMotion, by default the system's) holds the clips still whatever their own switch says,
// and makes the camera's turns a moment (never zero: an OCCT animation of no length stays where it started).
TEST(reduced_motion_holds_clips_still) {
  clips::load();
  QSettings().setValue("ui/tipAnimate", true);
  QSettings().setValue("ui/reduceMotion", true);
  CHECK(motion::reduced() && !clips::animations() && motion::seconds(0.5) > 0 && motion::seconds(0.5) < 0.01 && motion::milliseconds(180) == 0);
  ClipView view("design.extrude");
  view.resize(288, 162);
  view.show();
  CHECK(!view.playing() && view.still());
  view.hide();
  QSettings().setValue("ui/reduceMotion", false);
  CHECK(!motion::reduced() && clips::animations() && motion::seconds(0.5) == 0.5 && motion::milliseconds(180) == 180);
  QSettings().remove("ui/reduceMotion");
  CHECK(motion::reduced() == !motion::system());
}

// The player runs a timer only while visible and not reduced to its still frame; a step loops inside its segment.
TEST(clip_view_plays_only_when_visible) {
  clips::load();
  QSettings().setValue("ui/tipAnimate", true);
  QSettings().setValue("ui/reduceMotion", false);  // whatever the system says
  ClipView view("design.extrude");
  CHECK(!view.playing() && view.sizeHint() == QSize(288, 162));
  view.resize(288, 162);
  view.show();
  CHECK(view.playing() && !view.still());
  QTest::qWait(300);
  CHECK(view.time() > 0.15 && view.time() < 1.5);
  view.setStep(1);
  const auto steps = clips::steps("design.extrude");
  CHECK(view.time() >= steps[1].from - 1e-9 && view.time() <= steps[1].to + 1e-9);
  view.hide();
  CHECK(!view.playing());
  QSettings().setValue("ui/tipAnimate", false);
  view.setStep(-1);
  view.show();
  CHECK(!view.playing() && view.still() && view.time() == clips::stillTime("design.extrude"));
  CHECK(!view.grab().toImage().isNull());
  view.setClip("no.such.clip");
  CHECK(!view.playing());
}

// A tool panel's guide loops the clip steps of the step its tool waits for: the clip's "guide" list, else the clip's
// steps shared out over the tool's; the last one once every step is done. Bad entries are named.
TEST(clip_guide_ranges) {
  QTemporaryDir dir;
  QFile f(dir.filePath("clips.json"));
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(R"({"clips": [
    {"id": "four", "duration": 4, "items": [{"el": "grid"}], "steps": [{"to": 1, "caption": "A"}, {"to": 2, "caption": "B"}, {"to": 3, "caption": "C"}, {"to": 4, "caption": "D"}]},
    {"id": "guided", "duration": 3, "items": [{"el": "grid"}], "guide": [0, [1, 2]], "steps": [{"to": 1, "caption": "A"}, {"to": 2, "caption": "B"}, {"to": 3, "caption": "C"}]},
    {"id": "wrong", "duration": 2, "items": [{"el": "grid"}], "guide": [[1, 0], 5], "steps": [{"to": 2, "caption": "A"}]}]})");
  f.close();
  clips::load(f.fileName());
  using R = QPair<int, int>;
  CHECK(clips::guideRange("four", 0, 2) == R(0, 1) && clips::guideRange("four", 1, 2) == R(2, 3) && clips::guideRange("four", 2, 2) == R(3, 3));
  CHECK(clips::guideRange("four", 0, 3) == R(0, 0) && clips::guideRange("four", 1, 3) == R(1, 1) && clips::guideRange("four", 2, 3) == R(2, 3));
  CHECK(clips::guideRange("four", 0, 6) == R(0, 0) && clips::guideRange("four", 5, 6) == R(3, 3) && clips::guideRange("four", -1, 2) == R(0, 1));
  CHECK(clips::guideRange("four", 0, 0) == R(0, 3) && clips::guideRange("none", 0, 2) == R(-1, -1));
  CHECK(clips::guideRange("guided", 0, 3) == R(0, 0) && clips::guideRange("guided", 1, 3) == R(1, 2) && clips::guideRange("guided", 2, 3) == R(1, 2));
  CHECK(clips::guideRange("guided", 3, 3) == R(2, 2));
  CHECK(clips::problems().join("\n").contains("wrong: guide entries are clip steps"));
  clips::load();
  CHECK(clips::guideRange("sketch.line", 0, 3) == R(0, 0) && clips::guideRange("design.extrude", 1, 2) == R(1, 2));
}

// The player loops a range of steps as one segment; reduced motion shows the range's last frame.
TEST(clip_view_loops_a_range) {
  clips::load();
  QSettings().setValue("ui/tipAnimate", true);
  QSettings().setValue("ui/reduceMotion", false);  // whatever the system says
  ClipView view("design.extrude");
  view.resize(288, 162);
  view.show();
  const auto steps = clips::steps("design.extrude");
  view.setRange(1, 2);
  CHECK(view.range() == qMakePair(1, 2) && view.step() == 1);
  for (int i = 0; i < 4; ++i) {
    QTest::qWait(150);
    CHECK(view.time() >= steps[1].from - 1e-9 && view.time() <= steps[2].to + 1e-9);
  }
  QSettings().setValue("ui/tipAnimate", false);
  view.hide();
  view.show();
  CHECK(view.still() && !view.playing() && std::abs(view.time() - steps[2].to) < 1e-9);
}

// The guide slot: shown for a command with a clip, unfolded for its first kUses runs and folded after, a fold or unfold
// by hand remembered for that command; no slot without a clip or with ui/toolGuide off. In a tool's step panel it
// follows the waiting step.
TEST(tool_guide_follows_the_command) {
  clips::load();
  QSettings().remove("help");
  QSettings().remove("ui/toolGuide");
  QWidget window;
  auto* box = new QVBoxLayout(&window);
  auto* guide = new ToolGuide(&window);
  box->addWidget(guide);
  window.resize(360, 300);
  window.show();
  int resized = 0;
  QObject::connect(guide, &ToolGuide::resized, [&] { ++resized; });
  CHECK(!guide->shown());
  guide->setCommand("design.extrude");
  CHECK(guide->shown() && guide->expanded() && guide->view()->isVisible() && guide->view()->clip() == "design.extrude" && resized == 1);
  CHECK(QSettings().value("help/uses/design.extrude").toInt() == 1);
  guide->setWaiting(1, 2);
  CHECK(guide->view()->range() == qMakePair(1, 2));
  for (int i = 2; i <= ToolGuide::kUses; ++i) guide->setCommand("design.extrude");
  CHECK(guide->expanded());
  guide->setCommand("design.extrude");
  CHECK(guide->shown() && !guide->expanded() && !guide->view()->isVisible());
  auto* head = guide->findChild<QToolButton*>("guideHead");
  CHECK(head);
  head->click();
  CHECK(guide->expanded() && guide->view()->isVisible() && QSettings().value("help/guide/design.extrude").toBool());
  guide->setCommand("design.extrude");
  CHECK(guide->expanded());
  head->click();
  CHECK(!guide->expanded());
  guide->setCommand("file.quit");
  CHECK(!guide->shown() && QSettings().value("help/uses/file.quit", 0).toInt() == 0);
  QSettings().setValue("ui/toolGuide", false);
  guide->setCommand("sketch.line");
  CHECK(!guide->shown());
  QSettings().remove("ui/toolGuide");
  // In the measure tools' step panel: first pick, second pick, then the result.
  ToolStepsPanel steps;
  steps.resize(380, 400);
  steps.show();
  CHECK(!steps.guide());
  steps.setGuide("inspect.distance");
  CHECK(steps.guide() && steps.guide()->shown() && steps.guide()->view()->isVisible());
  steps.setSteps({{"Select first face", {}}, {"Select second face", {}}}, {});
  CHECK(steps.guide()->view()->range() == qMakePair(0, 0));
  steps.setSteps({{"Select first face", "Box › face 1"}, {"Select second face", {}}}, {});
  CHECK(steps.guide()->view()->range() == qMakePair(1, 1));
  steps.setSteps({{"Select first face", "Box › face 1"}, {"Select second face", "Box › face 2"}}, {});
  const int last = int(clips::steps("inspect.distance").size()) - 1;
  CHECK(steps.guide()->view()->range() == qMakePair(last, last));
  QSettings().remove("help");
}

// Commands are listed by area (the palette's group column, the reference's headings), every area named once.
TEST(command_areas) {
  help::load("en");
  CHECK_EQ(help::group("design.extrude"), QString("Design"));
  CHECK_EQ(help::group("sketch.line"), QString("Sketch"));
  CHECK(help::group("sketch.c.horizontal") == "Sketch constraints" && help::group("sketch.dimension") == "Sketch constraints");
  CHECK(help::group("view.fit") == "View" && help::group("nav.fusion") == "View" && help::group("help.about") == "Tools and help");
  CHECK(help::group("files.useOda") == "File" && help::group("help.licenses") == "Tools and help");
  QStringList areas = help::areas();
  CHECK(areas.size() == 11 && areas.removeDuplicates() == 0);
  for (const CommandHelp& h : help::all()) CHECK(help::areas().contains(help::group(h.id)) && help::group(h.id) != "Other");
}

// The reference lists every command but the ribbon's "more" menus by area, searches by name, keyword or summary, opens
// at a command (clearing a filter that hides it) and shows its card: the clip, "All steps" and its steps (a click loops
// one), and what it needs when its action is disabled.
TEST(command_reference_lists_searches_and_opens) {
  help::load("en");
  clips::load();
  QAction extrude("Extrude"), other("Other");
  extrude.setObjectName("design.extrude");
  extrude.setShortcut(QKeySequence("E"));
  extrude.setEnabled(false);
  // help.licenses has a record but no command here (as in a build without it): not listed.
  CommandReference reference([&](const QString& id) { return id == "design.extrude" ? &extrude : id == "help.licenses" ? nullptr : &other; });
  int listed = -1;
  for (const CommandHelp& h : help::all()) listed += !h.id.section('.', -1).startsWith("more");
  CHECK(help::find("help.licenses") && !reference.shown().contains("help.licenses") && reference.shown().contains("help.about"));
  CHECK(reference.shown().size() == listed && !reference.shown().contains("sketch.moreCreate") && reference.current() == reference.shown().first());
  reference.setFilter("push pull");
  CHECK(reference.shown().contains("design.offset_face") && !reference.shown().contains("design.fillet"));
  reference.open("design.extrude");  // listed: the filter stays
  CHECK(reference.current() == "design.extrude" && reference.shown().size() < listed);
  reference.open("design.fillet");  // hidden by it: the filter goes
  CHECK(reference.current() == "design.fillet" && reference.shown().size() == listed);
  reference.open("design.extrude");
  CommandPreview* card = reference.preview();
  CHECK(card->command() == "design.extrude" && card->clip()->clip() == "design.extrude" && card->showsRequirement());
  CHECK(card->steps()->count() == clips::steps("design.extrude").size() + 1 && card->clip()->range() == qMakePair(-1, -1));
  card->steps()->setCurrentRow(2);
  CHECK(card->clip()->range() == qMakePair(1, 1));
  extrude.setEnabled(true);
  CHECK(!card->showsRequirement());
  reference.open("file.quit");
  CHECK(card->command() == "file.quit" && card->clip()->isHidden() && card->steps()->isHidden());
  reference.close();
}

// The rich card's clip slot only for commands whose clip exists, when the factory says which do.
TEST(rich_tip_clip_slot_follows_the_library) {
  help::load("en");
  QWidget window;
  auto* button = new QToolButton(&window);
  auto* quit = new QAction("Quit", &window);
  quit->setObjectName("file.quit");
  button->setDefaultAction(quit);
  window.show();
  RichTip::attach(button, "file.quit");
  RichTip::setClipFactory([](const QString& clip, QWidget* parent) -> QWidget* { return new ClipView(clip, parent); }, &clips::has);
  RichTip* tip = RichTip::instance();
  tip->showFor(button, RichTip::State::Expanded);
  CHECK(!tip->clip());
  tip->hideTip();
  RichTip::attach(button, "design.extrude");
  tip->showFor(button, RichTip::State::Expanded);
  CHECK(qobject_cast<ClipView*>(tip->clip()) && static_cast<ClipView*>(tip->clip())->clip() == "design.extrude");
  tip->hideTip();
  RichTip::setClipFactory(nullptr);
  RichTip::detach(button);
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir settings;
  QCoreApplication::setOrganizationName("OPAD-tests");
  QCoreApplication::setApplicationName("help");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QSettings().setValue("ui/tipAnimate", false);
  const int result = check::run_all(argc, argv);
  delete RichTip::instance();
  return result;
}
