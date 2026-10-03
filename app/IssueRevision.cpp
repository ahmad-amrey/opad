#include "IssueRevision.hpp"

#include <QCheckBox>
#include <QDate>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

#include <atomic>
#include <filesystem>

#include "AppDocument.hpp"
#include "DocsArea.hpp"
#include "Jobs.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "Theme.hpp"
#include "opad/drawing/paint.hpp"
#include "opad/drawing/tables.hpp"

namespace git {
void run(const QString& dir, QList<QStringList> commands, QObject* context, std::function<void(bool, const QString&)> done) {
  if (commands.isEmpty()) return done(true, {});
  auto* p = new QProcess(context);
  p->setWorkingDirectory(dir);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("GIT_TERMINAL_PROMPT", "0");  // never a prompt nobody sees
  p->setProcessEnvironment(env);
  const QStringList args = commands.takeFirst();
  auto finished = std::make_shared<bool>(false);
  QObject::connect(p, &QProcess::finished, context, [p, dir, args, commands, context, done, finished](int code, QProcess::ExitStatus status) {
    *finished = true;
    const QString out = QString::fromUtf8(p->readAllStandardOutput()).trimmed(), err = QString::fromUtf8(p->readAllStandardError()).trimmed();
    p->deleteLater();
    if (status != QProcess::NormalExit || code != 0) return done(false, "git " + args.join(' ') + ": " + (err.isEmpty() ? out : err));
    if (commands.isEmpty()) return done(true, out);
    run(dir, commands, context, done);
  });
  QObject::connect(p, &QProcess::errorOccurred, context, [p, done, finished](QProcess::ProcessError e) {
    if (e != QProcess::FailedToStart || std::exchange(*finished, true)) return;
    p->deleteLater();
    done(false, IssueDialog::tr("git was not found: install it, or put it on PATH"));
  });
  p->start("git", args);
}

QString tagName(const QString& drawing, const QString& rev) {
  QString t = QString("%1-rev-%2").arg(drawing.trimmed(), rev.trimmed());
  t.replace(QRegularExpression("[\\s~^:?*\\[\\]\\\\]+"), "-");
  t.replace(QRegularExpression("\\.\\.+"), ".");
  t.replace(QRegularExpression("-+"), "-");
  while (t.startsWith('-') || t.startsWith('.')) t.remove(0, 1);
  while (t.endsWith('.') || t.endsWith('/') || t.endsWith(".lock")) t.chop(t.endsWith(".lock") ? 5 : 1);
  return t.isEmpty() ? QString("rev-%1").arg(rev) : t;
}
}  // namespace git

