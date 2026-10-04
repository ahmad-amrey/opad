#pragma once
// The Drawings workspace's page in the viewport's place (UI-78): the sheet canvas over a bar with the document's sheets as
// tabs (a drawing's sheets side by side, "+" adds one to the shown sheet's drawing), what a placement asks for, the snap
// switch (its menu: which kinds), the cursor on paper in mm with what it snapped to and the sheet's size, scale and
// projection, and how many annotations lost their references (its menu re-attaches them). Above the bar, while a tool
// runs or an annotation is selected, the annotation options (SheetAnnotate.hpp). With no sheet in the document it shows how
// to start one instead (New drawing…). Once the drawing was issued (UI-84), the bar names its revision, and warns when its
// views or values changed since (a click issues the next revision).
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
class SheetViewTool;

class SheetPage : public QWidget {
  Q_OBJECT
 public:
  SheetPage(AppDocument* doc, JobRunner* jobs, QWidget* parent = nullptr);
  SheetCanvas* canvas() const { return m_canvas; }
  SheetAnnotator* annotator() const { return m_annotator; }
  SheetViewTool* viewTool() const { return m_viewTool; }  // section, detail and auxiliary views, crops, breaks (UI-82)
  QToolButton* danglingButton() const { return m_dangling; }
  QToolButton* issueButton() const { return m_issue; }
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
  void issueRequested();
  void exportIssueRequested(const std::string& rev);  // the drawing as that revision was issued

 private:
  void rebuildTabs();
  void updateInfo();
  void updateDangling();
  void updateIssue();
  AppDocument* m_doc;
  SheetCanvas* m_canvas;
  SheetAnnotator* m_annotator;
  SheetViewTool* m_viewTool;
  QStackedWidget* m_stack;
  QTabBar* m_tabs;
  QToolButton *m_add, *m_snap, *m_dangling, *m_issue;
  QLabel *m_prompt, *m_cursor, *m_info;
  bool m_filling = false;
};
