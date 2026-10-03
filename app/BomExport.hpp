#pragma once
// File > Export bill of materials (TODO 11 UI-83): a dialog that previews drawing::bom for the options chosen (parts only,
// top level or indented; the document or a selected component; masses and their unit; copies of one solid counted as one
// part; mesh and drawing bodies) and writes it as CSV (UTF-8 with a byte order mark, comma, semicolon or tab) with its
// headers, materials and "yes" in the UI's language. The BoM is computed on a worker that reads the document in place
// (AppDocument::readAsync: nothing edits it meanwhile), again whenever an option changes; the file is written on a worker.
#include <QDialog>
#include <QPointer>
#include <string>
#include <vector>

#include "opad/json.hpp"

class AppDocument;
class Job;
class JobRunner;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QTreeWidget;

class BomDialog : public QDialog {
  Q_OBJECT
 public:
  BomDialog(AppDocument* doc, JobRunner* jobs, const std::vector<std::string>& selection, QWidget* parent);
  ~BomDialog() override;
  bool ready() const { return m_ready; }  // the preview is the BoM of the options as they are
  const opad::json& bom() const { return m_bom; }
  std::string csv() const;               // the preview's CSV, as exported
  void exportTo(const QString& path);    // writes the CSV; while the preview is computed, once it is there
 signals:
  void exportRequested();  // the Export button: where to is the caller's question
  void previewed();
  void exported(const QString& path, const QString& error);
 private:
  opad::json options() const;  // drawing::BomOptions as JSON: mode, root, mass, mass_unit, match_shapes, references
  void compute();
  void showBom();
  void write();
  AppDocument* m_doc;
  JobRunner* m_jobs;
  std::string m_component;  // the selected component, when one is
  QComboBox *m_mode, *m_unit, *m_separator;
  QRadioButton *m_whole, *m_selected;
  QCheckBox *m_mass, *m_match, *m_references;
  QTreeWidget* m_table;
  QLabel* m_status;
  QPushButton* m_export;
  QPointer<Job> m_job;
  bool m_running = false;  // a worker reads the document (also after its job was cancelled)
  bool m_again = false;    // the options changed meanwhile
  bool m_ready = false;
  unsigned long long m_generation = 0, m_revision = 0;  // the document the BoM was made of: an agent may edit it meanwhile
  opad::json m_bom;
  QString m_pending;  // a path to write to once the preview is there
};