IssueDialog::IssueDialog(AppDocument* doc, const std::string& sheet, QWidget* parent) : QDialog(parent), m_doc(doc), m_sheet(sheet) {
  setObjectName("issueDialog");
  setWindowTitle(tr("Issue revision"));
  const opad::Sheet* s = doc->scene.sheet(sheet);
  if (!s) throw opad::Error("That sheet is gone.");
  m_drawing = QString::fromStdString(s->drawing.empty() ? s->name : s->drawing);
  const auto issues = opad::drawing::drawing_issues(doc->scene, *s);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(10);
  auto* intro = new QLabel(tr("Releases %1 as a revision: its values and views as they are now are kept in the document, so the sheet shows when "
                              "they change afterwards, and the revision table and title block show it.")
                               .arg(m_drawing),
                           this);
  intro->setObjectName("secondary");
  intro->setWordWrap(true);
  v->addWidget(intro);
  auto* form = new QFormLayout();
  const auto line = [&](const QString& text, const char* name) {
    auto* e = new QLineEdit(text, this);
    e->setObjectName(name);
    return e;
  };
  m_rev = line(QString::fromStdString(opad::drawing::next_revision(doc->scene, *s)), "issue.rev");
  m_rev->setMaximumWidth(80);
  m_description = line(issues.empty() ? tr("First release") : QString(), "issue.description");
  m_date = line(QDate::currentDate().toString(Qt::ISODate), "issue.date");
  m_date->setMaximumWidth(120);
  m_by = line(QString::fromStdString(opad::default_author()), "issue.by");
  const opad::json approved = issues.empty() ? doc->scene.properties.value("approved", opad::json()) : issues.back()->def.value("approved", opad::json());
  m_approved = line(approved.is_string() ? QString::fromStdString(approved.get<std::string>()) : QString(), "issue.approved");
  form->addRow(tr("Revision"), m_rev);
  form->addRow(tr("Description"), m_description);
  form->addRow(tr("Date"), m_date);
  form->addRow(tr("Issued by"), m_by);
  form->addRow(tr("Approved by"), m_approved);
  v->addLayout(form);
  m_freeze = new QCheckBox(tr("Keep the views' linework in the document"), this);
  m_freeze->setObjectName("issue.freeze");
  m_freeze->setChecked(true);
  m_freeze->setToolTip(tr("The issued views stay in the file as drawn, on any machine and with any later version; the document grows by their lines"));
  v->addWidget(m_freeze);
  m_pdf = new QCheckBox(tr("Write the issued PDF"), this);
  m_pdf->setObjectName("issue.pdf");
  m_pdf->setChecked(true);
  m_pdf->setToolTip(tr("Every sheet of the drawing as a page; its SHA-256 goes into the revision, so the file can be told from any other"));
  v->addWidget(m_pdf);
  auto* pdfRow = new QHBoxLayout();
  pdfRow->setContentsMargins(24, 0, 0, 0);
  m_pdfPath = line(QString(), "issue.pdfPath");
  auto* browse = new QPushButton(tr("Browse…"), this);
  pdfRow->addWidget(m_pdfPath, 1);
  pdfRow->addWidget(browse);
  v->addLayout(pdfRow);
  m_git = new QCheckBox(tr("Commit and tag it in git"), this);
  m_git->setObjectName("issue.git");
  m_git->setEnabled(false);
  m_git->setToolTip(tr("Looking for the document's git repository…"));
  v->addWidget(m_git);
  auto* tagRow = new QHBoxLayout();
  tagRow->setContentsMargins(24, 0, 0, 0);
  auto* tagLabel = new QLabel(tr("Tag"), this);
  tagLabel->setObjectName("secondary");
  m_tag = line(QString(), "issue.tag");
  m_tag->setEnabled(false);
  tagRow->addWidget(tagLabel);
  tagRow->addWidget(m_tag, 1);
  v->addLayout(tagRow);
  m_error = new QLabel(this);
  m_error->setObjectName("error");
  m_error->setWordWrap(true);
  m_error->hide();
  v->addWidget(m_error);
  auto* footer = new QHBoxLayout();
  footer->addStretch();
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  auto* issue = new QPushButton(tr("Issue"), this);
  issue->setObjectName("primary");
  issue->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(issue);
  v->addLayout(footer);
  // Where the PDF goes: beside the document, else where files went last.
  const std::filesystem::path path = doc->doc.path;
  m_dir = path.empty() ? QSettings().value("ui/lastDir", QDir::homePath()).toString() : QString::fromStdU16String(path.parent_path().u16string());
  suggest();
  connect(m_rev, &QLineEdit::textEdited, this, [this] { suggest(); });
  connect(m_pdfPath, &QLineEdit::textEdited, this, [this] { m_pdfTouched = true; });
  connect(m_tag, &QLineEdit::textEdited, this, [this] { m_tagTouched = true; });
  connect(m_pdf, &QCheckBox::toggled, this, [this, browse](bool on) {
    m_pdfPath->setEnabled(on);
    browse->setEnabled(on);
  });
  connect(m_git, &QCheckBox::toggled, m_tag, &QLineEdit::setEnabled);
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString f = QFileDialog::getSaveFileName(this, tr("Issued PDF"), m_pdfPath->text(), tr("PDF files (*.pdf)"));
    if (f.isEmpty()) return;
    m_pdfPath->setText(QFileInfo(f).suffix().isEmpty() ? f + ".pdf" : f);
    m_pdfTouched = true;
  });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(issue, &QPushButton::clicked, this, &IssueDialog::validate);
  // The repository the document is in, asked in the background.
  if (path.empty()) {
    m_git->setToolTip(tr("Save the document in a git repository to commit and tag its revisions"));
    m_checked = true;
  } else {
    QPointer<IssueDialog> self(this);
    git::run(m_dir, {{"rev-parse", "--show-toplevel"}}, this, [self](bool ok, const QString& text) {
      if (!self) return;
      self->m_checked = true;
      self->m_git->setEnabled(ok);
      self->m_git->setToolTip(ok ? tr("Saves the document, commits it (and the PDF when it is in the repository) and tags that commit in %1")
                                       .arg(QDir::toNativeSeparators(text))
                                 : tr("The document is not in a git repository (%1)").arg(text));
      emit self->gitFound(ok);
    });
  }
  resize(480, sizeHint().height());
}

void IssueDialog::suggest() {
  const QString rev = m_rev->text().trimmed();
  QString stem = QString("%1 rev %2").arg(m_drawing, rev);
  for (const QChar c : QString("<>:\"/\\|?*")) stem.replace(c, '_');
  if (!m_pdfTouched) m_pdfPath->setText(QDir::toNativeSeparators(QDir(m_dir).filePath(stem + ".pdf")));
  if (!m_tagTouched) m_tag->setText(git::tagName(m_drawing, rev));
}

