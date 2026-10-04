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
#include "KeyText.hpp"
#include "Motion.hpp"
#include "RichTip.hpp"
#include "ShortcutEditor.hpp"
#include "Theme.hpp"
#include "check.hpp"
#include "opad/design/feature.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QKeySequenceEdit>
#include <QTranslator>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <set>

namespace {
QString source(const QString& path) {
  QFile f(QStringLiteral(OPAD_SOURCE_DIR) + "/" + path);
  if (!f.open(QIODevice::ReadOnly)) throw check::Failure("cannot read " + path.toStdString());
  return QString::fromUtf8(f.readAll());
}

// Command ids registered by MainWindow and the areas: literal addAction ids of MainWindow*.cpp (and the Help menu's
// help("help.about", ...) helper of the IP branch); CommandInfo records in any source of the app that is not a bench
// (info.id = "..." or CommandInfo name{"...", ...}) and the areas' helpers that make them (the Drawings workspace's add
// and its annotations' add, version control's command, the file location's fileCommand, the clipboard's add, the linked
// files' and the canvas' command, KiCad's add; GitWatch and VersionPanel's add only name menu items); an area's workspace (its
// "workspace.<id>" command, the contextual sketch's excepted); the generated families, the sketch tool tables and the
// feature kinds of the core spec table. The bench OPAD_BENCH_RICHTIP checks the live list of the running app.
std::set<QString> registeredIds() {
  std::set<QString> ids;
  const QString name = R"(([a-z][a-z0-9]*\.[A-Za-z0-9_.]+))";
  QString main;
  for (const QString& file : QDir(QStringLiteral(OPAD_SOURCE_DIR) + "/app").entryList({"MainWindow*.cpp"}, QDir::Files, QDir::Name)) main += source("app/" + file);
  for (const auto& m : QRegularExpression(R"(\b(?:addAction|help)\(")" + name + "\"").globalMatch(main)) ids.insert(m.captured(1));
  const QRegularExpression info(R"(\.id\s*=\s*")" + name + "\""), braced(R"(\bCommandInfo\s+\w+\s*\{\s*")" + name + "\""),
      workspace(R"re(\baddWorkspace\("([a-z]+)")re");
  const QHash<QString, QRegularExpression> helpers{{"DocsWorkspace.cpp", QRegularExpression(R"(\badd\(")" + name + "\"")},
                                                   {"VcsArea.cpp", QRegularExpression(R"(\bcommand\(")" + name + "\"")},
                                                   {"LocationArea.cpp", QRegularExpression(R"(\bfileCommand\(")" + name + "\"")},
                                                   {"SketchClipboard.cpp", QRegularExpression(R"(\badd\(")" + name + "\"")},
                                                   {"DocsAnnotate.cpp", QRegularExpression(R"(\badd\(")" + name + "\"")},
                                                   {"AssetsArea.cpp", QRegularExpression(R"(\bcommand\(")" + name + "\"")},
                                                   {"CanvasArea.cpp", QRegularExpression(R"(\bcommand\(")" + name + "\"")},
                                                   {"KicadArea.cpp", QRegularExpression(R"(\badd\(")" + name + "\"")}};
  for (const QString& file : QDir(QStringLiteral(OPAD_SOURCE_DIR) + "/app").entryList({"*.cpp"}, QDir::Files, QDir::Name)) {
    if (file.contains("Bench")) continue;
    const QString text = source("app/" + file);
    for (const auto& m : info.globalMatch(text)) ids.insert(m.captured(1));
    for (const auto& m : braced.globalMatch(text)) ids.insert(m.captured(1));
    if (helpers.contains(file))
      for (const auto& m : helpers.value(file).globalMatch(text)) ids.insert(m.captured(1));
    for (const auto& m : workspace.globalMatch(text))
      if (m.captured(1) != "sketch") ids.insert("workspace." + m.captured(1));
  }
  for (const char* v : {"top", "right", "left", "back", "bottom"}) ids.insert(QString("drawings.baseView.") + v);  // the Base view button's menu
  for (const auto& m : QRegularExpression(R"re(\{"([a-z0-9_:]+)", tr\(")re").globalMatch(main)) ids.insert("sketch." + m.captured(1).replace(':', '.'));
  for (const auto& m : QRegularExpression(R"re(QObject::tr\("[^"]+"\),"([a-z0-9_:]+)")re").globalMatch(source("app/SketchPanel.cpp")))
    ids.insert("sketch." + m.captured(1).replace(':', '.'));
  for (const char* v : {"top", "front", "right", "iso", "bottom", "back", "left"}) ids.insert(QString("view.") + v);
  for (const char* v : {"fusion", "solidworks", "onshape", "blender"}) ids.insert(QString("nav.") + v);
  for (const char* v : {"bodies", "faces", "edges", "vertices"}) ids.insert(QString("select.") + v);
  for (const char* v : {"view.extensions", "view.tracking", "view.gridSnap", "view.orthoSnap", "view.polarSnap", "sketch.selectionOptions", "sketch.constraints", "sketch.snaps"}) ids.insert(v);
  for (const char* v : {"vcs.nextChange", "vcs.previousChange"}) ids.insert(v);  // VcsArea's loop of tuples
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

// A key spelled in a help text instead of a token: Ctrl+X, F9, "Press D", "press Del", "(S)", and a letter that acts
// ("G again hides it", "· P pins"; not an axis or a size: "X offset") (English and Arabic). Fixed keys (Enter, Esc, Tab,
// Del in a sketch, Backspace, Shift+Tab, a modifier held with a click or a drag) are allowed, and so are the note
// editor's own pen keys (B, E: AnnotationEditor) and Compare's versions A and B.
QStringList literalKeys(QString text) {
  static const QRegularExpression token(R"(\{(key|press|fixed):[^}]*\})"),
      allowed(R"((Ctrl|Shift|Alt)\+(Tab\b|((left|middle|right) )?(click|drag))|\b(B pen|E eraser)\b|\bB (alone|continues|is|in)\b)", QRegularExpression::CaseInsensitiveOption);
  static const QList<QRegularExpression> patterns{
      QRegularExpression(R"(\b(Ctrl|Shift|Alt|Meta|Cmd)\+)"), QRegularExpression(R"(\bF\d{1,2}\b)"), QRegularExpression(R"([Pp]ress [A-Z0-9]\b)"),
      QRegularExpression(R"([Pp]ress (Del|Delete|Home|End|PgUp|PgDn|Space)\b)"), QRegularExpression(R"(\([A-Z0-9]\))"),
      QRegularExpression(R"((?:^|[·;:,(]\s*)[B-HJ-Z]\s+(?!(?:offsets?|axis|position|and|or|spacing|size|direction|coordinates?|values?|scale|parts?|sizes?)\b)[a-z]+)"),
      QRegularExpression(QString::fromUtf8(R"(اضغط\s+[A-Z0-9]\b)"))};
  text.remove(token).remove(allowed);
  QStringList out;
  for (const QRegularExpression& re : patterns)
    for (const auto& m : re.globalMatch(text)) out << m.captured(0);
  return out;
}

int sentences(const QString& text) { return static_cast<int>(text.count(QRegularExpression(R"([.!?](\s|$))"))); }
QString clean(QString s) { return s.remove('&').remove(QString::fromUtf8("…")).remove("...").trimmed(); }
}  // namespace

TEST(every_registered_command_has_help) {
  help::load("en");
  const auto ids = registeredIds();
  CHECK(ids.size() > 300 && ids.count("help.reference") && ids.count("drawings.baseView.top") && ids.count("vcs.push") && ids.count("edit.copy") &&
        ids.count("drawing2d.layers") && ids.count("file.reveal") && ids.count("workspace.drawings") && ids.count("sketch.commandLine") &&
        ids.count("assets.link") && ids.count("canvas.insert") && ids.count("kicad.insert") && ids.count("drawings.dimension"));
  // Commands of the tracks merged after the wave 3 help pass (assets, measure, hidden-line views, the drawings' views and
  // sheets, Reset Home): their records and clips are still to be written. A listed command that has help is stale here.
  // The second list: the commands of the linked files, the canvas, the drawings' annotations and KiCad, which the scan
  // found only once it learned their areas' helpers (the richtip bench listed them as without help).
  const QStringList pending = QStringList{"assets.autoSync", "drawings.auxiliaryView", "drawings.breakView", "drawings.breakoutView", "drawings.cropView",
                                          "drawings.detailView", "drawings.exportDrawing", "drawings.issue", "drawings.print", "drawings.sectionView",
                                          "drawings.templateFields", "inspect.length", "inspect.material", "view.hidden", "view.hiddenEdges", "view.hideothers",
                                          "view.resetHome"} +
                              QStringList{"assets.copyPath", "assets.embed", "assets.kicadSettings", "assets.link", "assets.pack", "assets.replace", "assets.reveal",
                                          "assets.settings", "assets.sync", "assets.syncAll", "canvas.align", "canvas.calibrate", "canvas.edit", "canvas.finish",
                                          "canvas.fromBackdrop", "canvas.insert", "canvas.replace", "canvas.trace", "drawings.autoBalloon", "drawings.balloon",
                                          "drawings.baseline", "drawings.centerLine", "drawings.centerMark", "drawings.centerMarks", "drawings.chain", "drawings.datum",
                                          "drawings.dimension", "drawings.fcf", "drawings.fromDatums", "drawings.fromDatums.baseline", "drawings.fromDatums.chain",
                                          "drawings.holeCallout", "drawings.holeTable", "drawings.note", "drawings.ordinate", "drawings.partsList", "drawings.reattach",
                                          "drawings.revisionTable", "drawings.surface", "kicad.clearance", "kicad.insert", "kicad.previewSync", "kicad.project",
                                          "view.hideSmallParts", "view.smallPartSize"};
  QStringList missing;
  for (const QString& id : ids) if (!help::find(id) && !pending.contains(id)) missing << id;
  for (const QString& id : pending) if (help::find(id)) missing << id + " (has help: drop it from pending)";
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

// TODO 11 wave 3: the card's caps are the command's key now, its hint names Help for this tool's key, and that key (not
// F1 once it is another) expands the card and then opens the guide; a change of keys relayouts a card that is up; right
// to left the caps keep their order (the group mirrors).
TEST(rich_tip_follows_the_help_key) {
  help::load("en");
  QWidget window;
  auto* layout = new QVBoxLayout(&window);
  QAction fit("Fit"), current("Help for this tool");
  fit.setObjectName("view.fit");
  fit.setShortcut(QKeySequence("Ctrl+Alt+F"));
  current.setObjectName("help.current");
  current.setShortcut(QKeySequence("Ctrl+F1"));
  auto* button = new QToolButton(&window);
  button->setDefaultAction(&fit);
  layout->addWidget(button);
  window.resize(300, 200);
  window.show();
  keys::setLookup([&](const QString& id) { return id == "view.fit" ? &fit : id == "help.current" ? &current : nullptr; });
  QString guided;
  RichTip::setGuideHook([&](const QString& id) { guided = id; });
  RichTip::attach(button, "view.fit");
  RichTip* tip = RichTip::instance();
  auto plain = [](QString s) { return s.remove(QChar(0x2066)).remove(QChar(0x2069)); };
  auto send = [&](QEvent::Type type, int key, Qt::KeyboardModifiers m) {
    QKeyEvent e(type, key, m);
    e.ignore();
    QApplication::sendEvent(button, &e);
    return e.isAccepted();
  };
  tip->showFor(button, RichTip::State::Compact);
  CHECK(tip->keyCaps() == QStringList({"Ctrl", "Alt", "F"}) && plain(tip->hint()) == "Shift or Ctrl+F1 for more");
  CHECK(!send(QEvent::ShortcutOverride, Qt::Key_F1, Qt::NoModifier));  // not Help for this tool's key now: the window's
  send(QEvent::KeyPress, Qt::Key_F1, Qt::NoModifier);
  CHECK(tip->state() == RichTip::State::Hidden);  // any other key hides the card
  tip->showFor(button, RichTip::State::Compact);
  CHECK(send(QEvent::ShortcutOverride, Qt::Key_F1, Qt::ControlModifier));
  send(QEvent::KeyPress, Qt::Key_F1, Qt::ControlModifier);
  CHECK(tip->state() == RichTip::State::Expanded && plain(tip->hint()) == "Ctrl+F1 for the tool guide");
  send(QEvent::KeyPress, Qt::Key_F1, Qt::ControlModifier);
  CHECK(guided == "view.fit" && tip->state() == RichTip::State::Hidden);
  // Unbound: Shift alone expands; no key for the guide.
  current.setShortcut(QKeySequence());
  tip->showFor(button, RichTip::State::Compact);
  CHECK(tip->hint() == "Shift for more");
  tip->showFor(button, RichTip::State::Expanded);
  CHECK(tip->hint().isEmpty());
  // Keys change while the card is up: it follows.
  tip->showFor(button, RichTip::State::Compact);
  shortcuts::bind(&fit, QKeySequence("F"));
  CHECK(tip->keyCaps() == QStringList({"F"}));
  // Right to left: the group mirrors, the caps keep their order.
  fit.setShortcut(QKeySequence("Ctrl+Alt+F"));
  QApplication::setLayoutDirection(Qt::RightToLeft);
  tip->hideTip();
  tip->showFor(button, RichTip::State::Compact);
  const QList<QRect> caps = tip->keyRects();
  CHECK(tip->layoutDirection() == Qt::RightToLeft && caps.size() == 3 && caps[0].left() < caps[1].left() && caps[1].left() < caps[2].left() && caps[2].right() < tip->width() / 2);
  QApplication::setLayoutDirection(Qt::LeftToRight);
  tip->hideTip();
  RichTip::detach(button);
  RichTip::setGuideHook({});
  keys::setLookup({});
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
  CHECK_EQ(help::keyRow("Redo", {QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")}).text(), QString("Ctrl+Y / Ctrl+Shift+Z"));
  CHECK(help::keyRow("Zoom in", {QKeySequence("Ctrl++")}).keys == QList<QStringList>({{"Ctrl", "+"}}));
  {  // the cheat sheet draws each alternate's caps, a plain "/" between them; a search finds Qt's names too
    ShortcutSheet sheet;
    sheet.setGroups({help::KeyGroup{"Edit", {help::keyRow("Redo", {QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")}),
                                             help::KeyRow{"Menu", {{"Menu key"}, {"Shift", "F10"}}, {}},
                                             help::keyRow("Confirm", {QKeySequence("Ctrl+Return")})}}});
    QStringList caps, plain;
    for (QLabel* l : sheet.findChildren<QLabel*>()) (l->objectName() == "keycap" ? caps : plain) << l->text();
    CHECK_EQ(caps, QStringList({"Ctrl", "Y", "Ctrl", "Shift", "Z", "Menu key", "Shift", "F10", "Ctrl", "Enter"}));
    CHECK(plain.count("/") == 2);
    CHECK(sheet.shown().contains("Redo Ctrl+Y / Ctrl+Shift+Z") && sheet.shown().contains("Confirm Ctrl+Enter"));
    sheet.setFilter("ctrl+return");
    CHECK_EQ(sheet.shown(), QStringList({"Confirm Ctrl+Enter"}));
    // Right to left: the caps still read Ctrl, Y (their own left-to-right box).
    sheet.setFilter(QString());
    sheet.setLayoutDirection(Qt::RightToLeft);
    sheet.resize(900, 400);
    sheet.show();
    QLabel *ctrl = nullptr, *y = nullptr;
    for (QLabel* l : sheet.findChildren<QLabel*>("keycap")) {
      if (!ctrl && l->text() == "Ctrl") ctrl = l;
      if (!y && l->text() == "Y") y = l;
    }
    CHECK(ctrl && y && ctrl->mapTo(&sheet, QPoint()).x() < y->mapTo(&sheet, QPoint()).x());
    sheet.close();
  }
  auto mouse = [](const QString& preset, int row) { return help::mouseRows(preset).value(row).text(); };
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
  CHECK(groups[0].rows.size() == 1 && groups[0].rows[0].label == help::find("view.fit")->title && groups[0].rows[0].text() == "F");
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
  // "In every tool": Help for this tool's row is its key now, and none without one.
  QAction current("Help for this tool");
  current.setObjectName("help.current");
  current.setShortcut(QKeySequence("Ctrl+F1"));
  auto every = [&] { return help::keyGroups({&fit, &current}, false, "fusion").last(); };
  CHECK(every().rows.last().text() == "Ctrl+F1");
  current.setShortcut(QKeySequence());
  for (const help::KeyRow& r : every().rows) CHECK(r.text() != "F1" && r.label != QCoreApplication::translate("help", "Guide of the tool you are using"));
}

// TODO 11 wave 3 (help audit §4.3, §6.3 test 3): the one key formatter. Caps from the key combination (never split on
// '+'), one naming table, multi-chord sequences, the macOS glyphs and order (tested here through the Style parameter),
// LRI..PDI around inline text, the fixed keys, and the binding of a command through the lookup (held during a sketch).
TEST(keys_caps_and_text) {
  using keys::Style;
  using L = QStringList;
  CHECK_EQ(keys::caps(QKeySequence("Ctrl+Shift+F"), Style::Pc), L({"Ctrl", "Shift", "F"}));
  CHECK_EQ(keys::caps(QKeySequence("Shift+Alt+Ctrl+Meta+X"), Style::Pc), L({"Meta", "Ctrl", "Alt", "Shift", "X"}));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_Return), Style::Pc), L({"Enter"}));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_Enter), Style::Pc), L({"Enter"}));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_PageDown), Style::Pc), L({"PgDn"}));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_PageUp), Style::Pc), L({"PgUp"}));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_Escape), Style::Pc), L({"Esc"}));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_Delete), Style::Pc), L({"Del"}));
  CHECK_EQ(keys::caps(QKeySequence("Shift+Del"), Style::Pc), L({"Shift", "Del"}));
  CHECK_EQ(keys::caps(QKeySequence("Ctrl+Up"), Style::Pc), L({"Ctrl", QString::fromUtf8("↑")}));
  CHECK_EQ(keys::caps(QKeySequence("Ctrl++"), Style::Pc), L({"Ctrl", "+"}));
  CHECK_EQ(keys::caps(QKeySequence("Ctrl+/"), Style::Pc), L({"Ctrl", "/"}));
  CHECK_EQ(keys::caps(QKeySequence("F1"), Style::Pc), L({"F1"}));
  CHECK_EQ(keys::caps(QKeySequence("Ctrl+K, Ctrl+S"), Style::Pc), L({"Ctrl", "K", keys::kThen, "Ctrl", "S"}));
  CHECK_EQ(keys::plain(QKeySequence("Ctrl+K, Ctrl+S"), Style::Pc), QString("Ctrl+K, Ctrl+S"));
  CHECK_EQ(keys::plain(QKeySequence("Ctrl+Alt+F"), Style::Pc), QString("Ctrl+Alt+F"));
  CHECK(keys::caps(QKeySequence()).isEmpty() && keys::plain(QKeySequence()).isEmpty() && keys::text(QKeySequence()).isEmpty());
  // macOS: Control, Option, Shift, Command (Qt's Control is Command there), glyphs, no '+'.
  CHECK_EQ(keys::caps(QKeySequence("Ctrl+Shift+F"), Style::Mac), L({QString::fromUtf8("⇧"), QString::fromUtf8("⌘"), "F"}));
  CHECK_EQ(keys::caps(QKeySequence("Ctrl+Shift+Alt+Meta+X"), Style::Mac), L({QString::fromUtf8("⌃"), QString::fromUtf8("⌥"), QString::fromUtf8("⇧"), QString::fromUtf8("⌘"), "X"}));
  CHECK_EQ(keys::plain(QKeySequence("Ctrl+Return"), Style::Mac), QString::fromUtf8("⌘↩"));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_Backspace), Style::Mac), L({QString::fromUtf8("⌫")}));
  CHECK_EQ(keys::caps(QKeySequence(Qt::Key_Delete), Style::Mac), L({QString::fromUtf8("⌦")}));
  CHECK_EQ(keys::plain(QKeySequence("Ctrl+K, Ctrl+S"), Style::Mac), QString::fromUtf8("⌘K, ⌘S"));
  // Inline text keeps its order in right-to-left sentences.
  CHECK_EQ(keys::text(QKeySequence("Ctrl+F"), Style::Pc), QString(QChar(0x2066)) + "Ctrl+F" + QChar(0x2069));
  CHECK_EQ(keys::isolate("F"), QString(QChar(0x2066)) + "F" + QChar(0x2069));
  // The fixed keys.
  for (const QString& name : keys::fixedNames()) {
    if (keys::fixedCaps(name, Style::Pc).isEmpty()) throw check::Failure("no caps for fixed key " + name.toStdString());
    CHECK(!keys::fixedCaps(name, Style::Mac).isEmpty());
  }
  CHECK_EQ(keys::fixedCaps("ctrlEnter", Style::Pc), L({"Ctrl", "Enter"}));
  CHECK_EQ(keys::fixedCaps("ctrlEnter", Style::Mac), L({QString::fromUtf8("⌘"), QString::fromUtf8("↩")}));
  CHECK_EQ(keys::fixedCaps("shiftTab", Style::Pc), L({"Shift", "Tab"}));
  CHECK_EQ(keys::fixedCaps("esc", Style::Pc), L({"Esc"}));
  CHECK_EQ(keys::fixedCaps("shift", Style::Mac), L({QString::fromUtf8("⇧")}));
  CHECK_EQ(keys::fixedCaps("redo", Style::Pc), keys::caps(QKeySequence::keyBindings(QKeySequence::Redo).value(0), Style::Pc));
  CHECK(keys::fixedCaps("nope").isEmpty() && keys::fixedText("nope").isEmpty());
  CHECK_EQ(keys::fixedText("enter", Style::Pc), keys::isolate("Enter"));
  // A command's key through the lookup: the user's binding, held while a sketch is open, none when unassigned.
  QAction fit("Fit"), faces("Faces"), home("Home");
  fit.setObjectName("view.fit");
  fit.setShortcut(QKeySequence("Ctrl+Alt+F"));
  faces.setObjectName("select.faces");
  faces.setShortcut(QKeySequence("Ctrl+Alt+2"));
  home.setObjectName("view.home");
  keys::setLookup([&](const QString& id) { return id == "view.fit" ? &fit : id == "select.faces" ? &faces : id == "view.home" ? &home : nullptr; });
  CHECK(keys::hasLookup() && keys::action("view.fit") == &fit && !keys::action("no.such"));
  CHECK_EQ(keys::text("view.fit", Style::Pc), keys::isolate("Ctrl+Alt+F"));
  shortcuts::suspendOutsideSketch({&faces}, true);  // a sketch holds the filters' keys: still the user's key
  CHECK(faces.shortcut().isEmpty() && keys::text("select.faces", Style::Pc) == keys::isolate("Ctrl+Alt+2"));
  shortcuts::suspendOutsideSketch({&faces}, false);
  CHECK(keys::text("view.home").isEmpty() && keys::text("no.such").isEmpty() && keys::binding("view.home").isEmpty());
  // Qt's standard Quit on Windows is Key_Exit, which no keyboard has: no key rather than "Exit".
  CHECK(!keys::pressable(QKeySequence(Qt::Key_Exit)) && keys::pressable(QKeySequence("F")) && !keys::pressable(QKeySequence()));
  home.setShortcuts({QKeySequence(Qt::Key_Exit), QKeySequence("Ctrl+Q")});
  CHECK(keys::binding("view.home") == QKeySequence("Ctrl+Q") && keys::bindings(&home).size() == 1);
  home.setShortcut(QKeySequence());
  CHECK_EQ(keys::hint("view.fit", "fit"), keys::text("view.fit") + " fit");
  CHECK(keys::hint("view.home", "home").isEmpty());
  // A hint's span of keys ("1–4 filter"), and a key given as a fixed name, a command or text (PanelFooter, SegmentButton).
  {
    QAction one("Bodies"), two("Faces"), three("Edges"), four("Vertices");
    QList<QAction*> filters{&one, &two, &three, &four};
    const QStringList ids{"select.bodies", "select.faces", "select.edges", "select.vertices"};
    for (int i = 0; i < 4; ++i) {
      filters[i]->setObjectName(ids[i]);
      filters[i]->setShortcut(QKeySequence(QString::number(i + 1)));
    }
    keys::setLookup([&](const QString& id) { const qsizetype i = ids.indexOf(id); return i >= 0 ? filters[i] : id == "view.fit" ? &fit : nullptr; });
    CHECK_EQ(keys::span(ids), keys::isolate(QString::fromUtf8("1–4")));
    two.setShortcut(QKeySequence("Ctrl+Alt+2"));
    CHECK_EQ(keys::span(ids), keys::isolate("1/Ctrl+Alt+2/3/4"));
    two.setShortcut(QKeySequence());
    CHECK_EQ(keys::span(ids), keys::isolate("1/3/4"));
    for (QAction* a : filters) a->setShortcut(QKeySequence());
    CHECK(keys::span(ids).isEmpty());
    CHECK(keys::isCommandId("inspect.pin") && keys::isCommandId("sketch.c.horizontal") && !keys::isCommandId("Alt+Left") && !keys::isCommandId("Esc") && !keys::isCommandId("P"));
    CHECK(keys::spec("esc") == "Esc" && keys::spec("view.fit") == "Ctrl+Alt+F" && keys::spec("select.faces").isEmpty() && keys::spec("Alt+Left") == "Alt+Left");
    CHECK(keys::fixedCaps("shiftDel", Style::Pc) == L({"Shift", "Del"}) && keys::fixedCaps("f2", Style::Pc) == L({"F2"}));
    keys::setLookup([&](const QString& id) { return id == "view.fit" ? &fit : id == "select.faces" ? &faces : id == "view.home" ? &home : nullptr; });
  }
  // The shortcut editor says when it applied, once; a fixed key (Esc) is not the user's to change and is listed as reserved.
  int announced = 0;
  const auto counting = QObject::connect(keys::notifier(), &keys::Notifier::changed, [&] { ++announced; });
  QSettings().setValue("shortcuts/inspect.clear", "Q");
  QAction clear("Clear measurement");
  clear.setObjectName("inspect.clear");
  clear.setProperty("fixedShortcut", true);
  {
    QSettings settings;
    shortcuts::initialize(&clear, QKeySequence("Esc"), settings);
    shortcuts::initialize(&fit, QKeySequence("F"), settings);
    shortcuts::initialize(&home, QKeySequence("H"), settings);
  }
  CHECK(clear.shortcut() == QKeySequence("Esc") && shortcuts::fixedKey(&clear));
  {
    QSettings settings;
    shortcuts::migrate(settings);
    CHECK(!settings.contains("shortcuts/inspect.clear"));
  }
  {
    ShortcutEditor editor({&fit, &home, &clear});
    auto* tree = editor.findChild<QTreeWidget*>("shortcutTree");
    QTreeWidgetItem *clearRow = nullptr, *fitRow = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
      if ((*it)->toolTip(0) == "inspect.clear") clearRow = *it;
      if ((*it)->toolTip(0) == "view.fit") fitRow = *it;
    }
    CHECK(clearRow && clearRow->data(0, Qt::UserRole).toInt() == -1 && clearRow->text(1) == "Esc" && clearRow->parent());  // reserved, not editable
    CHECK(fitRow);
    tree->setCurrentItem(fitRow);
    editor.findChild<QKeySequenceEdit*>("shortcutBinding")->setKeySequence(QKeySequence("Ctrl+Alt+F"));
    editor.accept();
  }
  CHECK(announced == 1 && fit.shortcut() == QKeySequence("Ctrl+Alt+F") && clear.shortcut() == QKeySequence("Esc"));
  shortcuts::bind(&home, QKeySequence("Ctrl+H"));
  CHECK(announced == 2 && keys::text("view.home", Style::Pc) == keys::isolate("Ctrl+H"));
  QObject::disconnect(counting);
  keys::setLookup({});
  CHECK(!keys::hasLookup() && keys::text("view.fit").isEmpty());
  QSettings().remove("shortcuts");
}

