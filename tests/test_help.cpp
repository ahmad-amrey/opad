// Command help (UI-106): every command id the app registers has an English record and its Arabic translation, the
// registry's lookups, search and tooltips, and the rich card's states, timing and layout (offscreen). The animated
// clips (UI-107): the library, rendering, translations, the loader's checks and templates, the player.
#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "I18n.hpp"
#include "RichTip.hpp"
#include "Theme.hpp"
#include "check.hpp"
#include "opad/design/feature.hpp"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>
#include <set>

namespace {
QString source(const QString& path) {
  QFile f(QStringLiteral(OPAD_SOURCE_DIR) + "/" + path);
  if (!f.open(QIODevice::ReadOnly)) throw check::Failure("cannot read " + path.toStdString());
  return QString::fromUtf8(f.readAll());
}

// Command ids registered by MainWindow: literal addAction ids, the generated families, the sketch tool tables and
// the feature kinds of the core spec table. The bench OPAD_BENCH_RICHTIP checks the live list of the running app.
std::set<QString> registeredIds() {
  std::set<QString> ids;
  const QString main = source("app/MainWindow.cpp");
  for (const auto& m : QRegularExpression(R"(addAction\("([a-z]+\.[A-Za-z0-9_.]+)\")").globalMatch(main)) ids.insert(m.captured(1));
  for (const auto& m : QRegularExpression(R"re(\{"([a-z0-9_:]+)", tr\(")re").globalMatch(main)) ids.insert("sketch." + m.captured(1).replace(':', '.'));
  for (const auto& m : QRegularExpression(R"re(QObject::tr\("[^"]+"\),"([a-z0-9_:]+)")re").globalMatch(source("app/SketchPanel.cpp")))
    ids.insert("sketch." + m.captured(1).replace(':', '.'));
  for (const char* v : {"top", "front", "right", "iso", "bottom", "back", "left"}) ids.insert(QString("view.") + v);
  for (const char* v : {"fusion", "solidworks", "onshape", "blender"}) ids.insert(QString("nav.") + v);
  for (const char* v : {"bodies", "faces", "edges", "vertices"}) ids.insert(QString("select.") + v);
  for (const char* v : {"view.extensions", "view.tracking", "view.gridSnap", "sketch.selectionOptions", "sketch.constraints", "sketch.snaps"}) ids.insert(v);
  for (const char* v : {"Create", "Modify", "Constrain", "Reference", "Files"}) ids.insert(QString("sketch.more") + v);
  ids.erase("sketch.more");  // the prefix of the generated menu ids
  for (const auto& spec : opad::design::feature_specs()) ids.insert("design." + QString::fromStdString(spec.kind));
  return ids;
}

int sentences(const QString& text) { return static_cast<int>(text.count(QRegularExpression(R"([.!?](\s|$))"))); }
QString clean(QString s) { return s.remove('&').remove(QString::fromUtf8("…")).remove("...").trimmed(); }
}  // namespace

TEST(every_registered_command_has_help) {
  help::load("en");
  const auto ids = registeredIds();
  CHECK(ids.size() > 200);
  QStringList missing;
  for (const QString& id : ids) if (!help::find(id)) missing << id;
  if (!missing.isEmpty()) throw check::Failure("no help for " + missing.join(", ").toStdString());
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

// The help area's own tr() strings have their Arabic in app/i18n/ar/help.json (or ar.json), and loading the Arabic help
// makes them translate even before i18n::install merges the area fragments; English removes them again.
TEST(help_area_strings_are_translated) {
  QJsonObject arabic = QJsonDocument::fromJson(source("app/i18n/ar.json").toUtf8()).object();
  const QJsonObject fragment = QJsonDocument::fromJson(source("app/i18n/ar/help.json").toUtf8()).object();
  for (auto it = fragment.begin(); it != fragment.end(); ++it) arabic.insert(it.key(), it.value());
  QStringList strings;
  for (const char* file : {"app/RichTip.cpp", "app/RichTip.hpp", "app/CommandHelp.cpp", "app/HelpBench.cpp", "app/HelpClip.cpp", "app/HelpClip.hpp"})
    for (const auto& m : QRegularExpression(R"re(\btr\("((?:[^"\\]|\\.)*)"\))re").globalMatch(source(file))) strings << m.captured(1);
  CHECK(strings.size() >= 2);
  for (const QString& s : strings)
    if (arabic.value(s).toString().isEmpty()) throw check::Failure("no Arabic for \"" + s.toStdString() + "\" in app/i18n/ar/help.json");
  help::load("ar");
  for (const QString& s : strings) CHECK_EQ(RichTip::tr(s.toUtf8().constData()), arabic.value(s).toString());
  help::load("en");
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
// app/i18n/ar/help.json); the help area's own ones translate at run time once the Arabic help is loaded.
TEST(clip_texts_are_translated) {
  QJsonObject arabic = QJsonDocument::fromJson(source("app/i18n/ar.json").toUtf8()).object();
  const QJsonObject fragment = QJsonDocument::fromJson(source("app/i18n/ar/help.json").toUtf8()).object();
  for (auto it = fragment.begin(); it != fragment.end(); ++it) arabic.insert(it.key(), it.value());
  clips::load();
  QStringList missing;
  for (const QString& id : clips::ids())
    for (const QString& text : clips::texts(id)) if (arabic.value(text).toString().isEmpty()) missing << id + ": " + text;
  if (!missing.isEmpty()) throw check::Failure("no Arabic for " + missing.join(" | ").toStdString());
  help::load("ar");
  CHECK_EQ(i18n::t("Click the start point"), fragment.value("Click the start point").toString());
  help::load("en");
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

// The player runs a timer only while visible and not reduced to its still frame; a step loops inside its segment.
TEST(clip_view_plays_only_when_visible) {
  clips::load();
  QSettings().setValue("ui/tipAnimate", true);
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
