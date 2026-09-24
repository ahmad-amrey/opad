#pragma once
#include "Viewport.hpp"
#include "Panels.hpp"
#include "Jobs.hpp"
#include <QPointer>
class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class ToolStepsPanel;
class PlaneTiles;

// One transient picker shared by camera, sketch and feature workflows.
class PlanePicker : public QObject {
  Q_OBJECT
 public:
  PlanePicker(AppDocument*,Viewport*,JobRunner*,QWidget*);
  ToolPanel* panel() const { return m_panel; }
  void start(bool positionOrigin,std::function<void(ToolPanel*)> open);
  void cancel();
  void choose(const opad::json& support);
  void selectionChanged();
  bool active() const { return m_active; }
  bool positioning() const { return m_originStage; }
  void apply();
  void back();
  void setOrigin(double u,double v);
  std::function<void(const opad::json&,const opad::Frame&)> accepted;
 signals:
  void cancelled();
 protected:
  bool eventFilter(QObject*,QEvent*) override;
 private:
  void stop(bool restoreCamera = true);
  void placeOrigin(const QPointF& point);
  void refresh();
  void constructionPlanes();
  void preview(const opad::Frame* frame);
  void pickOrigin(const opad::Ref&);
  AppDocument* m_doc;Viewport* m_view;JobRunner* m_jobs;
  ToolPanel* m_panel;PlaneTiles* m_tiles;ToolStepsPanel* m_steps;
  QWidget* m_originControls;QLineEdit *m_u,*m_v;QCheckBox* m_construction;QLabel* m_status;
  QPushButton *m_apply,*m_back;
  bool m_active=false,m_positionOrigin=false,m_originStage=false,m_drag=false,m_mouseDown=false,m_refreshing=false;
  int m_serial=0,m_candidateSerial=0;
  QPointer<Job> m_job;
  opad::json m_support,m_origin,m_cameraBefore;
  opad::Frame m_supportFrame,m_frame;
  Viewport::SelFilter m_oldFilter;
  Handle(AIS_Shape) m_preview;
  Handle(AIS_TextLabel) m_xLabel,m_yLabel;
};
