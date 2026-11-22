#include "CompareMode.hpp"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_GTrsf.hxx>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "DesignController.hpp"
#include "Git.hpp"
#include "GitWatch.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "RecoveryManager.hpp"
#include "TimelineWidget.hpp"
#include "ToolPanel.hpp"
#include "ViewportChips.hpp"
#include "opad/diff.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/variants.hpp"

using Kind = CompareVersion::Kind;
using Category = ComparePanel::Category;

struct CompareMode::Run {
  CompareVersion a, b;
  unsigned long long generation = 0, revision = 0;  // the session it was made from
  std::shared_ptr<opad::Document> docA, docB;       // dropped once the parts are drawn
  opad::json diff;
  std::vector<opad::BodyChange> bodies;
  std::unordered_map<std::string, size_t> at;  // body id -> bodies index
  std::vector<char> sessionDraws;  // per body: the session draws B's body as it is
  std::vector<std::shared_ptr<const opad::FaceChanges>> faces;  // per body: what became of a modified body's faces (null: not known)
  std::vector<std::string> hidden;  // session bodies B does not show as they are
  std::vector<Bnd_Box> boxA, boxB;  // world boxes of the changed bodies, each side
  std::vector<Bnd_Box> changeBox;   // per change: its changed bodies, both sides
  std::vector<std::vector<std::string>> changeBodies;  // per change: the body (or sketch) ids it is about
  std::map<std::string, QColor Tokens::*> marks;  // B's ops that A has not, or has otherwise
  std::map<std::string, int> sketches;            // B's added or edited sketches -> category
  std::array<int, ComparePanel::Categories> counts{};
  bool variants = false;  // two documents that share no history: bodies matched across them (opad/variants.hpp)
};

// Compare's icons: two versions side by side, the swap of A and B, an eye struck through (a hidden kind of change).
OPAD_ICON_TABLE(vcs,
                {"compare", R"(<rect x="3" y="4" width="8" height="16" rx="1"/><rect x="13" y="4" width="8" height="16" rx="1" stroke-dasharray="2 2"/><path d="M5.5 12h3M15 12h4M17 10v4"/>)"},
                {"swap", R"(<path d="M4 8h14M14 4l4 4-4 4M20 16H6M10 12l-4 4 4 4"/>)"},
                {"eye-off", R"(<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="3"/><path d="M4 20L20 4"/>)"});

namespace {
QString pendingStartup;

std::filesystem::path fsPath(const QString& file) { return std::filesystem::path(file.toStdU16String()); }

Category categoryOf(opad::BodyChange::Kind k) {
  switch (k) {
    case opad::BodyChange::Kind::Added: return ComparePanel::Added;
    case opad::BodyChange::Kind::Removed: return ComparePanel::Removed;
    case opad::BodyChange::Kind::Modified: return ComparePanel::Modified;
    case opad::BodyChange::Kind::Moved: return ComparePanel::Moved;
    default: return ComparePanel::Unchanged;
  }
}

QString ago(const QString& iso) {
  const QDateTime at = QDateTime::fromString(iso, Qt::ISODate);
  if (!at.isValid()) return iso;
  const qint64 s = at.secsTo(QDateTime::currentDateTimeUtc());
  if (s < 60) return CompareMode::tr("just now");
  if (s < 3600) return CompareMode::tr("%n min ago", nullptr, int(s / 60));
  if (s < 86400) return CompareMode::tr("%n h ago", nullptr, int(s / 3600));
  if (s < 30 * 86400) return CompareMode::tr("%n d ago", nullptr, int(s / 86400));
  return at.toLocalTime().toString("yyyy-MM-dd");
}

Bnd_Box worldBox(const Bnd_Box& local, const opad::Mat4& m) {
  Bnd_Box out;
  if (local.IsVoid()) return out;
  double x[2], y[2], z[2];
  local.Get(x[0], y[0], z[0], x[1], y[1], z[1]);
  for (double cx : x)
    for (double cy : y)
      for (double cz : z) {
        const opad::Vec3 p = m.apply({cx, cy, cz});
        out.Add(gp_Pnt(p[0], p[1], p[2]));
      }
  return out;
}
opad::Vec3 centre(const Bnd_Box& b) {
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2};
}

using Files = std::map<QString, std::shared_ptr<opad::Document>>;

// A version's document, read on a worker: index mode, so only the bodies the view asks for are ever parsed. Another file
// may be of any format OPAD reads (a STEP, an X_T: a variant, read as the viewer reads it), and is read once while
// Compare stays open (`files`).
std::shared_ptr<opad::Document> readVersion(const CompareVersion& v, const std::shared_ptr<opad::Document>& session, const QString& file, git::Context git,
                                            const Files& files = {}) {
  switch (v.kind) {
    case Kind::Session: return session;
    case Kind::Saved: return std::make_shared<opad::Document>(opad::Document::load_index(fsPath(v.ref)));
    case Kind::File: {
      if (const auto it = files.find(v.ref); it != files.end()) return it->second;
      return std::make_shared<opad::Document>(opad::read_variant(fsPath(v.ref)));
    }
    case Kind::Git: {
      if (git.program.isEmpty()) git.program = git::findProgram();
      if (git.program.isEmpty()) throw opad::Error(CompareMode::tr("git was not found.").toStdString());
      const git::Result r = git::run(git, {"show", v.ref + ":./" + QFileInfo(file).fileName()});
      if (!r.ok()) throw opad::Error(CompareMode::tr("%1 does not have this file: %2").arg(v.label, r.error()).toStdString());
      return std::make_shared<opad::Document>(opad::Document::parse_index(r.out.toStdString(), fsPath(file)));
    }
    case Kind::Recovery: return std::make_shared<opad::Document>(RecoveryManager::snapshotDocument(v.ref));
    case Kind::None: break;
  }
  throw opad::Error(CompareMode::tr("Choose the version to compare with.").toStdString());
}

// Big documents (a 334 MB assembly read twice) are let go on a thread of their own: freeing them is work that scales with
// the model, never the UI thread's. Reset there: QThread::create's callable is destroyed with the thread object, on the
// UI thread.
void dispose(std::shared_ptr<void> value) {
  if (!value) return;
  auto* thread = QThread::create([value = std::move(value)]() mutable { value.reset(); });
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start(QThread::LowPriority);
}

double deflection(const Bnd_Box& box) { return box.IsVoid() ? 0.1 : std::clamp(std::sqrt(box.SquareExtent()) * 0.002, 0.02, 2.0); }

std::array<double, 3> rgb(const QColor& c) { return {c.redF(), c.greenF(), c.blueF()}; }

// One side of a modified body's faces in their colours: B's added and changed ones, A's removed and changed ones (the
// added or removed only while that kind is shown). Null when no face is coloured.
std::shared_ptr<const opad::FaceColors> faceTint(const opad::FaceChanges& f, bool sideA, const Tokens& t, bool newOnes) {
  using K = opad::FaceChanges::Kind;
  auto out = std::make_shared<opad::FaceColors>();
  out->colors = {rgb(sideA ? t.diffRemoved : t.diffAdded), rgb(t.diffModified)};
  const auto& kinds = sideA ? f.a : f.b;
  out->face.assign(kinds.size(), -1);
  for (size_t i = 0; i < kinds.size(); ++i)
    if (kinds[i] == K::Modified) out->face[i] = 1;
    else if (newOnes && kinds[i] == (sideA ? K::Removed : K::Added)) out->face[i] = 0;
  return out->empty() ? nullptr : out;
}

// The rest of a body whose changed faces are coloured: between the modified colour and the ghost, so they stand out.
std::array<double, 3> muted(const Tokens& t) { return looks::mix(rgb(t.diffModified), rgb(t.ghost), 0.85); }
}  // namespace

