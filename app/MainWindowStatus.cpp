// The status bar (path, git, hover, selection, snapping toggles, progress) and git: branch state, an op's git log.
#include "MainWindow.hpp"

#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "I18n.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "Units.hpp"

void MainWindow::buildStatusBar() {
  const Tokens& t = theme::current();
  m_statusPath = new QLabel(this);
  m_statusPath->setFont(theme::mono(12));
  m_statusPath->setContentsMargins(12, 2, 4, 2);
  m_statusGitIcon = new QLabel(this);
  m_statusGitIcon->setPixmap(icons::pixmap("git", t.fg2, 14, devicePixelRatioF()));
  m_statusGit = new QLabel(this);
  m_statusGit->setTextFormat(Qt::RichText);
  m_statusHover = new QLabel(this);
  m_statusHover->setAlignment(Qt::AlignCenter);
  m_statusHover->setObjectName("tertiary");
  m_statusSel = new QLabel(this);
  buildUnitsButton();
  m_progress = new ProgressStrip(this);
  m_jobs = new JobRunner(m_progress, this);
  m_viewport->setJobs(m_jobs);
  statusBar()->addWidget(m_statusPath);
  statusBar()->addWidget(m_statusGitIcon);
  statusBar()->addWidget(m_statusGit);
  // Permanent: QStatusBar hides normal widgets while a temporary message shows and re-shows them after,
  // which fought with the strip's own show/hide and drew the message across the bars.
  statusBar()->addPermanentWidget(m_statusHover, 1);
  statusBar()->addPermanentWidget(m_progress, 1);
  struct Toggle { const char* id; const char* label; const char* icon; const char* key; const char* setting; bool defaultOn; };
  for(const auto& spec : {Toggle{"view.extensions","Extensions","extensions","F11","view/extensions",true},
      Toggle{"view.tracking","Tracking","tracking","F12","view/tracking",true},
      Toggle{"view.gridSnap","Grid snapping","grid","F9","view/gridSnap",false},
      Toggle{"view.orthoSnap","Ortho mode","orthoSnap","F8","view/orthoSnap",false}}) {
    CommandInfo info{spec.id,tr(spec.label),spec.icon,QKeySequence(spec.key)};info.checkable=true;
    if(info.id=="view.orthoSnap")info.keywords={tr("orthogonal"),tr("horizontal vertical lock")};
    auto* a=addCommand(info,[] {});
    a->setChecked(m_settings.value(spec.setting,spec.defaultOn).toBool());
    auto apply=[this,spec](bool on) {
      m_settings.setValue(spec.setting,on);
      if(QString(spec.id)=="view.extensions") m_viewport->setExtensionTracking(on);
      else if(QString(spec.id)=="view.tracking") m_viewport->setTracking(on);
      else if(QString(spec.id)=="view.gridSnap") m_viewport->setGridSnap(on);
      if(m_design && m_design->sketch()) m_design->sketch()->refreshSnap();  // the sketch reads them once (UI-27), again now
    };
    connect(a,&QAction::toggled,this,apply); apply(a->isChecked());
    auto* button=new QToolButton(this); button->setDefaultAction(a); button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAccessibleName(tr(spec.label)); button->setIconSize({18,18}); button->setFixedSize(30,26);
    auto paint=[button,a,spec] {
      const auto& t=theme::current();
      a->setIcon(icons::icon(spec.icon,a->isChecked()?t.onsel:t.fg2));
      button->setStyleSheet(QString("QToolButton { border: 1px solid %1; border-radius: 3px; background: %2; } QToolButton:checked { background: %3; border: 2px solid %3; } QToolButton:hover { border-color: %3; }").arg(t.line.name(),t.bg2.name(),t.sel.name()));
    };
    connect(theme::notifier(),&theme::Notifier::changed,button,paint); connect(a,&QAction::toggled,button,paint); paint();
    button->setFocusPolicy(Qt::NoFocus); statusBar()->addPermanentWidget(button);
  }
  // One grid snapping switch: the sketch panel's checkbox turns the viewport's, and F9 follows (and saves it).
  connect(m_viewport,&Viewport::gridSnapChanged,this,[this](bool on){action("view.gridSnap")->setChecked(on);});
  connect(m_viewport,&Viewport::gridShownChanged,action("view.grid"),&QAction::setChecked);  // G shows the sketch's own grid state in a sketch
  statusBar()->addPermanentWidget(m_statusSel);
  statusBar()->addPermanentWidget(m_statusUnits);
  statusBar()->setSizeGripEnabled(false);
  connect(m_jobs, &JobRunner::stripShown, this, [this](bool shown) { m_statusHover->setVisible(!shown); });  // free room for the bars
  for (AreaController* area : m_areas) area->statusWidgets(statusBar());
}

