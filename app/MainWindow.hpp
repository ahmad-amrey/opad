#pragma once
#include <QLabel>
#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QPointer>
#include <functional>
#include <optional>

#include "AnnotationEditor.hpp"
#include "AreaController.hpp"
#include "AppDocument.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "EmptyState.hpp"
#include "GuidedTool.hpp"
#include "Jobs.hpp"
#include "Notes.hpp"
#include "Panels.hpp"
#include "Ribbon.hpp"
#include "Toast.hpp"
#include "Viewport.hpp"
#include "BrowserOverlay.hpp"
class PathChip;
class RecoveryManager;
class StatusRow;
class AgentBridge;
class QMessageBox;
class QKeyEvent;
class KeyGuard;
class QToolButton;
class Toast;
template <class Tag>
struct MainWindowBench;

class MainWindow : public QMainWindow {
  Q_OBJECT
  template <class Tag>
  friend struct MainWindowBench;  // the benches of BenchRegistry.hpp (OPAD_BENCH), each in its own file
  friend class AreaServices;      // what feature areas reach of the window (AreaController.hpp, MainWindowAreas.cpp)
 public:
  MainWindow();
  ~MainWindow() override;
  void openPath(const QString& path, bool readOnly = false);  // readOnly: a .opad opens read-only (opad --read-only)
  void warmUpViewport() { m_viewport->warmUp(); }  // startup (StartUp.hpp): the viewer, then its first frame
  void drawFirstViewportFrame() { m_viewport->firstFrame(); }
  void setBenchSelect(bool on);  // --bench-select: select every root after loading, log, quit; nothing else shows on screen