CompareMode::CompareMode(AreaServices& services, GitWatch* git) : QObject(services.window()), m_services(services), m_git(git) {
  if (ViewportChips* chips = services.chips()) {
    m_chip = new QLabel(chips);
    m_chip->setObjectName("chipSel");
    m_chip->hide();
    chips->addChip(m_chip);
  }
  m_rerun.setSingleShot(true);
  m_rerun.setInterval(400);
  connect(&m_rerun, &QTimer::timeout, this, &CompareMode::start);
  m_restyle.setSingleShot(true);
  m_restyle.setInterval(30);
  connect(&m_restyle, &QTimer::timeout, this, &CompareMode::restyle);
  connect(services.document(), &AppDocument::aboutToReplace, this, &CompareMode::close);
  connect(services.document(), &AppDocument::loadFinished, this, [this](bool ok) {  // nothing to compare with
    if (ok) return;
    pendingStartup.clear();
    m_pending.reset();
    m_pendingFiles.clear();
  });
  connect(services.document(), &AppDocument::saved, this, [this] {  // the saved file is another version now
    if (!m_active) return;
    showVersions();
    m_rerun.start();
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
    if (!m_active || !m_run) return;
    restyle();
    m_services.viewport()->setCompare(m_parts, m_arrows, theme::current().diffMoved);  // the arrows' colour too
  });
}

CompareMode::~CompareMode() { release(m_run); }

void CompareMode::release(std::shared_ptr<Run>& run, bool all) {
  if (!run) return;
  dispose(std::move(run->docA));
  dispose(std::move(run->docB));
  if (all) run.reset();
}

// The panel is made when Compare first opens: a session that never compares carries none of its widgets (startup, theme
// switches).
void CompareMode::makePanel() {
  if (m_tool) return;
  QWidget* window = m_services.window();
  m_panel = new ComparePanel(window);
  m_tool = new ToolPanel("compare", "compare", &Tokens::sel, tr("Compare"), m_panel, 660, window);
  m_tool->setEscapeHandler([this] { close(); });
  m_services.addPanel(m_tool);
  connect(m_tool, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on && !m_tool->isVisible()) close();  // Done, Esc, its close button, or another panel took its place
  });
  connect(m_panel, &ComparePanel::doneRequested, this, &CompareMode::close);
  connect(m_panel, &ComparePanel::versionsChosen, this, [this](int a, int b) {
    m_a = a;
    m_b = b;
    showVersions();
    start();
  });
  connect(m_panel, &ComparePanel::otherFileRequested, this, &CompareMode::otherFile);
  connect(m_panel, &ComparePanel::swapRequested, this, [this] {
    std::swap(m_a, m_b);
    showVersions();
    start();
  });
  connect(m_panel, &ComparePanel::layoutChosen, this, &CompareMode::setSideBySide);
  connect(m_panel, &ComparePanel::emphasisChanged, this, [this] { m_restyle.start(); });
  connect(m_panel, &ComparePanel::categoryToggled, this, [this] { m_restyle.start(); });
  connect(m_panel, &ComparePanel::changeActivated, this, &CompareMode::activate);
  connect(m_panel, &ComparePanel::stepRequested, this, &CompareMode::step);
  connect(m_panel, &ComparePanel::variantChosen, this, [this](int column) {  // a column of the Variants table: that file as B
    if (column < 0 || size_t(column) >= m_variantColumns.size() || m_variantColumns[size_t(column)] == m_b) return;
    m_b = m_variantColumns[size_t(column)];
    showVersions();
    start();
  });
}

void CompareMode::setStartup(const QString& version) { pendingStartup = version.startsWith("git:") ? version : QFileInfo(version).absoluteFilePath(); }

CompareVersion CompareMode::parseVersion(const QString& spec) {
  if (spec.startsWith("git:")) {
    const QString rev = spec.mid(4).isEmpty() ? QStringLiteral("HEAD") : spec.mid(4);
    return {Kind::Git, rev, rev == "HEAD" ? tr("Last commit (HEAD)") : rev, spec};
  }
  const QString file = QFileInfo(spec).absoluteFilePath();
  return {Kind::File, file, QFileInfo(file).fileName(), QDir::toNativeSeparators(file)};
}

CompareVersion CompareMode::savedVersion(const QString& file) { return {Kind::Saved, file, tr("Saved file"), QDir::toNativeSeparators(file)}; }

CompareVersion CompareMode::recoveryVersion(const QString& snapshot, const QString& time) {
  return {Kind::Recovery, snapshot, tr("Recovery snapshot · %1").arg(i18n::localTime(time.toStdString())), QDir::toNativeSeparators(snapshot)};
}

void CompareMode::compareIn(const QString& file, const CompareVersion& a, const CompareVersion& b) {
  AppDocument* doc = m_services.document();
  if (doc->hasDocument && !doc->loading && !doc->browse && !doc->doc.path.empty() && QFileInfo(doc->path()) == QFileInfo(file)) return compare(a, b);
  m_pending = Pending{file, a, b};
  m_services.open(file);
  if (!doc->loading) m_pending.reset();  // the open document was kept (its unsaved changes): nothing to compare in
}

int CompareMode::indexOf(const CompareVersion& v) const {
  for (size_t i = 0; i < m_versions.size(); ++i)
    if (m_versions[i] == v) return int(i);
  return -1;
}

void CompareMode::addVersion(const CompareVersion& v) {
  if (v.kind != Kind::None && indexOf(v) < 0) m_versions.push_back(v);
}

void CompareMode::showVersions() {
  AppDocument* doc = m_services.document();
  for (auto& v : m_versions)
    if (v.kind == Kind::Session) v.label = doc->isDirty() ? tr("This session (unsaved changes)") : tr("This session");
  m_panel->setVersions(m_versions, m_a, m_b);
  const QString a = m_a >= 0 ? m_versions[size_t(m_a)].label : QString(), b = m_b >= 0 ? m_versions[size_t(m_b)].label : QString();
  if (m_chip) {  // side by side, each view names its own version
    m_chip->setText(m_sideBySide && !b.isEmpty() ? tr("B · %1").arg(b) : !a.isEmpty() && !b.isEmpty() ? tr("Compare: %1 → %2").arg(a, b) : tr("Compare"));
    m_chip->setVisible(m_active);
  }
  if (m_sideBySide) m_services.viewport()->setSideCaption(tr("A · %1").arg(a));
}

void CompareMode::setSideBySide(bool on) {
  QSettings().setValue("compare/sideBySide", on);
  m_panel->setSideBySide(on);
  if (on == m_sideBySide || !m_active) return;
  trace::Scope scope(on ? "Compare: side by side" : "Compare: overlay");
  m_sideBySide = on;
  Viewport* vp = m_services.viewport();
  vp->setSideBySide(on, tr("A · %1").arg(m_a >= 0 ? m_versions[size_t(m_a)].label : QString()));
  showVersions();
  buildExtras();  // more files: their panes (C, D), or none; both whole, or weighted again
  vp->fitAll();  // the view is half as wide (or twice): the model framed again
}

void CompareMode::sideHidden() {
  std::vector<std::string> ids;
  if (m_run) {
    for (size_t i = 0; i < m_run->bodies.size(); ++i)
      if (m_run->sessionDraws[i] && m_run->bodies[i].kind != opad::BodyChange::Kind::Unchanged) ids.push_back(m_run->bodies[i].id);
    for (const auto& [id, category] : m_run->sketches) ids.push_back(id);
  }
  m_services.viewport()->setSideHidden(ids);
}

