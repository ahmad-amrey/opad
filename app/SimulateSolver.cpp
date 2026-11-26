#include "SimulateSolver.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#include "EngineInstall.hpp"
#include "SimChecks.hpp"
#include "Theme.hpp"

namespace {
QLabel* para(const QString& s, QWidget* parent, const char* object = "") {
  auto* l = new QLabel(s, parent);
  l->setWordWrap(true);
  l->setObjectName(object);
  l->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return l;
}
}  // namespace

SolverWindow::SolverWindow(const QString& solver, std::function<void()> found, QWidget* parent, std::function<void()> install)
    : QDialog(parent), m_id(solver), m_found(std::move(found)) {
  setObjectName("solverWindow");
  const simchecks::Solver* s = simchecks::solver(solver);
  const QString name = s ? s->name : solver;
  // OPAD's own pack of it (opad/sim/engine_packs.hpp), when this OPAD has one for this system and someone can install it.
  const auto pack = s && s->external && install ? engineinstall::pack(solver) : std::nullopt;
  setWindowTitle(tr("%1 is not installed").arg(name));
  setMinimumWidth(480);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(20, 18, 20, 16);
  v->setSpacing(12);
  auto* title = new QLabel(solver == "openfoam" ? tr("This check needs the airflow solver") : tr("This check needs %1").arg(name), this);
  QFont f = title->font();
  f.setBold(true);
  f.setPointSizeF(f.pointSizeF() * 1.25);
  title->setFont(f);
  v->addWidget(title);
  if (!s) {
    v->addWidget(para(tr("OPAD does not know where to get it."), this));
  } else if (s->external && pack) {
    v->addWidget(para(tr("OPAD works this out with %1. %2 It is not on this computer yet.").arg(s->name, s->what), this));
    auto* facts = new QLabel(this);
    facts->setObjectName("solverFacts");
    facts->setWordWrap(true);
    facts->setText(tr("<b>From:</b> %1<br><b>Size:</b> %2<br><b>Used for:</b> %3")
                       .arg(tr("OPAD's own releases on GitHub, built from %1's official source").arg(s->name).toHtmlEscaped(),
                            engineinstall::sizeText(pack->size).toHtmlEscaped(), s->usedFor.toHtmlEscaped()));
    v->addWidget(facts);
    v->addWidget(para(tr("Install downloads %1, checks that it is exactly the file OPAD expects and installs it for you only: no "
                         "administrator rights, nothing else on the computer changes. %2 is free software (GNU GPL), not made by "
                         "OPAD: see Help > Third-party licences.")
                          .arg(QString::fromStdString(pack->name), s->name),
                      this, "secondary"));
  } else if (s->external) {
    v->addWidget(para(tr("OPAD works this out with %1. %2 It is not on this computer yet.").arg(s->name, s->what), this));
    auto* facts = new QLabel(this);
    facts->setObjectName("solverFacts");
    facts->setWordWrap(true);
    facts->setText(tr("<b>From:</b> %1<br><b>Size:</b> about %2<br><b>Used for:</b> %3").arg(s->from.toHtmlEscaped(), s->size.toHtmlEscaped(), s->usedFor.toHtmlEscaped()));
    v->addWidget(facts);
    v->addWidget(para(tr("You can finish setting up the check now; it runs as soon as the solver is in place. The button opens the "
                         "solver's official download page."),
                      this, "secondary"));
  } else {
    v->addWidget(para(tr("%1 %2 It is part of OPAD itself, and this build of OPAD was made without it.").arg(s->name, s->what), this));
    v->addWidget(para(tr("OPAD's releases include it: install the latest release to use %1.").arg(s->usedFor), this, "secondary"));
  }
  m_note = para({}, this);
  m_note->setObjectName("solverNote");
  m_note->hide();
  v->addWidget(m_note);
  auto* buttons = new QHBoxLayout();
  if (s && s->external) {
    auto* have = new QPushButton(tr("I already have it: find it"), this);
    have->setObjectName("solverFind");
    have->setFlat(true);
    buttons->addWidget(have);
    connect(have, &QPushButton::clicked, this, &SolverWindow::find);
  }
  buttons->addStretch(1);
  auto* later = new QPushButton(tr("Later"), this);
  later->setObjectName("solverLater");
  buttons->addWidget(later);
  connect(later, &QPushButton::clicked, this, &QDialog::reject);
  if (s) {
    // With a pack the official page is the second way (flat); without one it is the way.
    auto* get = new QPushButton(pack ? tr("Official page") : s->external ? tr("Download and install") : tr("Get OPAD's latest release"), this);
    get->setObjectName("solverGet");
    get->setFlat(bool(pack));
    get->setDefault(!pack);
    buttons->insertWidget(pack ? 1 : buttons->count(), get);
    connect(get, &QPushButton::clicked, this, [this, url = s->url] {
      QDesktopServices::openUrl(QUrl(url));  // the official page, in the browser: the user downloads and installs
      m_note->setText(tr("The download page is open in your browser. Once it is installed, come back and choose I already have it: find it (or restart OPAD)."));
      m_note->show();
    });
  }
  if (pack) {
    auto* get = new QPushButton(tr("Install (%1)").arg(engineinstall::sizeText(pack->size)), this);
    get->setObjectName("solverInstall");
    get->setDefault(true);
    buttons->addWidget(get);
    connect(get, &QPushButton::clicked, this, [this, install] {
      install();  // the download and its progress go on in the status bar; the window's work is done
      accept();
    });
  }
  v->addLayout(buttons);
}

