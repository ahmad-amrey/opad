#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QSlider>
#include <QToolButton>
#include <QWidget>
#include <string>

#include "AppDocument.hpp"

// ---------------------------------------------------------------- section
class SectionPanel : public QWidget {
  Q_OBJECT
 public:
  explicit SectionPanel(AppDocument* doc, QWidget* parent = nullptr);
  bool enabled() const { return m_enabled; }
  opad::Vec3 origin() const;
  bool pickRange(double& dmin, double& dmax) const;  // the model's extent along the picked normal
  opad::Vec3 normal() const;
  bool caps() const;
  bool picking() const { return m_pick; }  // "Pick face" is active: the next planar face picked in the view sets the plane
  void beginPick();                          // what the "Pick face" button does
 signals:
  void planeChanged();
  void pickRequested();  // the user wants to pick a face: switch the select filter to faces
  void saveRequested(const QString& name, const opad::Vec3& origin, const opad::Vec3& normal);
  void enabledChanged(bool on);
 public slots:
  void setEnabled(bool on);
  void flip();
  void setFromFace(const opad::Vec3& origin, const opad::Vec3& normal);
  void setOrigin(const opad::Vec3& origin);  // the plane was dragged in the view: move the slider to it (clamped to the model)
  void rebuild();
  void applyNamed(const std::string& id);
 private:
  void emitChange();
  void setAlong(double along);  // slider from a distance along the axis (or the picked normal)
  AppDocument* m_doc;
  bool m_enabled = false;
  int m_axis = 2;
  bool m_flip = false;
  bool m_pick = false;
  opad::Vec3 m_pickOrigin{0, 0, 0}, m_pickNormal{0, 0, 1};
  QList<QToolButton*> m_axisButtons;
  QSlider* m_slider;
  QLineEdit* m_value;
  QToolButton* m_flipButton;
  QToolButton* m_capButton;
  QListWidget* m_named;
  QLabel* m_state;
};
