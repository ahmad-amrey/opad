#include "VersionPanel.hpp"

#include <QApplication>
#include <algorithm>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "GitWatch.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"
#include "VersionControl.hpp"

namespace {
QString span(const QColor& c, const QString& s) { return QStringLiteral("<span style='color:%1'>%2</span>").arg(c.name(), s.toHtmlEscaped()); }

QTreeWidget* makeTree(QWidget* parent, const char* name, int columns) {
  auto* t = new QTreeWidget(parent);
  t->setObjectName(QString::fromLatin1(name));
  t->setColumnCount(columns);
  t->setHeaderHidden(true);
  t->setRootIsDecorated(false);
  t->setIndentation(0);
  t->setUniformRowHeights(true);
  t->setContextMenuPolicy(Qt::CustomContextMenu);
  t->setTextElideMode(Qt::ElideRight);
  t->header()->setStretchLastSection(false);
  return t;
}
}  // namespace

QString VersionPanel::ago(const QString& iso) {
  const QDateTime at = QDateTime::fromString(iso, Qt::ISODate);
  if (!at.isValid()) return iso;
  const qint64 s = at.secsTo(QDateTime::currentDateTimeUtc());
  if (s < 60) return tr("just now");
  if (s < 3600) return tr("%n min ago", nullptr, int(s / 60));
  if (s < 86400) return tr("%n h ago", nullptr, int(s / 3600));
  if (s < 30 * 86400) return tr("%n d ago", nullptr, int(s / 86400));
  return at.toLocalTime().toString("yyyy-MM-dd");
}

QPushButton* VersionPanel::addButton(QWidget* parent, const QString& action, const QString& text, const QString& icon) {
  auto* b = new QPushButton(text, parent);
  b->setProperty("action", action);
  if (!icon.isEmpty()) b->setIcon(icons::themed(icon, 16));
  b->setFocusPolicy(Qt::TabFocus);
  if (parent->layout()) parent->layout()->addWidget(b);
  return b;
}

QPushButton* VersionPanel::button(const QString& action) const {
  for (QPushButton* b : findChildren<QPushButton*>())
    if (b->property("action").toString() == action) return b;
  return nullptr;
}

