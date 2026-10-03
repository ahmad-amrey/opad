#pragma once
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPlainTextEdit;

// How KiCad boards are read (settings kicad/*, AppDocument::kicadOptions): the footprints' 3D models, do-not-populate parts,
// vias, the origin, the boxes for missing models, the user's model folders, and whether models of KiCad's library are
// downloaded. Asked before a board is imported ("Import"), and from Settings > KiCad boards ("Save"); opening a board
// for viewing uses what was saved.
class KicadDialog : public QDialog {
  Q_OBJECT
 public:
  KicadDialog(QWidget* parent, bool import);
  void save() const;  // the choices into the settings (done on accept)

 private:
  QCheckBox* m_components;
  QCheckBox* m_dnp;
  QCheckBox* m_vias;
  QComboBox* m_origin;
  QDoubleSpinBox* m_height;
  QPlainTextEdit* m_dirs;
  QComboBox* m_download;
};