 protected:
  void closeEvent(QCloseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void moveEvent(QMoveEvent* e) override;
  void changeEvent(QEvent* e) override;  // restored from the taskbar: the view draws its whole frame again

 private:
  QAction* addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn, bool checkable = false);
  QAction* addCommand(const CommandInfo& info, std::function<void()> fn);  // every command is made here (Commands.hpp)
  QAction* action(const QString& id) const;
  CommandContext commandContext() const;
  void updateCommands();  // the commands with an enabledWhen: after document, selection, workspace or sketch changes
  void buildActions();  // the area builders below, in command order
  void buildFileActions();        // MainWindowFile.cpp
  void buildViewActions();        // MainWindowView.cpp: view.*, panel toggles, workspaces
  void buildNavigationActions();  // MainWindowView.cpp: layout reset, theme, navigation presets, selection filters
  void buildInspectActions();     // MainWindowInspect.cpp
  void buildAnnotateActions();    // MainWindowAnnotate.cpp
  void buildEditActions();        // MainWindowEdit.cpp
  void buildToolsActions();       // MainWindowRibbon.cpp: tools.*, help.*
  // Feature areas (AreaController.hpp, MainWindowAreas.cpp): made after the built-in commands, hooks called from here.
  void createAreas();
  SelectionContext selectionContext() const;
  // An area takes this built-in command for the selection, or for a timeline marker (AreaController::command).
  bool areaCommand(const QString& id, const std::string& op = {});
  template <class Hook>
  void forEachArea(Hook hook) {  // the hooks that run once the window is built
    if (m_areasReady)
      for (AreaController* area : m_areas) hook(area);
  }
  void buildMenus();
  std::vector<std::string> shownBodies() const;  // visible bodies (and their components), inside the isolation
  void selectShown(bool invert);                 // Select all / Invert selection (UI-111)
  static QString renameBase(QString name);  // "Bolt 3" -> "Bolt": what renaming several objects numbers
  void noteCommand(const QString& id);  // a command ran: Repeat runs it again
  bool repeatOnEnter(const QKeyEvent* key);  // Enter in the view with nothing running: Repeat
  struct SnapToggle {  // a drafting switch of the status bar: its command, label (tr), icon, key and setting
    const char *id, *label, *icon, *key, *setting;
    bool defaultOn;
  };
  static const std::vector<SnapToggle>& snapToggles();
  void buildSnapCommands();  // their commands, before the ribbon (buildStatusBar adds their buttons)
  void buildRibbon();
  // The View tabs' dropdowns: Named views ▾, Rendering ▾ and Panels ▾ as commands whose menu the ribbon drops down (from the
  // palette or a key: at the pointer).
  void buildRibbonMenus();
  QAction* menuCommand(const CommandInfo& info, QMenu* menu);
  void ribbonTable(RibbonLayout& layout);    // Review, Design and the Sketch tabs: every command's slot (MainWindowRibbonTable.cpp)
  void draftingTable(RibbonLayout& layout);  // Drafting, after the areas' workspaces
  void sketchTab(RibbonLayout& layout, const QString& tab);  // the Sketch tab's groups (Design's and Drafting's)
  // A command of Design started from a workspace that does not show it (E in Review, the palette) switches to Design first.
  bool switchesToDesign(const QString& id) const;
  void followDrawing(bool drawing);  // a viewed drawing file goes into Drafting, and the next other document back out of it
  void buildDesignActions();  // design.* and sketch.* (MainWindow "design workspace")
  void drawOnDrawing();       // design.drawOnDrawing: a sketch on the drawing's plane
  void buildDesign();         // the controller, its floating panel and the wiring
  void updateDesignState();   // sketch mode <-> the Sketch tab first in Design, action enabling
  void updateSketchPrompt();  // the sketch's prompt: its steps and what the keys do now
  // "review", "design", "drafting" or an area's (RibbonLayout ids): swaps the ribbon tab set (same document, same
  // timeline); an id that is not there changes nothing. While a sketch is open Design stays (updateDesignState).
  void setWorkspace(const QString& id);
  QString workspaceKeys() const;  // "Ctrl+1 / 2": the keys that switch workspace now (status bar)
  bool setContextualTab(const QString& id, bool shown);  // a contextual tab (RibbonLayout::addContextualTab) shown or hidden
  QString workspaceId() const { return m_workspaceId; }  // the one shown
  void buildCentral();
  void buildDocks();
  void bindPanel(QAction* a, QDockWidget* dock);
  void resetLayout();
  void buildStatusBar();
  void setPrompt(const QString& text);  // what the running tool waits for (a status-bar message goes first while it lasts)
  QString m_promptText;
  void buildUnitsButton();
  void toggleMenu(QToolButton* button, const QString& id);  // right-click on a drafting toggle (UI-112)
  class CoordinateReadout* m_readout = nullptr;  // the cursor's X/Y/Z in the status bar
  void setDocumentUnit(const std::string& unit);
  void applyTheme(bool dark);
  void refreshIcons();
  void updateTitle();
  QString newerRecords() const;  // what of the file only a newer build reads (UI-65): one sentence, empty when nothing
  void updateChips();
  void saveLastView();
  QString m_viewPath;
  void guarded(const std::function<void()>& fn);
  // A precondition not met (opad::UserHint, UI-109): a toast instead of a message box. pick: the command that raised it
  // waits for the selection it asks for and runs again once there is one (resumePendingPick).
  void hint(const QString& text, bool pick);
  void resumePendingPick();
  void cancelPendingPick();
  void resultToast(const QString& text, const QString& folder = QString());  // a result; folder: an Open folder action
  void failedToast(const QString& text);  // a change that failed, the document unchanged (DesignController::failed)
  QString m_runningCommand, m_pendingPick;  // the command whose function runs now; the one waiting for a selection
  QPointer<Toast> m_pendingToast;
  // Before the document goes: unfinished work, then unsaved changes. resume: what asked, run again once a sketch the
  // user chose to finish is in the document (finishing is a design job), so Open or New goes on by itself (UI-111).
  bool maybeSave(std::function<void()> resume = {});
  bool leaveSketch(std::function<void()> resume);  // an unfinished sketch: finish (then resume), discard, or stay
  // The unsaved-changes question, built but not shown (maybeSave runs it; a bench presses its buttons): Save, Discard,
  // Cancel and, while the file is on disk, Review changes… (UI-59: Compare, the saved file against this session).
  QMessageBox* unsavedPrompt();
  void showDocument(bool has);
  void showCentral();  // the start page, the viewport (also while loading) or an area's page; the browser floats over it
  // title: the job's and the shade's ("Opening box.step"); done: the completion toast, %1 = the bodies loaded.
  void beginLoad(std::function<void()> after, const QString& title = QString(), const QString& done = QString());
  void setLoadPhase(const QString& phase, int pct, int overall = -1);  // overall < 0: from pct, in the display part
  QString meshPhase() const;
  void restoreLastView();  // the camera this file was last seen with (not in benches)
  // An edit asked for while the bodies of a load still stream in (UI-40): it runs once they are all shown.
  void deferEdit(QAction* a);
  QPointer<QAction> m_afterStream;
  QLabel* m_activityDot = nullptr;  // Background jobs (Jobs.hpp): shown while any has run 0.5 s, its tooltip names them
  void scheduleSelectionSync();
  // Volume, area and the tight box (TODO 10 B10) of a body or component, measured on a worker, then shown.
  void showNodeGeometry(const std::string& id, const QString& title, const QString& subtitle, const QString& nid);
  // A face's, edge's or vertex's details (inspect_ref: area, the adjacent faces, the walk over the body), measured on a
  // worker after the panel showed what is known at once (UI-51).
  void showRefGeometry(const opad::Ref& ref, const QString& subtitle, const QString& nid);
  void runBench();
  bool benchTodo5();
  bool benchTodo9();
  bool benchDrawingImport();
  bool benchAnnotateLarge();  // OPAD_BENCH_ANNOTATE: the note / drawing editors on the loaded file's heaviest body
  bool benchShortcuts();
  bool benchViewer();  // OPAD_BENCH_VIEWER

