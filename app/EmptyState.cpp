#include "EmptyState.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "FileLocation.hpp"
#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "Icons.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "opad/cache.hpp"

OPAD_ICON_TABLE(startpage, {"folder", R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>)"},
                {"template", R"(<rect x="4" y="3" width="16" height="18" rx="2"/><path d="M8 8h8M8 12h8M8 16h5"/>)"},
                {"inch", R"(<path d="M3 17 17 3l4 4L7 21z"/><path d="M7 13l2 2M10 10l2 2M13 7l2 2"/>)"});

namespace {

QString typeIcon(const QString& path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix == "opad") return "doc";
  if (QStringList{"dxf", "dwg", "svg"}.contains(suffix)) return "drawing";
  if (QStringList{"stl", "3mf", "obj", "ply", "gltf", "glb", "wrl", "vrml"}.contains(suffix)) return "mesh";
  return "step";
}

// What renders the pictures: opad-cli beside the app (the Explorer thumbnails run the same one), else the app itself
// (opad --thumbnail, main.cpp: the single-file exe has no opad-cli beside it). OPAD_THUMBNAILS=self takes the app always.
struct Thumbnailer {
  QString program;
  QStringList args;  // before <file> --out <picture> --size 256
};
Thumbnailer thumbnailer() {
#ifdef Q_OS_WIN
  const QString cli = QCoreApplication::applicationDirPath() + "/opad-cli.exe";
#else
  const QString cli = QCoreApplication::applicationDirPath() + "/opad-cli";
#endif
  if (QFileInfo::exists(cli) && qEnvironmentVariable("OPAD_THUMBNAILS") != "self") return {cli, {"--compact", "thumbnail"}};
  return {QCoreApplication::applicationFilePath(), {"--thumbnail"}};
}

// The .bgra the CLI writes: "OPADTHMB", width and height (uint32 little endian), premultiplied BGRA rows top-down.
QImage readBgra(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  const QByteArray data = f.readAll();
  if (data.size() < 16 || !data.startsWith("OPADTHMB")) return {};
  auto u32 = [&data](int at) { return quint32(uchar(data[at])) | quint32(uchar(data[at + 1])) << 8 | quint32(uchar(data[at + 2])) << 16 | quint32(uchar(data[at + 3])) << 24; };
  const int w = int(u32(8)), h = int(u32(12));
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || data.size() < 16 + qsizetype(w) * h * 4) return {};
  QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < h; ++y) memcpy(image.scanLine(y), data.constData() + 16 + qsizetype(y) * w * 4, size_t(w) * 4);
  return image;
}

struct FileState {
  bool exists = false;
  QDateTime modified;
  QImage picture;
};

// The thumbnailer goes with OPAD: a quit while it renders (the single-file build's own exe) never leaves it behind.
void tieToApp(const QProcess& process) {
#ifdef Q_OS_WIN
  static const HANDLE killOnClose = [] {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    return job;
  }();
  if (HANDLE child = killOnClose ? OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId())) : nullptr) {
    AssignProcessToJobObject(killOnClose, child);
    CloseHandle(child);
  }
#else
  Q_UNUSED(process);
#endif
}