VersionPanel::VersionPanel(VersionControl* vc, QWidget* parent) : QWidget(parent), m_vc(vc) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  m_stack = new QStackedWidget(this);
  v->addWidget(m_stack, 1);
  // ---- the repository
  auto* body = new QWidget(m_stack);
  auto* b = new QVBoxLayout(body);
  b->setContentsMargins(12, 10, 12, 8);
  b->setSpacing(8);
  auto* head = new QHBoxLayout();
  head->setSpacing(6);
  auto* mark = new QLabel(body);
  mark->setObjectName("branchMark");
  head->addWidget(mark);
  m_branch = new QLabel(body);
  m_branch->setObjectName("versionBranch");
  m_branch->setFont(theme::ui(14, QFont::DemiBold));
  m_branch->setTextInteractionFlags(Qt::TextSelectableByMouse);
  head->addWidget(m_branch);
  m_sync = new QLabel(body);
  m_sync->setObjectName("secondary");
  m_sync->setTextFormat(Qt::RichText);
  head->addWidget(m_sync, 1);
  auto* more = new QToolButton(body);
  more->setObjectName("versionMore");
  more->setText(QStringLiteral("⋯"));
  more->setAutoRaise(true);
  more->setPopupMode(QToolButton::InstantPopup);
  more->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));
  more->setToolTip(tr("More"));
  more->setAccessibleName(tr("More version control actions"));
  auto* moreMenu = new QMenu(more);
  more->setMenu(moreMenu);
  connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
    moreMenu->clear();
    auto add = [this, moreMenu](const QString& id) {
      if (QAction* a = m_vc->services().action(id)) moreMenu->addAction(a);
    };
    for (const char* id : {"vcs.fetch", "vcs.newBranch", "vcs.pack", "vcs.compare"}) add(QString::fromLatin1(id));
    moreMenu->addSeparator();
    GitWatch* git = m_vc->git();
    const git::Repo& r = git->repo();
    auto own = [moreMenu](const char* id, const QString& text, std::function<void()> fn) {
      QAction* a = moreMenu->addAction(text);
      a->setObjectName(QString::fromLatin1(id));
      QObject::connect(a, &QAction::triggered, moreMenu, fn);
    };
    own("git.setup", tr("Set up OPAD in this repository…"), [git] { git->setUp(); });
    own("git.identity", r.userName.trimmed().isEmpty() ? tr("Your name for commits…") : tr("Author: %1…").arg(r.userName), [git] { git->editIdentity(); });
    add("file.clone");
    own("git.locate", tr("Locate git…"), [git] { git->locateGit(); });
    own("git.refresh", tr("Refresh"), [git] {
      git::forgetTools();
      git->refresh(true);
    });
  });
  head->addWidget(more);
  b->addLayout(head);
  m_doc = new QLabel(body);
  m_doc->setObjectName("versionDocument");
  m_doc->setTextFormat(Qt::RichText);
  m_doc->setWordWrap(true);
  b->addWidget(m_doc);
  // A merge that stopped: abort it, or commit it once nothing is in conflict.
  m_mergeBar = new QWidget(body);
  m_mergeBar->setObjectName("card");
  auto* mb = new QVBoxLayout(m_mergeBar);
  mb->setContentsMargins(8, 6, 8, 6);
  mb->setSpacing(6);
  m_mergeText = new QLabel(m_mergeBar);
  m_mergeText->setWordWrap(true);
  m_mergeText->setTextFormat(Qt::RichText);
  mb->addWidget(m_mergeText);
  auto* mergeButtons = new QWidget(m_mergeBar);
  auto* mbl = new QHBoxLayout(mergeButtons);
  mbl->setContentsMargins(0, 0, 0, 0);
  mbl->setSpacing(6);
  connect(addButton(mergeButtons, "vcsAbortMerge", tr("Abort merge")), &QPushButton::clicked, this, [this] { m_vc->abortMerge(); });
  connect(addButton(mergeButtons, "vcsResolve", tr("Resolve conflicts…")), &QPushButton::clicked, this, [this] { m_vc->resolveConflicts(); });
  connect(addButton(mergeButtons, "vcsCommitMerge", tr("Commit the merge…")), &QPushButton::clicked, this, [this] { m_vc->commit(); });
  mbl->addStretch(1);
  mb->addWidget(mergeButtons);
  b->addWidget(m_mergeBar);
  auto* actions = new QWidget(body);
  auto* ar = new QHBoxLayout(actions);
  ar->setContentsMargins(0, 0, 0, 0);
  ar->setSpacing(6);
  connect(addButton(actions, "vcsPull", tr("Pull"), "pull"), &QPushButton::clicked, this, [this] { m_vc->pull(); });
  connect(addButton(actions, "vcsPush", tr("Push"), "push"), &QPushButton::clicked, this, [this] { m_vc->push(); });
  connect(addButton(actions, "vcsNewBranch", tr("New branch…"), "branch"), &QPushButton::clicked, this, [this] { m_vc->newBranch(); });
  ar->addStretch(1);
  b->addWidget(actions);
  // History | Branches
  auto* tabs = new QWidget(body);
  tabs->setObjectName("segmented");
  auto* tl = new QHBoxLayout(tabs);
  tl->setContentsMargins(1, 1, 1, 1);
  tl->setSpacing(0);
  for (QToolButton** t : {&m_historyTab, &m_branchesTab}) {
    auto* s = *t = new QToolButton(tabs);
    s->setObjectName("segment");
    s->setCheckable(true);
    s->setAutoExclusive(true);
    s->setFixedHeight(26);
    s->setAutoRaise(true);
    s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tl->addWidget(s, 1);
  }
  m_historyTab->setText(tr("History"));
  m_historyTab->setToolTip(tr("The commits that changed this document, newest first"));
  m_branchesTab->setText(tr("Branches"));
  m_branchesTab->setToolTip(tr("The repository's branches, here and on its remotes"));
  m_historyTab->setChecked(true);
  b->addWidget(tabs);
  m_pages = new QStackedWidget(body);
  // History: hash, message (with what points at it), when.
  auto* historyPage = new QWidget(m_pages);
  auto* hp = new QVBoxLayout(historyPage);
  hp->setContentsMargins(0, 0, 0, 0);
  hp->setSpacing(6);
  m_filterBar = new QWidget(historyPage);  // the history narrowed to an op or a node (VersionControl::showHistoryOf)
  m_filterBar->setObjectName("historyFilter");
  auto* fb = new QHBoxLayout(m_filterBar);
  fb->setContentsMargins(0, 0, 0, 0);
  fb->setSpacing(6);
  m_filterText = new QLabel(m_filterBar);
  m_filterText->setWordWrap(true);
  fb->addWidget(m_filterText, 1);
  auto* all = new QToolButton(m_filterBar);
  all->setObjectName("historyFilterClear");
  all->setText(tr("Show all"));
  all->setAutoRaise(true);
  fb->addWidget(all);
  connect(all, &QToolButton::clicked, this, [this] { m_vc->showHistoryOf({}, {}); });
  m_filterBar->hide();
  hp->addWidget(m_filterBar);
  m_history = makeTree(historyPage, "versionHistory", 3);
  m_history->setAccessibleName(tr("History of the document"));
  m_history->header()->setSectionResizeMode(0, QHeaderView::Fixed);
  m_history->header()->resizeSection(0, 66);
  m_history->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_history->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  hp->addWidget(m_history, 1);
  auto* hb = new QWidget(historyPage);
  auto* hbl = new QHBoxLayout(hb);
  hbl->setContentsMargins(0, 0, 0, 0);
  hbl->setSpacing(6);
  auto withCommit = [this](void (VersionControl::*fn)(const git::Commit&)) {
    return [this, fn] {
      if (const git::Commit* c = commitOf(m_history->currentItem())) (m_vc->*fn)(*c);
    };
  };
  connect(addButton(hb, "vcsCompare", tr("Compare"), "compare"), &QPushButton::clicked, this, withCommit(&VersionControl::compareWith));
  connect(addButton(hb, "vcsRestore", tr("Restore…")), &QPushButton::clicked, this, withCommit(&VersionControl::restore));
  connect(addButton(hb, "vcsBranchFrom", tr("Branch…"), "branch"), &QPushButton::clicked, this, [this] {
    if (const git::Commit* c = commitOf(m_history->currentItem())) m_vc->newBranch(c->hash, tr("%1 · %2").arg(c->shortHash, c->subject));
  });
  hbl->addStretch(1);
  hp->addWidget(hb);
  m_pages->addWidget(historyPage);
  // Branches: name, against its upstream, when.
  auto* branchPage = new QWidget(m_pages);
  auto* bp = new QVBoxLayout(branchPage);
  bp->setContentsMargins(0, 0, 0, 0);
  bp->setSpacing(6);
  m_branches = makeTree(branchPage, "versionBranches", 3);
  m_branches->setAccessibleName(tr("Branches"));
  m_branches->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_branches->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_branches->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  bp->addWidget(m_branches, 1);
  auto* bb = new QWidget(branchPage);
  auto* bbl = new QHBoxLayout(bb);
  bbl->setContentsMargins(0, 0, 0, 0);
  bbl->setSpacing(6);
  connect(addButton(bb, "vcsSwitch", tr("Switch")), &QPushButton::clicked, this, [this] {
    if (const git::Branch* br = branchOf(m_branches->currentItem())) m_vc->switchTo(*br);
  });
  connect(addButton(bb, "vcsMerge", tr("Merge into current…"), "merge"), &QPushButton::clicked, this, [this] {
    if (const git::Branch* br = branchOf(m_branches->currentItem())) m_vc->mergeBranch(br->name);
  });
  connect(addButton(bb, "vcsDelete", tr("Delete…")), &QPushButton::clicked, this, [this] {
    if (const git::Branch* br = branchOf(m_branches->currentItem())) m_vc->deleteBranch(br->name);
  });
  bbl->addStretch(1);
  bp->addWidget(bb);
  m_pages->addWidget(branchPage);
  b->addWidget(m_pages, 1);
  m_stack->addWidget(body);
  // ---- no repository (or no git): what can be done about it
  auto* empty = new QWidget(m_stack);
  auto* e = new QVBoxLayout(empty);
  e->setContentsMargins(16, 16, 16, 16);
  e->setSpacing(10);
  m_emptyText = new QLabel(empty);
  m_emptyText->setObjectName("versionEmpty");
  m_emptyText->setWordWrap(true);
  e->addWidget(m_emptyText);
  auto* eb = new QWidget(empty);
  auto* ebl = new QVBoxLayout(eb);
  ebl->setContentsMargins(0, 0, 0, 0);
  ebl->setSpacing(6);
  GitWatch* git = m_vc->git();
  connect(addButton(eb, "vcsSetUp", tr("Set up repository…"), "git"), &QPushButton::clicked, git, &GitWatch::setUp);
  connect(addButton(eb, "vcsClone", tr("Clone repository…")), &QPushButton::clicked, git, &GitWatch::cloneRepository);
  connect(addButton(eb, "vcsLocate", tr("Locate git…")), &QPushButton::clicked, git, &GitWatch::locateGit);
  connect(addButton(eb, "vcsTrust", tr("Trust this folder…")), &QPushButton::clicked, git, &GitWatch::trustFolder);
  connect(addButton(eb, "vcsRefresh", tr("Refresh")), &QPushButton::clicked, git, [git] {
    git::forgetTools();
    git->refresh(true);
  });
  e->addWidget(eb);
  e->addStretch(1);
  m_stack->addWidget(empty);
  m_footer = new PanelFooter(this);
  m_footer->setCancel(tr("Close"));
  m_footer->setPrimary(tr("Commit…"), QString());
  m_footer->setHint(tr("Right-click for more"));
  v->addWidget(m_footer);
  connect(m_footer, &PanelFooter::accepted, this, [this] { m_vc->commit(); });
  connect(m_footer, &PanelFooter::cancelled, this, &VersionPanel::closeRequested);
  connect(m_historyTab, &QToolButton::clicked, this, [this] { setPage(VersionControl::History); });
  connect(m_branchesTab, &QToolButton::clicked, this, [this] { setPage(VersionControl::Branches); });
  connect(m_history, &QTreeWidget::currentItemChanged, this, &VersionPanel::updateButtons);
  connect(m_branches, &QTreeWidget::currentItemChanged, this, &VersionPanel::updateButtons);
  connect(m_history, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
    if (item && item->data(0, Qt::UserRole).toInt() == -1) m_vc->moreHistory();
    else if (const git::Commit* c = commitOf(item)) m_vc->compareWith(*c);
  });
  connect(m_branches, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
    if (const git::Branch* br = branchOf(item)) m_vc->switchTo(*br);
  });
  connect(m_history, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
    const git::Commit* c = commitOf(m_history->itemAt(at));
    if (!c) return;
    QMenu* m = commitMenu(*c);
    m->setAttribute(Qt::WA_DeleteOnClose);
    m->popup(m_history->viewport()->mapToGlobal(at));
  });
  connect(m_branches, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
    const git::Branch* br = branchOf(m_branches->itemAt(at));
    if (!br) return;
    QMenu* m = branchMenu(*br);
    m->setAttribute(Qt::WA_DeleteOnClose);
    m->popup(m_branches->viewport()->mapToGlobal(at));
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, [this, mark] {
    mark->setPixmap(icons::pixmap("branch", theme::current().fg2, 16, devicePixelRatioF()));
    for (QPushButton* b : findChildren<QPushButton*>())
      if (const QString icon = b->property("icon").toString(); !icon.isEmpty()) b->setIcon(icons::themed(icon, 16));
    showState();
    showLists();
  });
  for (QPushButton* p : findChildren<QPushButton*>()) {  // the icon names, for a theme switch
    static const QHash<QString, QString> named = {{"vcsPull", "pull"}, {"vcsPush", "push"}, {"vcsNewBranch", "branch"}, {"vcsCompare", "compare"},
                                                  {"vcsBranchFrom", "branch"}, {"vcsMerge", "merge"}, {"vcsSetUp", "git"}};
    p->setProperty("icon", named.value(p->property("action").toString()));
  }
  mark->setPixmap(icons::pixmap("branch", theme::current().fg2, 16, devicePixelRatioF()));
  setPage(VersionControl::History);
  showState();
}

