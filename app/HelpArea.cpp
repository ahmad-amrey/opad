// The help area (UI-106/107/108): every ribbon button, status-bar toggle and menu command shows its command's rich card
// (RichTip) with the command's animated clip (ClipView). The tool, feature and sketch panels play their own guides
// (ToolGuide), and the "?" of every tool panel opens the tool guide at its command; the command palette previews the
// current command. The Help menu: Help for this tool (F1, at the command running now), Tool guide, Shortcuts cheat
// sheet (Ctrl+/), Getting started, Report a problem, above the window's own entries (licences, About). An empty document
// shows the coach card: how a design starts, with buttons for the first step.
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QSysInfo>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

#include <Standard_Version.hxx>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "CommandHelp.hpp"
#include "CommandPalette.hpp"
#include "Commands.hpp"
#include "HelpClip.hpp"
#include "HelpReference.hpp"
#include "HelpWindows.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Ribbon.hpp"
#include "RichTip.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"
#include "opad/util.hpp"

OPAD_ICON_TABLE(help, {"help", R"(<circle cx="12" cy="12" r="9"/><path d="M9.5 9.5 a2.6 2.6 0 0 1 5.1 0.4 c0 1.9 -2.6 2.3 -2.6 4.1"/><path d="M12 17.2h.01"/>)"},
                {"keyboard", R"(<rect x="2.5" y="6" width="19" height="12" rx="2"/><path d="M6.5 10h.01M10 10h.01M14 10h.01M17.5 10h.01M7.5 14h9"/>)"},
                {"start", R"(<path d="M5 21V4"/><path d="M5 4h12l-2.5 4 2.5 4H5"/>)"},
                {"report", R"(<path d="M4 5h16v11H10l-5 4v-4H4z"/><path d="M12 8v3.5M12 14h.01"/>)"});

class HelpArea : public AreaController {
 public:
  explicit HelpArea(AreaServices& services) : AreaController(services) {
    RibbonBar::setCommandButtonHook(&RichTip::attach);  // the ribbon is built after the areas are made
    RichTip::setActionLookup([&services](const QString& id) { return services.action(id); });
    RichTip::setClipFactory([](const QString& clip, QWidget* parent) -> QWidget* { return new ClipView(clip, parent); }, &clips::has);
    RichTip::setMenuCards(true);  // every menu's command entries
    ToolPanel::setHelpHook([this](ToolPanel* panel) { openReference(panelCommand(panel)); });  // the panels are built later
  }
  ~HelpArea() override {
    RibbonBar::setCommandButtonHook({});
    RichTip::setActionLookup({});
    RichTip::setClipFactory({});
    RichTip::setMenuCards(false);
    ToolPanel::setHelpHook({});
  }

  void buildActions() override {
    CommandInfo current;  // over a ribbon button or a menu entry F1 expands that command's card instead (RichTip)
    current.id = "help.current";
    current.label = tr("Help for this tool");
    current.icon = "help";
    current.key = QKeySequence("F1");
    current.keywords = {"F1", "how to", "current tool"};
    services().addCommand(current, [this] { openReference(currentCommand()); });
    CommandInfo guide;
    guide.id = "help.reference";
    guide.label = tr("Tool guide");
    guide.icon = "list";
    guide.keywords = {"reference", "manual", "documentation", "all commands"};
    services().addCommand(guide, [this] { openReference(QString()); });
    CommandInfo sheet;
    sheet.id = "help.shortcuts";
    sheet.label = tr("Shortcuts cheat sheet");
    sheet.icon = "keyboard";
    sheet.key = QKeySequence("Ctrl+/");
    sheet.keywords = {"keys", "keyboard", "hotkeys", "mouse"};
    services().addCommand(sheet, [this] { openSheet(); });
    CommandInfo start;
    start.id = "help.start";
    start.label = tr("Getting started");
    start.icon = "start";
    start.keywords = {"tutorial", "learn", "first steps", "basics"};
    services().addCommand(start, [this] { openGettingStarted(0); });
    CommandInfo report;
    report.id = "help.report";
    report.label = tr("Report a problem…");
    report.icon = "report";
    report.keywords = {"bug", "issue", "feedback", "crash", "support"};
    services().addCommand(report, [this] { reportProblem(); });
  }