// On the worker: the file's state, and its picture from the cache or rendered by the thumbnailer (killed when the job is
// cancelled, when the page hides, or after two minutes). A file it cannot picture (an empty document) or not in two minutes
// is remembered as such, until it changes.
FileState readFile(const QString& path, const Thumbnailer& cli, const QString& cache, const Progress& progress) {
  FileState s;
  const QFileInfo info(path);
  s.exists = info.isFile();
  if (!s.exists) return s;
  s.modified = info.lastModified();
  const QByteArray key = QCryptographicHash::hash(QString("%1|%2|%3|256").arg(QDir::cleanPath(info.absoluteFilePath())).arg(info.size()).arg(s.modified.toMSecsSinceEpoch()).toUtf8(),
                                                  QCryptographicHash::Sha1).toHex();
  const QString png = cache + "/" + key + ".png", none = cache + "/" + key + ".none";
  if (QFile::exists(png) && s.picture.load(png)) return s;
  if (QFile::exists(none) || info.size() > (qint64(400) << 20)) return s;
  QDir().mkpath(cache);
  const QString raw = cache + "/" + key + ".bgra";
  QProcess process;
  process.start(cli.program, cli.args + QStringList{info.absoluteFilePath(), "--out", raw, "--size", "256"});
  if (!process.waitForStarted(10000)) return s;
  tieToApp(process);
  QElapsedTimer clock;
  clock.start();
  while (!process.waitForFinished(100))
    if (progress.cancelled() || clock.elapsed() > 120000) {
      process.kill();
      process.waitForFinished(3000);
      QFile::remove(raw);
      if (!progress.cancelled()) QFile(none).open(QIODevice::WriteOnly);  // too slow: not tried again on every show
      return s;
    }
  if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) s.picture = readBgra(raw);
  if (!s.picture.isNull()) s.picture.save(png);
  else if (process.exitStatus() == QProcess::NormalExit) QFile(none).open(QIODevice::WriteOnly);
  QFile::remove(raw);
  return s;
}

QPushButton* link(const QString& icon, const QString& text, QWidget* parent) {
  auto* b = new QPushButton(text, parent);
  b->setObjectName("startLink");
  b->setProperty("iconName", icon);
  b->setIcon(icons::themed(icon, 16));
  b->setCursor(Qt::PointingHandCursor);
  return b;
}

QLabel* header(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setObjectName("sectionHeader");
  return l;
}

QFrame* infoCard(const QString& icon, const QString& title, const QString& body, QWidget* parent) {
  auto* card = new QFrame(parent);
  card->setObjectName("infoCard");
  auto* l = new QVBoxLayout(card);
  l->setContentsMargins(12, 12, 12, 12);
  l->setSpacing(6);
  auto* head = new QHBoxLayout();
  auto* ic = new QLabel(card);
  ic->setPixmap(icons::pixmap(icon, theme::current().fg2, 16, card->devicePixelRatioF()));
  QObject::connect(theme::notifier(), &theme::Notifier::changed, ic, [ic, icon, card] { ic->setPixmap(icons::pixmap(icon, theme::current().fg2, 16, card->devicePixelRatioF())); });
  head->addWidget(ic);
  auto* t = new QLabel(title, card);
  t->setFont(theme::ui(13, QFont::Medium));
  head->addWidget(t, 1);
  l->addLayout(head);
  auto* b = new QLabel(body, card);
  b->setObjectName("secondary");
  b->setWordWrap(true);
  l->addWidget(b);
  return card;
}
}  // namespace

// ---------------------------------------------------------------- templates
namespace templates {
QString folder() {
  const QString chosen = QSettings().value("files/templates").toString();
  if (!chosen.isEmpty()) return chosen;
  const QSettings settings;
  if (settings.format() == QSettings::IniFormat) return QFileInfo(settings.fileName()).absolutePath() + "/templates";
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/templates";
}

QList<Entry> list() {
  QList<Entry> out{{"builtin:mm", QCoreApplication::translate("EmptyState", "Design in millimetres"), "doc"},
                   {"builtin:in", QCoreApplication::translate("EmptyState", "Design in inches"), "inch"},
                   {"builtin:sketch", QCoreApplication::translate("EmptyState", "Sketch on a plane"), "sketch"}};
  // A few files in the user's own folder: listed where the page or the menu shows them.
  for (const QFileInfo& f : QDir(folder()).entryInfoList({"*.opad"}, QDir::Files, QDir::Name | QDir::IgnoreCase))
    out.push_back({f.absoluteFilePath(), f.completeBaseName(), "template"});
  return out;
}
}  // namespace templates

