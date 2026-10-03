// A merge git stopped on, resolved here (UI-63; VersionControl.hpp). The document's three index stages (base, ours,
// theirs) are read and merged anyway on a worker (opad::merge_files keeping the conflicts: ours as written, then theirs'
// new ops, so theirs win as they stand), and what both sides changed is listed to decide one by one, all mine or all
// theirs; Resolve appends the deciding records (opad::resolve_merge), writes the file, adds it to git and takes it in
// (DiskSync::adopt), then offers Regenerate when both sides changed the design, and Commit the merge. The stages are read,
// not the file: it works whether OPAD's driver stopped (the file is ours) or a clone without it wrote conflict markers.
// When the two cannot be merged at all (a rewritten history, a changed header), one side's whole file can be kept.
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <memory>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "DiskSync.hpp"
#include "GitWatch.hpp"
#include "VersionControl.hpp"
#include "opad/merge.hpp"
#include "opad/scene.hpp"

struct VersionControl::Conflicts {
  std::string ours, theirs, merged;  // stages 2 and 3, and the merge as it stands (theirs win every conflict)
  std::vector<opad::MergeConflict> list;
  QStringList what, mine, theirsText;  // as shown
  QString error;                       // why the two cannot be merged: only a whole side can be kept
  bool design = false;                 // both sides changed the design: regenerate after
};

namespace {
QString value(const opad::json& v) {
  if (v.is_string()) return QString::fromStdString(v.get<std::string>());
  QString text = QString::fromStdString(v.dump());
  return text.size() > 60 ? text.left(57) + QStringLiteral("…") : text;
}

// What an op makes of a conflict's key, as a reader takes it, and by whom.
QString effectText(const opad::Op* op, const opad::MergeConflict& c) {
  if (!op) return {};
  const opad::json& d = op->data;
  QString text;
  if (op->type == "delete") text = VersionControl::tr("deletes it");
  else if (op->type == "param") text = value(d.value("expr", opad::json()));
  else if (op->type == "regen") text = VersionControl::tr("its geometry recomputed");
  else {
    const opad::json e = opad::op_effect(d, c.target, c.field)[1];
    if (e.is_object()) {
      QStringList parts;
      for (const auto& [key, v] : e.items()) parts << (e.size() == 1 ? value(v) : QString::fromStdString(key) + ": " + value(v));
      text = parts.join(", ");
    } else {
      text = value(e);
    }
  }
  const std::string by = d.value("by", "");
  return by.empty() ? text : VersionControl::tr("%1 (%2)").arg(text, QString::fromStdString(by));
}
}  // namespace

void VersionControl::resolveConflicts() {
  AppDocument* doc = m_services.document();
  if (!ready()) return openPanel();
  if (m_git->repo().doc() != git::Repo::Doc::Conflict) return say(tr("%1 is not in conflict.").arg(QFileInfo(doc->path()).fileName()));
  if (doc->isDirty()) return failed(tr("Resolve conflicts"), tr("Save or undo the unsaved changes first: resolving replaces the file."));
  auto c = std::make_shared<Conflicts>();
  const QString rel = documentPath();
  ++m_running;
  m_git->job(tr("Reading the conflict in %1").arg(QFileInfo(rel).fileName()), [c, rel](const git::Context& ctx, const git::RunOptions& o) {
    auto stage = [&](const char* n) {
      try {
        return git::show(ctx, QString::fromLatin1(n), rel).toStdString();
      } catch (const std::exception&) {
        return std::string();  // added or deleted on a side
      }
    };
    std::string base = stage(":1");
    c->ours = stage(":2");
    c->theirs = stage(":3");
    if (o.progress) o.progress(tr("Merging"), -1);
    if (base.empty() || c->ours.empty() || c->theirs.empty()) {
      c->error = c->ours.empty() ? tr("it is new on their side") : c->theirs.empty() ? tr("they deleted it") : tr("both sides added it");
      return;
    }
    const opad::Document b = opad::Document::parse_index(base), ours = opad::Document::parse_index(c->ours), theirs = opad::Document::parse_index(c->theirs);
    c->design = opad::changes_design(b, ours) && opad::changes_design(b, theirs);
    opad::FileMerge m = opad::merge_files(std::move(base), c->ours, c->theirs, true);
    if (!m.error.empty()) {
      c->error = QString::fromStdString(m.error);
      return;
    }
    c->merged = m.text();
    c->list = std::move(m.conflicts);
    const opad::Scene scene = opad::resolve(ours);
    for (const auto& x : c->list) {
      c->what << conflictText(x, scene);
      c->mine << effectText(ours.find_op(x.ours), x);
      c->theirsText << effectText(theirs.find_op(x.theirs), x);
    }
  }, [this, self = QPointer<VersionControl>(this), c](bool ok, const QString& error) {
    if (!self) return;
    --m_running;
    if (!ok) {
      failed(tr("Could not read the conflict"), error);
      return done("resolve", false, error);
    }
    showConflicts(c);
  });
}