void CompareMode::open() {
  AppDocument* doc = m_services.document();
  if (doc->hasDocument && doc->browse && !doc->loading) return openWithFile();  // a file viewed: compared with another file
  if (!doc->hasDocument || doc->loading) {
    m_services.showMessage(tr("Open a file first, or compare files (Compare files…)."));
    return;
  }
  if (DesignController* d = m_services.design(); d && (d->sketchActive() || d->featureActive())) {
    m_services.showMessage(tr("Finish the sketch or the feature first."));
    return;
  }
  const QString file = doc->doc.path.empty() ? QString() : doc->path();
  CompareVersion a;
  using D = git::Repo::Doc;
  const D state = m_git && QFileInfo(m_git->repo().file) == QFileInfo(file) ? m_git->repo().doc() : D::None;
  if (!file.isEmpty() && (state == D::Clean || state == D::Modified || state == D::Conflict)) a = parseVersion("git:HEAD");
  else if (!file.isEmpty() && QFileInfo::exists(file)) a = savedVersion(file);
  compare(a, sessionVersion());
}

// Compare with ▾ > A file… (Review > Compare, the Version menu): another file of any format (a copy, a variant, an export
// of the same model) against this session, or against the file viewed, as the panel's Other file… does once it is open.
void CompareMode::openWithFile() {
  AppDocument* doc = m_services.document();
  if (!doc->hasDocument || doc->loading) {
    m_services.showMessage(tr("Open a file first, or compare files (Compare files…)."));
    return;
  }
  if (DesignController* d = m_services.design(); d && (d->sketchActive() || d->featureActive())) {
    m_services.showMessage(tr("Finish the sketch or the feature first."));
    return;
  }
  const QString file = QFileDialog::getOpenFileName(m_services.window(), tr("Compare with"), doc->browse ? QFileInfo(doc->viewing).absolutePath() : doc->doc.path.empty() ? QString() : QFileInfo(doc->path()).absolutePath(),
                                                    filesFilter());
  if (!file.isEmpty()) compare(parseVersion(file), sessionVersion());
}

QString CompareMode::filesFilter() {
  QStringList patterns{"*.opad"};
  for (const auto& ext : opad::importable_extensions()) patterns << "*" + QString::fromStdString(ext);
  return tr("Design files (%1)").arg(patterns.join(' '));
}

CompareVersion CompareMode::sessionVersion() const {
  const AppDocument* doc = m_services.document();
  if (doc->browse && !doc->viewing.isEmpty()) return {Kind::Session, QString(), tr("This file · %1").arg(QFileInfo(doc->viewing).fileName()), QDir::toNativeSeparators(doc->viewing)};
  return {Kind::Session, QString(), tr("This session"), QString()};
}

void CompareMode::openFiles() {
  AppDocument* doc = m_services.document();
  const QString shown = doc->browse ? doc->viewing : doc->doc.path.empty() ? QString() : doc->path();
  const QString folder = doc->hasDocument && !shown.isEmpty() ? QFileInfo(shown).absolutePath() : QString();
  const QStringList files = QFileDialog::getOpenFileNames(m_services.window(), tr("Compare files"), folder, filesFilter());
  if (!files.isEmpty()) openFiles(files);
}

void CompareMode::openFiles(const QStringList& files) {
  if (files.size() < 2) {
    m_services.showMessage(tr("Choose two files or more: the first is compared with each of the others."));
    return;
  }
  AppDocument* doc = m_services.document();
  if (DesignController* d = m_services.design(); d && (d->sketchActive() || d->featureActive())) {
    m_services.showMessage(tr("Finish the sketch or the feature first."));
    return;
  }
  if (!doc->hasDocument || doc->loading) {  // nothing to show them over: the first opens (viewed), then they are compared
    m_pendingFiles = files;
    if (!doc->loading) m_services.open(files.front());
    return;
  }
  std::vector<CompareVersion> all;
  for (const QString& f : files) all.push_back(parseVersion(f));
  compare(all[0], all[1]);
  for (size_t i = 2; i < all.size(); ++i) addVersion(all[i]);
  showVersions();
}

void CompareMode::showUnsaved() {
  AppDocument* doc = m_services.document();
  const QString file = doc->hasDocument && !doc->browse && !doc->doc.path.empty() ? doc->path() : QString();
  if (file.isEmpty() || !QFileInfo::exists(file)) {
    m_services.showMessage(tr("Not saved yet: everything in it is unsaved."));
    return;
  }
  if (DesignController* d = m_services.design(); d && (d->sketchActive() || d->featureActive())) {
    m_services.showMessage(tr("Finish the sketch or the feature first."));
    return;
  }
  compare(savedVersion(file), sessionVersion());
}

void CompareMode::compare(const CompareVersion& a, const CompareVersion& b) {
  trace::Scope scope("Compare: open");
  makePanel();
  AppDocument* doc = m_services.document();
  const QString file = doc->doc.path.empty() ? QString() : doc->path();
  const bool list = !m_active || m_listed != file;
  if (list) {  // opened (again), or another document: its versions as they are now
    m_versions.clear();
    m_listed = file;
    addVersion(sessionVersion());
    if (!doc->browse && !file.isEmpty() && QFileInfo::exists(file)) addVersion(savedVersion(file));  // a file viewed is the session itself
    if (!doc->browse && !file.isEmpty() && m_git && m_git->repo().state == git::Repo::State::Ready) addVersion(parseVersion("git:HEAD"));
  }
  addVersion(a);
  addVersion(b);
  m_a = indexOf(a);
  m_b = indexOf(b);
  const bool was = m_active;
  m_active = true;
  showVersions();
  m_services.openPanel(m_tool);
  if (!was) {
    emit activeChanged(true);
    m_services.updateCommands();
    setSideBySide(QSettings().value("compare/sideBySide", false).toBool());  // as it was left
  }
  start();
  if (list && !doc->browse) listVersions();
}

void CompareMode::listVersions() {
  AppDocument* doc = m_services.document();
  const QString file = m_listed;
  const std::string uuid = doc->doc.header.uuid;
  const QString root = RecoveryManager::recoveryRoot();
  git::Context git = m_git ? m_git->context() : git::Context();
  git.dir = QFileInfo(file).absolutePath();
  const QString head = m_git ? m_git->repo().status.oid : QString();
  auto out = std::make_shared<std::vector<CompareVersion>>();
  m_services.jobs()->quiet(tr("Listing versions"), [out, file, uuid, root, git, head](Progress p) mutable {
    if (!file.isEmpty()) {
      if (git.program.isEmpty()) git.program = git::findProgram();
      const git::Result r = git.program.isEmpty() ? git::Result() : git::run(git, {"log", "-n", "30", "-z", "--format=%H%x1f%h%x1f%an%x1f%aI%x1f%s", "--", QFileInfo(file).fileName()});
      if (r.ok())
        for (const QByteArray& record : r.out.split('\0')) {
          const QStringList f = QString::fromUtf8(record).trimmed().split(QChar(0x1f));
          if (f.size() < 5 || f[0] == head) continue;  // HEAD is listed already
          const QString subject = f[4].size() > 40 ? f[4].left(39) + QString::fromUtf8("…") : f[4];
          out->push_back({Kind::Git, f[0], CompareMode::tr("%1 · %2 · %3").arg(f[1], subject, ago(f[3])),
                          CompareMode::tr("%1\n%2, %3\n%4").arg(f[0], f[2], i18n::localTime(f[3].toStdString()), f[4])});
        }
    }
    if (p.cancelled()) return;
    for (const auto& s : RecoveryManager::snapshotsOf(root, uuid))
      out->push_back(recoveryVersion(s.file, s.time));
  }, [this, self = QPointer<CompareMode>(this), out, file](bool ok, const QString&) {
    if (!self || !ok || !m_active || m_listed != file) return;
    for (const auto& v : *out) addVersion(v);
    showVersions();
  });
}

