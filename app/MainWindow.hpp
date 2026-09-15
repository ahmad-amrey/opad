#pragma once
#include <QLabel>
#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <functional>

#include "AppDocument.hpp"
#include "EmptyState.hpp"
#include "Jobs.hpp"
#include "Panels.hpp"
#include "Ribbon.hpp"
#include "Viewport.hpp"

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow();
  void openPath(const QString& path);
  void warmUpViewport() { m_viewport->warmUp(); }
  void setBenchSelect(bool on) { m_benchSelect = on; }  // --bench-select: select every root after loading, log, quit

 protected:
  void closeEvent(QCloseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;

 private:
  QAction* addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn, bool checkable = false);
  QAction* action(const QString& id) const;
  void buildActions();
  void buildMenus();
  void buildRibbon();
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
  void guarded(const std::function<void()>& fn);
  bool maybeSave();
  void showDocument(bool has);
  void beginLoad(std::function<void()> after);
  void setLoadPhase(const QString& phase, int pct);
  QString meshPhase() const;
  int overallPercent(const QString& phase, int pct) const;
  void scheduleSelectionSync();
  void showComponentBbox(const std::string& id, const QString& title, const QString& subtitle, const QString& nid, opad::json props);
  void runBench();

  void onViewportSelection();
  void onBrowserSelection(const std::vector<std::string>& ids);
  void showProperties(const std::vector<opad::Ref>& refs);
  void showContextMenu(const QPoint& globalPos, std::vector<std::string> ids);
  void timelineMenu(const std::string& opId, const QPoint& globalPos);
  void measure(const QString& kind);
  void pinMeasurement();
  void clearMeasurement();
  void addAnnotation();
  void resolveCurrentAnnotation();
  void exportDialog();
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
  void addRecent(const QString& path);
  QStringList recent() const;
  void rebuildRecentMenu();
  std::vector<std::string> currentNodeIds() const;

  AppDocument* m_doc = nullptr;
  QStackedWidget* m_stack = nullptr;
  EmptyState* m_empty = nullptr;
  Viewport* m_viewport = nullptr;
  ViewportChips* m_chips = nullptr;
  MeasureCard* m_measureCard = nullptr;
  RibbonBar* m_ribbon = nullptr;
  BrowserPanel* m_browser = nullptr;
  QTabWidget* m_inspector = nullptr;
  PropertiesPanel* m_props = nullptr;
  AnnotationsPanel* m_annotations = nullptr;
  SectionPanel* m_section = nullptr;
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
  Job* m_selFileJob = nullptr;      // selection.json writer
  Job* m_propsJob = nullptr;        // component bbox for the properties panel
  bool m_loadDocDone = false;
  int m_meshTotal = 0, m_meshRemaining = 0;
  std::function<void()> m_afterLoad;
  QTimer m_selFileTimer;
  bool m_benchSelect = false;
  QDockWidget* m_browserDock = nullptr;
  QDockWidget* m_inspectorDock = nullptr;
  QDockWidget* m_timelineDock = nullptr;
  opad::json m_lastMeasure;
  QStringList m_lastMeasureTargets;
  QSettings m_settings;
  QTimer m_gitTimer;
  bool m_syncing = false;
};