// §6.3 test 4: key tokens in help texts with a fake lookup: bound and unbound keys, "(...)" dropped with the space before
// it, {press} as "Press ..." or "Choose <title>", {fixed}, a clip step's captionNoKey, requirements, search by key, Arabic.
TEST(help_expand_tokens) {
  help::load("en");
  auto plain = [](QString s) { return s.remove(QChar(0x2066)).remove(QChar(0x2069)); };
  QAction pin("Pin"), home("Home"), commands("Search commands");
  pin.setObjectName("inspect.pin");
  pin.setShortcut(QKeySequence("Ctrl+Alt+P"));
  home.setObjectName("view.home");  // no key
  commands.setObjectName("tools.commands");
  commands.setShortcut(QKeySequence("Ctrl+Space"));
  keys::setLookup([&](const QString& id) { return id == "inspect.pin" ? &pin : id == "view.home" ? &home : id == "tools.commands" ? &commands : nullptr; });
  bool bound = false;
  CHECK_EQ(plain(help::expand("Pin ({key:inspect.pin}) keeps it.", &bound)), QString("Pin (Ctrl+Alt+P) keeps it."));
  CHECK(bound);
  CHECK(help::expand("Pin ({key:inspect.pin}) keeps it.").contains(keys::isolate(keys::plain(QKeySequence("Ctrl+Alt+P")))));
  CHECK_EQ(help::expand("Home ({key:view.home}) goes home.", &bound), QString("Home goes home."));
  CHECK(!bound);
  CHECK_EQ(help::expand("Home ( {key:view.home} ), then on."), QString("Home, then on."));
  CHECK_EQ(help::expand("Then {key:view.home} goes home."), QString("Then Home goes home."));  // bare: the command's title
  CHECK_EQ(plain(help::expand("Pin ({key:inspect.pin}) or Home ({key:view.home}).")), QString("Pin (Ctrl+Alt+P) or Home."));
  CHECK_EQ(plain(help::expand("{press:tools.commands} and type")), QString("Press Ctrl+Space and type"));
  CHECK_EQ(help::expand("{press:view.home} to go back", &bound), QString("Choose Home to go back"));
  CHECK(!bound);
  CHECK_EQ(plain(help::expand("{fixed:enter} applies, {fixed:esc} steps back")), QString("Enter applies, Esc steps back"));
  CHECK_EQ(help::expand("{fixed:nope} stays"), QString("{fixed:nope} stays"));
  CHECK_EQ(help::expand("Select {n} bodies"), QString("Select {n} bodies"));  // not a key token
  CHECK_EQ(help::tokens("A ({key:view.fit}) {press:x.y} {fixed:esc} {n}"), QStringList({"key:view.fit", "press:x.y", "fixed:esc"}));
  CHECK_EQ(help::title("view.home"), help::find("view.home")->title);
  CHECK_EQ(help::title("no.such"), QString("no.such"));
  // A clip step: its captionNoKey when a key in the caption is unassigned.
  clips::Step step{0, 1, "Press {key:view.home} to go home", "Choose Home in the view's menu"};
  CHECK_EQ(clips::caption(step), QString("Choose Home in the view's menu"));
  step.caption = "Pin ({key:inspect.pin}) keeps it";
  CHECK_EQ(plain(clips::caption(step)), QString("Pin (Ctrl+Alt+P) keeps it"));
  step = clips::Step{0, 1, "Home ({key:view.home}) goes home", {}};
  CHECK_EQ(clips::caption(step), QString("Home goes home"));  // no keyless caption: the brackets drop
  // Requirements and tooltips expand too; search finds a command by the key it has now.
  CommandHelp h;
  h.requirement = "Pin ({key:inspect.pin}) needs {n} result";
  CHECK_EQ(plain(help::requirement(h, {{"n", 1}})), QString("Pin (Ctrl+Alt+P) needs 1 result"));
  CHECK(plain(help::tooltip(&pin)).contains("(Ctrl+Alt+P)"));
  CHECK(help::matches(*help::find("inspect.pin"), "ctrl+alt+p") && help::matches(*help::find("inspect.pin"), "Ctrl + Alt + P"));
  CHECK(!help::matches(*help::find("view.home"), "ctrl+alt+p") && !help::matches(*help::find("inspect.pin"), "ctrl+alt"));
  {  // Arabic: the translation keeps the tokens; the words around the key are translated.
    Arabic on;
    help::load("ar");
    CHECK_EQ(plain(help::expand("{press:tools.commands}")), QString::fromUtf8("اضغط Ctrl+Space"));
    CHECK_EQ(help::expand("{press:view.home}"), QString::fromUtf8("اختر ") + help::find("view.home")->title);
    const QString details = help::expand(QString::fromUtf8("ويحفظ التثبيت ({key:inspect.pin}) النتيجة"));
    CHECK(details.contains(QChar(0x2066)) && details.contains(QChar(0x2069)) && plain(details) == QString::fromUtf8("ويحفظ التثبيت (Ctrl+Alt+P) النتيجة"));
    CHECK_EQ(help::expand(QString::fromUtf8("ويعيد الرئيسي ({key:view.home}) العرض")), QString::fromUtf8("ويعيد الرئيسي العرض"));
  }
  help::load("en");
  keys::setLookup({});
}