void CompareMode::otherFile(int side) {
  AppDocument* doc = m_services.document();
  const QString file = QFileDialog::getOpenFileName(m_services.window(), side == 0 ? tr("Compare with") : tr("Show"),
                                                    doc->doc.path.empty() ? QString() : QFileInfo(doc->path()).absolutePath(), filesFilter());
  if (!file.isEmpty()) {
    const CompareVersion v = parseVersion(file);
    addVersion(v);
    (side == 0 ? m_a : m_b) = indexOf(v);
  }
  showVersions();
  if (!file.isEmpty()) start();
}

void CompareMode::close() {
  if (!m_active) return;
  trace::Scope scope("Compare: close");
  m_active = false;
  ++m_serial;  // what still runs is dropped
  m_running = m_again = m_meshing = false;
  m_rerun.stop();
  m_restyle.stop();
  release(m_run);
  for (auto& [file, d] : m_files) dispose(std::move(d));
  m_files.clear();
  m_variantColumns.clear();
  m_tableKey.clear();
  m_variantBodies.clear();
  for (auto& d : m_variantDocs) dispose(std::move(d));
  m_variantDocs.clear();
  ++m_extraSerial;
  m_mainParts = 0;
  m_parts.clear();
  m_partInfo.clear();
  m_arrows.clear();
  if (Viewport* vp = m_services.viewport()) {
    if (std::exchange(m_sideBySide, false)) vp->setSideBySide(false);
    vp->setSideHidden({});
    vp->clearLookLayer(LookSource::Compare);
    vp->setFaceTints({});
    vp->clearCompare();
  }
  if (TimelineWidget* t = m_services.timeline()) t->setMarkedOps({});
  if (m_chip) m_chip->hide();
  m_panel->clear();
  if (m_tool->isVisible()) m_tool->hide();
  emit activeChanged(false);
  m_services.updateCommands();
}

void CompareMode::failed(const QString& error) {
  m_panel->setFailed(error);
  release(m_run);
  m_parts.clear();
  m_partInfo.clear();
  m_arrows.clear();
  m_mainParts = 0;
  ++m_extraSerial;
  m_services.viewport()->setSideExtras({});
  m_services.viewport()->setSideHidden({});
  m_services.viewport()->clearLookLayer(LookSource::Compare);
  m_services.viewport()->setFaceTints({});
  m_services.viewport()->clearCompare();
  if (TimelineWidget* t = m_services.timeline()) t->setMarkedOps({});
  if (trace::enabled()) trace::log("compare: " + error);
}

void CompareMode::start() {
  if (!m_active) return;
  if (m_running) {
    m_again = true;
    return;
  }
  AppDocument* doc = m_services.document();
  if (!doc->hasDocument) return close();
  if (m_a < 0 || m_b < 0) {
    m_panel->setFailed(tr("Choose the version to compare with."));
    return;
  }
  const unsigned serial = ++m_serial;
  const auto generation = doc->generation, revision = doc->revision;
  m_running = true;
  m_panel->setBusy(tr("Reading the versions…"));
  // The session as it is now, copied on a worker: the comparison never reads the live document.
  const bool started = doc->captureSnapshot(m_services.jobs(), [this, self = QPointer<CompareMode>(this), serial, generation, revision](
                                                                 std::shared_ptr<opad::Document> session, const QString& error) {
    if (!self || serial != m_serial) return;
    if (!session) {
      m_running = false;
      return failed(error.isEmpty() ? tr("The session could not be read.") : error);
    }
    diff(serial, std::move(session), generation, revision);
  });
  if (!started) {  // a save, a load or a recovery snapshot holds the document: again in a moment
    m_running = false;
    m_rerun.start();
  }
}