// The shown length unit (UI-123), live: the document's units op, or the session's in viewer mode. A click offers the
// document unit (one units op through the design, so expressions without a unit regenerate) and the display precision.
void MainWindow::buildUnitsButton() {
  units::loadSettings();
  m_statusUnits = new QToolButton(this);
  m_statusUnits->setObjectName("statusUnits");
  m_statusUnits->setAutoRaise(true);
  m_statusUnits->setPopupMode(QToolButton::InstantPopup);
  m_statusUnits->setFocusPolicy(Qt::NoFocus);
  m_statusUnits->setStyleSheet("QToolButton#statusUnits { padding: 2px 14px 2px 8px; } QToolButton#statusUnits::menu-indicator { image: none; width: 0; }");
  auto* menu = new QMenu(m_statusUnits);
  m_statusUnits->setMenu(menu);
  auto shown = [this] {
    const auto& d = units::current();
    m_statusUnits->setText(units::symbol(units::Kind::Length));
    QString tip = tr("Lengths are shown in %1. Click to change the document unit or the precision.").arg(units::unitName(d.length).toLower());
    if (!units::sessionUnit().empty()) tip += '\n' + tr("Viewer mode: shown in %1 for this session; the file says %2.").arg(units::unitName(d.length).toLower(), units::unitName(units::documentUnit()).toLower());
    m_statusUnits->setToolTip(tip);
    m_statusUnits->setAccessibleName(tr("Units: %1").arg(units::unitName(d.length)));
  };
  connect(menu, &QMenu::aboutToShow, this, [this, menu] {
    qDeleteAll(menu->findChildren<QMenu*>(Qt::FindDirectChildrenOnly));  // clear() keeps submenus: they own their action
    menu->clear();
    const auto& d = units::current();
    menu->addSection(m_doc->browse ? tr("Show lengths in") : tr("Document unit"));
    for (const QString& unit : units::lengthUnits()) {
      const std::string u = unit.toStdString();
      QAction* a = menu->addAction(QString("%1 (%2)").arg(units::unitName(u), units::symbol(units::Kind::Length, units::Display{u})));
      a->setObjectName("unit." + unit);
      a->setCheckable(true);
      a->setChecked(d.length == u);
      a->setEnabled(m_doc->hasDocument);
      connect(a, &QAction::triggered, this, [this, u] { setDocumentUnit(u); });
    }
    menu->addSection(tr("Precision"));
    auto* decimals = menu->addMenu(tr("Decimal places"));
    for (int n = 0; n <= 6; ++n) {
      QAction* a = decimals->addAction(QString::number(n));
      a->setCheckable(true);
      a->setChecked(d.decimals == n);
      connect(a, &QAction::triggered, this, [n] { units::setPrecision(n, units::current().radians, units::current().fraction); });
    }
    QAction* fractions = menu->addAction(tr("Fractions of an inch (1/64)"));
    fractions->setCheckable(true);
    fractions->setChecked(d.fraction > 0);
    fractions->setEnabled(d.length == "in");
    connect(fractions, &QAction::toggled, this, [](bool on) { units::setPrecision(units::current().decimals, units::current().radians, on ? 64 : 0); });
    QAction* radians = menu->addAction(tr("Angles in radians"));
    radians->setCheckable(true);
    radians->setChecked(d.radians);
    connect(radians, &QAction::toggled, this, [](bool on) { units::setPrecision(units::current().decimals, on, units::current().fraction); });
  });
  connect(m_doc, &AppDocument::aboutToReplace, this, [] { units::setSessionUnit({}); });
  connect(m_doc, &AppDocument::changed, this, [this] {
    units::setDocumentUnit(m_doc->hasDocument ? m_doc->scene.units : m_doc->doc.header.units);
    if (!m_doc->browse) units::setSessionUnit({});  // viewer mode left (Edit unsaved copy, an import): the file's unit
  });
  connect(units::notifier(), &units::Notifier::changed, this, [this, shown] {
    shown();
    refreshToolUi();  // the result rows of a guided tool
    updateChips();    // the section chip
  });
  units::setDocumentUnit(m_doc->scene.units);
  shown();
}

