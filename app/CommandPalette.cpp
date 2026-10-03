#include "CommandPalette.hpp"

#include <QAction>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

#include "Icons.hpp"
#include "Theme.hpp"

QString opGroup(const QAction* a) {
  QString id = a->objectName();
  if (id.startsWith("file.")) return "File";
  if (id.startsWith("view.")) return "View";
  if (id.startsWith("nav.")) return "Navigation";
  if (id.startsWith("select.")) return "Select";
  if (id.startsWith("inspect.")) return "Inspect";
  if (id.startsWith("annotate.") || id.startsWith("edit.")) return "Edit";
  if (id.startsWith("tools.")) return "Tools";
  return "Help";
}

// ---------------------------------------------------------------- CommandPalette
namespace {
int fuzzyScore(const QString& text, const QString& query, QList<int>* positions) {
  if (query.isEmpty()) return 1;
  int score = 0, qi = 0, last = -2;
  QString lt = text.toLower(), lq = query.toLower();
  for (int i = 0; i < lt.size() && qi < lq.size(); ++i) {
    if (lt[i] == lq[qi]) {
      score += (i == last + 1) ? 3 : 1;
      if (i == 0 || lt[i - 1] == ' ') score += 2;
      if (positions) positions->append(i);
      last = i;
      ++qi;
    }
  }
  return qi == lq.size() ? score : 0;
}

class PaletteDelegate : public QStyledItemDelegate {
 public:
  QString query;
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
    const Tokens& t = theme::current();
    auto* a = static_cast<QAction*>(index.data(Qt::UserRole).value<void*>());
    if (!a) return;
    bool sel = opt.state & QStyle::State_Selected;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (sel) p->fillRect(opt.rect, t.sel);
    else if (opt.state & QStyle::State_MouseOver) p->fillRect(opt.rect, t.bg4);
    QColor fg = sel ? t.onsel : (a->isEnabled() ? t.fg : t.fg3);
    QString iconName = a->data().toString();
    if (!iconName.isEmpty()) p->drawPixmap(opt.rect.left() + 8, opt.rect.top() + 6, icons::pixmap(iconName, sel ? t.onsel : t.fg2, 16, p->device()->devicePixelRatioF()));
    QString text = a->text().remove('&');
    QList<int> pos;
    fuzzyScore(text, query, &pos);
    QFont normal = theme::ui(13), bold = theme::ui(13, QFont::DemiBold);
    int x = opt.rect.left() + 32;
    for (int i = 0; i < text.size(); ++i) {
      bool hit = pos.contains(i);
      p->setFont(hit ? bold : normal);
      p->setPen(fg);
      QString ch = text.mid(i, 1);
      p->drawText(QRect(x, opt.rect.top(), 40, opt.rect.height()), Qt::AlignVCenter | Qt::AlignLeft, ch);
      x += QFontMetrics(hit ? bold : normal).horizontalAdvance(ch);
    }
    p->setFont(theme::ui(11));
    p->setPen(sel ? t.onsel : t.fg3);
    p->drawText(QRect(x + 10, opt.rect.top(), 120, opt.rect.height()), Qt::AlignVCenter | Qt::AlignLeft, opGroup(a));
    QString sc = a->shortcut().toString(QKeySequence::NativeText);
    if (!sc.isEmpty()) {
      QFontMetrics mm(theme::mono(11));
      int w = mm.horizontalAdvance(sc) + 10;
      QRect key(opt.rect.right() - w - 8, opt.rect.top() + 6, w, 16);
      p->setPen(QPen(sel ? t.onsel : t.line, 1));
      p->setBrush(sel ? QColor(255, 255, 255, 40) : t.bg4);
      p->drawRoundedRect(key, 3, 3);
      p->setFont(theme::mono(11));
      p->setPen(sel ? t.onsel : t.fg2);
      p->drawText(key, Qt::AlignCenter, sc);
    }
    p->restore();
  }
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(100, 28); }
};
}  // namespace

CommandPalette::CommandPalette(const QList<QAction*>& actions, QWidget* parent) : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint), m_actions(actions) {
  setObjectName("overlay");
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(560);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(1, 1, 1, 1);
  layout->setSpacing(0);
  m_edit = new QLineEdit(this);
  m_edit->setObjectName("paletteInput");
  m_edit->setPlaceholderText(tr("Search commands…"));
  m_edit->addAction(icons::icon("search", theme::current().fg3), QLineEdit::LeadingPosition);
  m_list = new QListWidget(this);
  m_list->setObjectName("paletteList");
  m_list->setItemDelegate(new PaletteDelegate(m_list));
  m_list->setMouseTracking(true);
  m_list->setFixedHeight(28 * 9);
  layout->addWidget(m_edit);
  layout->addWidget(m_list);
  auto* foot = new QLabel(tr("↑↓ navigate · Enter run · Esc close"), this);
  foot->setObjectName("tertiary");
  foot->setContentsMargins(12, 6, 12, 6);
  layout->addWidget(foot);
  connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::refill);
  connect(m_edit, &QLineEdit::returnPressed, this, &CommandPalette::runCurrent);
  connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { runCurrent(); });
  m_edit->installEventFilter(this);
  refill(QString());
  m_edit->setFocus();
}

bool CommandPalette::eventFilter(QObject* o, QEvent* e) {
  if (o == m_edit && e->type() == QEvent::KeyPress) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) {
      int row = m_list->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1);
      if (row >= 0 && row < m_list->count()) m_list->setCurrentRow(row);
      return true;
    }
  }
  return QDialog::eventFilter(o, e);
}

void CommandPalette::refill(const QString& filter) {
  static_cast<PaletteDelegate*>(m_list->itemDelegate())->query = filter;
  m_list->clear();
  QList<QPair<int, QAction*>> scored;
  for (QAction* a : m_actions) {
    if (a->text().isEmpty() || a->isSeparator()) continue;
    int s = fuzzyScore(a->text().remove('&'), filter, nullptr);
    if (s > 0) scored << qMakePair(s, a);
  }
  std::stable_sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  for (const auto& [s, a] : scored) {
    auto* it = new QListWidgetItem(m_list);
    it->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(a)));
    it->setToolTip(a->toolTip());
  }
  if (m_list->count() > 0) m_list->setCurrentRow(0);
}

void CommandPalette::runCurrent() {
  QListWidgetItem* it = m_list->currentItem();
  if (!it) return;
  auto* a = static_cast<QAction*>(it->data(Qt::UserRole).value<void*>());
  accept();
  if (a && a->isEnabled()) a->trigger();
}
