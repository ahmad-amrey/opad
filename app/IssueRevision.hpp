#pragma once
// Issue revision (TODO 11 UI-84): releasing a sheet's drawing as a revision. The dialog takes its letter (the next one by
// default), description, date, who issues and approves it, whether the views' linework is kept in the document, where its
// PDF goes and, when the document lives in a git repository, whether the issue is committed and tagged there (local git
// only, through git::run). DocsArea::issue then plans it, writes the PDF as the drawing will show it (its revision in the
// title block and the revision table) and hashes it on a worker, appends one sheet_issue step, and saves, commits and tags
// in the background.
#include <QDialog>
#include <QStringList>
#include <functional>
#include <string>

#include "opad/json.hpp"

class AppDocument;
class QCheckBox;
class QLabel;
class QLineEdit;

namespace git {
// Runs git commands one after another in `dir` (each starts when the one before has finished: nothing waits on the UI thread),
// with prompts off; done(ok, the last one's output, or the failing command and its error). `context` owns the processes.
void run(const QString& dir, QList<QStringList> commands, QObject* context, std::function<void(bool ok, const QString& text)> done);
// A tag name made of a drawing's name and revision ("Housing-rev-B"): what git refuses in a ref name taken out.
QString tagName(const QString& drawing, const QString& rev);
}  // namespace git

class IssueDialog : public QDialog {
  Q_OBJECT
 public:
  IssueDialog(AppDocument* doc, const std::string& sheet, QWidget* parent);
  opad::json args() const;  // sheet_issue's: sheet, rev, description, date, by, approved, freeze, tag (when tagged)
  QString pdf() const;      // where the PDF goes; empty: none
  bool tagged() const;      // commit and tag in git
  // What keeping the linework adds to the document (drawing::frozen_bytes, measured on a worker by DocsArea::issueRevision).
  void setFrozenBytes(qint64 bytes);
  // For benches.
  QLineEdit* revisionEdit() const { return m_rev; }
  QLineEdit* descriptionEdit() const { return m_description; }
  QLineEdit* approvedEdit() const { return m_approved; }
  QLineEdit* pdfEdit() const { return m_pdfPath; }
  QCheckBox* gitBox() const { return m_git; }
  QCheckBox* freezeBox() const { return m_freeze; }
  bool gitChecked() const { return m_checked; }  // the repository look-up has answered

 signals:
  void gitFound(bool found);

 private:
  void suggest();  // the PDF's file and the tag follow the revision until edited
  void validate();
  AppDocument* m_doc;
  std::string m_sheet;
  QString m_drawing, m_dir;
  QLineEdit *m_rev, *m_description, *m_date, *m_by, *m_approved, *m_pdfPath, *m_tag;
  QCheckBox *m_freeze, *m_pdf, *m_git;
  QLabel* m_error;
  bool m_pdfTouched = false, m_tagTouched = false, m_checked = false;
};
