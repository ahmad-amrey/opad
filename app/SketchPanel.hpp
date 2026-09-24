#pragma once
#include <QWidget>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include "GuidedTool.hpp"

class SketchEditor;
class SketchPanel : public QWidget {
  Q_OBJECT
 public:
  explicit SketchPanel(SketchEditor* editor, QWidget* parent = nullptr);
  void refresh();
  QList<ToolStep> steps() const;
 signals:
  void finishRequested();
 private:
  void chooseGroup();
  void chooseTool();
  void buildFields();
  SketchEditor* m_editor;
  QComboBox *m_group, *m_tools, *m_coordinates;
  QLineEdit *m_u, *m_v;
  QFormLayout* m_fields;
  QLabel *m_status, *m_state;
  ToolStepsPanel* m_steps;
  QTreeWidget* m_constraints;
  QString m_shown;
  bool m_refreshing = false;
};