  void onViewportSelection();
  void onBrowserSelection(const std::vector<std::string>& ids);
  void showProperties(const std::vector<opad::Ref>& refs);  // fills the Properties panel; it is opened only from the context menu
  void selectionMoved(const std::vector<opad::Ref>& refs);
  void openPanel(ToolPanel* panel);  // places it over the viewport; replaces the other unpinned panels
  bool closeTopPanel();              // Esc: hides one unpinned panel
  void bindPanel(QAction* a, ToolPanel* panel);
  void showContextMenu(const QPoint& globalPos, std::vector<std::string> ids, bool documentRow = false);
  // The context menu by what it is about (UI-100): picked faces, edges or vertices, bodies, components, sketches, nothing;
  // the areas add theirs (smart selection the history of the picks after the title, "contextTitle"). Benches fill one.
  void buildContextMenu(QMenu& menu, const std::vector<std::string>& ids, bool documentRow = false);  // documentRow: the browser's document row
  QAction* repeatAction();  // edit.repeat worded for the last tool started ("Repeat Fillet"); null when there is none to offer
  bool repeatable(const QString& id) const;  // a tool worth repeating: a feature, a sketch tool, a measurement, a note
  QString m_lastCommand;
  void timelineMenu(const std::string& opId, const QPoint& globalPos);
  void buildTimelineMenu(QMenu& menu, const std::string& opId);  // what timelineMenu shows (benches fill one without showing it)
  // Guided tools: the tool is started first and asks for its picks one step at a time (see GuidedTool.hpp).
  void toggleTool(const QString& id);  // distance, angle, radius, bbox, note, sectionface
  void startTool(const QString& id);
  void cancelTool(bool restoreFilter = true);  // false: another tool starts at once (it keeps the filter to set back)
  void toolEscape();  // Esc: result -> measure again; otherwise one step back; with no pick left, leave the tool
  void toolPicksChanged(const std::vector<opad::Ref>& refs, bool fromClick);
  void runToolMeasure();
  void refreshToolUi();
  QList<ToolStep> toolSteps() const;
  QString refLabel(const opad::Ref& r) const;
  bool toolMeasures() const {
    return m_tool.id == "distance" || m_tool.id == "angle" || m_tool.id == "radius" || m_tool.id == "bbox" || m_tool.id == "area" || m_tool.id == "length";
  }
  // A result's name ("Distance", "Centre to centre", "Area") and its rows as the tool panel lists them, in the shown units:
  // the value, components, what was recognised, perimeters, the measured points' XYZ (UI-144).
  QString measureTitle(const opad::json& result) const;
  QList<QPair<QString, QString>> measureRows(const opad::json& result) const;
  void copyMeasurement(const opad::json& result);
  void updateUndoActions();
  QMenu* historyMenu(bool undo);  // the steps under the quick-access Undo ▾ / Redo ▾
  void sectionFromFace(const opad::Ref& face);  // "Pick face": a planar face sets the section plane
  void pinMeasurement(opad::json result = {});  // the current result when none is given
  bool measuredExploded() const;  // the last measurement was taken in an exploded view: it is not pinned
  void clearMeasurement();
  void startAnnotation(bool drawing);  // Note (false) or Hand drawing (true); the same command again closes it
  void syncAnnotationActions();
  void resolveCurrentAnnotation();
  void restyleAnnotation(const std::string& opId, const std::string& style);  // an edit op on the note
  // A card clicked in Annotations lights up what its note is pinned to, as the note editor shows a target (help audit
  // P9.4); a click in the view, another selection, a change of the document, Esc, a note being written or the panel
  // closing puts it out.
  void showAnnotationCardTarget(const std::vector<opad::Ref>& anchors);  // a note's anchor, every pick of a pinned measurement
  void clearAnnotationCardTarget();
  void exportDialog(std::vector<std::string> ids = {});
  void runExport(const opad::json& args, const QString& out);  // ExportDialog.cpp: on a worker (ExportJob.hpp), the result in m_lastExport
  void drawingToSketch();
  void browseInstances(const std::string& id);
  void screenshot();
  void saveNamedView();  // asks for the name and whether to keep what is hidden and shown (remembered)
  // A view op of the current camera; `visibility`: also the nodes hidden now (display.hidden), shown again when it is
  // recalled, everything else shown.
  void saveNamedView(const QString& name, bool visibility);
  void restoreNamedView(const std::string& id);
  void recallNamedView(int n);  // Named view 1-9 (view.named<n>): the Named views list's n-th entry, as a click on it
  void rebuildViewsMenu();
  void selectOpTargets(const std::string& opId);
  void deleteOp(const std::string& opId);
  void deleteOps(const std::vector<std::string>& opIds);  // several tombstones as one step (UI-02)
  void hideOthers(const std::vector<std::string>& keep);  // one step hiding the fewest nodes (Scene::others_to_hide)
  void restoreOp(const std::string& opId);
  void deleteCurrent();
  // Del on objects (UI-04): what the selection covers and nothing more (smart::routeDelete), one undo step, a toast with
  // Undo instead of a question. Faces and edges never come here: they go to smart selection (SmartSelect).
  void deleteNodes(const std::vector<std::string>& ids);
  Toast* undoToast(const QString& text, int ms = 8000);  // a result toast whose Undo takes back that step (not one made after it)
  void writeSelectionFile();
  void unpublishSelection();
  void positionOverlays();
  void setLoading(bool on);  // shade + spinner over the workspace, input blocked, until the load job ends
  void addRecent(const QString& path);
  void removeRecent(const QString& path);
  QMenu* recentMenu(const QString& path, QWidget* parent);  // a recent file's context menu (File > Recent, the start page)
  void tell(const QString& text);  // a result: a toast over the view, the status bar while no document shows
  QStringList recent() const;
  void rebuildRecentMenu();
  // New from a template (UI-113, EmptyState.hpp templates): a built-in one ("builtin:in") or an .opad of the templates folder,
  // opened as an untitled copy; saveAsTemplate puts a copy of this document there.
  void newFromTemplate(const QString& id);
  void saveAsTemplate();                    // asks for a name, then saveTemplate
  void saveTemplate(const QString& path);  // on workers; a toast says when it is there
  void rebuildTemplateMenu();              // File › New from template, as the menu opens
  QMenu* m_templateMenu = nullptr;
  std::vector<std::string> currentNodeIds() const;
  QColor nodeColour(const std::string& id) const;  // its own colour, or the default body grey
  // Viewer mode (a file other than .opad shown read-only): edits ask to save it as an OPAD document first.
  static bool isEditAction(const QString& id);
  static QString fileFilter(bool withOpad);                 // "*.step *.stl ..." for the file dialogs
  // Import…'s file: mode -1 asks whether it comes in linked or as a copy when linking suits it (assets::askImport), 0 a
  // copy, 1 linked (a drawing is placed first either way, DrawingPlacer); under the selected component if the user says so.
  void importPath(const QString& path, int mode);
  bool requireEditable(std::function<void()> resume = {});  // true when the document can be edited
  void saveViewerAs(std::function<void()> then = {});       // Save as OPAD: made editable in place, then written
  void makeEditable(const QString& savePath, std::function<void()> then = {});
  void saveReadOnlyCopy(std::function<void()> then = {});  // read-only .opad: Save a copy… (the file dialog), then saveCopy
  void saveCopy(const QString& path, std::function<void()> then = {});  // written on a worker; the copy is edited from then on
  void updateViewerCard();