void IssueDialog::validate() {
  QString why;
  const opad::Sheet* s = m_doc->scene.sheet(m_sheet);
  const QString rev = m_rev->text().trimmed();
  if (!s) why = tr("The sheet is gone.");
  else if (rev.isEmpty()) why = tr("Give the revision a name, such as A or 1.");
  else
    for (const opad::SheetItem* t : opad::drawing::drawing_issues(m_doc->scene, *s))
      if (QString::fromStdString(t->def.value("rev", "")) == rev) why = tr("Revision %1 was issued already.").arg(rev);
  if (why.isEmpty() && !QDate::fromString(m_date->text().trimmed(), Qt::ISODate).isValid()) why = tr("The date is year-month-day: 2026-10-04.");
  if (why.isEmpty() && m_pdf->isChecked() && !QDir::isAbsolutePath(QDir::fromNativeSeparators(m_pdfPath->text().trimmed())))
    why = tr("Choose where the PDF goes.");
  if (why.isEmpty() && tagged() && (m_tag->text().trimmed().isEmpty() || m_tag->text().contains(QRegularExpression("[\\s~^:?*\\[\\\\]|\\.\\."))))
    why = tr("A tag has no spaces, no backslash, no two dots in a row and none of ~ ^ : ? * [");
  m_error->setText(why);
  m_error->setVisible(!why.isEmpty());
  if (why.isEmpty()) accept();
}

opad::json IssueDialog::args() const {
  opad::json a = {{"sheet", m_sheet}, {"rev", m_rev->text().trimmed().toStdString()}, {"date", m_date->text().trimmed().toStdString()},
                  {"freeze", m_freeze->isChecked()}};
  for (const auto& [key, edit] : std::initializer_list<std::pair<const char*, QLineEdit*>>{{"description", m_description}, {"by", m_by}, {"approved", m_approved}})
    if (!edit->text().trimmed().isEmpty()) a[key] = edit->text().trimmed().toStdString();
  if (tagged()) a["tag"] = m_tag->text().trimmed().toStdString();
  return a;
}

void IssueDialog::setFrozenBytes(qint64 bytes) {
  m_freeze->setText(bytes > 0 ? tr("Keep the views' linework in the document (adds about %1)").arg(QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat))
                              : tr("Keep the views' linework in the document (already in it: unchanged since the last revision)"));
}

QString IssueDialog::pdf() const { return m_pdf->isChecked() ? QDir::fromNativeSeparators(m_pdfPath->text().trimmed()) : QString(); }
bool IssueDialog::tagged() const { return m_git->isEnabled() && m_git->isChecked(); }

// ---------------------------------------------------------------- the area's part
void DocsArea::issueRevision() {
  const std::string sheet = m_page ? m_page->sheet() : std::string();
  if (sheet.empty()) throw opad::Error("Open a sheet first.");
  if (!services().requireEditable([this] { services().guarded([&] { issueRevision(); }); })) return;
  auto* dialog = new IssueDialog(services().document(), sheet, services().window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this, [this, dialog] {
    const opad::json args = dialog->args();
    const QString pdf = dialog->pdf();
    const bool tagged = dialog->tagged();
    services().guarded([&] { issue(args, pdf, tagged); });
  });
  dialog->open();
  // What the frozen linework adds, measured while the dialog is open (the views' projections are cached for the issue then);
  // stopped when it closes, so the issue never waits for it.
  auto bytes = std::make_shared<size_t>(0);
  auto stop = std::make_shared<std::atomic<bool>>(false);
  connect(dialog, &QDialog::finished, this, [stop] { *stop = true; });
  QPointer<IssueDialog> shown(dialog);
  const QString phase = tr("Measuring the views' linework");
  m_page->canvas()->read(
      phase,
      [sheet, bytes, stop, phase](const opad::Document& doc, const opad::Scene& scene, Progress p) {
        const opad::Sheet* s = scene.sheet(sheet);
        if (s && !*stop) *bytes = opad::drawing::frozen_bytes(doc, scene, *s, [&](double f, const std::string&) {
          p.setPhase(phase, f < 0 ? -1 : static_cast<int>(100 * f));
          return !p.cancelled() && !*stop;
        });
      },
      [shown, bytes, stop](bool ok, const QString&) {
        if (shown && ok && !*stop) shown->setFrozenBytes(static_cast<qint64>(*bytes));
      });
}