void VersionControl::showConflicts(std::shared_ptr<Conflicts> c) {
  const QString name = QFileInfo(documentPath()).fileName();
  auto* d = new QDialog(m_services.window());
  d->setObjectName("vcsResolve");
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowTitle(tr("Resolve conflicts in %1").arg(name));
  d->setMinimumWidth(680);
  auto* col = new QVBoxLayout(d);
  auto* intro = new QLabel(d);
  intro->setObjectName("resolveIntro");
  intro->setWordWrap(true);
  intro->setText(!c->error.isEmpty() ? tr("The two versions cannot be merged (%1): keep one of them whole.").arg(c->error)
                 : c->list.empty() ? tr("Nothing collides: both sides' changes merge as they are (OPAD's merging was not set up when git merged). Resolve takes both.")
                                   : tr("Both sides changed these. Choose whose change to keep for each; everything else of both sides is merged."));
  col->addWidget(intro);
  auto* list = new QTreeWidget(d);
  list->setObjectName("resolveList");
  list->setRootIsDecorated(false);
  list->setColumnCount(4);
  list->setHeaderLabels({tr("What"), tr("Mine"), tr("Theirs"), tr("Keep")});
  list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  list->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  const bool choices = c->error.isEmpty() && !c->list.empty();
  list->setVisible(choices);
  auto keeps = std::make_shared<std::vector<QComboBox*>>();
  for (int i = 0; i < c->what.size(); ++i) {
    auto* item = new QTreeWidgetItem(list, {c->what[i], c->mine[i], c->theirsText[i]});
    for (int k = 0; k < 3; ++k) item->setToolTip(k, item->text(k));
    auto* keep = new QComboBox(list);
    keep->setObjectName("keep");
    keep->addItem(tr("Mine"), true);
    keep->addItem(tr("Theirs"), false);
    list->setItemWidget(item, 3, keep);
    keeps->push_back(keep);
  }
  col->addWidget(list, 1);
  if (c->design && c->error.isEmpty()) {
    auto* regen = new QLabel(tr("Both sides changed the design: once resolved, OPAD offers to regenerate it, so that each side's results follow the other's changes."), d);
    regen->setObjectName("resolveDesign");
    regen->setWordWrap(true);
    col->addWidget(regen);
  }
  auto* row = new QHBoxLayout;
  auto button = [d, row](const char* action, const QString& text) {
    auto* b = new QPushButton(text, d);
    b->setProperty("action", QString::fromLatin1(action));
    row->addWidget(b);
    return b;
  };
  QPushButton* allMine = button("vcsResolveAllMine", tr("All mine"));
  QPushButton* allTheirs = button("vcsResolveAllTheirs", tr("All theirs"));
  for (QPushButton* b : {allMine, allTheirs}) b->setVisible(choices);
  connect(allMine, &QPushButton::clicked, d, [keeps] { for (QComboBox* k : *keeps) k->setCurrentIndex(0); });
  connect(allTheirs, &QPushButton::clicked, d, [keeps] { for (QComboBox* k : *keeps) k->setCurrentIndex(1); });
  row->addStretch(1);
  QPushButton* ours = button("vcsResolveOurs", tr("Keep my whole file"));
  QPushButton* theirs = button("vcsResolveTheirs", tr("Take their whole file"));
  ours->setToolTip(tr("%1 as it is on this branch; what they changed is left out").arg(name));
  theirs->setToolTip(tr("%1 as it is on their side; what this branch changed is left out").arg(name));
  QPushButton* cancel = button("vcsResolveCancel", tr("Cancel"));
  QPushButton* run = button("vcsResolveRun", tr("Resolve"));
  run->setObjectName("primary");
  run->setDefault(true);
  run->setEnabled(c->error.isEmpty());
  col->addLayout(row);
  connect(cancel, &QPushButton::clicked, d, &QDialog::close);
  connect(ours, &QPushButton::clicked, d, [this, d, c] { d->close(); runResolve(c, {}, 2); });
  connect(theirs, &QPushButton::clicked, d, [this, d, c] { d->close(); runResolve(c, {}, 3); });
  connect(run, &QPushButton::clicked, d, [this, d, c, keeps] {
    std::vector<bool> mine;
    for (QComboBox* k : *keeps) mine.push_back(k->currentData().toBool());
    d->close();
    runResolve(c, mine, 0);
  });
  d->open();
}

void VersionControl::runResolve(std::shared_ptr<Conflicts> c, std::vector<bool> mine, int whole) {
  if (m_services.document()->isDirty()) return failed(tr("Resolve conflicts"), tr("Save or undo the unsaved changes first: resolving replaces the file."));
  const QString rel = documentPath(), file = QDir(m_git->repo().top).filePath(rel);
  const std::string author = QSettings().value("user/name").toString().trimmed().toStdString();
  ++m_running;
  m_git->job(tr("Resolving the conflicts in %1").arg(QFileInfo(rel).fileName()), [c, mine, whole, rel, file, author](const git::Context& ctx, const git::RunOptions& o) {
    const std::string text = whole == 2 ? c->ours : whole == 3 ? c->theirs : c->list.empty() ? c->merged : opad::resolve_merge(c->merged, c->list, mine, author);
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(text.data(), qint64(text.size())) != qint64(text.size()) || !out.commit())
      throw std::runtime_error(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(file), out.errorString()).toStdString());
    git::check(ctx, {"add", "--", rel}, o);
  }, [this, self = QPointer<VersionControl>(this), c, whole](bool ok, const QString& error) {
    if (!self) return;
    --m_running;
    m_disk->adopt();  // the resolved file comes in
    if (!ok) {
      failed(tr("Could not resolve the conflicts"), error);
      return done("resolve", false, error);
    }
    if (c->design && whole == 0)
      say(tr("Conflicts resolved. Both sides changed the design: regenerate it, then commit the merge."), tr("Regenerate"), [this] {
        if (QAction* a = m_services.action("design.regenerate")) a->trigger();
      }, 0);
    else
      say(tr("Conflicts resolved: commit the merge to finish it."), tr("Commit…"), [this] { commit(); }, 0);
    done("resolve", true);
  });
}
