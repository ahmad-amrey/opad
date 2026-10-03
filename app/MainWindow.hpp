#pragma once
#include <QLabel>
#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QPointer>
#include <functional>

#include "AnnotationEditor.hpp"
#include "AppDocument.hpp"
#include "DesignController.hpp"
#include "EmptyState.hpp"
#include "GuidedTool.hpp"
#include "Jobs.hpp"
#include "Notes.hpp"
#include "Panels.hpp"
#include "Ribbon.hpp"
#include "Viewport.hpp"
#include "BrowserOverlay.hpp"
class RecoveryManager;
class AgentBridge;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow();
  ~MainWindow() override;
  void openPath(const QString& path);
  void warmUpViewport() { m_viewport->warmUp(); }
  void setBenchSelect(bool on);  // --bench-select: select every root after loading, log, quit; nothing else shows on screen

 protected:
  void closeEvent(QCloseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void moveEvent(QMoveEvent* e) override;

 private:
  QAction* addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn, bool checkable = false);
  QAction* action(const QString& id) const;
  void buildActions();
  void buildMenus();
  void selectGeometry();
  void buildRibbon();
  void buildDesignActions();  // design.* and sketch.* (MainWindow "design workspace")
  void buildDesign();         // the controller, its floating panel and the wiring
  void updateDesignState();   // sketch mode <-> ribbon tab set, action enabling
  void setWorkspace(int index);  // 0 Review, 1 Design: swaps the ribbon tab set (same document, same timeline)
  void buildCentral();
  void buildDocks();
  void bindPanel(QAction* a, QDockWidget* dock);
  void resetLayout();
  void buildStatusBar();
  void applyTheme(bool dark);
  void refreshIcons();
  void updateTitle();
  void updateChips();
  void refreshGit();
  void showOpGitLog(const std::string& opId,const QString& path);
  void saveLastView();
  QString m_viewPath;
  void guarded(const std::function<void()>& fn);
  bool maybeSave();
  void showDocument(bool has);
  void beginLoad(std::function<void()> after);
  void setLoadPhase(const QString& phase, int pct);
  QString meshPhase() const;
  int overallPercent(const QString& phase, int pct) const;
  void scheduleSelectionSync();
  // Volume, area and the tight box (TODO 10 B10) of a body or component, measured on a worker, then shown.
  void showNodeGeometry(const std::string& id, const QString& title, const QString& subtitle, const QString& nid);
  void runBench();
  bool benchTodo5();
  bool benchTodo9();
  bool benchDrawingImport();
  bool benchAnnotateLarge();  // OPAD_BENCH_ANNOTATE: the note / drawing editors on the loaded file's heaviest body
  bool benchLargeSketch();
  bool benchShortcuts();
  bool benchViewer();  // OPAD_BENCH_VIEWER

  void onViewportSelection();
  void onBrowserSelection(const std::vector<std::string>& ids);
  void showProperties(const std::vector<opad::Ref>& refs);  // fills the Properties panel; it is opened only from the context menu
  void selectionMoved(const std::vector<opad::Ref>& refs);
  void openPanel(ToolPanel* panel);  // places it over the viewport; replaces the other unpinned panels
  bool closeTopPanel();              // Esc: hides one unpinned panel
  void bindPanel(QAction* a, ToolPanel* panel);
  void showContextMenu(const QPoint& globalPos, std::vector<std::string> ids);
  void timelineMenu(const std::string& opId, const QPoint& globalPos);
  // Guided tools: the tool is started first and asks for its picks one step at a time (see GuidedTool.hpp).
  void toggleTool(const QString& id);  // distance, angle, radius, bbox, note, sectionface
  void startTool(const QString& id);
  void cancelTool();
  void toolEscape();  // Esc: result -> measure again; otherwise one step back; with no pick left, leave the tool
  void toolPicksChanged(const std::vector<opad::Ref>& refs, bool fromClick);
  void runToolMeasure();
  void refreshToolUi();
  QList<ToolStep> toolSteps() const;
  QString refLabel(const opad::Ref& r) const;
  bool toolMeasures() const { return m_tool.id == "distance" || m_tool.id == "angle" || m_tool.id == "radius" || m_tool.id == "bbox"; }
  void updateUndoActions();
  void sectionFromFace(const opad::Ref& face);  // "Pick face": a planar face sets the section plane
  void pinMeasurement();
  void clearMeasurement();
  void startAnnotation(bool drawing);  // Note (false) or Hand drawing (true); the same command again closes it
  void syncAnnotationActions();
  void resolveCurrentAnnotation();
  void restyleAnnotation(const std::string& opId, const std::string& style);  // an edit op on the note
  void exportDialog(std::vector<std::string> ids = {});
  void drawingToSketch();
  void browseInstances(const std::string& id);
  void screenshot();
  void saveNamedView();
  void restoreNamedView(const std::string& id);
  void rebuildViewsMenu();
  void selectOpTargets(const std::string& opId);
  void deleteOp(const std::string& opId);
  void restoreOp(const std::string& opId);
  void deleteCurrent();
  void writeSelectionFile();
  void positionOverlays();
  void setLoading(bool on);  // shade + spinner over the workspace, input blocked, until the load job ends
  void addRecent(const QString& path);
  QStringList recent() const;
  void rebuildRecentMenu();
  std::vector<std::string> currentNodeIds() const;
  QColor nodeColour(const std::string& id) const;  // its own colour, or the default body grey
  // Viewer mode (a file other than .opad shown read-only): edits ask to save it as an OPAD document first.
  static bool isEditAction(const QString& id);
  static QString fileFilter(bool withOpad);                 // "*.step *.stl ..." for the file dialogs
  bool requireEditable(std::function<void()> resume = {});  // true when the document can be edited
  void saveViewerAs(std::function<void()> then = {});       // Save as OPAD: made editable in place, then written
  void makeEditable(const QString& savePath, std::function<void()> then = {});
  void updateViewerCard();

  AppDocument* m_doc = nullptr;
  RecoveryManager* m_recovery = nullptr;
  AgentBridge* m_agent = nullptr;
  bool m_closePending = false, m_recoveryClosed = false;
  DesignController* m_design = nullptr;
  ToolPanel* m_featurePanel = nullptr;
  int m_sketchWorkspace = -1, m_workspaceBeforeSketch = 0;
  QStackedWidget* m_stack = nullptr;
  EmptyState* m_empty = nullptr;
  Viewport* m_viewport = nullptr;
  ViewportChips* m_chips = nullptr;
  QWidget* m_homeBtn = nullptr;  // floating Home button above the view cube
  QToolButton* m_rollLeft = nullptr;   // 90 degree turns about the view axis, either side of the cube
  QToolButton* m_rollRight = nullptr;
  QToolButton* m_alignPlane = nullptr;
  struct Tool {
    QString id, title, icon;
    int steps = 0;
  };
  Tool m_tool;  // id empty: no tool is running
  std::vector<opad::Ref> m_toolPicks;
  std::vector<std::pair<bool, opad::Vec3>> m_toolPoints;  // where each pick was clicked (false: picked some other way)
  int m_toolRun = 0;  // bumps whenever the picks change: a measure result for an older run is dropped
  QString m_toolHover;
  PromptBar* m_prompt = nullptr;
  ToolStepsPanel* m_toolSteps = nullptr;
  ToolPanel* m_toolPanel = nullptr;
  // Design checks in the same panel (TODO 10 B13 print check, B17 interference).
  class QStackedWidget* m_toolStack = nullptr;
  class CheckPanel* m_checks = nullptr;
  Job* m_checkJob = nullptr;
  Job* m_overlapJob = nullptr;
  std::vector<std::string> m_checkSelect;  // what the check looks at: the selection when it started, else everything
  void startCheck(bool print);
  void runCheck();
  void showFinding(const opad::json& finding);
  void endCheck();
  LoadShade* m_loadShade = nullptr;
  bool m_timelineHiddenByViewer = false;
  bool m_autoTwoD = false, m_settingTwoD = false;  // 2D mode turned on for a viewed drawing (and turned off after it)
  RibbonBar* m_ribbon = nullptr;
  BrowserPanel* m_browser = nullptr;
  PropertiesPanel* m_props = nullptr;
  AnnotationsPanel* m_annotations = nullptr;
  QPointer<AnnotationEditor> m_annotationEditor;
  ToolPanel* m_annotationPanel = nullptr;  // the editor's: type, pen, text
  NoteCards* m_noteCards = nullptr;  // one card beside every open note, over the viewport
  SectionPanel* m_section = nullptr;
  ToolPanel* m_propsPanel = nullptr;  // floating tool panels over the viewport (no fixed right dock)
  ToolPanel* m_annotationsPanel = nullptr;
  ToolPanel* m_sectionPanel = nullptr;
  QList<ToolPanel*> m_panels;
  std::vector<opad::Ref> m_selRefs;   // the current selection as last reported by the viewport or the browser
  TimelineWidget* m_timeline = nullptr;
  QMenu* m_viewsMenu = nullptr;
  QMenu* m_recentMenu = nullptr;
  QLabel* m_statusPath = nullptr;
  QLabel* m_statusGitIcon = nullptr;
  QLabel* m_statusGit = nullptr;
  QLabel* m_statusHover = nullptr;
  QLabel* m_statusSel = nullptr;
  QLabel* m_statusUnits = nullptr;
  QList<QAction*> m_actions;
  QAction* m_pinAction = nullptr;
  QAction* m_darkAction = nullptr;
  ProgressStrip* m_progress = nullptr;
  JobRunner* m_jobs = nullptr;      // every long operation runs through this (see Jobs.hpp)
  Job* m_loadJob = nullptr;         // open/import: document worker + tessellation, one job
  Job* m_displayJob = nullptr;      // bodies shown after a load (unhide, un-isolate): same status-bar progress
  class DrawingPlacer* m_drawingPlacer = nullptr;
  // A drawing imported onto the selected planar face, or onto a picked plane and moved there first (TODO 10 A12).
  void importDrawing(const QString& path, const QString& parent);
  int m_displayTotal = 0;
  Job* m_selFileJob = nullptr;      // selection.json writer
  Job* m_measureJob = nullptr;      // the guided tool's measurement; cancelled as soon as the picks move on
  Job* m_propsJob = nullptr;        // geometry for the properties panel
  bool m_loadDocDone = false;
  int m_meshTotal = 0, m_meshRemaining = 0;
  std::function<void()> m_afterLoad;
  QTimer m_selFileTimer;
  bool m_benchSelect = false;
  BrowserOverlay* m_browserOverlay = nullptr;
  QDockWidget* m_timelineDock = nullptr;
  opad::json m_lastMeasure;
  QSettings m_settings;
  QTimer m_gitTimer;
  bool m_syncing = false;
};
