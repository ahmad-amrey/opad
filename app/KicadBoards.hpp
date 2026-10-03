#pragma once
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QDoubleSpinBox;
class QPlainTextEdit;

// How KiCad boards are read (settings kicad/*, AppDocument::kicadOptions): by OPAD's reader or by KiCad's own STEP export
// (kicad-cli when installed, with tracks, pads and silkscreen; imported linked to the board), the footprints' 3D models,
// do-not-populate parts, vias, the origin, the boxes for missing models, the user's model folders, and whether models of
// KiCad's library are downloaded. Asked before a board is imported ("Import"), and from Settings > KiCad boards ("Save");
// opening a board for viewing uses what was saved.
class KicadDialog : public QDialog {
  Q_OBJECT
 public:
  KicadDialog(QWidget* parent, bool import);
  void save() const;  // the choices into the settings (done on accept)
  // Whether a board is imported linked: read through KiCad's export, the board is its source (opad::link_file).
  static bool linked();

 private:
  QComboBox* m_reader;
  QCheckBox* m_tracks;
  QCheckBox* m_pads;
  QCheckBox* m_silkscreen;
  QLabel* m_readerNote;
  QCheckBox* m_components;
  QCheckBox* m_dnp;
  QCheckBox* m_vias;
  QComboBox* m_origin;
  QDoubleSpinBox* m_height;
  QPlainTextEdit* m_dirs;
  QComboBox* m_download;
};
