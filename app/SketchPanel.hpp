#pragma once
#include <QWidget>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include "GuidedTool.hpp"

class SketchEditor;
class QPushButton;
class QCheckBox;
class QTabWidget;
class ToolGuide;
class SketchPanel : public QWidget {
  Q_OBJECT
 public:
  explicit SketchPanel(SketchEditor* editor, QWidget* parent = nullptr);
  void refresh();
  QList<ToolStep> steps() const;
  struct Tool {QString group,id,label;};
  static QList<Tool> tools();
  void showPage(int page);
  // The page the user opened (Select, Constraints, Snaps; 0: none): a tool that ends goes back to it and the panel stays.
  int userPage() const { return m_userPage; }
  QString pageTitle(int page) const;
  void forgetUserPage() { m_userPage = 0; }
  QSize toolSizeHint(int width) const;
 protected:
  void keyPressEvent(QKeyEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;  // Apply clicked with Shift held
 signals:
  void finishRequested();
  void contentChanged();  // refreshed: the tool page may need another height
 private:
  void chooseGroup();
  void chooseTool();
  void buildFields();
  void keysToView();  // after a footer button: the keys go on in the view
  void fitSteps();
  SketchEditor* m_editor;
  QComboBox *m_group, *m_tools, *m_coordinates;
  QLineEdit *m_u, *m_v;
  QFormLayout* m_fields;
  QLabel *m_status, *m_state;
  ToolStepsPanel* m_steps;
  ToolGuide* m_guide;  // UI-107: the tool's animated guide above its steps
  QTreeWidget* m_constraints;
  QString m_shown;
  bool m_refreshing = false;
  QTabWidget* m_pages;
  QWidget* m_precise;
  QPushButton *m_apply, *m_undoPoint, *m_closeTool;
  QCheckBox* m_showConstraints;
  QLabel* m_keepHint;  // under Apply for a tool that closes after it: Shift keeps it
  int m_userPage = 0;
  bool m_applyShift = false;  // the click on Apply had Shift held
};