// ---------------------------------------------------------------- RecentCard
RecentCard::RecentCard(const QString& path, QWidget* parent) : QAbstractButton(parent), m_path(path) {
  setObjectName("recentCard");
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::PointingHandCursor);
  setFixedSize(kWidth, kHeight);
  describe();
}

void RecentCard::setState(State state, const QDateTime& modified) {
  m_state = state;
  m_modified = modified;
  describe();
  update();
}

void RecentCard::setPicture(const QImage& picture) {
  m_picture = picture;
  update();
}

void RecentCard::describe() {
  const QFileInfo info(m_path);
  const QString folder = QDir::toNativeSeparators(info.absolutePath());
  setAccessibleName(info.fileName());
  QString state = m_state == State::Missing ? tr("Missing: the file is not there any more") : m_modified.isValid() ? tr("Modified %1").arg(QLocale().toString(m_modified, QLocale::ShortFormat)) : QString();
  setAccessibleDescription(state.isEmpty() ? folder : folder + " · " + state);
  setToolTip(QDir::toNativeSeparators(m_path) + (state.isEmpty() ? QString() : "\n" + state));
}

void RecentCard::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setLayoutDirection(Qt::LeftToRight);  // a card's insides are laid out by hand; mirrored below where they need it
  const bool hover = underMouse(), missing = m_state == State::Missing;
  const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
  p.setPen(QPen(m_keyFocus ? t.sel : hover ? t.fg3 : t.line, m_keyFocus ? 2 : 1));
  p.setBrush(isDown() ? t.bg4 : hover ? t.bg3 : t.bg2);
  p.drawRoundedRect(r, 4, 4);
  const QRect picture(7, 7, width() - 14, kPicture);
  QPainterPath clip;
  clip.addRoundedRect(picture, 3, 3);
  p.save();
  p.setClipPath(clip);
  p.fillRect(picture, t.vp);
  if (!m_picture.isNull()) {
    const QSizeF fitted = QSizeF(m_picture.size()).scaled(QSizeF(picture.size()) * 0.96, Qt::KeepAspectRatio);
    const QRectF at(QPointF(picture.center()) - QPointF(fitted.width(), fitted.height()) / 2, fitted);
    p.setOpacity(missing ? 0.35 : 1.0);
    p.drawImage(at, m_picture);
    p.setOpacity(1.0);
  } else {
    const int s = 40;
    p.drawPixmap(QRect(picture.center().x() - s / 2, picture.center().y() - s / 2, s, s), icons::pixmap(typeIcon(m_path), missing ? t.fg3 : t.fg2, s, devicePixelRatioF()));
  }
  p.restore();
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  if (missing) {  // never colour alone: the cue's sign and the word
    const theme::Cue* cue = theme::cue("assetMissing");
    const QString text = (cue ? QString::fromUtf8(cue->mark) + " " : QString()) + tr("Missing");
    p.setFont(theme::ui(11, QFont::Medium));
    const int w = p.fontMetrics().horizontalAdvance(text) + 12;
    const QRect pill(rtl ? picture.right() - 6 - w : picture.left() + 6, picture.top() + 6, w, 18);
    p.setPen(Qt::NoPen);
    p.setBrush(cue ? t.*(cue->colour) : t.error);
    p.drawRoundedRect(pill, 9, 9);
    p.setPen(t.onsel);
    p.drawText(pill, Qt::AlignCenter, text);
  }
  const QFileInfo info(m_path);
  const int x = 10, w = width() - 20;
  const Qt::Alignment side = rtl ? Qt::AlignRight : Qt::AlignLeft;
  p.setFont(theme::ui(12, QFont::Medium));
  p.setPen(missing ? t.fg3 : t.fg);
  p.drawText(QRect(x, picture.bottom() + 7, w, 18), side | Qt::AlignVCenter, p.fontMetrics().elidedText(info.fileName(), Qt::ElideRight, w));
  p.setFont(theme::ui(11));
  p.setPen(t.fg3);
  p.drawText(QRect(x, picture.bottom() + 25, w, 16), side | Qt::AlignVCenter, p.fontMetrics().elidedText(QDir::toNativeSeparators(info.absolutePath()), Qt::ElideMiddle, w));
  const QString when = missing ? tr("Not found") : m_modified.isValid() ? QLocale().toString(m_modified, QLocale::ShortFormat) : QString();
  p.drawText(QRect(x, picture.bottom() + 41, w, 16), side | Qt::AlignVCenter, when);
}

