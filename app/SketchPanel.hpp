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
  QSize toolSizeHint(int width) const;
 protected:
  void keyPressEvent(QKeyEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
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
};