// §6.3 test 1: every key element of every clip names a command this app registers or a fixed key, and shows its key
// now; no literal caps (a load problem). A command without a key draws its name instead.
TEST(clip_keys_resolve) {
  clips::load();
  const auto ids = registeredIds();
  QStringList wrong, literal;
  for (const QString& id : clips::ids())
    for (const clips::KeyRef& k : clips::keyRefs(id)) {
      if (!k.command.isEmpty() && !ids.count(k.command)) wrong << id + ": " + k.command;
      if (!k.fixed.isEmpty() && !keys::fixedNames().contains(k.fixed)) wrong << id + ": fixed " + k.fixed;
      if (!k.caps.isEmpty()) literal << id + ": " + k.caps.join('+');
    }
  if (!wrong.isEmpty()) throw check::Failure("key elements for no command: " + wrong.join(", ").toStdString());
  if (!literal.isEmpty()) throw check::Failure("literal key caps (name the command: {\"el\": \"key\", \"command\": ...}): " + literal.join(", ").toStdString());
  for (const QString& p : clips::problems())
    if (p.contains("literal caps")) throw check::Failure("the loader lets literal caps by: " + p.toStdString());
  // How they resolve: the user's key, a fixed key, the command's name without one; a lookup names unknown commands.
  QTemporaryDir dir;
  QFile f(dir.filePath("clips.json"));
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(R"({"clips": [{"id": "keys", "duration": 3, "items": [{"el": "grid"},
      {"el": "key", "command": "view.fit", "press": 0.5, "from": 0, "to": 1},
      {"el": "key", "command": "view.home", "from": 1, "to": 2},
      {"el": "key", "fixed": "ctrlEnter", "from": 2, "to": 3},
      {"el": "key", "command": "no.such", "from": 2, "to": 3}],
     "steps": [{"to": 1, "caption": "{press:view.fit}"}, {"to": 2, "caption": "{press:view.home}", "captionNoKey": "Use Home from the view cube's menu"}, {"to": 3, "caption": "{fixed:ctrlEnter}"}]},
    {"id": "bad", "duration": 1, "items": [{"el": "key", "fixed": "hyper"}, {"el": "key", "caps": ["F"], "command": "view.fit"}, {"el": "key", "caps": ["Ctrl", "S"]},
      {"el": "key", "fixed": ""}], "steps": [{"to": 1, "caption": "A"}]}]})");
  f.close();
  QAction fit("Fit"), home("Home");
  fit.setObjectName("view.fit");
  fit.setShortcut(QKeySequence("Ctrl+Alt+F"));
  home.setObjectName("view.home");
  keys::setLookup([&](const QString& id) { return id == "view.fit" ? &fit : id == "view.home" ? &home : nullptr; });
  clips::load(f.fileName());
  const QString problems = clips::problems().join("\n");
  for (const char* expected : {"bad: key: unknown fixed key hyper", "bad: key: one of caps, command or fixed", "keys: key: no command no.such", "bad: key: literal caps Ctrl+S"})
    if (!problems.contains(expected)) throw check::Failure("missing problem \"" + std::string(expected) + "\" in:\n" + problems.toStdString());
  CHECK(!problems.contains("unknown fixed key \n") && !problems.endsWith("unknown fixed key "));  // "": no key, drawn as nothing
  using L = QList<QStringList>;
  CHECK(clips::resolvedKeys("keys", 0.5) == L({{"Ctrl", "Alt", "F"}}));
  CHECK(clips::resolvedKeys("keys", 1.5) == L({{help::title("view.home")}}));
  CHECK(clips::resolvedKeys("keys", 2.5) == L({keys::fixedCaps("ctrlEnter"), {help::title("no.such")}}));
  CHECK(clips::resolvedKeys("keys").size() == 4);
  auto plain = [](QString s) { return s.remove(QChar(0x2066)).remove(QChar(0x2069)); };
  const auto steps = clips::steps("keys");
  CHECK(plain(clips::caption(steps[0])) == "Press Ctrl+Alt+F" && clips::caption(steps[1]) == "Use Home from the view cube's menu");
  // The frames show the new key at once (the still frame too) and the name chip without one.
  const QImage before = clips::frame("keys", 0.5, {288, 162});
  fit.setShortcut(QKeySequence("F"));
  CHECK(clips::resolvedKeys("keys", 0.5) == L({{"F"}}) && clips::frame("keys", 0.5, {288, 162}) != before);
  CHECK(!clips::frame("keys", 1.5, {288, 162}).isNull());
  keys::setLookup({});
  clips::load();
}

