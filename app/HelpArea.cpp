// The help area (UI-106/107): every ribbon button, status-bar toggle and menu command shows its command's rich card
// (RichTip) with the command's animated clip (ClipView), and Help > Command reference (F1) opens at the command running now. The tool,
// feature and sketch panels play their own guides (ToolGuide), and the "?" of every tool panel opens the reference at
// its command; the command palette previews the current command.
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>
#include <QToolButton>

#include "AreaController.hpp"
#include "CommandHelp.hpp"
#include "CommandPalette.hpp"
#include "Commands.hpp"
#include "HelpClip.hpp"
#include "HelpReference.hpp"
#include "Icons.hpp"
#include "Ribbon.hpp"
#include "RichTip.hpp"
#include "ToolPanel.hpp"

OPAD_ICON_TABLE(help, {"help", R"(<circle cx="12" cy="12" r="9"/><path d="M9.5 9.5 a2.6 2.6 0 0 1 5.1 0.4 c0 1.9 -2.6 2.3 -2.6 4.1"/><path d="M12 17.2h.01"/>)"});

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
    // F1 over a ribbon button expands its card instead (RichTip takes F1 there).
    CommandInfo reference;
    reference.id = "help.reference";
    reference.label = tr("Command reference");
    reference.icon = "list";
    reference.key = QKeySequence("F1");
    services().addCommand(reference, [this] { openReference(currentCommand()); });
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    if (QMenu* help = menus.value("help")) help->insertAction(help->actions().value(0), services().action("help.reference"));
  }

  void ready() override {
    for (auto* b : services().window()->statusBar()->findChildren<QToolButton*>())  // the status bar's toggles (extensions, tracking, grid snapping)
      if (b->defaultAction() && help::find(b->defaultAction()->objectName())) RichTip::attach(b, b->defaultAction()->objectName());
    for (QAction* a : services().commands().actions())  // the palette's recent commands: whatever ran them (button, menu, key, palette)
      if (a) connect(a, &QAction::triggered, this, [id = a->objectName()] { palette::noteRun(id); });
  }

 private:
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

  // The reference at `id` (empty: where it was).
  void openReference(const QString& id) {
    QWidget* window = services().window();
    auto* reference = window->findChild<CommandReference*>();
    if (!reference) reference = new CommandReference([this](const QString& command) { return services().action(command); }, window);
    reference->open(id);
  }
};

OPAD_AREA(HelpArea)