void RecentCard::contextMenuEvent(QContextMenuEvent* e) {
  emit menuRequested(e->globalPos());
  e->accept();
}

void RecentCard::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Menu || (e->key() == Qt::Key_F10 && e->modifiers() == Qt::ShiftModifier)) return emit menuRequested(mapToGlobal(QPoint(12, kPicture)));
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) return click();
  QAbstractButton::keyPressEvent(e);
}

void RecentCard::focusInEvent(QFocusEvent* e) {
  m_keyFocus = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason || e->reason() == Qt::ShortcutFocusReason;
  QAbstractButton::focusInEvent(e);
  update();
}

void RecentCard::focusOutEvent(QFocusEvent* e) {
  m_keyFocus = false;
  QAbstractButton::focusOutEvent(e);
  update();
}

// ---------------------------------------------------------------- EmptyState
EmptyState::EmptyState(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setObjectName("central");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* scroll = new QScrollArea(this);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* page = new QWidget(scroll);
  page->setObjectName("startPage");
  scroll->setWidget(page);
  outer->addWidget(scroll);
  auto* rows = new QVBoxLayout(page);
  rows->setContentsMargins(24, 40, 24, 24);
  auto* center = new QHBoxLayout();
  rows->addLayout(center);
  rows->addStretch(1);
  auto* content = new QWidget(page);
  content->setMaximumWidth(1080);
  center->addStretch(1);
  center->addWidget(content, 100);
  center->addStretch(1);
  auto* columns = new QHBoxLayout(content);
  columns->setContentsMargins(0, 0, 0, 0);
  columns->setSpacing(40);

  // Start, templates, learn.
  auto* left = new QWidget(content);
  left->setFixedWidth(248);
  auto* l = new QVBoxLayout(left);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(6);
  l->addWidget(header(tr("START"), left));
  auto* open = new QPushButton(icons::icon("open", theme::current().onsel), tr("Open   Ctrl+O"), left);
  open->setObjectName("primary");
  auto* create = new QPushButton(icons::themed("doc", 16), tr("New Document   Ctrl+N"), left);
  m_clone = link("git", QString(), left);
  m_clone->hide();
  for (QPushButton* b : {open, create}) b->setProperty("startButton", true);
  l->addWidget(open);
  l->addWidget(create);
  l->addWidget(m_clone);
  l->addSpacing(10);
  l->addWidget(header(tr("NEW FROM TEMPLATE"), left));
  m_templates = new QVBoxLayout();
  m_templates->setSpacing(2);
  l->addLayout(m_templates);
  auto* folder = link("folder", tr("Templates folder…"), left);
  folder->setObjectName("startLinkSmall");
  folder->setToolTip(tr("The folder whose .opad files are offered here; File › Save as template puts one there"));
  l->addWidget(folder);
  l->addSpacing(10);
  l->addWidget(header(tr("LEARN"), left));
  auto* learn = new QVBoxLayout();
  learn->setSpacing(2);
  l->addLayout(learn);
  for (const char* id : {"help.start", "help.reference", "help.shortcuts"}) {
    QPushButton* b = link("help", QString(), left);
    b->hide();
    learn->addWidget(b);
    m_learnButtons.push_back({QString::fromLatin1(id), b});
  }
  l->addStretch(1);
  columns->addWidget(left, 0, Qt::AlignTop);

  // Recent files, or the drop zone while there are none.
  auto* right = new QWidget(content);
  auto* r = new QVBoxLayout(right);
  r->setContentsMargins(0, 0, 0, 0);
  r->setSpacing(8);
  auto* head = new QHBoxLayout();
  m_recentTitle = header(tr("RECENT"), right);
  head->addWidget(m_recentTitle);
  head->addStretch(1);
  m_clearMissing = link("delete", tr("Remove missing files"), right);
  m_clearMissing->setObjectName("startLinkSmall");
  m_clearMissing->hide();
  head->addWidget(m_clearMissing);
  r->addLayout(head);
  m_grid = new QWidget(right);
  m_gridLayout = new QGridLayout(m_grid);
  m_gridLayout->setContentsMargins(0, 0, 0, 0);
  m_gridLayout->setSpacing(12);
  m_gridLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  m_grid->installEventFilter(this);
  r->addWidget(m_grid);

  m_zone = new QWidget(right);
  auto* zl = new QVBoxLayout(m_zone);
  zl->setContentsMargins(0, 0, 0, 0);
  zl->setSpacing(16);
  auto* zone = new QFrame(m_zone);
  zone->setObjectName("dropzone");
  zone->setFixedHeight(220);
  auto* z = new QVBoxLayout(zone);
  z->setAlignment(Qt::AlignCenter);
  z->setSpacing(8);
  auto* ic = new QLabel(zone);
  ic->setObjectName("dropIcon");
  ic->setAlignment(Qt::AlignCenter);
  z->addWidget(ic);
  auto* title = new QLabel(tr("Drop a design file"), zone);
  title->setFont(theme::ui(20, QFont::Medium));
  title->setAlignment(Qt::AlignCenter);
  z->addWidget(title);
  const QString extensions = QString::fromUtf8(".opad · .step · .iges · .stl · .3mf · .obj · .gltf · .dxf · .dwg · .svg");
  auto* ext = new QLabel(extensions, zone);
  ext->setObjectName("secondary");
  ext->setFont(theme::mono(12));
  ext->setAlignment(Qt::AlignCenter);
  z->addWidget(ext);
  zl->addWidget(zone);
  auto* cards = new QHBoxLayout();
  cards->setSpacing(16);
  cards->addWidget(infoCard("browse", tr("View"), tr("Open any STEP, IGES, STL, 3MF, OBJ, glTF, DXF, DWG or SVG file read-only, at once: measure, section, hide, colour. Nothing is converted until you save."), m_zone));
  cards->addWidget(infoCard("import", tr("Edit"), tr("Save a viewed file as an .opad document to edit it. Geometry is stored once, every later change is one operation, and the file diffs and merges in git."), m_zone));
  zl->addLayout(cards);
  r->addWidget(m_zone);

  m_dropHint = new QFrame(right);
  m_dropHint->setObjectName("dropzone");
  auto* dl = new QHBoxLayout(m_dropHint);
  dl->setContentsMargins(12, 8, 12, 8);
  dl->setSpacing(8);
  auto* hintIcon = new QLabel(m_dropHint);
  hintIcon->setObjectName("dropIconSmall");
  dl->addWidget(hintIcon);
  auto* hint = new QLabel(tr("Drop files anywhere here to open them"), m_dropHint);
  hint->setObjectName("secondary");
  dl->addWidget(hint);
  dl->addStretch(1);
  auto* hintExt = new QLabel(extensions, m_dropHint);
  hintExt->setObjectName("tertiary");
  hintExt->setFont(theme::mono(11));
  dl->addWidget(hintExt);
  r->addSpacing(8);
  r->addWidget(m_dropHint);
  r->addStretch(1);
  columns->addWidget(right, 1, Qt::AlignTop);

  connect(open, &QPushButton::clicked, this, &EmptyState::openRequested);
  connect(create, &QPushButton::clicked, this, &EmptyState::newRequested);
  connect(folder, &QPushButton::clicked, this, [] {
    const QString dir = templates::folder();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
  });
  connect(m_clearMissing, &QPushButton::clicked, this, [this] {
    QStringList gone;
    for (RecentCard* c : m_cards)
      if (c->state() == RecentCard::State::Missing) gone << c->path();
    removeRecent(gone);
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, &EmptyState::restyle);
  restyle();
  fillTemplates();
  setRecent({});
}

void EmptyState::restyle() {
  const Tokens& t = theme::current();
  setStyleSheet(theme::scaledSheet(QString("QWidget#startPage { background: %1; }\n"
                        "QPushButton#startLink, QPushButton#startLinkSmall { text-align: left; border: 1px solid transparent; background: transparent; padding: 0 8px; color: %2; }\n"
                        "QPushButton#startLinkSmall { color: %3; font-size: 12px; height: 24px; min-height: 22px; }\n"
                        "QPushButton#startLink:hover, QPushButton#startLinkSmall:hover { background: %4; }\n"
                        "QPushButton#startLink[keyFocus=\"true\"], QPushButton#startLinkSmall[keyFocus=\"true\"] { border-color: %5; }\n"
                        "QPushButton[startButton=\"true\"] { text-align: left; padding: 0 12px; }\n")
                    .arg(theme::css(t.bg), theme::css(t.fg), theme::css(t.fg2), theme::css(t.bg3), theme::css(t.sel)), theme::textScale()));
  for (QPushButton* b : findChildren<QPushButton*>())
    if (b->objectName() == "primary") b->setIcon(icons::icon("open", t.onsel));
    else if (!b->property("iconName").toString().isEmpty()) b->setIcon(icons::themed(b->property("iconName").toString(), 16));
    else if (b->property("startButton").toBool()) b->setIcon(icons::themed("doc", 16));
  if (auto* ic = findChild<QLabel*>("dropIcon")) ic->setPixmap(icons::pixmap("import", t.fg2, 40, devicePixelRatioF()));
  if (auto* ic = findChild<QLabel*>("dropIconSmall")) ic->setPixmap(icons::pixmap("import", t.fg3, 16, devicePixelRatioF()));
  for (RecentCard* c : m_cards) c->update();
}

void EmptyState::setCommands(std::function<QAction*(const QString&)> lookup) {
  m_lookup = std::move(lookup);
  fillCommands();
}

// Learn and Clone mirror their commands: the label, the enabled state, the click; a command the build lacks hides its link.
void EmptyState::fillCommands() {
  auto bind = [this](QPushButton* b, QAction* a) {
    b->setVisible(a);
    if (!a) return;
    b->setText(a->text().remove('&'));
    b->setEnabled(a->isEnabled());
    b->setToolTip(a->toolTip());
    b->setAccessibleName(a->text().remove('&'));
    if (!b->property("bound").toBool()) {
      b->setProperty("bound", true);
      connect(b, &QPushButton::clicked, a, &QAction::trigger);
      connect(a, &QAction::changed, b, [b, a] { b->setEnabled(a->isEnabled()); });
    }
  };
  for (const auto& [id, b] : m_learnButtons) bind(b, m_lookup ? m_lookup(id) : nullptr);
  bind(m_clone, m_lookup ? m_lookup("file.clone") : nullptr);
  if (m_lookup)
    for (const auto& [id, icon] : QList<QPair<QString, QString>>{{"help.start", "start"}, {"help.reference", "list"}, {"help.shortcuts", "keyboard"}})
      for (const auto& [learn, b] : m_learnButtons)
        if (learn == id) {
          b->setProperty("iconName", icon);
          b->setIcon(icons::themed(icon, 16));
        }
}

QList<QPushButton*> EmptyState::learnButtons() const {
  QList<QPushButton*> out;
  for (const auto& [id, b] : m_learnButtons)
    if (!b->isHidden()) out << b;
  return out;
}

void EmptyState::fillTemplates() {
  for (QPushButton* b : std::exchange(m_templateButtons, {})) b->deleteLater();
  for (const templates::Entry& e : templates::list()) {
    QPushButton* b = link(e.icon, e.title, this);
    b->setToolTip(e.id.startsWith("builtin:") ? b->text() : QDir::toNativeSeparators(e.id));
    connect(b, &QPushButton::clicked, this, [this, id = e.id] { emit templateChosen(id); });
    m_templates->addWidget(b);
    m_templateButtons << b;
  }
}

void EmptyState::setJobs(JobRunner* jobs) {
  m_jobs = jobs;
  if (isVisible()) refresh();
}

void EmptyState::setRecent(const QStringList& paths) {
  const bool same = paths == m_paths && !m_cards.isEmpty();
  m_paths = paths;
  if (!same) {
    QHash<QString, RecentCard*> kept;  // a card that stays keeps its picture and state
    for (RecentCard* c : std::exchange(m_cards, {})) {
      if (paths.contains(c->path()) && !kept.contains(c->path())) kept.insert(c->path(), c);
      else {
        c->hide();
        c->deleteLater();
      }
    }
    for (const QString& path : paths) {
      if (RecentCard* card = kept.take(path)) {
        m_cards << card;
        continue;
      }
      auto* card = new RecentCard(path, m_grid);
      connect(card, &RecentCard::clicked, this, [this, card] {
        if (card->state() != RecentCard::State::Missing) return emit recentChosen(card->path());
        QMenu* menu = cardMenu(card);  // a file that is gone: Locate… or Remove
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(card->mapToGlobal(QPoint(12, RecentCard::kPicture)));
      });
      connect(card, &RecentCard::menuRequested, this, [this, card](const QPoint& at) {
        QMenu* menu = cardMenu(card);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(at);
      });
      m_cards << card;
    }
    m_columns = 0;
    layoutCards();
  }
  const bool any = !paths.isEmpty();
  m_recentTitle->setVisible(any);
  m_grid->setVisible(any);
  m_dropHint->setVisible(any);
  m_zone->setVisible(!any);
  m_clearMissing->setVisible(std::any_of(m_cards.begin(), m_cards.end(), [](const RecentCard* c) { return c->state() == RecentCard::State::Missing; }));
  if (isVisible()) refresh();
}

bool EmptyState::eventFilter(QObject* watched, QEvent* e) {
  if (watched == m_grid && e->type() == QEvent::Resize) layoutCards();
  return QWidget::eventFilter(watched, e);
}

void EmptyState::layoutCards() {
  const int gap = m_gridLayout->spacing();
  const int columns = std::max(1, (m_grid->width() + gap) / (RecentCard::kWidth + gap));
  if (columns == m_columns && m_gridLayout->count() == m_cards.size()) return;
  m_columns = columns;
  while (QLayoutItem* item = m_gridLayout->takeAt(0)) delete item;
  for (int i = 0; i < m_cards.size(); ++i) m_gridLayout->addWidget(m_cards[i], i / columns, i % columns);
  // Keyboard order follows the rows, as the cards read.
  for (int i = 1; i < m_cards.size(); ++i) setTabOrder(m_cards[i - 1], m_cards[i]);
}

void EmptyState::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  fillTemplates();  // a template saved meanwhile
  fillCommands();
  refresh();
}