void CompareMode::diff(unsigned serial, std::shared_ptr<opad::Document> session, unsigned long long generation, unsigned long long revision) {
  auto run = std::make_shared<Run>();
  run->a = m_versions[size_t(m_a)];
  run->b = m_versions[size_t(m_b)];
  run->generation = generation;
  run->revision = revision;
  AppDocument* doc = m_services.document();
  const QString file = doc->doc.path.empty() ? QString() : doc->path();
  git::Context git = m_git ? m_git->context() : git::Context();
  git.dir = QFileInfo(file).absolutePath();
  QElapsedTimer clock;
  clock.start();
  const Files files = m_files;
  m_services.jobs()->async(tr("Comparing versions"), [run, session, file, git, files](Progress p) mutable {
    struct Drop {  // this copy goes here, not with the thread object on the UI thread
      std::shared_ptr<opad::Document>& document;
      ~Drop() { document.reset(); }
    } drop{session};
    p.setPhase(CompareMode::tr("Reading %1").arg(run->a.label));
    run->docA = readVersion(run->a, session, file, git, files);
    p.setPhase(CompareMode::tr("Reading %1").arg(run->b.label));
    run->docB = readVersion(run->b, session, file, git, files);
    if (p.cancelled()) return;
    p.setPhase(CompareMode::tr("Comparing"));
    const opad::Scene sa = opad::resolve(*run->docA), sb = opad::resolve(*run->docB);
    // Two documents that share no history (variants exported apart, a STEP beside an X_T): bodies matched by the files'
    // ids, geometry, names and overlap, listed as body changes; else the versions' histories compared.
    run->variants = run->docA->header.uuid != run->docB->header.uuid;
    if (run->variants) {
      p.setPhase(CompareMode::tr("Matching the bodies"));
      run->bodies = opad::match_bodies(*run->docA, sa, *run->docB, sb, [p] { return p.cancelled(); });
      run->diff = opad::variant_diff(run->bodies, sa, sb);
    } else {
      run->diff = opad::semantic_diff(*run->docA, sa, *run->docB, sb);
      run->bodies = opad::body_changes(sa, sb);
    }
    const auto& bodies = run->bodies;
    for (size_t i = 0; i < bodies.size(); ++i) run->at[bodies[i].id] = i;
    // B over the session the view draws: the bodies it draws as B has them, and the ones it has to leave out.
    run->sessionDraws.assign(bodies.size(), 0);
    if (run->b.kind == Kind::Session) {
      for (size_t i = 0; i < bodies.size(); ++i) run->sessionDraws[i] = bodies[i].shown_b;
    } else {
      const opad::Scene ss = run->a.kind == Kind::Session ? sa : opad::resolve(*session);
      std::unordered_set<std::string> drawn;
      for (size_t i = 0; i < bodies.size(); ++i) {
        const auto& c = bodies[i];
        const opad::Node* n = c.shown_b ? ss.node(c.id) : nullptr;
        if (n && n->kind == opad::Node::Kind::Body && n->body_key == c.key_b && ss.effectively_visible(c.id) && opad::same_placement(ss.world(c.id), c.world_b)) {
          run->sessionDraws[i] = 1;
          drawn.insert(c.id);
        }
      }
      for (const auto& id : ss.all_bodies())
        if (!drawn.count(id) && ss.effectively_visible(id)) run->hidden.push_back(id);
    }
    if (p.cancelled()) return;
    p.setPhase(CompareMode::tr("Measuring the changed bodies"));
    auto boxOf = [](const opad::Document& d, const std::string& key, const opad::Mat4& m) {
      try {
        return worldBox(opad::body_bbox(d, key), m);
      } catch (const std::exception&) {
        return Bnd_Box();  // not readable: listed, not drawn
      }
    };
    run->boxA.resize(bodies.size());
    run->boxB.resize(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
      const auto& c = bodies[i];
      if (c.kind == opad::BodyChange::Kind::Unchanged || p.cancelled()) continue;
      if (c.shown_a && !c.key_a.empty()) run->boxA[i] = boxOf(*run->docA, c.key_a, c.world_a);
      if (c.shown_b && !c.key_b.empty()) run->boxB[i] = boxOf(*run->docB, c.key_b, c.world_b);
    }
    // A modified body's faces, matched by geometry: the view colours the ones the change added, cut into or took away.
    p.setPhase(CompareMode::tr("Comparing the changed faces"));
    run->faces.resize(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
      const auto& c = bodies[i];
      if (c.kind != opad::BodyChange::Kind::Modified || c.key_a.empty() || c.key_b.empty() || p.cancelled()) continue;
      try {
        auto f = std::make_shared<opad::FaceChanges>(opad::face_changes(opad::body_shape(*run->docA, c.key_a), c.world_a, opad::body_shape(*run->docB, c.key_b), c.world_b));
        if (!f->empty()) run->faces[i] = std::move(f);
      } catch (const std::exception&) {  // not readable: the body is shown changed as a whole
      }
    }
    // What each listed change is about, to select and fit it.
    const opad::json& changes = run->diff["changes"];
    run->changeBox.resize(changes.size());
    run->changeBodies.resize(changes.size());
    for (size_t k = 0; k < changes.size(); ++k) {
      const opad::json& c = changes[k];
      const std::string kind = c.value("kind", ""), id = c.value("id", "");
      std::vector<std::string> ids;
      if (kind == "body" || kind == "sketch") ids = {id};
      else if (kind == "component") ids = sb.node(id) ? sb.bodies_under(id) : sa.bodies_under(id);
      else if (kind == "feature")
        if (const opad::Feature* f = sb.feature(id) ? sb.feature(id) : sa.feature(id))
          for (const auto& b : f->result.value("bodies", opad::json::array())) ids.push_back(b.value("id", ""));
      for (const auto& b : ids)
        if (const auto it = run->at.find(b); it != run->at.end()) {
          run->changeBox[k].Add(run->boxA[it->second]);
          run->changeBox[k].Add(run->boxB[it->second]);
        }
      run->changeBodies[k] = std::move(ids);
      if (kind == "sketch" && (c.value("change", "") == "added" || c.value("change", "") == "edited" || c.value("change", "") == "renamed"))
        run->sketches[id] = c.value("change", "") == "added" ? ComparePanel::Added : ComparePanel::Modified;
    }
    // The timeline's marks: B's ops A has not (an edit, a delete or a regenerated result marks the op it changes).
    std::unordered_map<std::string, const opad::Op*> inA;
    for (const auto& o : run->docA->ops) inA[o.id] = &o;
    auto mark = [&](const std::string& id, QColor Tokens::* role) {
      auto& m = run->marks[id];
      if (m != &Tokens::diffAdded) m = role;
    };
    for (const auto& o : run->docB->ops) {
      if (run->variants) break;  // another history: no op of B is A's
      const auto it = inA.find(o.id);
      if (it == inA.end()) {
        if (o.type == "edit" || o.type == "delete") mark(o.data.value("target", ""), &Tokens::diffModified);
        else if (o.type == "regen") {
          if (o.data.contains("results") && o.data["results"].is_object())
            for (const auto& [target, result] : o.data["results"].items()) mark(target, &Tokens::diffModified);
        } else mark(o.id, &Tokens::diffAdded);
      } else if (!o.raw.empty() && !it->second->raw.empty() ? o.raw != it->second->raw : o.data != it->second->data) {
        mark(o.id, &Tokens::diffModified);
      }
    }
    for (const auto& c : bodies) {
      const Category cat = categoryOf(c.kind);
      if (cat == ComparePanel::Removed ? c.shown_a : (c.shown_b || cat == ComparePanel::Modified || cat == ComparePanel::Moved)) ++run->counts[cat];
    }
  }, [this, self = QPointer<CompareMode>(this), run, serial, clock](bool ok, const QString& error) {
    if (!self || serial != m_serial) {
      dispose(std::move(run->docA));
      dispose(std::move(run->docB));
      return;
    }
    m_running = false;
    if (ok) {  // other files read once while Compare stays open (switching between variants)
      if (run->a.kind == Kind::File && run->docA) m_files.emplace(run->a.ref, run->docA);
      if (run->b.kind == Kind::File && run->docB) m_files.emplace(run->b.ref, run->docB);
    }
    if (std::exchange(m_again, false)) return start();  // asked again meanwhile: this result is already old
    if (!ok) return failed(error == "cancelled" ? tr("Compare was cancelled.") : error);
    if (trace::enabled()) trace::log(QStringLiteral("compare: %1 -> %2: %3 changes, %4 bodies in %5 ms").arg(run->a.label, run->b.label)
                                         .arg(run->diff["changes"].size()).arg(run->bodies.size()).arg(clock.elapsed()));
    show(run);
  });
}

void CompareMode::show(std::shared_ptr<Run> run) {
  trace::Scope scope("Compare: show");
  AppDocument* doc = m_services.document();
  if (doc->generation != run->generation || doc->revision != run->revision) return start();  // the session moved on meanwhile
  m_run = std::move(run);
  const int keep = m_panel->current();
  m_panel->setResult(m_run->diff, m_run->counts);
  if (keep >= 0 && size_t(keep) < m_run->changeBodies.size()) m_panel->setCurrent(keep);
  if (TimelineWidget* t = m_services.timeline()) t->setMarkedOps(m_run->marks, tr("%1 in B"));
  buildParts();
  variantTable();
}