void VersionPanel::setPage(int page) {
  m_pages->setCurrentIndex(page == VersionControl::Branches ? 1 : 0);
  (page == VersionControl::Branches ? m_branchesTab : m_historyTab)->setChecked(true);
  updateButtons();
}

int VersionPanel::page() const { return m_pages->currentIndex() == 1 ? VersionControl::Branches : VersionControl::History; }

bool VersionPanel::showsRepository() const { return m_stack->currentIndex() == 0; }

const git::Commit* VersionPanel::commitOf(QTreeWidgetItem* item) const {
  if (!item) return nullptr;
  const int i = item->data(0, Qt::UserRole).toInt();
  const auto& shown = m_vc->shownHistory();
  return i >= 0 && size_t(i) < shown.size() && item->data(1, Qt::UserRole).toString() == shown[size_t(i)].hash ? &shown[size_t(i)] : nullptr;
}

const git::Branch* VersionPanel::branchOf(QTreeWidgetItem* item) const {
  if (!item || !item->data(0, Qt::UserRole + 1).toBool()) return nullptr;
  const int i = item->data(0, Qt::UserRole).toInt();
  return i >= 0 && size_t(i) < m_vc->branches().size() && item->data(1, Qt::UserRole).toString() == m_vc->branches()[size_t(i)].ref ? &m_vc->branches()[size_t(i)]
                                                                                                                                    : nullptr;
}