// A document opened, the window closed: no thumbnailer keeps rendering behind them (shown again: read again).
void EmptyState::hideEvent(QHideEvent* e) {
  QWidget::hideEvent(e);
  if (m_job) m_job->cancel();
}

EmptyState::~EmptyState() {
  if (m_job) m_job->cancel();
}

// The files' states and pictures, read on a worker; the cards fill in as each file is read. A newer reading drops the older.
void EmptyState::refresh() {
  if (!m_jobs || m_paths.isEmpty()) return;
  if (m_job) m_job->cancel();
  const int generation = ++m_generation;
  const QStringList paths = m_paths;
  const Thumbnailer cli = thumbnailer();
  const auto dir = opad::cache_dir() / "start-page";
  const QString cache = QString::fromStdU16String(dir.u16string());
  QPointer<EmptyState> self(this);
  m_jobs->backgroundNext();  // nobody waits for it: no busy cursor, no completion toast
  m_job = m_jobs->async(tr("Reading recent files"), [paths, cli, cache, self, generation](Progress progress) {
    for (int i = 0; i < paths.size() && !progress.cancelled(); ++i) {
      progress.setPhase(QCoreApplication::translate("EmptyState", "Reading recent files (%1 of %2)").arg(i + 1).arg(paths.size()), i * 100 / paths.size());
      const FileState s = readFile(paths[i], cli, cache, progress);
      QMetaObject::invokeMethod(qApp, [self, generation, path = paths[i], s] { if (self) self->applyFile(generation, path, s.exists, s.modified, s.picture); }, Qt::QueuedConnection);
    }
  });
}

