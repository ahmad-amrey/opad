#pragma once
// The Drawings workspace's page in the viewport's place (UI-78): the sheet canvas over a bar with the document's sheets as
// tabs (a drawing's sheets side by side, "+" adds one to the shown sheet's drawing), what a placement asks for, the snap
// switch (its menu: which kinds), the cursor on paper in mm with what it snapped to and the sheet's size, scale and
// projection, and how many annotations lost their references (its menu re-attaches them). Above the bar, while a tool
// runs or an annotation is selected, the annotation options (SheetAnnotate.hpp). With no sheet in the document it shows how
// to start one instead (New drawing…).
#include <QWidget>
#include <string>
#include <vector>

class AppDocument;
class JobRunner;
class QLabel;
class QStackedWidget;
class QTabBar;
class QToolButton;
class SheetAnnotator;
class SheetCanvas;

class SheetPage : public QWidget {
  Q_OBJECT
 public:
  SheetPage(AppDocument* doc, JobRunner* jobs, QWidget* parent = nullptr);
  SheetCanvas* canvas() const { return m_canvas; }
  SheetAnnotator* annotator() const { return m_annotator; }
  QToolButton* danglingButton() const { return m_dangling; }
  void documentChanged();  // tabs again; the sheet shown stays while it exists, else the first
  void showSheet(const std::string& id);
  const std::string& sheet() const;
  QTabBar* tabs() const { return m_tabs; }  // benches
  QToolButton* snapButton() const { return m_snap; }
  QLabel* cursorLabel() const { return m_cursor; }
  bool empty() const;                       // the "no drawing yet" card is shown

 signals:
  void newDrawingRequested();
  void newSheetRequested();
  void sheetShown(const std::string& id);
  void reattachRequested(const std::string& item);

 private:
  void rebuildTabs();
  void updateInfo();
  void updateDangling();
  AppDocument* m_doc;
  SheetCanvas* m_canvas;
  SheetAnnotator* m_annotator;
  QStackedWidget* m_stack;
  QTabBar* m_tabs;
  QToolButton *m_add, *m_snap, *m_dangling;
  QLabel *m_prompt, *m_cursor, *m_info;
  bool m_filling = false;
};