// §6.3 test 2: no help text spells a key the user can change: records (English and Arabic), every clip text and its
// Arabic, and the help area's Arabic. Every token names a registered command (or a fixed key), and English and Arabic
// carry the same tokens.
TEST(help_texts_have_no_literal_keys) {
  // A token may name any command the app makes (registeredIds: the window's and the areas', every one with help).
  const auto ids = registeredIds();
  QStringList found, tokens;
  auto checkTokens = [&](const QString& where, const QString& english, const QString& arabic) {
    for (const QString& t : help::tokens(english)) {
      const QString kind = t.section(':', 0, 0), name = t.section(':', 1);
      if (kind == "fixed" ? !keys::fixedNames().contains(name) : !ids.count(name)) tokens << where + ": " + t;
    }
    QStringList en = help::tokens(english), ar = help::tokens(arabic);
    en.sort();
    ar.sort();
    if (!arabic.isEmpty() && en != ar) tokens << where + ": the Arabic has " + ar.join(' ') + " for " + en.join(' ');
  };
  // Records, English and Arabic, field by field.
  const QJsonArray english = QJsonDocument::fromJson(source("app/help/commands.json").toUtf8()).object().value("commands").toArray();
  const QJsonObject translated = QJsonDocument::fromJson(source("app/help/commands.ar.json").toUtf8()).object();
  for (const QJsonValue& v : english) {
    const QJsonObject o = v.toObject();
    const QString id = o.value("id").toString();
    const QJsonObject ar = translated.value(id).toObject();
    QStringList keysFound;
    for (const char* field : {"summary", "details", "requires"}) {
      keysFound << literalKeys(o.value(field).toString()) << literalKeys(ar.value(field).toString());
      checkTokens(id + "." + field, o.value(field).toString(), ar.value(field).toString());
    }
    // Keywords are words, not keys: search matches the key the command has now (help::matches).
    static const QRegularExpression keyword(R"(^(ctrl|shift|alt|cmd|meta)[ +]\S|\+|^f\d{1,2}$|^(del|esc|ctrl|alt)$|^[a-z0-9]$)", QRegularExpression::CaseInsensitiveOption);
    for (const QJsonObject& r : {o, ar})
      for (const QJsonValue& k : r.value("keywords").toArray())
        if (keyword.match(k.toString()).hasMatch()) keysFound << "keyword " + k.toString();
    if (!keysFound.isEmpty()) found << "record " + id + ": " + keysFound.join(", ");
  }
  // Clip texts (captions, keyless captions, labels, chips, cards) and their Arabic.
  const QHash<QString, QString> arabic = i18n::table("ar", {QStringLiteral(OPAD_SOURCE_DIR) + "/app/i18n"});
  clips::load();
  QSet<QString> clipTexts;
  for (const QString& id : clips::ids()) {
    QStringList keysFound;
    for (const QString& text : clips::texts(id)) {
      clipTexts.insert(text);
      keysFound << literalKeys(text) << literalKeys(arabic.value(text));
      checkTokens(id + ": " + text, text, arabic.value(text));
    }
    if (!keysFound.isEmpty()) found << "clip " + id + ": " + keysFound.join(", ");
  }
  // Every other text the app shows in Arabic (app/i18n/ar.json and its fragments: the tr() texts of every area, the help
  // area's lessons and coach card, toasts and error messages), the English and its translation.
  for (auto it = arabic.constBegin(); it != arabic.constEnd(); ++it) {
    if (it.key().startsWith('@') || clipTexts.contains(it.key())) continue;
    const QStringList keysFound = literalKeys(it.key()) + literalKeys(it.value());
    checkTokens("ar: " + it.key(), it.key(), it.value());
    if (!keysFound.isEmpty()) found << "text \"" + it.key() + "\": " + keysFound.join(", ");
  }
  if (!tokens.isEmpty()) throw check::Failure("tokens: " + tokens.join(" | ").toStdString());
  if (!found.isEmpty()) throw check::Failure("literal keys (use {key:id}, {press:id} or {fixed:name}): " + found.join(" | ").toStdString());
  // The checker itself.
  CHECK_EQ(literalKeys("Press D, then Ctrl+Z or F9 (S)"), QStringList({"Ctrl+", "F9", "Press D", "(S)"}));
  CHECK_EQ(literalKeys("Select it and press Del"), QStringList({"press Del"}));
  CHECK_EQ(literalKeys("G again hides it"), QStringList({"G again"}));
  CHECK_EQ(literalKeys("%1: pick more, P pins, Esc clears"), QStringList({", P pins"}));
  CHECK(literalKeys("Press Enter or Esc; Shift+Tab goes back; Ctrl+click adds, Shift+drag pans; {press:view.fit} ({key:inspect.pin})").isEmpty());
  CHECK(literalKeys("X offset · Y axis · Orbit · B pen · E eraser · B is an earlier A · {press:view.grid} again hides it").isEmpty());
  CHECK_EQ(literalKeys(QString::fromUtf8("اضغط F لملاءمة العرض")), QStringList({QString::fromUtf8("اضغط F")}));
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
  // Every clip is shown somewhere: a command's own (its help record), one a record adopts by its "clip" field, or a panel's
  // own guide (the drawing placer's); insert.canvas waits for the canvas commands (future commands name theirs here first).
  help::load("en");
  QSet<QString> adopted;
  for (const CommandHelp& h : help::all()) adopted.insert(h.clip);
  for (const QString& id : clips::ids())
    if (!help::find(id) && !adopted.contains(id) && !QStringList{"drawing.place", "insert.canvas"}.contains(id))
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
  // The drawing placer's panel (DrawingPlacer: plane, move, snap, Place): moving loops move, snap and Place; snapping its step.
  CHECK(clips::guideRange("drawing.place", 1, 4) == R(1, 3) && clips::guideRange("drawing.place", 2, 4) == R(2, 2) && clips::guideRange("drawing.place", 4, 4) == R(3, 3));
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

// The palette ranks the query as a whole name first, then as a whole word, then as the start of a word, all above letters
// scattered through a name (an Arabic label that keeps a Latin name, "ODA File Converter", never wins "fit").
TEST(palette_ranks_whole_words_first) {
  help::load("en");
  QSettings().remove("palette/recent");
  QAction oda(QString::fromUtf8("استخدام ODA File Converter")), fitting("Fitting"), sheet("Fit sheet"), fit("Fit");
  oda.setObjectName("x.oda");
  fitting.setObjectName("x.fitting");
  sheet.setObjectName("x.sheet");
  fit.setObjectName("x.fit");
  CommandPalette palette({&oda, &fitting, &sheet, &fit});
  palette.findChild<QLineEdit*>("paletteInput")->setText("fit");
  auto* list = palette.findChild<QListWidget*>("paletteList");
  QStringList order;
  for (int i = 0; i < list->count(); ++i) order << static_cast<QAction*>(list->item(i)->data(Qt::UserRole).value<void*>())->objectName();
  CHECK_EQ(order, QStringList({"x.fit", "x.sheet", "x.fitting", "x.oda"}));
  // A key typed: first with a modifier or a name ("ctrl+l", "f2"), but one plain letter is the start of a name search.
  QAction line("Line"), copy("Copy"), rename("Rename");
  line.setObjectName("x.line");
  copy.setObjectName("x.copy");
  rename.setObjectName("x.rename");
  line.setShortcut(QKeySequence("C"));
  copy.setShortcut(QKeySequence("Ctrl+L"));
  rename.setShortcut(QKeySequence("F2"));
  CommandPalette keyed({&line, &copy, &rename});
  auto first = [&keyed](const QString& query) {
    keyed.findChild<QLineEdit*>("paletteInput")->setText(query);
    QStringList ids;
    auto* rows = keyed.findChild<QListWidget*>("paletteList");
    for (int i = 0; i < rows->count(); ++i) ids << static_cast<QAction*>(rows->item(i)->data(Qt::UserRole).value<void*>())->objectName();
    return ids;
  };
  CHECK_EQ(first("c"), QStringList({"x.copy", "x.line"}));  // Copy's name, then Line's key
  CHECK_EQ(first("ctrl+l").value(0), QString("x.copy"));
  CHECK_EQ(first("f2").value(0), QString("x.rename"));
}

// Commands are listed by area (the palette's group column, the reference's headings), every area named once.
TEST(command_areas) {
  help::load("en");
  CHECK_EQ(help::group("design.extrude"), QString("Design"));
  CHECK_EQ(help::group("sketch.line"), QString("Sketch"));
  CHECK(help::group("sketch.c.horizontal") == "Sketch constraints" && help::group("sketch.dimension") == "Sketch constraints");
  CHECK(help::group("view.fit") == "View" && help::group("nav.fusion") == "View" && help::group("help.about") == "Tools and help");
  CHECK(help::group("files.useOda") == "File" && help::group("help.licenses") == "Tools and help");
  CHECK(help::group("vcs.commit") == "File" && help::group("timeline.names") == "View" && help::group("drawing2d.layers") == "View" &&
        help::group("assembly.explode") == "Design" && help::group("drawings.baseView.top") == "Drawings");
  QStringList areas = help::areas();
  CHECK(areas.size() == 12 && areas.removeDuplicates() == 0);
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
  // A panel's "?" and Help for this tool open a command with the clip of what is being done in it (Import's drawing
  // placer); opened again without one, its own clip is back.
  reference.open("file.import", "drawing.place");
  CHECK(card->command() == "file.import" && card->clip()->clip() == "drawing.place" && card->steps()->count() == clips::steps("drawing.place").size() + 1);
  reference.open("file.import");
  CHECK(card->command() == "file.import" && card->clip()->clip() == "file.import" && card->steps()->count() == clips::steps("file.import").size() + 1);
  reference.open("design.extrude", "no.such.clip");
  CHECK(card->clip()->clip() == "design.extrude");
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
