// Preferences (UI-110): the command (Ctrl+, in the Edit menu; the application menu on macOS) and the built-in pages.
// Every control applies as it changes, through the same calls as before (the viewport's setters, the actions of the
// status toggles, the units service, the recovery timer, the agent bridge), so a setting changed here and one changed
// where it lived before stay the same setting. The gear menu keeps the quick switches and leads here for the rest.
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>

#include "AgentBridge.hpp"
#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "BrowserOverlay.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "FileAssociations.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "Preferences.hpp"
#include "RecoveryManager.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/commands.hpp"

OPAD_ICON_TABLE(preferences, {"agent", R"(<rect x="5" y="7" width="14" height="11" rx="3"/><path d="M12 3v4M9 12h.01M15 12h.01M9.5 15h5"/>)"});


class PreferencesArea : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    CommandInfo info;
    info.id = "tools.preferences";
    info.label = tr("Preferences…");
    info.icon = "settings";
    info.key = QKeySequence("Ctrl+,");
    info.keywords = {"settings", "options", "configure", "setup"};
    QAction* a = services().addCommand(info, [this] { PreferencesDialog::open(services().window()); });
    a->setMenuRole(QAction::PreferencesRole);
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    if (QMenu* edit = menus.value("edit")) {
      edit->addSeparator();
      edit->addAction(services().action("tools.preferences"));
    }
  }

  void ready() override {
    using preferences::Form;
    QWidget* window = services().window();
    preferences::addPage({"general", tr("General"), "settings", 10, {"language", "theme", "author", "undo", "help", "tips"}, [this] { return general(); }});
    preferences::addPage({"display", tr("Display"), "shaded", 20, {"rendering", "background", "hover", "quality", "panels"}, [this] { return display(); }});
    preferences::addPage({"units", tr("Units and precision"), "dimension", 30, {"mm", "inch", "decimals", "radians", "fractions"}, [this] { return unitsPage(); }});
    preferences::addPage({"sketch", tr("Sketch and snaps"), "magnet", 40, {"snap", "inference", "solver", "ortho", "polar", "tracking"}, [this] { return sketch(); }});
    preferences::addPage({"grid", tr("Grid"), "grid", 50, {"spacing", "extent", "snap"}, [this] { return grid(); }});
    preferences::addPage({"files", tr("Files"), "open", 60, {"viewer", "read-only", "cache", "file types", "associations"}, [this] { return files(); }});
    preferences::addPage({"recovery", tr("Autosave and recovery"), "restore", 70, {"autosave", "snapshot", "crash"}, [this] { return recovery(); }});
    preferences::addPage({"vcs", tr("Version control"), "git", 80, {"git", "merge", "branch"}, [this] { return versionControl(); }});
    preferences::addPage({"keyboard", tr("Keyboard and mouse"), "keyboard", 90, {"shortcuts", "keys", "navigation", "mouse", "orbit", "preset"}, [this] { return keyboard(); }});
    preferences::addPage({"ai", tr("AI integration"), "agent", 100, {"agent", "mcp", "assistant"}, [window] { return ai(window); }});
  }

 private:
  QAction* action(const char* id) const { return services().action(id); }

  QWidget* general() {
    auto* page = new QWidget;
    preferences::Form form(page);
    form.section(tr("Appearance"));
    form.option(action("view.dark"), tr("Dark theme"));
    auto* language = new QComboBox;
    language->setObjectName("ui/language");
    for (const i18n::Language& l : i18n::languages()) {
      language->addItem(l.name, l.code);
      if (l.code == i18n::current()) language->setCurrentIndex(language->count() - 1);
    }
    form.row(tr("Language"), language);
    QLabel* restart = form.note(tr("The language changes the next time OPAD starts."));
    restart->hide();
    QObject::connect(language, &QComboBox::currentIndexChanged, language, [language, restart] {
      i18n::setLanguage(language->currentData().toString());
      restart->setVisible(language->currentData().toString() != i18n::current());
    });
    form.section(tr("You"), tr("The name recorded on your notes and changes."));
    form.text("user/name", tr("Name"), QString::fromStdString(opad::default_author()));
    form.section(tr("History"));
    form.integer("edit/undoDepth", tr("Undo steps kept"), 50, 1, 1000, QString(), [this](int n) { services().document()->setUndoLimit(n); });
    form.section(tr("Help and feedback"));
    form.choice("ui/tips", tr("Hover help"), {tr("Off"), tr("Plain tooltips"), tr("Cards with details and clips")}, 2);
    form.integer("ui/tipExpandMs", tr("Show the details after"), 1200, 500, 5000, tr(" ms"));
    form.check("ui/tipAnimate", tr("Animate the help clips"), true);
    form.check("ui/toolGuide", tr("Show the guide in tool panels"), true);
    form.check("help/coach", tr("Show the coach card in an empty design"), true);
    form.number("ui/doneToastSeconds", tr("Say when a task ends that took at least"), 3, 0, 600, 0, tr(" s"));
    form.note(tr("0 s: never. Tasks that run in the background, such as recovery snapshots, never say so."));
    form.finish();
    return page;
  }

  QWidget* display() {
    auto* page = new QWidget;
    preferences::Form form(page);
    Viewport* view = services().viewport();
    form.section(tr("Rendering"));
    form.choice("view/qualityV2", tr("Rendering quality"), {tr("Draft (fast)"), tr("Studio"), tr("Realistic shadows")}, Viewport::savedRenderQuality(),
                [view](int i) { view->setRenderQuality(i); });
    form.note(tr("Ray tracing requires a compatible OpenGL driver; Studio is used when unavailable."));
    form.choice("view/background", tr("Scene background"), {tr("Theme"), tr("Studio gradient"), tr("White"), tr("Dark slate")}, Viewport::savedSceneBackground(),
                [view](int i) { view->setSceneBackground(i); });
    form.section(tr("Highlighting"));
    QCheckBox* fade = form.check("view/hoverFade", tr("Fade hover highlight"), true);
    QDoubleSpinBox* seconds = form.number("view/hoverFadeSeconds", tr("Fade after"), 5, 0.1, 60, 1, tr(" seconds"));
    auto apply = [view, fade, seconds] { view->setHoverFade(fade->isChecked(), seconds->value()); };
    QObject::connect(fade, &QCheckBox::toggled, view, apply);
    QObject::connect(seconds, &QDoubleSpinBox::valueChanged, view, apply);
    form.section(tr("Panels"));
    form.check("ui/browserAutoHide", tr("Auto-hide scene browser"), true, [this](bool on) {
      if (auto* overlay = static_cast<BrowserOverlay*>(services().window()->findChild<QFrame*>("browserOverlay"))) overlay->setAutoHide(on);
    });
    form.finish();
    return page;
  }

  // The unit is the document's (a units op, through the design so values typed without a unit follow) or, in viewer
  // mode, the session's; the precision is the user's.
  QWidget* unitsPage() {
    auto* page = new QWidget;
    preferences::Form form(page);
    form.section(tr("Lengths"), tr("The document unit is saved with the document; a file in viewer mode is only shown in it."));
    auto* unit = new QComboBox;
    unit->setObjectName("units/length");
    for (const QString& u : units::lengthUnits()) unit->addItem(QString("%1 (%2)").arg(units::unitName(u.toStdString()), units::symbol(units::Kind::Length, units::Display{u.toStdString()})), u);
    form.row(tr("Document unit"), unit);
    auto* decimals = new QSpinBox;
    decimals->setObjectName("units/decimals");
    decimals->setRange(0, 6);
    form.row(tr("Decimal places"), decimals);
    auto* fraction = new QCheckBox(tr("Fractions of an inch (1/64)"));
    fraction->setObjectName("units/fraction");
    form.row(QString(), fraction);
    form.section(tr("Angles"));
    auto* radians = new QCheckBox(tr("Angles in radians"));
    radians->setObjectName("units/angle");
    form.row(QString(), radians);
    form.finish();
    auto shown = [this, unit, decimals, fraction, radians] {
      const units::Display& d = units::current();
      const QSignalBlocker a(unit), b(decimals), c(fraction), e(radians);
      unit->setCurrentIndex(unit->findData(QString::fromStdString(d.length)));
      unit->setEnabled(services().document()->hasDocument);
      decimals->setValue(d.decimals);
      fraction->setChecked(d.fraction > 0);
      fraction->setEnabled(d.length == "in");
      radians->setChecked(d.radians);
    };
    shown();
    QObject::connect(units::notifier(), &units::Notifier::changed, page, shown);
    QObject::connect(services().document(), &AppDocument::changed, page, shown);
    QObject::connect(unit, &QComboBox::currentIndexChanged, page, [this, unit] {
      const std::string u = unit->currentData().toString().toStdString();
      AppDocument* doc = services().document();
      if (!doc->hasDocument) return;
      if (doc->browse) return units::setSessionUnit(u == units::documentUnit() ? std::string() : u);
      if (u != doc->scene.units) services().design()->applyOps({opad::json{{"op", "units"}, {"length", u}}}, tr("Change document units"));
    });
    QObject::connect(decimals, &QSpinBox::valueChanged, page, [](int n) { units::setPrecision(n, units::current().radians, units::current().fraction); });
    QObject::connect(fraction, &QCheckBox::toggled, page, [](bool on) { units::setPrecision(units::current().decimals, units::current().radians, on ? 64 : 0); });
    QObject::connect(radians, &QCheckBox::toggled, page, [](bool on) { units::setPrecision(units::current().decimals, on, units::current().fraction); });
    return page;
  }

  QWidget* sketch() {
    auto* page = new QWidget;
    preferences::Form form(page);
    form.section(tr("Snaps"), tr("Where a point placed in a sketch may jump to. Alt while placing turns them off for that point."));
    for (const auto& [key, label] : QList<QPair<QString, QString>>{{"endpoint", tr("Endpoints")}, {"midpoint", tr("Midpoints")}, {"center", tr("Centres")},
                                                                   {"quadrant", tr("Quadrants")}, {"intersection", tr("Intersections")}, {"nearest", tr("Nearest on curve")},
                                                                   {"grid", tr("Grid snapping")}, {"inference", tr("Automatic constraints")}})
      form.check("sketch/snap/" + key, label, true);
    form.section(tr("Directions"));
    if (QAction* polar = action("view.polarSnap")) form.option(polar, tr("Angle increments (Polar)"));
    form.number("sketch/angleStep", tr("Angle step"), 15, 1, 90, 1, QString::fromUtf8("°"));
    if (QAction* ortho = action("view.orthoSnap")) form.option(ortho, tr("Ortho: lines horizontal or vertical"));
    form.option(action("view.extensions"), tr("Extensions"));
    form.option(action("view.tracking"), tr("Tracking"));
    form.section(tr("Solver"));
    QLineEdit* tolerance = form.text("sketch/tolerance", tr("Solver tolerance"), "1e-8");
    QObject::connect(tolerance, &QLineEdit::editingFinished, tolerance, [tolerance] {  // the Form saved it: keep only a sane value
      bool ok = false;
      const double v = tolerance->text().toDouble(&ok);
      if (!ok || v < 1e-12 || v > 1e-2) {
        QSettings().setValue("sketch/tolerance", 1e-8);
        tolerance->setText("1e-8");
      }
    });
    form.integer("sketch/iterations", tr("Solver iterations"), 100, 1, 1000);
    form.finish();
    return page;
  }

  QWidget* grid() {
    auto* page = new QWidget;
    preferences::Form form(page);
    Viewport* view = services().viewport();
    form.section(tr("Grid"));
    form.option(action("view.grid"), tr("Show the grid"));
    form.option(action("view.gridSnap"), tr("Grid snapping"));
    auto* automatic = new QCheckBox(tr("Automatic spacing"));
    automatic->setObjectName("view/gridAutomatic");
    form.row(QString(), automatic);
    auto* spacing = new QDoubleSpinBox;
    spacing->setObjectName("view/gridSpacing");
    auto* extent = new QDoubleSpinBox;
    extent->setObjectName("view/gridExtent");
    form.row(tr("Spacing"), spacing);
    form.row(tr("Minimum extent"), extent);
    form.finish();
    // In the shown unit; saved in millimetres (Viewport::configureGrid).
    auto shown = [spacing, extent, automatic] {
      const QSignalBlocker a(spacing), b(extent), c(automatic);
      spacing->setRange(0, units::toDisplay(units::Kind::Length, 100000));
      spacing->setDecimals(units::decimalsFor(0.001));
      extent->setRange(units::toDisplay(units::Kind::Length, 1), units::toDisplay(units::Kind::Length, 1000000));
      extent->setDecimals(units::decimalsFor(1));
      for (auto* box : {spacing, extent}) box->setSuffix(' ' + units::symbol(units::Kind::Length));
      const double mm = QSettings().value("view/gridSpacing", 0).toDouble();
      spacing->setValue(units::toDisplay(units::Kind::Length, mm));
      extent->setValue(units::toDisplay(units::Kind::Length, QSettings().value("view/gridExtent", 100).toDouble()));
      automatic->setChecked(mm == 0);
      spacing->setEnabled(mm != 0);
    };
    shown();
    QObject::connect(units::notifier(), &units::Notifier::changed, page, shown);
    auto apply = [view, spacing, extent, automatic] {
      view->configureGrid(automatic->isChecked() ? 0 : units::fromDisplay(units::Kind::Length, spacing->value()), units::fromDisplay(units::Kind::Length, extent->value()));
    };
    QObject::connect(automatic, &QCheckBox::toggled, page, [spacing, apply](bool on) {
      spacing->setEnabled(!on);
      if (!on && spacing->value() == 0) spacing->setValue(units::toDisplay(units::Kind::Length, 1));
      apply();
    });
    QObject::connect(spacing, &QDoubleSpinBox::valueChanged, page, apply);
    QObject::connect(extent, &QDoubleSpinBox::valueChanged, page, apply);
    return page;
  }

  QWidget* files() {
    auto* page = new QWidget;
    preferences::Form form(page);
    form.section(tr("Opening files"));
    form.check("files/viewerMode", tr("Open other formats read-only (viewer mode)"), true, [this](bool on) { services().document()->viewerOpens = on; });
    form.note(tr("STEP, IGES, STL, 3MF, OBJ, DXF, SVG and the other formats open read-only and fast; Save makes them editable OPAD documents."));
    if (associations::supported()) form.button(tr("File types…"), [this] { FileTypesDialog(services().window()).exec(); }, "files/types");
    form.section(tr("Tessellation cache"), tr("Meshes of files opened before, so they open faster next time. Clearing it is safe."));
    QLabel* where = form.note(tr("Measuring…"));
    where->setObjectName("files/cacheInfo");
    auto measure = [this, where] {
      auto info = std::make_shared<opad::json>();
      QPointer<QLabel> label(where);
      services().jobs()->async(tr("Measuring the cache"), [info](Progress) { *info = opad::commands::run("cache", opad::json{{"action", "info"}}); }, [label, info](bool ok, const QString&) {
        if (!label || !ok) return;
        label->setText(tr("%1 in %2").arg(QLocale().formattedDataSize(info->value("bytes", 0LL)), QDir::toNativeSeparators(QString::fromStdString(info->value("dir", "")))));
      });
    };
    measure();
    form.button(tr("Clear the cache"), [this, measure] {
      services().jobs()->async(tr("Clearing the cache"), [](Progress) { opad::commands::run("cache", opad::json{{"action", "clear"}}); }, [this, measure](bool ok, const QString& error) {
        services().toast(ok ? tr("Cache cleared") : i18n::t(error));
        measure();
      });
    }, "files/clearCache");
    form.finish();
    return page;
  }

  QWidget* recovery() {
    auto* page = new QWidget;
    preferences::Form form(page);
    auto* manager = services().window()->findChild<RecoveryManager*>();
    auto timer = [manager] { if (manager) manager->configureTimer(); };
    form.section(tr("Autosave and recovery"));
    form.check("recovery/enabled", tr("Keep automatic recovery snapshots"), true, [timer](bool) { timer(); });
    form.integer("recovery/minutes", tr("Every"), 2, 1, 30, tr(" minutes"), [timer](int) { timer(); });
    form.note(tr("Recovery keeps a full base at each save and stores changed operations and new geometry between saves. Unchanged documents are skipped. Recovery preserves sketch drafts and opens with no active tool; unfinished features are cancelled. Your file is never overwritten."));
    if (manager) form.button(tr("Recover documents…"), [manager] { manager->offerRecovery(); }, "recovery/recover");
    form.finish();
    return page;
  }

  QWidget* versionControl() {
    auto* page = new QWidget;
    preferences::Form form(page);
    form.section(tr("Status bar"));
    form.integer("git/refreshSeconds", tr("Check the branch and file state every"), 5, 2, 600, tr(" s"));
    form.section(tr("Merging"), tr("OPAD files merge line by line with OPAD's record-aware merge driver. Add this line to the repository's .gitattributes:"));
    const QString attribute = "*.opad text eol=lf merge=opad";
    QLabel* line = form.note(attribute);
    line->setObjectName("vcs/attributes");
    line->setTextInteractionFlags(Qt::TextSelectableByMouse);
    line->setFont(QFont("Consolas"));
    form.button(tr("Copy"), [attribute] { QApplication::clipboard()->setText(attribute + '\n'); }, "vcs/copyAttributes");
    form.note(tr("Then register the driver as the README's Git section shows (git config merge.opad.driver ...)."));
    form.finish();
    return page;
  }

  QWidget* keyboard() {
    auto* page = new QWidget;
    preferences::Form form(page);
    form.section(tr("Navigation"), tr("Which mouse buttons orbit, pan and zoom, as in the program you are used to."));
    auto* group = new QButtonGroup(page);
    for (const char* id : {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"}) {
      QAction* a = action(id);
      if (!a) continue;
      auto* radio = new QRadioButton(QString(a->text()).remove('&'));
      radio->setObjectName(id);
      radio->setChecked(a->isChecked());
      group->addButton(radio);
      form.row(QString(), radio);
      QObject::connect(radio, &QRadioButton::toggled, a, [a](bool on) { if (on && !a->isChecked()) a->trigger(); });
      QObject::connect(a, &QAction::toggled, radio, [radio](bool on) { if (on) radio->setChecked(true); });
    }
    form.section(tr("Selection"));
    form.option(action("select.through"), tr("Select through objects"));
    form.section(tr("Keyboard"));
    form.button(tr("Keyboard shortcuts…"), [this] { action("tools.shortcuts")->trigger(); }, "keyboard/shortcuts");
    if (QAction* sheet = action("help.shortcuts")) form.button(tr("Shortcuts cheat sheet"), [sheet] { sheet->trigger(); }, "keyboard/sheet");
    form.finish();
    return page;
  }

  static QWidget* ai(QWidget* window) {
    auto* page = new QWidget;
    preferences::Form form(page);
    auto* agent = window->findChild<AgentBridge*>();
    form.section(tr("Local agent access"), tr("An AI client on this computer can read the open document through OPAD's MCP server and, when allowed, edit it live."));
    QCheckBox* enabled = form.check("agent/enabled", tr("Enable local agent access"), false);
    QCheckBox* edit = form.check("agent/edit", tr("Allow design edits"), false);
    auto apply = [agent, enabled, edit] { if (agent) agent->setAccess(enabled->isChecked(), edit->isChecked()); };
    QObject::connect(enabled, &QCheckBox::toggled, page, apply);
    QObject::connect(edit, &QCheckBox::toggled, page, apply);
    if (agent) {
      form.button(tr("Connection and client setup…"), [agent] { agent->settings(); }, "agent/setup");
      form.button(tr("Agent activity"), [agent] { agent->showActivity(); }, "agent/activity");
    }
    form.finish();
    return page;
  }
};

OPAD_AREA(PreferencesArea)