void CompareMode::buildParts() {
  Viewport* vp = m_services.viewport();
  AppDocument* doc = m_services.document();
  const Run& r = *m_run;
  m_parts.clear();
  m_partInfo.clear();
  m_arrows.clear();
  struct Need {
    size_t part;
    std::shared_ptr<opad::Document> doc;
    std::string key;
    opad::Mat4 world;
    std::vector<int> faces;  // only these faces (ordinals); empty: the whole body
    std::shared_ptr<const opad::FaceColors> colors;
  };
  auto needs = std::make_shared<std::vector<Need>>();
  struct How {
    Category category;
    char layout = 0;
    std::shared_ptr<const opad::FaceColors> colors;
    std::vector<int> faces;
  };
  auto add = [&](const opad::BodyChange& c, bool sideA, How how) {
    Viewport::ComparePart part;
    part.id = c.id;
    part.world = sideA ? c.world_a : c.world_b;
    part.view = c.kind == opad::BodyChange::Kind::Unchanged ? 0 : sideA ? 'A' : 'B';
    const std::string& key = sideA ? c.key_a : c.key_b;
    if (how.faces.empty() && !how.colors)
      if (auto arrays = vp->displayArrays(key)) try {  // the session draws this geometry already: its arrays, nothing to mesh
          part.shape = opad::body_shape(doc->doc, key);
          part.prs = std::move(arrays);
        } catch (const std::exception&) {
        }
    if (!part.prs) needs->push_back({m_parts.size(), sideA ? r.docA : r.docB, key, part.world, std::move(how.faces), how.colors});
    m_parts.push_back(std::move(part));
    m_partInfo.push_back({how.category, sideA, how.layout, how.colors != nullptr});
  };
  const Tokens& t = theme::current();
  for (size_t i = 0; i < r.bodies.size(); ++i) {
    const auto& c = r.bodies[i];
    using K = opad::BodyChange::Kind;
    const opad::FaceChanges* faces = c.kind == K::Modified && i < r.faces.size() ? r.faces[i].get() : nullptr;
    if (faces && c.shown_a) {
      // Its faces known: A whole only side by side (its removed and changed faces coloured); over B only what it lost.
      add(c, true, {Category::Modified, 'S', faceTint(*faces, true, t, true), {}});
      std::vector<int> removed;
      for (size_t f = 0; f < faces->a.size(); ++f)
        if (faces->a[f] == opad::FaceChanges::Removed) removed.push_back(int(f));
      if (!removed.empty()) add(c, true, {Category::Removed, 'O', nullptr, std::move(removed)});
    } else if (c.shown_a && (c.kind == K::Removed || c.kind == K::Moved || c.kind == K::Modified)) {
      add(c, true, {categoryOf(c.kind)});
    }
    if (c.shown_b && !r.sessionDraws[i]) add(c, false, {categoryOf(c.kind), 0, faces ? faceTint(*faces, false, t, true) : nullptr, {}});
    if (c.kind == K::Moved && !r.boxA[i].IsVoid() && !r.boxB[i].IsVoid()) m_arrows.push_back({centre(r.boxA[i]), centre(r.boxB[i])});
  }
  m_mainParts = m_parts.size();
  sideHidden();
  buildExtras();  // restyles and draws them all
  if (needs->empty()) return release(m_run, false);
  // The other version's geometry, meshed on a worker; drawn once it is all there.
  m_meshing = true;
  struct Out {
    std::vector<TopoDS_Shape> shapes;
    std::vector<std::shared_ptr<const BodyPrs>> prs;
    std::vector<opad::Mat4> worlds;
  };
  auto out = std::make_shared<Out>();
  out->shapes.resize(needs->size());
  out->prs.resize(needs->size());
  out->worlds.resize(needs->size());
  const unsigned serial = m_serial;
  m_services.jobs()->async(tr("Drawing the compared version"), [needs, out](Progress p) {
    struct Drop {  // the documents are let go here once meshed, not on the UI thread
      std::vector<Need>& needs;
      ~Drop() {
        for (auto& n : needs) n.doc.reset();
      }
    } drop{*needs};
    std::map<std::pair<const opad::Document*, std::string>, size_t> meshed;  // instances share their mesh
    for (size_t i = 0; i < needs->size(); ++i) {
      if (p.cancelled()) return;
      const Need& n = (*needs)[i];
      p.setPhase(CompareMode::tr("Meshing body %1 of %2").arg(i + 1).arg(needs->size()), int(i * 100 / needs->size()));
      out->worlds[i] = n.world;
      const bool rigid = opad::mat_is_rigid(n.world), plain = n.faces.empty() && !n.colors;
      if (const auto it = meshed.find({n.doc.get(), n.key}); rigid && plain && it != meshed.end()) {
        out->shapes[i] = out->shapes[it->second];
        out->prs[i] = out->prs[it->second];
        continue;
      }
      try {
        TopoDS_Shape shape = BRepBuilderAPI_Copy(opad::body_shape(*n.doc, n.key)).Shape();
        if (!n.faces.empty()) {  // a copy keeps the faces' order: the ordinals are the body's
          TopTools_IndexedMapOfShape all;
          TopExp::MapShapes(shape, TopAbs_FACE, all);
          BRep_Builder builder;
          TopoDS_Compound some;
          builder.MakeCompound(some);
          for (int f : n.faces)
            if (f >= 0 && f < all.Extent()) builder.Add(some, all(f + 1));
          shape = some;
        }
        if (!rigid) {  // a scaled placement: baked into a copy drawn at identity
          gp_GTrsf g;
          for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col) g.SetValue(row + 1, col + 1, n.world.at(row, col));
          shape = BRepBuilderAPI_GTransform(shape, g, Standard_True).Shape();
          out->worlds[i] = opad::Mat4();
        }
        Bnd_Box box;
        BRepBndLib::Add(shape, box, Standard_False);
        BodyPrs::meshForDisplay(shape, deflection(box));
        out->shapes[i] = shape;
        auto prs = BodyPrs::build(shape, box, true, n.colors);
        prs->deflection = deflection(box);  // what the zoom refinement compares with the pixel (unset, it never refined a part)
        prs->buildNavigation();  // the orbit pivot lands on it
        out->prs[i] = std::move(prs);
        if (rigid && plain) meshed[{n.doc.get(), n.key}] = i;
      } catch (const std::exception&) {  // unreadable: listed, not drawn
      }
    }
  }, [this, self = QPointer<CompareMode>(this), needs, out, serial](bool ok, const QString&) {
    if (!self || serial != m_serial) return;
    m_meshing = false;
    release(m_run, false);
    if (!ok) return;
    for (size_t i = 0; i < needs->size(); ++i) {
      auto& part = m_parts[(*needs)[i].part];
      part.shape = out->shapes[i];
      part.prs = out->prs[i];
      part.world = out->worlds[i];
    }
    restyle();
    m_services.viewport()->setCompare(m_parts, m_arrows, theme::current().diffMoved);
  });
}

// Side by side with three or four files: the first two of the table's files other than A and B, each in a pane of its own
// (C, D) with every body drawn as A's matcher found it (unchanged ghosted, moved, changed and new in their colours), the
// cameras all one. The variant documents are the files Compare already read (m_files), meshed on a worker.
void CompareMode::buildExtras() {
  if (!m_active || !m_run) return;
  Viewport* vp = m_services.viewport();
  m_parts.resize(m_mainParts);
  m_partInfo.resize(m_mainParts);
  const unsigned serial = ++m_extraSerial;
  std::vector<size_t> columns;
  if (m_sideBySide)
    for (size_t c = 0; c < m_variantColumns.size() && columns.size() < 2; ++c)
      if (m_variantColumns[c] != m_b && c < m_variantBodies.size() && c < m_variantDocs.size() && m_variantDocs[c]) columns.push_back(c);
  QStringList captions;
  struct Need {
    size_t part;
    std::shared_ptr<opad::Document> doc;
    std::string key;
    opad::Mat4 world;
  };
  auto needs = std::make_shared<std::vector<Need>>();
  for (size_t e = 0; e < columns.size(); ++e) {
    const size_t c = columns[e];
    const QChar letter('C' + int(e));
    captions << tr("%1 · %2").arg(letter, m_versions[size_t(m_variantColumns[c])].label);
    for (const VariantBody& vb : m_variantBodies[c]) {
      Viewport::ComparePart part;
      part.id = vb.id;
      part.world = vb.world;
      part.view = char('C' + int(e));
      const Category category = vb.change == "moved" ? ComparePanel::Moved : vb.change == "modified" ? ComparePanel::Modified
                              : vb.change == "added" ? ComparePanel::Added : ComparePanel::Unchanged;
      needs->push_back({m_parts.size(), m_variantDocs[c], vb.key, vb.world});
      m_parts.push_back(std::move(part));
      m_partInfo.push_back({category, false, 'S', false});
    }
  }
  vp->setSideExtras(captions);
  restyle();
  vp->setCompare(m_parts, m_arrows, theme::current().diffMoved);
  if (needs->empty()) return;
  struct Out {
    std::vector<TopoDS_Shape> shapes;
    std::vector<std::shared_ptr<const BodyPrs>> prs;
    std::vector<opad::Mat4> worlds;
  };
  auto out = std::make_shared<Out>();
  out->shapes.resize(needs->size());
  out->prs.resize(needs->size());
  out->worlds.resize(needs->size());
  m_services.jobs()->async(tr("Drawing the other files"), [needs, out](Progress p) {
    std::map<std::pair<const opad::Document*, std::string>, size_t> meshed;  // instances share their mesh
    for (size_t i = 0; i < needs->size(); ++i) {
      if (p.cancelled()) return;
      const Need& n = (*needs)[i];
      p.setPhase(CompareMode::tr("Meshing body %1 of %2").arg(i + 1).arg(needs->size()), int(i * 100 / needs->size()));
      out->worlds[i] = n.world;
      const bool rigid = opad::mat_is_rigid(n.world);
      if (const auto it = meshed.find({n.doc.get(), n.key}); rigid && it != meshed.end()) {
        out->shapes[i] = out->shapes[it->second];
        out->prs[i] = out->prs[it->second];
        continue;
      }
      try {
        TopoDS_Shape shape = BRepBuilderAPI_Copy(opad::body_shape(*n.doc, n.key)).Shape();
        if (!rigid) {  // a scaled placement: baked into a copy drawn at identity
          gp_GTrsf g;
          for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col) g.SetValue(row + 1, col + 1, n.world.at(row, col));
          shape = BRepBuilderAPI_GTransform(shape, g, Standard_True).Shape();
          out->worlds[i] = opad::Mat4();
        }
        Bnd_Box box;
        BRepBndLib::Add(shape, box, Standard_False);
        BodyPrs::meshForDisplay(shape, deflection(box));
        out->shapes[i] = shape;
        auto prs = BodyPrs::build(shape, box, true, nullptr);
        prs->deflection = deflection(box);  // refined when seen close up, as the document's bodies are
        prs->buildNavigation();  // the orbit pivot lands on it, in its pane
        out->prs[i] = std::move(prs);
        if (rigid) meshed[{n.doc.get(), n.key}] = i;
      } catch (const std::exception&) {  // unreadable: not drawn
      }
    }
  }, [this, self = QPointer<CompareMode>(this), needs, out, serial](bool ok, const QString&) {
    if (!self || !ok || serial != m_extraSerial || !m_active) return;
    for (size_t i = 0; i < needs->size(); ++i) {
      const size_t k = (*needs)[i].part;
      if (k >= m_parts.size()) continue;
      m_parts[k].shape = out->shapes[i];
      m_parts[k].prs = out->prs[i];
      m_parts[k].world = out->worlds[i];
    }
    restyle();
    m_services.viewport()->setCompare(m_parts, m_arrows, theme::current().diffMoved);
  });
}