QMenu* VersionPanel::commitMenu(const git::Commit& c) {
  auto* m = new QMenu(this);
  m->setObjectName("versionCommitMenu");
  const git::Commit commit = c;  // the lists may be read again while the menu is open
  auto add = [this, m](const char* id, const QString& text, std::function<void()> fn) {
    QAction* a = m->addAction(text);
    a->setObjectName(QString::fromLatin1(id));
    connect(a, &QAction::triggered, this, fn);
    return a;
  };
  add("vcs.compareWith", tr("Compare with this session"), [this, commit] { m_vc->compareWith(commit); });
  add("vcs.comparePrevious", tr("Compare with the commit before"), [this, commit] { m_vc->comparePrevious(commit); })->setEnabled(!commit.parents.isEmpty());
  add("vcs.openReadOnly", tr("Open read-only in a new window"), [this, commit] { m_vc->openReadOnly(commit); });
  m->addSeparator();
  add("vcs.restore", tr("Restore as new changes…"), [this, commit] { m_vc->restore(commit); });
  add("vcs.branchFrom", tr("New branch from here…"), [this, commit] { m_vc->newBranch(commit.hash, tr("%1 · %2").arg(commit.shortHash, commit.subject)); });
  m->addSeparator();
  add("vcs.copyHash", tr("Copy hash"), [commit] { QApplication::clipboard()->setText(commit.hash); });
  return m;
}