void DocsArea::issue(const opad::json& args, const QString& pdf, bool tagged, std::function<void(const opad::json&)> done) {
  if (!m_page) return;
  struct Planned {
    opad::json op, frozen = opad::json::object(), edits = opad::json::array();
  };
  auto planned = std::make_shared<Planned>();
  const std::filesystem::path file(pdf.toStdU16String());
  const QString phase = tr("Writing the issued PDF");
  QPointer<DocsArea> self(this);
  lastIssue = nullptr;
  m_page->canvas()->read(
      tr("Issuing the revision"),
      [args, pdf, file, planned, phase](const opad::Document& doc, const opad::Scene& scene, Progress p) {
        namespace dr = opad::drawing;
        std::map<std::string, std::string> frozen;
        const opad::json plan = dr::plan_issue(doc, scene, args, &frozen);
        planned->op = plan["op"];
        planned->edits = plan["edits"];
        for (const auto& [view, brep] : frozen) planned->frozen[view] = brep;
        if (pdf.isEmpty()) return;
        const opad::Scene issued = dr::with_issue(scene, planned->op);  // the revision in its title block and revision table
        std::vector<dr::Display> pages;
        const opad::json& sheets = planned->op["sheets"];
        for (size_t i = 0; i < sheets.size(); ++i) {
          const opad::Sheet* s = issued.sheet(sheets[i].get<std::string>());
          if (!s) continue;
          pages.push_back(dr::sheet_display(doc, issued, *s, [&](double f, const std::string&) {
            p.setPhase(phase, f < 0 ? -1 : static_cast<int>(100 * (static_cast<double>(i) + f) / static_cast<double>(sheets.size())));
            return !p.cancelled();
          }));
        }
        std::vector<const dr::Display*> list;
        for (const auto& d : pages) list.push_back(&d);
        dr::write_pages(list, file, "pdf");
        const auto name = file.filename().u8string();
        planned->op["pdf"] = std::string(name.begin(), name.end());
        planned->op["pdf_sha256"] = opad::sha256_hex(opad::read_text_file(file));
      },
      [self, planned, pdf, tagged, done](bool ok, const QString& error) {
        if (!self) return;
        if (!ok) {
          self->lastIssue = {{"error", error.toStdString()}};
          if (error != "cancelled") self->services().guarded([&] { throw opad::Error(error.toStdString()); });
          if (done) done(self->lastIssue);
          return;
        }
        self->run("sheet_issue", {{"op", planned->op}, {"frozen", planned->frozen}, {"edits", planned->edits}}, [self, planned, pdf, tagged, done](const opad::json& out) {
          if (!self) return;
          if (out.is_null()) {
            if (done) done(nullptr);
            return;
          }
          self->lastIssue = out;
          const QString rev = QString::fromStdString(out.value("rev", ""));
          self->services().toast(pdf.isEmpty() ? tr("Revision %1 issued").arg(rev) : tr("Revision %1 issued: %2").arg(rev, QDir::toNativeSeparators(pdf)), {}, {}, 8000);
          if (!tagged) {
            if (done) done(out);
            return;
          }
          self->commitAndTag(rev, QString::fromStdString(planned->op.value("tag", "")), pdf, done);
        });
      });
}

void DocsArea::commitAndTag(const QString& rev, const QString& tag, const QString& pdf, std::function<void(const opad::json&)> done) {
  AppDocument* doc = services().document();
  const QString path = QString::fromStdU16String(doc->doc.path.u16string());
  QPointer<DocsArea> self(this);
  const auto finish = [self, done, tag](const QString& error) {  // empty: tagged
    if (!self) return;
    self->lastIssue["git"] = error.isEmpty() ? ("tagged " + tag).toStdString() : error.toStdString();
    if (error.isEmpty()) self->services().toast(tr("Committed and tagged %1").arg(tag));
    else self->services().guarded([&] { throw opad::Error(("The revision is issued, but git did not take it: " + error).toStdString()); });
    if (done) done(self->lastIssue);
  };
  const opad::Sheet* sheet = m_page ? doc->scene.sheet(m_page->sheet()) : nullptr;
  const QString drawing = sheet ? QString::fromStdString(sheet->drawing.empty() ? sheet->name : sheet->drawing) : QString();
  whenFree([this, self, doc, path, rev, tag, pdf, drawing, finish] {
    services().guarded([&] {
      doc->saveAsync(services().jobs(), path, true, [self, path, rev, tag, pdf, drawing, finish](bool ok, const QString& error) {
        if (!self) return;
        if (!ok) return finish(error.isEmpty() ? QString("not saved") : error);
        const QFileInfo info(path);
        const QDir dir = info.absoluteDir();
        QStringList files{info.fileName()};
        if (!pdf.isEmpty() && !dir.relativeFilePath(pdf).startsWith("..")) files << dir.relativeFilePath(pdf);
        const QString message = QString("%1 rev %2").arg(drawing, rev);
        git::run(dir.absolutePath(), {QStringList{"add", "--"} + files, QStringList{"commit", "-m", message, "--"} + files, {"tag", "-a", tag, "-m", message}}, self,
                 [finish](bool ok, const QString& text) { finish(ok ? QString() : text); });
      });
    });
  });
}
