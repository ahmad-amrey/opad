#pragma once
// Dialogs of the Drawings workspace (UI-78). New drawing: a template (ISO A4-A0 or ANSI A-E, shown as thumbnails of their
// frames and title blocks), orientation, projection angle, scale (auto: the largest standard one that fits), the standard
// views (a base view, the top and side views projected from it, an isometric view), their style, what they draw (the
// model or the selection) and the title block's own fields; the result is the `sheet` command's arguments. Sheet
// properties: a sheet's name, drawing, paper, template, standard, projection, scale and units and every title block field
// (empty fields show what is filled in for them); the result is a sheet_edit `set`.
#include <QDialog>
#include <map>
#include <string>
#include <vector>

#include "opad/json.hpp"

class AppDocument;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QRadioButton;

class NewDrawingDialog : public QDialog {
  Q_OBJECT
 public:
  NewDrawingDialog(AppDocument* doc, const std::vector<std::string>& nodes, QWidget* parent);
  opad::json args() const;  // the sheet command's
  QListWidget* templates() const { return m_templates; }

 private:
  void renderThumbnails();
  AppDocument* m_doc;
  std::vector<std::string> m_nodes;
  QListWidget* m_templates;
  QRadioButton *m_landscape, *m_portrait, *m_whole, *m_selection;
  QComboBox *m_projection, *m_scale, *m_base, *m_tangent;
  QCheckBox *m_top, *m_side, *m_iso, *m_hidden;
  QLineEdit *m_name, *m_title, *m_number, *m_owner, *m_author, *m_revision;
  bool m_projectionTouched = false;
};

class SheetPropertiesDialog : public QDialog {
  Q_OBJECT
 public:
  SheetPropertiesDialog(AppDocument* doc, const std::string& sheet, QWidget* parent);
  opad::json change() const;  // sheet_edit `set`; null when nothing changes

 private:
  AppDocument* m_doc;
  std::string m_sheet;
  opad::json m_def;
  QLineEdit *m_name, *m_drawing;
  QComboBox *m_size, *m_template, *m_standard, *m_projection, *m_scale, *m_units;
  QRadioButton *m_landscape, *m_portrait;
  std::map<std::string, QLineEdit*> m_fields;
};
