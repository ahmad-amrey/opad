#pragma once
// The Printed part dialog (Simulate workspace, simulate.print): a structural study's settings.print as a form, from the
// filament, layer height, line width, walls, top and bottom layers, infill density, pattern and angle, flow and build
// direction, or read from a slicer's profile (PrusaSlicer, OrcaSlicer, Bambu Studio, Cura; a .3mf project or a G-code
// file). Below the form: what the study will make of it (skin thicknesses, the infill model, the road fill), or what is
// wrong with it.
#include <QDialog>

#include "opad/util.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QWidget;

class PrintDialog : public QDialog {
  Q_OBJECT
 public:
  // print: the study's settings.print (null or absent: not printed).
  PrintDialog(const opad::json& print, QWidget* parent);
  // The settings to keep: null when the bodies are not printed. Keys the form does not show (per-body overrides) stay.
  opad::json print() const;
  // Fills the form from a slicer profile; throws opad::Error for what it cannot read.
  void importProfile(const QString& path);

 private:
  void load(const opad::json& print);
  void update();

  opad::json m_kept;  // keys not on the form
  QString m_source;   // the profile's file name, if one was read
  QCheckBox* m_on = nullptr;
  QWidget* m_form = nullptr;
  QComboBox* m_material = nullptr;
  QComboBox* m_up = nullptr;
  QDoubleSpinBox* m_layer = nullptr;
  QDoubleSpinBox* m_line = nullptr;
  QSpinBox* m_walls = nullptr;
  QSpinBox* m_top = nullptr;
  QSpinBox* m_bottom = nullptr;
  QDoubleSpinBox* m_infill = nullptr;
  QComboBox* m_pattern = nullptr;
  QDoubleSpinBox* m_angle = nullptr;
  QDoubleSpinBox* m_flow = nullptr;
  QLabel* m_note = nullptr;
};
