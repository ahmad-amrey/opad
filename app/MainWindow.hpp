#pragma once
#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QSettings>
#include <QTimer>

#include "AppDocument.hpp"
#include "Panels.hpp"
#include "Viewport.hpp"

class QSlider;
class QComboBox;
class QCheckBox;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow();
  void openPath(const QString& path);

 protected:
  void closeEvent(QCloseEvent* e) override;

 private:
  QAction* addAction(const QString& id, const QString& text, const QKeySequence& shortcut, std::function<void()> fn, bool checkable = false);
  void buildActions();
  void buildMenus();
  void buildToolbar();
  void buildDocks();
  void applyTheme(bool dark);
  void updateTitle();
  void refreshGit();
  void guarded(const std::function<void()>& fn);
  bool maybeSave();

  void onViewportSelection();
  void onBrowserSelection(const std::vector<std::string>& ids);
  void showProperties(const std::vector<opad::Ref>& refs);
  void showContextMenu(const QPoint& globalPos, std::vector<std::string> ids);
  void measure(const QString& kind);
  void pinMeasurement();
  void addAnnotation();
  void exportDialog();
  void screenshot();
  void sectionDialog();
  void saveNamedView();
  void restoreNamedView(const std::string& id);
  void rebuildViewsMenu();
  void selectOpTargets(const std::string& opId);
  void deleteOp(const std::string& opId);
  void writeSelectionFile();
  std::vector<std::string> currentNodeIds() const;

  AppDocument* m_doc;
  Viewport* m_viewport;
  BrowserPanel* m_browser;
  PropertiesPanel* m_props;
  AnnotationsPanel* m_annotations;
  TimelineWidget* m_timeline;
  QMenu* m_viewsMenu = nullptr;
  QLabel* m_statusPath;
  QLabel* m_statusGit;
  QLabel* m_statusSel;
  QLabel* m_statusHover;
  QDialog* m_sectionDialog = nullptr;
  QSlider* m_sectionSlider = nullptr;
  QComboBox* m_sectionAxis = nullptr;
  QCheckBox* m_sectionFlip = nullptr;
  QCheckBox* m_sectionOn = nullptr;
  QList<QAction*> m_actions;
  QAction* m_pinAction = nullptr;
  QAction* m_darkAction = nullptr;
  opad::json m_lastMeasure;
  QSettings m_settings;
  QTimer m_gitTimer;
  bool m_syncing = false;
};