// A viewer-mode file is not changed: it is shown in that unit for the session. A document gets a units op, applied
// through the design so that values typed without a unit are read in the new one (and regenerate).
void MainWindow::setDocumentUnit(const std::string& unit) {
  if (!m_doc->hasDocument) return;
  if (m_doc->browse) {
    units::setSessionUnit(unit == units::documentUnit() ? std::string() : unit);
    statusBar()->showMessage(tr("Lengths are shown in %1; the file is not changed (viewer mode).").arg(units::unitName(unit).toLower()), 6000);
    return;
  }
  if (unit == m_doc->scene.units) return;
  m_design->applyOps({opad::json{{"op", "units"}, {"length", unit}}}, tr("Change document units"));
}

void MainWindow::updateTitle() {
  setWindowTitle(m_doc->title());
  QString path = m_doc->hasDocument ? (m_doc->browse ? tr("Viewer (read-only): %1").arg(QDir::toNativeSeparators(m_doc->viewing)) : (m_doc->path().isEmpty() ? tr("unsaved document") : m_doc->path())) : tr("No document");
  if (!m_doc->scene.unresolved.empty()) path += tr("   ·   %1 unresolved").arg(m_doc->scene.unresolved.size());
  m_statusPath->setText(path);
  // What is unresolved and why: a newer build's records in one sentence, the others by op type and reason.
  QStringList tip;
  if (const QString newer = newerRecords(); !newer.isEmpty()) tip << newer;
  int others = 0;
  for (const auto& u : m_doc->scene.unresolved)
    if (opad::Document::known_type(u.op_type) && ++others <= 10) tip << QString("%1: %2").arg(QString::fromStdString(u.op_type), i18n::t(QString::fromStdString(u.reason)));
  if (others > 10) tip << tr("… and %1 more").arg(others - 10);
  m_statusPath->setToolTip(tip.join('\n'));
  if (!m_doc->hasDocument) m_statusHover->setText(tr("File › Open a design file (OPAD, STEP, STL, 3MF, DXF, …), or drop one here"));
  else if (m_statusHover->text() == tr("File › Open a design file (OPAD, STEP, STL, 3MF, DXF, …), or drop one here")) m_statusHover->clear();
}

QString MainWindow::newerRecords() const {
  int count = 0;
  QStringList types;
  for (const auto& op : m_doc->doc.ops)
    if (!opad::Document::known_type(op.type)) {
      ++count;
      if (!types.contains(QString::fromStdString(op.type))) types << QString::fromStdString(op.type);
    }
  return count ? tr("This file has %1 records from a newer OPAD (%2); they are kept and saved back unchanged.").arg(count).arg(types.join(", ")) : QString();
}