QMenu* VersionPanel::branchMenu(const git::Branch& br) {
  auto* m = new QMenu(this);
  m->setObjectName("versionBranchMenu");
  const git::Branch branch = br;
  auto add = [this, m](const char* id, const QString& text, std::function<void()> fn) {
    QAction* a = m->addAction(text);
    a->setObjectName(QString::fromLatin1(id));
    connect(a, &QAction::triggered, this, fn);
    return a;
  };
  add("vcs.switch", tr("Switch to %1").arg(branch.name), [this, branch] { m_vc->switchTo(branch); })->setEnabled(!branch.head);
  add("vcs.merge", tr("Merge into current…"), [this, branch] { m_vc->mergeBranch(branch.name); })->setEnabled(!branch.head);
  add("vcs.branchFrom", tr("New branch from here…"), [this, branch] { m_vc->newBranch(branch.name, branch.name); });
  m->addSeparator();
  add("vcs.deleteBranch", tr("Delete %1…").arg(branch.name), [this, branch] { m_vc->deleteBranch(branch.name); })->setEnabled(!branch.head && !branch.remote);
  return m;
}

void VersionPanel::showState() {
  using S = git::Repo::State;
  using D = git::Repo::Doc;
  const git::Repo& r = m_vc->git()->repo();
  const Tokens& t = theme::current();
  AppDocument* doc = m_vc->services().document();
  const bool saved = doc && doc->hasDocument && !doc->browse && !doc->doc.path.empty();
  QStringList show;  // the empty page's buttons
  QString why;
  if (!saved) why = tr("Save the document as an OPAD file to keep its versions with git.");
  else if (doc->readOnly) why = tr("%1 is open read-only. Save a copy to commit, branch or restore versions of it.").arg(QFileInfo(doc->path()).fileName());
  else
    switch (r.state) {
      case S::None: why = tr("Reading the repository…"); break;
      case S::GitMissing:
        why = tr("git was not found on this computer. OPAD keeps versions with git: install it (git-scm.com), or show OPAD where it is.");
        show << "vcsLocate" << "vcsRefresh";
        break;
      case S::NotRepo:
        why = tr("%1 is not in a git repository yet. Set one up in its folder to keep its versions, branch and merge.").arg(QFileInfo(r.file).fileName());
        show << "vcsSetUp" << "vcsClone";
        break;
      case S::Untrusted:
        why = r.error;
        show << "vcsTrust";
        break;
      case S::Failed:
        why = r.error;
        show << "vcsRefresh";
        break;
      case S::Ready: break;
    }
  m_stack->setCurrentIndex(why.isEmpty() ? 0 : 1);
  m_footer->setPrimaryVisible(why.isEmpty());
  m_footer->setHint(why.isEmpty() ? tr("Right-click for more") : QString());
  if (!why.isEmpty()) {
    m_emptyText->setText(why);
    for (const char* id : {"vcsSetUp", "vcsClone", "vcsLocate", "vcsTrust", "vcsRefresh"})
      if (QPushButton* b = button(QString::fromLatin1(id))) b->setVisible(show.contains(QString::fromLatin1(id)));
    return;
  }
  const git::Status& s = r.status;
  m_branch->setText(s.branch == "(detached)" ? tr("detached at %1").arg(s.oid.left(7)) : s.branch);
  QStringList sync;
  if (s.upstream.isEmpty()) sync << span(t.fg3, m_vc->remotes().isEmpty() ? tr("no remote yet") : tr("not pushed yet"));
  else if (!s.tracking) sync << span(t.amber, tr("%1 is gone").arg(s.upstream));
  else {
    sync << span(t.fg3, tr("follows %1").arg(s.upstream));
    if (s.ahead) sync << span(t.fg2, tr("%n to push", nullptr, s.ahead));
    if (s.behind) sync << span(t.amber, tr("%n to pull", nullptr, s.behind));
    if (!s.ahead && !s.behind) sync << span(t.fg3, tr("in step"));
  }
  m_sync->setText(sync.join(QStringLiteral(" · ")));
  const QString name = QFileInfo(r.file).fileName().toHtmlEscaped();
  QString state;
  if (doc->isDirty()) state = span(t.amber, tr("unsaved changes"));
  else
    switch (r.doc()) {
      case D::Untracked: state = span(t.amber, tr("not committed yet")); break;
      case D::Added:
      case D::Modified: state = span(t.amber, tr("changes not committed")); break;
      case D::Conflict: state = span(t.red, tr("in conflict")); break;
      case D::Ignored: state = span(t.fg3, tr("ignored by git")); break;
      default: state = span(t.fg3, s.oid == "(initial)" ? tr("no commits yet") : tr("committed")); break;
    }
  m_doc->setText(QStringLiteral("<b>%1</b> · %2").arg(name, state));
  m_doc->setProperty("state", doc->isDirty() ? QStringLiteral("unsaved") : QString());
  const int conflicts = s.count('u');
  m_mergeBar->setVisible(r.merging);
  if (r.merging)
    m_mergeText->setText(conflicts ? span(t.red, tr("A merge stopped on changes both sides made. Files in conflict: %n.", nullptr, conflicts))
                                   : span(t.fg, tr("A merge is ready: commit it, or abort it.")));
  if (QPushButton* b = button("vcsCommitMerge")) b->setEnabled(r.merging && !conflicts);
  if (QPushButton* b = button("vcsResolve")) b->setVisible(r.merging && r.doc() == D::Conflict);
  updateButtons();
}