  // First in the Help menu; the window's own entries (third-party licences, About) stay below.
  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* help = menus.value("help");
    if (!help) return;
    QAction* first = help->actions().value(0);
    for (const char* id : {"help.current", "help.reference", "help.shortcuts", "help.start"}) help->insertAction(first, services().action(id));
    help->insertSeparator(first);
    help->insertAction(first, services().action("help.report"));
    help->insertSeparator(first);
  }

  void ready() override {
    for (auto* b : services().window()->statusBar()->findChildren<QToolButton*>())  // the status bar's toggles (extensions, tracking, grid snapping)
      if (b->defaultAction() && help::find(b->defaultAction()->objectName())) RichTip::attach(b, b->defaultAction()->objectName());
    for (QAction* a : services().commands().actions())  // the palette's recent commands: whatever ran them (button, menu, key, palette)
      if (a) connect(a, &QAction::triggered, this, [this, id = a->objectName()] {
        palette::noteRun(id);
        QTimer::singleShot(0, this, &HelpArea::updateCoach);  // a tool started over an empty document: the card gives way
      });
    m_coach = new CoachCard(services().viewport());
    m_coach->hide();
    connect(m_coach, &CoachCard::run, this, [this](const QString& id) { run(id); updateCoach(); });
    connect(m_coach, &CoachCard::dismissed, this, [this] { m_coachDismissed = services().document()->generation; });
    connect(m_coach, &CoachCard::neverAgain, this, [] { QSettings().setValue("help/coach", false); });
    m_coachPoll.setInterval(500);  // while an empty document is open: a tool left without a change brings the card back
    connect(&m_coachPoll, &QTimer::timeout, this, &HelpArea::updateCoach);
    updateCoach();
  }

  void documentChanged(bool) override { updateCoach(); }
  void workspaceChanged(const QString&) override { updateCoach(); }
  void positionOverlays(const QRect&) override { placeCoach(); }

 private:
  // The coach card: an editable document with no body and no sketch, no tool running, not hidden for this document (its
  // ×) nor for good (help/coach).
  bool coachWanted() const {
    const AppDocument* d = services().document();
    if (!d || !d->hasDocument || d->browse || d->loading || !d->scene.sketches.empty() || d->generation == m_coachDismissed) return false;
    if (std::any_of(d->scene.nodes.begin(), d->scene.nodes.end(), [](const auto& n) { return n.second.kind == opad::Node::Kind::Body; })) return false;
    return services().activeCommand().isEmpty() && services().workspace() != "sketch" && QSettings().value("help/coach", true).toBool();
  }

  void updateCoach() {
    if (!m_coach) return;
    const AppDocument* d = services().document();
    const bool open = d && d->hasDocument && !d->browse && d->scene.sketches.empty();
    if (open != m_coachPoll.isActive()) open ? m_coachPoll.start() : m_coachPoll.stop();
    const bool show = coachWanted();
    if (show == m_coach->isVisible()) return;
    m_coach->setVisible(show);
    if (show) placeCoach();
  }

  void placeCoach() {  // bottom centre of the viewport, above the toasts
    if (!m_coach || !m_coach->isVisible()) return;
    const QWidget* view = m_coach->parentWidget();
    m_coach->adjustSize();
    m_coach->move(std::max(8, (view->width() - m_coach->width()) / 2), std::max(8, view->height() - m_coach->height() - 72));
    m_coach->raise();
  }

  // What Report a problem tells about OPAD and this computer: versions, screens, settings that change behaviour, the
  // document's kind and size (never its path or name), the tool running.
  QStringList facts() const {
    QStringList f{"OPAD " + QString::fromStdString(opad::version_string()),
                  QString("%1 (%2), Qt %3, Open CASCADE %4").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), qVersion(), OCC_VERSION_COMPLETE)};
    for (QScreen* s : QGuiApplication::screens()) f << QString("Screen %1 x %2 at scale %3").arg(s->size().width()).arg(s->size().height()).arg(s->devicePixelRatio());
    f << QString("Language %1, %2 theme, %3 navigation, %4 workspace").arg(i18n::current(), theme::current().dark ? "dark" : "light", preset(), services().workspace());
    const AppDocument* d = services().document();
    if (!d || !d->hasDocument) f << "Document: none";
    else if (d->browse) f << QString("Document: a .%1 file in viewer mode, %2 bodies").arg(QFileInfo(d->viewing).suffix().toLower()).arg(d->scene.all_bodies().size());
    else f << QString("Document: OPAD, %1 operations, %2 bodies, %3 sketches%4").arg(d->doc.ops.size()).arg(d->scene.all_bodies().size()).arg(d->scene.sketches.size()).arg(d->path().isEmpty() ? ", not saved yet" : "");
    if (const QString tool = services().activeCommand(); !tool.isEmpty()) f << "Running: " + tool;
    const QString trace = qEnvironmentVariable("OPAD_TRACE");
    f << (trace.isEmpty() ? QString("Trace: off (start OPAD with OPAD_TRACE=<file> to record one)") : "Trace: " + QDir::toNativeSeparators(trace));
    return f;
  }

  // The command running now (a measure tool, a sketch tool, a feature) or whose card is up; empty: none.
  QString currentCommand() const {
    QString id = services().activeCommand();
    if (id.isEmpty() && RichTip::instance()->state() != RichTip::State::Hidden) id = RichTip::instance()->commandId();
    return id;
  }

  // What a panel's "?" opens: its own help id, else the tool running in it, else the panel's command.
  QString panelCommand(const ToolPanel* panel) const {
    if (help::find(panel->helpId())) return panel->helpId();
    static const QHash<QString, QString> commands{{"properties", "inspect.properties"}, {"annotations", "panel.annotations"}, {"section", "panel.section"},
                                                  {"parameters", "design.parameters"}, {"sketch", "sketch.panel"}, {"drawing", "design.convertDrawing"},
                                                  {"drawing-place", "file.import"}, {"sketch-plane", "design.sketch"}};
    const QString active = services().activeCommand();
    if (!active.isEmpty() && QStringList({"tool", "feature", "annotation", "sketch"}).contains(panel->id())) return active;
    return commands.value(panel->id(), active);
  }

  QString preset() const {  // the navigation preset's id: its command is the checked nav.* one
    for (const char* p : {"fusion", "solidworks", "onshape", "blender"})
      if (QAction* a = services().action(QString("nav.") + p); a && a->isChecked()) return p;
    return "fusion";
  }

  // The tool guide at `id` (empty: where it was).
  void openReference(const QString& id) {
    QWidget* window = services().window();
    auto* reference = window->findChild<CommandReference*>();
    if (!reference) reference = new CommandReference([this](const QString& command) { return services().action(command); }, window);
    reference->open(id);
  }

  void openSheet() {
    QWidget* window = services().window();
    auto* sheet = window->findChild<ShortcutSheet*>();
    if (!sheet) {
      sheet = new ShortcutSheet(window);
      connect(sheet, &ShortcutSheet::editRequested, this, [this, sheet] {
        if (QAction* editor = services().action("tools.shortcuts")) editor->trigger();  // modal: the keys are final when it returns
        sheet->setGroups(help::keyGroups(services().commands().actions(), services().selection().sketching, preset()));
      });
    }
    sheet->setGroups(help::keyGroups(services().commands().actions(), services().selection().sketching, preset()));
    sheet->show();
    sheet->raise();
    sheet->activateWindow();
  }

  void openGettingStarted(int lesson) {
    QWidget* window = services().window();
    auto* start = window->findChild<GettingStarted*>();
    if (!start)
      start = new GettingStarted([this](const QString& id) { return services().action(id); }, [this](const QString& id) { run(id); }, window);
    start->setPreset(preset());
    start->open(lesson);
  }

  // A lesson's command: design ones in Design (a sketch keeps its own workspace).
  void run(const QString& id) {
    QAction* a = services().action(id);
    if (!a || !a->isEnabled()) return;
    if (id.startsWith("design.") && services().workspace() == "review") services().setWorkspace("design");
    services().window()->activateWindow();
    a->trigger();
  }

  void reportProblem() {
    const QString url = QSettings().value("help/issueUrl", "https://github.com/ahmad-amrey/opad/issues/new").toString();
    auto* report = new ProblemReport(facts(), url, services().window());
    report->setAttribute(Qt::WA_DeleteOnClose);
    report->show();
  }

  CoachCard* m_coach = nullptr;
  QTimer m_coachPoll;
  unsigned long long m_coachDismissed = 0;  // the document generation whose card was closed
};

OPAD_AREA(HelpArea)