// ---------------------------------------------------------------- git status (F33)
void MainWindow::refreshGit() {
  const Tokens& t = theme::current();
  if (!m_doc->hasDocument || m_doc->browse || m_doc->doc.path.empty()) {
    m_statusGit->clear();
    m_statusGitIcon->hide();
    return;
  }
  // git is queried asynchronously: waiting for it here blocked the UI for up to 0.8 s per query.
  QFileInfo fi(m_doc->path());
  auto notInGit = [this, t] {
    m_statusGitIcon->show();
    m_statusGit->setText(QString("<span style='color:%1'>%2</span>").arg(t.fg3.name(), tr("not in git")));
  };
  auto* git = new QProcess(this);
  git->setWorkingDirectory(fi.absolutePath());
  connect(git, &QProcess::errorOccurred, this, [git, notInGit](QProcess::ProcessError) { git->deleteLater(); notInGit(); });
  connect(git, &QProcess::finished, this, [this, git, fi, t, notInGit](int code, QProcess::ExitStatus) {
    git->deleteLater();
    if (code != 0) { notInGit(); return; }
    const QString branch = QString::fromUtf8(git->readAllStandardOutput()).trimmed();
    auto* st = new QProcess(this);
    st->setWorkingDirectory(fi.absolutePath());
    connect(st, &QProcess::errorOccurred, this, [st](QProcess::ProcessError) { st->deleteLater(); });
    connect(st, &QProcess::finished, this, [this, st, branch, t](int, QProcess::ExitStatus) {
      st->deleteLater();
      const QString status = QString::fromUtf8(st->readAllStandardOutput()).trimmed();
      const QString state = status.isEmpty() ? QString() : status.startsWith("??") ? tr("untracked") : tr("modified");
      m_statusGitIcon->show();
      m_statusGit->setText(branch.toHtmlEscaped() + (state.isEmpty() ? QString() : QString(" <span style='color:%1'>· %2</span>").arg(t.amber.name(), state)));
    });
    st->start("git", {"status", "--porcelain", "--", fi.fileName()});
  });
  git->start("git", {"rev-parse", "--abbrev-ref", "HEAD"});
}

void MainWindow::showOpGitLog(const std::string& opId,const QString& path) {
  auto* dialog=new QDialog(this); dialog->setObjectName("opGitLog"); dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(tr("git log for op %1").arg(QString::fromStdString(opId.substr(0,8)))); dialog->resize(700,400);
  auto* layout=new QVBoxLayout(dialog); auto* output=new QPlainTextEdit(dialog); output->setReadOnly(true); layout->addWidget(output);
  dialog->show();
  if(path.isEmpty()) { output->setPlainText(tr("Save the document in a git repository first.")); dialog->setProperty("finished",true); return; }
  output->setPlainText(tr("Reading Git history..."));
  auto* git=new QProcess(this); const QFileInfo file(path); git->setWorkingDirectory(file.absolutePath());
  auto* timeout=new QTimer(git); timeout->setSingleShot(true);
  connect(timeout,&QTimer::timeout,git,[git] {git->setProperty("timedOut",true);git->kill();});
  connect(dialog,&QObject::destroyed,git,[git] { if(git->state()!=QProcess::NotRunning) git->kill(); });
  connect(git,&QProcess::errorOccurred,dialog,[=](QProcess::ProcessError error) {
    if(error==QProcess::FailedToStart) { output->setPlainText(git->errorString()); dialog->setProperty("finished",true); git->deleteLater(); }
  });
  connect(git,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),dialog,[=](int code,QProcess::ExitStatus status) {
    timeout->stop();
    QString text=QString::fromUtf8(git->readAllStandardOutput()).trimmed();
    if(git->property("timedOut").toBool()) text=tr("Git history timed out.");
    else if(code!=0 || status!=QProcess::NormalExit) text=QString::fromUtf8(git->readAllStandardError()).trimmed();
    else if(text.isEmpty()) text=tr("Not committed yet.");
    output->setPlainText(text); dialog->setProperty("finished",true);
  });
  connect(git,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),git,&QObject::deleteLater);
  git->start("git",{"log","--format=%h %ad %an  %s","--date=short","-S",QString::fromStdString(opId),"--",file.fileName()});
  timeout->start(10000);
}