void EmptyState::applyFile(int generation, const QString& path, bool exists, const QDateTime& modified, const QImage& picture) {
  if (generation != m_generation) return;
  bool missing = false;
  for (RecentCard* c : m_cards) {
    if (c->path() == path) {
      c->setState(exists ? RecentCard::State::Present : RecentCard::State::Missing, modified);
      if (!picture.isNull()) c->setPicture(picture);
    }
    missing = missing || c->state() == RecentCard::State::Missing;
  }
  m_clearMissing->setVisible(missing);
}

// The window's recent-file menu (File > Recent's too, MainWindow::recentMenu: Open, Open file location, Copy path, Copy
// relative path, Remove from list); a file that is gone gets Locate… first.
QMenu* EmptyState::cardMenu(RecentCard* card) {
  const QString path = card->path();
  QMenu* menu = m_menu ? m_menu(path, this)
                       : location::recentMenu(path, this, [](const QString&) {}, [this, path] { emit recentChosen(path); }, [this, path] { removeRecent({path}); });
  menu->setObjectName("recentMenu");
  if (card->state() == RecentCard::State::Missing) {
    QAction* first = menu->actions().value(0);
    auto* locateAction = new QAction(icons::themed("locate", 16), tr("Locate…"), menu);
    locateAction->setObjectName("recent.locate");
    connect(locateAction, &QAction::triggered, this, [this, path] { locate(path); });
    menu->insertAction(first, locateAction);
  }
  return menu;
}

void EmptyState::removeRecent(const QStringList& paths) {
  QStringList kept = m_paths;
  for (const QString& p : paths) kept.removeAll(p);
  if (kept == m_paths) return;
  setRecent(kept);
  emit recentChanged(kept);
}

// A file that moved: chosen again where it is now, it takes the old one's place in the list.
void EmptyState::locate(const QString& path) {
  const QFileInfo old(path);
  QString dir = old.absolutePath();
  if (!QFileInfo(dir).isDir()) dir = QSettings().value("ui/lastDir").toString();
  const QString found = QFileDialog::getOpenFileName(this, tr("Locate %1").arg(old.fileName()), dir + "/" + old.fileName());
  if (found.isEmpty()) return;
  QStringList list = m_paths;
  const qsizetype at = list.indexOf(path);
  list.removeAll(found);
  if (at >= 0 && at < list.size()) list[at] = found;
  else list.prepend(found);
  setRecent(list);
  emit recentChanged(list);
}

void EmptyState::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void EmptyState::dropEvent(QDropEvent* e) {
  QStringList paths;
  for (const QUrl& u : e->mimeData()->urls()) paths << u.toLocalFile();
  if (!paths.isEmpty()) emit filesDropped(paths);
}