void SolverWindow::find() {
  const simchecks::Solver* s = simchecks::solver(m_id);
  if (!s) return;
  QString path;
  if (s->folder) path = QFileDialog::getExistingDirectory(this, tr("The folder %1 is installed in").arg(s->name));
  else
    path = QFileDialog::getOpenFileName(this, tr("The program %1").arg(m_id == "calculix" ? QStringLiteral("ccx") : s->name), {},
#ifdef _WIN32
                                        tr("Programs (*.exe)")
#else
                                        {}
#endif
    );
  if (path.isEmpty()) return;
  const QString why = check(m_id, path);
  if (!why.isEmpty()) {
    m_note->setText(why);
    m_note->show();
    return;
  }
  remember(m_id, path);
  if (m_found) m_found();
  accept();
}

QString SolverWindow::check(const QString& solver, const QString& path) {
  const QFileInfo fi(path);
  if (solver == "openfoam") {
    const QDir d(path);
    const bool ok = QFileInfo(d.filePath("bin/simpleFoam")).isFile() || QFileInfo(d.filePath("bin/simpleFoam.exe")).isFile() ||
                    (QFileInfo(d.filePath("etc/openfoam")).isFile() && QFileInfo(d.filePath("bin/foamEtcFile")).isFile());
    return ok ? QString() : tr("No OpenFOAM there: its folder holds bin/simpleFoam (or etc/openfoam). Pick the folder OpenFOAM was installed in.");
  }
  if (solver == "calculix") return fi.isFile() && fi.baseName().startsWith("ccx", Qt::CaseInsensitive) ? QString() : tr("That is not CalculiX's program: pick ccx.exe (or ccx_2.22.exe).");
  return tr("This solver cannot be found by hand.");
}

void SolverWindow::remember(const QString& solver, const QString& path) {
  const simchecks::Solver* s = simchecks::solver(solver);
  if (!s || s->envVar.isEmpty()) return;
  QSettings().setValue("simulate/solvers/" + solver, path);
  qputenv(s->envVar.toLocal8Bit(), path.toUtf8());
}

void SolverWindow::applySaved() {
  QSettings settings;
  for (const auto& s : simchecks::solvers()) {
    if (s.envVar.isEmpty() || !qEnvironmentVariableIsEmpty(s.envVar.toLocal8Bit())) continue;  // set from outside: that one wins
    const QString path = settings.value("simulate/solvers/" + s.id).toString();
    if (!path.isEmpty() && check(s.id, path).isEmpty()) qputenv(s.envVar.toLocal8Bit(), path.toUtf8());
  }
}
