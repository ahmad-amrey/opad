// Command help (UI-106): every command id the app registers has an English record and its Arabic translation, the
// registry's lookups, search and tooltips, and the rich card's states, timing and layout (offscreen).
#include "CommandHelp.hpp"
#include "RichTip.hpp"
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
  for (const CommandHelp& h : help::all()) english.insert(h.id, h.title);
  help::load("ar");
  for (const CommandHelp& h : help::all()) {
    const std::string id = h.id.toStdString();
    if (!h.translated) throw check::Failure(id + ": not translated in app/help/commands.ar.json");
    if (!h.title.contains(QRegularExpression("[\\x{0600}-\\x{06FF}]"))) throw check::Failure(id + ": the Arabic title has no Arabic");
    if (label.contains(english.value(h.id)) && !label.values(english.value(h.id)).contains(h.title)) throw check::Failure(id + ": title differs from ar.json");
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
  for (const char* file : {"app/RichTip.cpp", "app/RichTip.hpp", "app/CommandHelp.cpp", "app/HelpBench.cpp"})
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
