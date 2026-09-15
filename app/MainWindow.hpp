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
#include "Panels.hpp"
#include "Ribbon.hpp"
#include "Viewport.hpp"

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow();
  void openPath(const QString& path);

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
  QDockWidget* m_browserDock = nullptr;
  QDockWidget* m_inspectorDock = nullptr;
  QDockWidget* m_timelineDock = nullptr;
  opad::json m_lastMeasure;
  QStringList m_lastMeasureTargets;
  QSettings m_settings;
  QTimer m_gitTimer;
  bool m_syncing = false;
};
