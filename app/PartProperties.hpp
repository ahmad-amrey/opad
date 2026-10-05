#pragma once
// Part properties in the app (TODO 11 UI-83, UI-140): the Properties panel's PART section (what a body or component sets
// itself: part number, description, material, density, mass, vendor, notes, its place in the bill of materials, custom
// fields) and the dialog that edits them for one node or several at once, as one part_properties step (one undo).
// Materials come from the library (materials.hpp) by translated name and are stored by id; other text is kept as typed.
#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>
#include <string>
#include <vector>

#include "opad/json.hpp"

class AppDocument;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace parts {
// The PART section for node_properties' output (its "part" object): an empty title when it is not a body or component.
// Shaped like a Properties section provider's (header, label/value rows); the panel adds the "Edit part properties…" link.
struct Section {
  QString title;
  QList<QPair<QString, QString>> rows;
};
Section section(const opad::json& props);
QString materialName(const std::string& text);  // a library material (id or name) by its translated name, other text as it is
}  // namespace parts

class PartPropertiesDialog : public QDialog {
  Q_OBJECT
 public:
  PartPropertiesDialog(AppDocument* doc, std::vector<std::string> nodes, QWidget* parent);
  opad::json fields() const;  // as edited, for drawing::part_properties_change (a mixed choice left alone is not there)
  opad::json command() const;  // part_properties arguments; null when nothing would change
 signals:
  void applied();  // the step is on the log
 private:
  void refresh();  // hints, errors, Apply
  void apply();
  AppDocument* m_doc;
  std::vector<std::string> m_nodes;
  opad::json m_shared;      // drawing::shared_part_properties
  QString m_materialShown;  // the material text the combo started with: unchanged, the stored value is kept
  QLineEdit *m_number, *m_description, *m_density, *m_mass, *m_vendor, *m_notes;
  QComboBox *m_material, *m_bom;
  QCheckBox *m_appearance, *m_section;
  QLabel *m_materialHint, *m_error;
  QPushButton* m_apply;
};