  AppDocument* m_doc = nullptr;
  AreaServices m_areaServices{this};
  std::vector<AreaController*> m_areas;  // owned; deleted first in ~MainWindow
  qsizetype m_ownCommands = 0;           // the window's own commands, first in m_actions; the areas' follow
  bool m_areasReady = false;
  unsigned long long m_areaGeneration = 0;  // the document generation the areas last saw (documentChanged's "replaced")
  RecoveryManager* m_recovery = nullptr;
  AgentBridge* m_agent = nullptr;
  bool m_closePending = false, m_recoveryClosed = false;
  DesignController* m_design = nullptr;
  ToolPanel* m_featurePanel = nullptr;
  QString m_workspaceBeforeSketch;    // where a sketch was started from: Finish goes back there
  QString m_sketchTab;                // the open sketch's contextual tab: "design.sketch", or "drafting.sketch" in Drafting
  QString m_workspaceBeforeDrafting;  // where a viewed drawing came from into Drafting (followDrawing)
  // How the switch being made came about. The app's own (a drawing viewed into Drafting and out again, a sketch shown in
  // Design and back) is never saved as the workspace the next start opens in; a Design command's keeps the selection
  // filter and the faces or edges picked for it (Fillet from Review), where a switch by hand goes back to bodies.
  bool m_automaticSwitch = false, m_commandSwitch = false;
  QStringList m_workspaceIds;            // by RibbonBar index
  QString m_workspaceId;  // the one shown (as the areas were told)
  class QActionGroup* m_workspaceGroup = nullptr;  // the workspace.* commands: one checked
  QMenu* m_viewMenu = nullptr;
  QMenu* m_sketchMenu = nullptr;     // the menu bar's Sketch menu: shown while a sketch is open
  QMenu* m_snappingMenu = nullptr;   // View > Snapping: the status bar's snapping switches (filled with them)
  QStackedWidget* m_stack = nullptr;
  QWidget* m_centralPage = nullptr;  // an area's page in the viewport's place (AreaServices::setCentralPage)
  bool m_loadShown = false;
  EmptyState* m_empty = nullptr;
  Viewport* m_viewport = nullptr;
  ViewportChips* m_chips = nullptr;
  ToastStack* m_toasts = nullptr;  // results and warnings at the bottom centre of the viewport (Toast.hpp)
  QWidget* m_homeBtn = nullptr;  // floating Home button above the view cube
  QToolButton* m_rollLeft = nullptr;   // 90 degree turns about the view axis, either side of the cube
  QToolButton* m_rollRight = nullptr;
  QToolButton* m_alignPlane = nullptr;
  struct Tool {
    QString id, title, icon;
    int steps = 0;  // 0: open (Area): measured after every pick, as many as the user makes
  };
  Tool m_tool;  // id empty: no tool is running
  // The filter a tool switched to for its picks (faces for Distance, Angle and Radius from the Bodies filter, for the
  // section's pick; edges where there are no faces) and the one it switched from: set back when the tool ends, unless
  // another filter was chosen meanwhile. A tool started straight from another keeps the first one's.
  struct ToolFilter {
    Viewport::SelFilter before, set;
    bool autoEdges = false;  // m_autoEdges as it was (the select actions clear it)
  };
  std::optional<ToolFilter> m_toolFilter;
  std::vector<opad::Ref> m_toolPicks;
  std::vector<std::pair<bool, opad::Vec3>> m_toolPoints;  // where each pick was clicked (false: picked some other way)
  int m_toolRun = 0;  // bumps whenever the picks change: a measure result for an older run is dropped
  QString m_toolHover;
  QString m_toolError;  // why the last pick could not be measured, shown in the tool panel until the next pick (UI-50)
  int m_distanceMode = 0;  // Distance: 0 minimum, 1 centre to centre, 2 maximum (setting measure/distanceMode, UI-144)
  // Measured points and Δ in 0 world axes, 1 the axes of the component the first pick lies in (setting measure/frame).
  int m_measureFrame = 0;
  std::string measureComponent(const std::string& body) const;  // the component a pick lies in, empty at the root
  bool measureFrame(const std::string& body, gp_Trsf& toWorld) const;  // false: world axes
  struct MeasureRecord { opad::json result; bool pinned = false; };
  std::vector<MeasureRecord> m_measureHistory;  // this document's results, newest first (UI-144)
  void refreshMeasureHistory();
  PromptBar* m_prompt = nullptr;
  ToolStepsPanel* m_toolSteps = nullptr;
  ToolPanel* m_toolPanel = nullptr;
  // Design checks in the same panel (TODO 10 B13 print check, B17 interference).
  class QStackedWidget* m_toolStack = nullptr;
  class CheckPanel* m_checks = nullptr;
  Job* m_checkJob = nullptr;
  Job* m_overlapJob = nullptr;
  bool m_checkGap = false;  // a clearance finding's gap is drawn (the measurement dimension): it goes with the check's colours
  std::vector<std::string> m_checkSelect;  // what the check looks at: the selection when it started, else everything
  void startCheck(bool print);
  void runCheck();
  void recheck();  // the document changed while the check's panel is open: its findings and colours follow (help audit P8)
  void showFinding(const opad::json& finding);
  void showPrintTints(const opad::json& except);  // the print check's findings coloured on the model, but `except` (the one shown)
  void endCheck();
  void clearCheckOverlays();  // the check's colours, overlap and gap off the model, the overlap job given up
  void keepCheck();  // Keep as check: the checked bodies and clearance as an Interference check feature, in Design
  LoadShade* m_loadShade = nullptr;
  KeyGuard* m_keyGuard = nullptr;
  bool m_timelineHiddenByViewer = false;
  bool m_autoTwoD = false, m_settingTwoD = false;  // 2D mode turned on for a viewed drawing (any 2D mode ends with its document)
  bool m_autoEdges = false;      // the Edge filter set for a drawing (and set back to Bodies after it)
  bool viewingDrawing() const;   // a drawing (DXF, DWG, SVG) in viewer mode: every body is 2D
  void setAutoTwoD(bool on);     // 2D mode on or off for that, not by hand
  RibbonBar* m_ribbon = nullptr;
  BrowserPanel* m_browser = nullptr;
  PropertiesPanel* m_props = nullptr;
  AnnotationsPanel* m_annotations = nullptr;
  QPointer<AnnotationEditor> m_annotationEditor;
  ToolPanel* m_annotationPanel = nullptr;  // the editor's: type, pen, text
  bool m_cardTarget = false;  // the viewport's annotation target is an Annotations card's (not the editor's)
  NoteCards* m_noteCards = nullptr;  // one card beside every open note, over the viewport
  SectionPanel* m_section = nullptr;
  ToolPanel* m_propsPanel = nullptr;  // floating tool panels over the viewport (no fixed right dock)
  ToolPanel* m_annotationsPanel = nullptr;
  ToolPanel* m_sectionPanel = nullptr;
  QList<ToolPanel*> m_panels;
  std::vector<opad::Ref> m_selRefs;   // the current selection as last reported by the viewport or the browser
  std::vector<std::string> m_selRows;  // areas' browser rows selected (provided folders): not nodes, so only in SelectionContext::ids
  TimelineWidget* m_timeline = nullptr;
  QMenu* m_viewsMenu = nullptr;
  QMenu* m_recentMenu = nullptr;
  StatusRow* m_statusRow = nullptr;  // the path and its chips at the leading end, never collapsed (UI-08)
  PathChip* m_statusPath = nullptr;
  QLabel* m_statusPrompt = nullptr;  // the prompt and status-bar messages (setPrompt)
  QLabel* m_statusHover = nullptr;   // what is under the mouse (Viewport::hoverChanged)
  QLabel* m_statusSel = nullptr;
  QToolButton* m_statusUnits = nullptr;  // the shown length unit (UI-123): a click offers the document's
  QList<QAction*> m_actions;
  CommandRegistry m_commands;  // the record of every action in m_actions, same order
  QAction* m_pinAction = nullptr;
  QAction* m_darkAction = nullptr;
  ProgressStrip* m_progress = nullptr;
  JobRunner* m_jobs = nullptr;      // every long operation runs through this (see Jobs.hpp)
  Job* m_loadJob = nullptr;         // open/import: document worker + tessellation, one job
  Job* m_displayJob = nullptr;      // bodies shown after a load (unhide, un-isolate): same status-bar progress
  class DrawingPlacer* m_drawingPlacer = nullptr;
  // A drawing imported onto the selected planar face, or onto a picked plane and moved there first (TODO 10 A12); `link`:
  // as a linked asset (its placement recorded with it, kept by every sync).
  void importDrawing(const QString& path, const QString& parent, bool link = false);
  // KiCad boards (KicadBoards.cpp): after a board loads with models of KiCad's library missing, offer to download them
  // (setting kicad/download: ask, always, never; once per board and session), then read a viewed board again.
  void offerKicadModels();
  // Linked files (AssetLinks.cpp): after an open, one question before reading those outside the document's project
  // (read once, trust their folders in the settings, or not now). Whether it asked. Never after an import (trustAfterLoad):
  // the answer to the open stands for the session.
  bool offerAssetTrust();
  bool trustAfterLoad() const;
  QStringList m_kicadOffered;
  int m_displayTotal = 0;
  Job* m_selFileJob = nullptr;      // selection.json writer
  bool m_selPublishing = false;     // agent access is on: the selection is published (UI-06)
  Job* m_measureJob = nullptr;      // the guided tool's measurement; cancelled as soon as the picks move on
  Job* m_propsJob = nullptr;        // geometry for the properties panel
  Job* m_sectionJob = nullptr;      // the section plane from a picked face (UI-51: inspected on a worker)
  bool m_loadDocDone = false;
  int m_meshTotal = 0, m_meshRemaining = 0;
  std::function<void()> m_afterLoad;
  QString m_loadDone;  // beginLoad's done text
  QTimer m_selFileTimer;
  bool m_benchSelect = false;
  BrowserOverlay* m_browserOverlay = nullptr;
  QDockWidget* m_timelineDock = nullptr;
  opad::json m_lastMeasure;
  opad::json m_lastExport;  // the last export's result, or {"error"}
  QSettings m_settings;
  bool m_syncing = false;
};