void VersionPanel::updateButtons() {
  const git::Repo& r = m_vc->git()->repo();
  const git::Status& s = r.status;
  const bool ready = m_vc->ready(), branch = ready && s.branch != "(detached)";
  auto enable = [this](const char* action, bool on) {
    if (QPushButton* b = button(QString::fromLatin1(action))) b->setEnabled(on);
  };
  enable("vcsPull", ready && !s.upstream.isEmpty() && !r.merging);
  enable("vcsPush", branch && s.oid != "(initial)" && !r.merging);
  enable("vcsNewBranch", ready && s.oid != "(initial)");
  const git::Commit* c = commitOf(m_history->currentItem());
  enable("vcsCompare", c);
  enable("vcsRestore", c && !r.merging);
  enable("vcsBranchFrom", c);
  const git::Branch* br = branchOf(m_branches->currentItem());
  enable("vcsSwitch", br && !br->head && !r.merging);
  enable("vcsMerge", br && !br->head && !r.merging && s.oid != "(initial)");
  enable("vcsDelete", br && !br->head && !br->remote);
}

void VersionPanel::showLists() {
  const Tokens& t = theme::current();
  const auto& history = m_vc->shownHistory();
  const QString filter = m_vc->historyFilter();
  m_filterBar->setVisible(!filter.isEmpty());
  m_filterText->setText(tr("Only the commits that touched %1").arg(filter));
  const QString keep = m_history->currentItem() ? m_history->currentItem()->data(1, Qt::UserRole).toString() : QString();
  m_history->clear();
  QTreeWidgetItem* current = nullptr;
  for (size_t i = 0; i < history.size(); ++i) {
    const git::Commit& c = history[i];
    auto* item = new QTreeWidgetItem(m_history);
    item->setText(0, c.shortHash);
    item->setFont(0, theme::mono(12));
    item->setForeground(0, t.fg3);
    QStringList refs;
    for (const QString& ref : c.refs)
      if (ref != "HEAD" && !ref.endsWith("/HEAD")) refs << (ref.startsWith("HEAD -> ") ? ref.mid(8) : ref);  // a remote's HEAD: an alias
    item->setText(1, refs.isEmpty() ? c.subject : QStringLiteral("[%1] %2").arg(refs.join(", "), c.subject));
    item->setText(2, ago(c.date));
    item->setForeground(2, t.fg3);
    item->setToolTip(1, tr("%1\n%2 <%3>, %4\n\n%5").arg(c.hash, c.author, c.email, i18n::localTime(c.date.toStdString()), c.subject) +
                            (c.refs.isEmpty() ? QString() : "\n" + c.refs.join(", ")));
    item->setData(0, Qt::UserRole, int(i));
    item->setData(1, Qt::UserRole, c.hash);
    if (c.refs.contains("HEAD") || std::any_of(c.refs.begin(), c.refs.end(), [](const QString& r) { return r.startsWith("HEAD -> "); })) {
      QFont f = item->font(1);
      f.setBold(true);
      item->setFont(1, f);
    }
    if (c.hash == keep) current = item;
  }
  if (history.empty()) {
    auto* none = new QTreeWidgetItem(m_history);
    none->setText(1, !filter.isEmpty() ? (m_vc->historyFilterRead() ? tr("No commit touched it yet.") : tr("Reading who changed what in git…"))
                     : m_vc->ready() ? tr("No commits of this document yet: Commit… makes the first.") : QString());
    none->setForeground(1, t.fg3);
    none->setFlags(Qt::NoItemFlags);
    none->setData(0, Qt::UserRole, -2);
  }
  if (m_vc->hasMoreHistory()) {
    auto* more = new QTreeWidgetItem(m_history);
    more->setText(1, tr("Show older commits…"));
    more->setForeground(1, t.sel);
    more->setData(0, Qt::UserRole, -1);
  }
  if (current) m_history->setCurrentItem(current);
  const auto& branches = m_vc->branches();
  const QString keepRef = m_branches->currentItem() ? m_branches->currentItem()->data(1, Qt::UserRole).toString() : QString();
  m_branches->clear();
  QTreeWidgetItem* currentBranch = nullptr;
  bool remoteHeader = false;
  auto header = [this, &t](const QString& text) {
    auto* h = new QTreeWidgetItem(m_branches);
    h->setText(0, text);
    h->setFlags(Qt::ItemIsEnabled);
    h->setForeground(0, t.fg3);
    QFont f = h->font(0);
    f.setPointSizeF(f.pointSizeF() * 0.9);
    f.setBold(true);
    h->setFont(0, f);
  };
  header(tr("Branches here"));
  for (size_t i = 0; i < branches.size(); ++i) {
    const git::Branch& br = branches[i];
    if (br.remote && !std::exchange(remoteHeader, true)) header(tr("On remotes"));
    auto* item = new QTreeWidgetItem(m_branches);
    item->setText(0, (br.head ? QStringLiteral("● ") : QStringLiteral("    ")) + br.name);
    if (br.head) {
      QFont f = item->font(0);
      f.setBold(true);
      item->setFont(0, f);
    }
    QString track;
    if (br.gone) track = tr("upstream gone");
    else if (br.ahead || br.behind) track = (br.ahead ? tr("↑%1").arg(br.ahead) : QString()) + (br.ahead && br.behind ? " " : "") + (br.behind ? tr("↓%1").arg(br.behind) : QString());
    item->setText(1, track);
    item->setForeground(1, br.gone ? t.amber : t.fg2);
    item->setText(2, ago(br.date));
    item->setForeground(2, t.fg3);
    if (br.remote) item->setForeground(0, t.fg2);
    item->setToolTip(0, tr("%1\n%2\nLast commit: %3").arg(br.name, br.upstream.isEmpty() ? (br.remote ? tr("a branch of the remote") : tr("follows no remote branch"))
                                                                                         : tr("follows %1").arg(br.upstream),
                                                         br.subject));
    item->setData(0, Qt::UserRole, int(i));
    item->setData(0, Qt::UserRole + 1, true);
    item->setData(1, Qt::UserRole, br.ref);
    if (br.ref == keepRef) currentBranch = item;
  }
  if (currentBranch) m_branches->setCurrentItem(currentBranch);
  updateButtons();
}