void CompareMode::restyle() {
  if (!m_active || !m_run) return;
  trace::Scope scope("Compare: restyle");
  const Run& r = *m_run;
  const Tokens& t = theme::current();
  // The emphasis: B alone at 100, A alone at 0, both from the middle out; A's ghosts stay translucent while B is there.
  // Side by side, each view shows its version whole.
  const double e = m_sideBySide ? 0.5 : m_panel->emphasis() / 100.0;
  const double wA = std::min(1.0, 2 * (1 - e)), wB = std::min(1.0, 2 * e);
  const double opB = wB, opA = m_sideBySide ? 1.0 : wA * (0.45 + 0.55 * (1 - wB));
  constexpr double kGone = 0.04;
  auto rgb = [](const QColor& c) { return std::array<double, 3>{c.redF(), c.greenF(), c.blueF()}; };
  auto shown = [this](int c) { return m_panel->shown(Category(c)); };
  for (size_t k = 0; k < m_parts.size(); ++k) {
    auto& part = m_parts[k];
    const PartInfo& info = m_partInfo[k];
    if (info.category == ComparePanel::Unchanged) {
      part.color = rgb(t.ghost);
      part.opacity = t.ghost.alphaF();
    } else {
      part.color = info.faced ? muted(t) : rgb(t.*ComparePanel::colour(Category(info.category)));
      part.opacity = info.sideA ? opA : opB;
    }
    const bool laidOut = info.layout == 0 || (info.layout == 'S') == m_sideBySide;
    part.visible = laidOut && shown(info.category) && part.opacity > kGone;
  }
  for (auto& a : m_arrows) a.visible = shown(ComparePanel::Moved);
  // The session's bodies: tinted where B changed them, ghosts where it did not (the roots' entry), left out where B has
  // none of them as they are.
  std::map<std::string, LookDelta> deltas;
  LookDelta ghost;
  ghost.ghost = true;
  if (!shown(ComparePanel::Unchanged)) ghost.visible = false;
  AppDocument* doc = m_services.document();
  for (const auto& root : doc->scene.roots) deltas[root] = ghost;
  LookDelta gone;
  gone.visible = false;
  for (const auto& id : r.hidden) deltas[id] = gone;
  auto tint = [&](int category, double opacity) {
    LookDelta d;
    if (!shown(category) || opacity <= kGone) d.visible = false;
    else {
      d.color = rgb(t.*ComparePanel::colour(Category(category)));
      d.opacity = opacity;
      d.pickable = true;
    }
    return d;
  };
  // A modified body whose faces were matched: the faces the change added or cut into in their colours, the rest muted.
  std::map<std::string, std::shared_ptr<const opad::FaceColors>> faceTints;
  for (size_t i = 0; i < r.bodies.size(); ++i) {
    const auto& c = r.bodies[i];
    if (!r.sessionDraws[i] || c.kind == opad::BodyChange::Kind::Unchanged) continue;
    LookDelta d = tint(categoryOf(c.kind), opB);
    if (d.color && i < r.faces.size() && r.faces[i])
      if (auto colors = faceTint(*r.faces[i], false, t, shown(ComparePanel::Added))) {
        d.color = muted(t);
        faceTints[c.id] = std::move(colors);
      }
    deltas[c.id] = d;
  }
  for (const auto& s : doc->scene.sketches) {
    const auto it = r.sketches.find(s.id);
    deltas[s.id] = it == r.sketches.end() ? ghost : tint(it->second, std::max(opB, 0.5));
  }
  Viewport* vp = m_services.viewport();
  vp->setLookLayer(LookSource::Compare, std::move(deltas));
  vp->setFaceTints(std::move(faceTints));
  vp->restyleCompare(m_parts, m_arrows);
}

void CompareMode::activate(int change) {
  if (!m_active || !m_run || change < 0 || size_t(change) >= m_run->changeBodies.size()) return;
  AppDocument* doc = m_services.document();
  Viewport* vp = m_services.viewport();
  const opad::json& c = m_run->diff["changes"][size_t(change)];
  const std::string kind = c.value("kind", ""), id = c.value("id", "");
  Bnd_Box box = m_run->changeBox[size_t(change)];
  std::vector<std::string> drawn;  // what the session draws of it: selected
  for (const auto& b : m_run->changeBodies[size_t(change)]) {
    const auto it = m_run->at.find(b);
    if (it == m_run->at.end() || !m_run->sessionDraws[it->second] || !doc->scene.node(b)) continue;
    drawn.push_back(b);
    box.Add(opad::node_world_bbox(doc->doc, doc->scene, b));
  }
  if (!drawn.empty()) vp->selectNodes(drawn);
  else vp->clearSelection();
  if (kind == "sketch" && doc->scene.sketch(id)) vp->fitNodes({id});
  else if (!box.IsVoid()) vp->fitBox(box);
  if ((kind == "feature" || kind == "sketch") && doc->doc.find_op(id))
    if (TimelineWidget* t = m_services.timeline()) t->setCurrentOp(id);
}

void CompareMode::step(int delta) {
  if (!m_active) return;
  const std::vector<int> order = m_panel->order();
  if (order.empty()) return;
  const int n = int(order.size());
  const auto it = std::find(order.begin(), order.end(), m_panel->current());
  const int k = it == order.end() ? (delta > 0 ? 0 : n - 1) : ((int(it - order.begin()) + delta) % n + n) % n;
  m_panel->setCurrent(order[size_t(k)]);
  activate(order[size_t(k)]);
}

