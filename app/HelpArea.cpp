// The help area (UI-106/107): every ribbon button, status-bar toggle and menu command shows its command's rich card
// (RichTip) with the command's animated clip (ClipView), and Help > Command reference (F1) opens at the command running now. The tool,
// feature and sketch panels play their own guides (ToolGuide); the command palette previews the current command.
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>
#include <QToolButton>

#include "AreaController.hpp"
#include "CommandHelp.hpp"
#include "Commands.hpp"
#include "HelpClip.hpp"
#include "HelpReference.hpp"
#include "Ribbon.hpp"
#include "RichTip.hpp"

class HelpArea : public AreaController {
 public:
  explicit HelpArea(AreaServices& services) : AreaController(services) {
    RibbonBar::setCommandButtonHook(&RichTip::attach);  // the ribbon is built after the areas are made
    RichTip::setActionLookup([&services](const QString& id) { return services.action(id); });
    RichTip::setClipFactory([](const QString& clip, QWidget* parent) -> QWidget* { return new ClipView(clip, parent); }, &clips::has);
    RichTip::setMenuCards(true);  // every menu's command entries
  }
  ~HelpArea() override {
    RibbonBar::setCommandButtonHook({});
    RichTip::setActionLookup({});
    RichTip::setClipFactory({});
    RichTip::setMenuCards(false);
  }

  void buildActions() override {
    // F1 over a ribbon button expands its card instead (RichTip takes F1 there).
    CommandInfo reference;
    reference.id = "help.reference";
    reference.label = tr("Command reference");
    reference.icon = "list";
    reference.key = QKeySequence("F1");
    services().addCommand(reference, [this] { openReference(); });
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    if (QMenu* help = menus.value("help")) help->insertAction(help->actions().value(0), services().action("help.reference"));
  }

  void ready() override {  // the status bar's toggles (extensions, tracking, grid snapping)
    for (auto* b : services().window()->statusBar()->findChildren<QToolButton*>())
      if (b->defaultAction() && help::find(b->defaultAction()->objectName())) RichTip::attach(b, b->defaultAction()->objectName());
  }

 private:
  // At the command running now (a measure tool, a sketch tool, a feature) or whose card is up, else where it was.
  void openReference() {
    QString id = services().activeCommand();
    if (id.isEmpty() && RichTip::instance()->state() != RichTip::State::Hidden) id = RichTip::instance()->commandId();
    QWidget* window = services().window();
    auto* reference = window->findChild<CommandReference*>();
    if (!reference) reference = new CommandReference([this](const QString& command) { return services().action(command); }, window);
    reference->open(id);
  }
};

OPAD_AREA(HelpArea)
