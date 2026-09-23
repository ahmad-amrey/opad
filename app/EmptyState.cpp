#include "EmptyState.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QVBoxLayout>

#include "Icons.hpp"
#include "Theme.hpp"

static QFrame* infoCard(const QString& icon, const QString& title, const QString& body, QWidget* parent) {
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

EmptyState::EmptyState(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setObjectName("central");
  auto* outer = new QVBoxLayout(this);
  outer->addStretch(2);
  auto* center = new QHBoxLayout();
  outer->addLayout(center);
  outer->addStretch(3);
  center->addStretch();
  auto* column = new QWidget(this);
  column->setFixedWidth(640);
  auto* col = new QVBoxLayout(column);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(16);
  center->addWidget(column);
  center->addStretch();

  auto* zone = new QFrame(column);
  zone->setObjectName("dropzone");
  zone->setFixedHeight(260);
  auto* z = new QVBoxLayout(zone);
  z->setAlignment(Qt::AlignCenter);
  z->setSpacing(8);
  auto* ic = new QLabel(zone);
  ic->setPixmap(icons::pixmap("import", theme::current().fg2, 40, devicePixelRatioF()));
  ic->setAlignment(Qt::AlignCenter);
  connect(theme::notifier(), &theme::Notifier::changed, ic, [this, ic] { ic->setPixmap(icons::pixmap("import", theme::current().fg2, 40, devicePixelRatioF())); });
  z->addWidget(ic);
  auto* title = new QLabel(tr("Drop a design file"), zone);
  title->setFont(theme::ui(20, QFont::Medium));
  title->setAlignment(Qt::AlignCenter);
  z->addWidget(title);
  auto* ext = new QLabel(QString::fromUtf8(".step · .stp · .opad"), zone);
  ext->setObjectName("secondary");
  ext->setFont(theme::mono(12));
  ext->setAlignment(Qt::AlignCenter);
  z->addWidget(ext);
  col->addWidget(zone);
  // Buttons sit below the drop zone (design: Open primary · Import · Recent ▾).
  auto* buttons = new QHBoxLayout();
  buttons->setSpacing(8);
  buttons->addStretch();
  auto* open = new QPushButton(icons::icon("open", theme::current().onsel), tr("Open   Ctrl+O"), column);
  open->setObjectName("primary");
  auto* import = new QPushButton(icons::themed("import", 16), tr("Import   Ctrl+I"), column);
  m_recentButton = new QPushButton(icons::themed("recent", 16), tr("Recent ▾"), column);
  buttons->addWidget(open);
  buttons->addWidget(import);
  connect(theme::notifier(), &theme::Notifier::changed, this, [open, import, this] {
    open->setIcon(icons::icon("open", theme::current().onsel));
    import->setIcon(icons::themed("import", 16));
    m_recentButton->setIcon(icons::themed("recent", 16));
    setRecent(m_paths);
  });
  buttons->addWidget(m_recentButton);
  buttons->addStretch();
  col->addLayout(buttons);

  auto* cards = new QHBoxLayout();
  cards->setSpacing(16);
  cards->addWidget(infoCard("browse", tr("Browse"), tr("Open a design in a new document, then save geometry, measurements and notes together as OPAD."), column));
  cards->addWidget(infoCard("import", tr("Import"), tr("Bring the STEP into an .opad document. Geometry is stored once, every later change is one operation, and the file diffs and merges in git."), column));
  col->addLayout(cards);

  auto* recentTitle = new QLabel(tr("RECENT"), column);
  recentTitle->setObjectName("sectionHeader");
  col->addWidget(recentTitle);
  m_recent = new QListWidget(column);
  m_recent->setFixedHeight(28 * 5);
  m_recent->setFont(theme::mono(12));
  col->addWidget(m_recent);

  connect(open, &QPushButton::clicked, this, &EmptyState::openRequested);
  connect(import, &QPushButton::clicked, this, &EmptyState::importRequested);
  connect(m_recent, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { emit recentChosen(it->data(Qt::UserRole).toString()); });
  connect(m_recentButton, &QPushButton::clicked, this, [this] {
    QMenu menu(this);
    for (int i = 0; i < m_recent->count(); ++i) {
      QString path = m_recent->item(i)->data(Qt::UserRole).toString();
      menu.addAction(path, this, [this, path] { emit recentChosen(path); });
    }
    if (menu.isEmpty()) menu.addAction(tr("No recent files"))->setEnabled(false);
    menu.exec(m_recentButton->mapToGlobal(QPoint(0, m_recentButton->height())));
  });
}

void EmptyState::setRecent(const QStringList& paths) {
  m_paths = paths;
  m_recent->clear();
  for (const QString& p : paths) {
    auto* it = new QListWidgetItem(icons::themed(QFileInfo(p).suffix().toLower() == "opad" ? "doc" : "step", 16), QFileInfo(p).fileName() + "    " + QFileInfo(p).absolutePath());
    it->setData(Qt::UserRole, p);
    m_recent->addItem(it);
  }
}

void EmptyState::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void EmptyState::dropEvent(QDropEvent* e) {
  QStringList paths;
  for (const QUrl& u : e->mimeData()->urls()) paths << u.toLocalFile();
  if (!paths.isEmpty()) emit filesDropped(paths);
}