void CompareMode::selectionChanged(const SelectionContext& selection) {
  if (!m_active || !m_run || selection.ids.empty()) return;
  const std::string& id = selection.ids.front();
  auto about = [&](int k) {
    const auto& ids = m_run->changeBodies[size_t(k)];
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  if (m_panel->current() >= 0 && about(m_panel->current())) return;  // a row's own selection
  for (int k : m_panel->order())
    if (about(k)) return m_panel->setCurrent(k);
}

void CompareMode::documentChanged(bool replaced) {
  AppDocument* doc = m_services.document();
  if (!pendingStartup.isEmpty() && doc->hasDocument && !doc->loading) {  // opad --compare a b: b has opened
    const CompareVersion a = parseVersion(std::exchange(pendingStartup, QString()));
    return compare(a, sessionVersion());
  }
  if (!m_pendingFiles.isEmpty() && doc->hasDocument && !doc->loading)  // Compare files… with nothing open: the first has opened
    return openFiles(std::exchange(m_pendingFiles, {}));
  if (m_pending && doc->hasDocument && !doc->loading) {  // compareIn: its file has opened
    const Pending p = *std::exchange(m_pending, std::nullopt);
    if (!doc->browse && !doc->doc.path.empty() && QFileInfo(doc->path()) == QFileInfo(p.file)) return compare(p.a, p.b);
  }
  if (!m_active) return;
  if (replaced || !doc->hasDocument) return close();
  showVersions();  // "unsaved changes" comes and goes
  m_rerun.start();  // B (or the session under it) changed
}

// Three files or more (Compare files…): every body of A against each other file in one table, a column per file, the one B
// shows marked; a click on a column shows that file as B. Read and matched on a worker (opad::variant_table), again only
// when A or the files change.
void CompareMode::variantTable() {
  if (!m_active || !m_run || m_a < 0) return;
  AppDocument* doc = m_services.document();
  // The file viewed is read again as a file (the session may hold edits; the table is of files): a column of its own unless
  // it is also listed as a file.
  auto path = [&](const CompareVersion& v) { return v.kind == Kind::Session ? doc->viewing : v.ref; };
  auto listed = [&](const QString& file) {
    return std::any_of(m_versions.begin(), m_versions.end(), [&](const CompareVersion& v) { return v.kind == Kind::File && QFileInfo(v.ref) == QFileInfo(file); });
  };
  const auto fileLike = [&](const CompareVersion& v) {
    return v.kind == Kind::File || (v.kind == Kind::Session && doc->browse && !doc->viewing.isEmpty() && !listed(doc->viewing));
  };
  const CompareVersion a = m_versions[size_t(m_a)];
  std::vector<int> columns;
  for (size_t i = 0; i < m_versions.size(); ++i)
    if (int(i) != m_a && fileLike(m_versions[i]) && QFileInfo(path(m_versions[i])) != QFileInfo(path(a))) columns.push_back(int(i));
  if (!fileLike(a) || columns.size() < 2) {
    m_variantColumns.clear();
    m_tableKey.clear();
    m_variantBodies.clear();
    for (auto& d : m_variantDocs) dispose(std::move(d));
    m_variantDocs.clear();
    m_panel->setVariants({}, opad::json::array(), -1);
    if (m_sideBySide) buildExtras();
    return;
  }
  auto column = [&] { return int(std::find(columns.begin(), columns.end(), m_b) - columns.begin()); };
  QStringList labels;
  QString key = a.ref + "|" + QString::number(int(a.kind));
  for (int c : columns) labels << m_versions[size_t(c)].label, key += "|" + m_versions[size_t(c)].ref;
  if (key == m_tableKey) return m_panel->setVariants(labels, std::nullopt, column());  // the same table: B's column marked
  m_tableKey = key;
  m_variantColumns = columns;
  m_variantBodies.clear();
  for (auto& d : m_variantDocs) dispose(std::move(d));
  m_variantDocs.clear();
  m_panel->setVariants(labels, opad::json(), column());  // reading
  struct Out {
    std::vector<std::pair<QString, std::shared_ptr<opad::Document>>> read;
    opad::json rows;
    std::vector<std::vector<VariantBody>> bodies;  // per column: its file's bodies as paired with A's
    std::vector<std::shared_ptr<opad::Document>> docs;
  };
  auto out = std::make_shared<Out>();
  std::vector<QString> paths{path(a)};
  for (int c : columns) paths.push_back(path(m_versions[size_t(c)]));
  const Files files = m_files;
  m_services.jobs()->async(tr("Comparing the variants"), [paths, files, out](Progress p) {
    std::vector<std::shared_ptr<opad::Document>> docs;
    std::vector<opad::Scene> scenes;
    for (size_t i = 0; i < paths.size(); ++i) {
      if (p.cancelled()) return;
      p.setPhase(CompareMode::tr("Reading %1").arg(QFileInfo(paths[i]).fileName()), int(i * 50 / paths.size()));
      const auto it = files.find(paths[i]);
      docs.push_back(it != files.end() ? it->second : std::make_shared<opad::Document>(opad::read_variant(fsPath(paths[i]))));
      if (it == files.end()) out->read.emplace_back(paths[i], docs.back());
    }
    for (const auto& d : docs) scenes.push_back(opad::resolve(*d));
    p.setPhase(CompareMode::tr("Matching the bodies"), 50);
    std::vector<const opad::Document*> dp;
    std::vector<const opad::Scene*> sp;
    for (size_t i = 0; i < docs.size(); ++i) dp.push_back(docs[i].get()), sp.push_back(&scenes[i]);
    out->rows = opad::variant_table(dp, sp, [p] { return p.cancelled(); });
    // each column's bodies, for its pane side by side (C, D)
    out->bodies.resize(docs.size() - 1);
    for (size_t k = 1; k < docs.size(); ++k) out->docs.push_back(docs[k]);
    for (const auto& row : out->rows)
      for (size_t k = 0; k + 1 < docs.size(); ++k) {
        const opad::json cells = row.value("cells", opad::json::array());
        if (k >= cells.size() || !cells[k].is_object()) continue;
        const opad::json& cell = cells[k];
        const std::string id = cell.value("id", "");
        const opad::Node* n = id.empty() ? nullptr : scenes[k + 1].node(id);
        if (!n || n->kind != opad::Node::Kind::Body || !scenes[k + 1].effectively_visible(id)) continue;
        out->bodies[k].push_back({id, n->body_key, scenes[k + 1].world(id), cell.value("change", "")});
      }
  }, [this, self = QPointer<CompareMode>(this), out, key, labels](bool ok, const QString& error) {
    if (!self || !m_active || key != m_tableKey) return;
    for (auto& [file, d] : out->read) m_files.emplace(file, d);
    if (!ok) {
      m_tableKey.clear();
      if (error != "cancelled") m_services.showMessage(i18n::t(error), 6000);
      return m_panel->setVariants({}, opad::json::array(), -1);
    }
    const int current = int(std::find(m_variantColumns.begin(), m_variantColumns.end(), m_b) - m_variantColumns.begin());
    m_panel->setVariants(labels, out->rows, current);
    m_variantBodies = std::move(out->bodies);
    m_variantDocs = std::move(out->docs);
    if (m_sideBySide) buildExtras();
  });
}

bool CompareMode::settled() const {
  return m_active && m_run && !m_running && !m_meshing && !m_rerun.isActive() && !m_restyle.isActive() && !m_services.viewport()->faceTintsPending();
}
opad::json CompareMode::changes() const { return m_run ? m_run->diff["changes"] : opad::json::array(); }
std::string CompareMode::relation() const { return m_run ? m_run->diff.value("relation", "") : std::string(); }

Bnd_Box CompareMode::boxInA(const std::string& body) const {
  const auto it = m_run ? m_run->at.find(body) : std::unordered_map<std::string, size_t>::const_iterator();
  return m_run && it != m_run->at.end() && it->second < m_run->boxA.size() ? m_run->boxA[it->second] : Bnd_Box();
}

std::string CompareMode::summary() const { return m_run ? m_run->diff.value("summary", "") : std::string(); }
