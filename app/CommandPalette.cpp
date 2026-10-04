#include "CommandPalette.hpp"

#include <QAction>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QSettings>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>

#include "CommandHelp.hpp"
#include "HelpReference.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "Theme.hpp"

QString opGroup(const QAction* a) {
  if (const QString group = a->property("commandGroup").toString(); !group.isEmpty()) return group;  // its record's (Commands.hpp)
  return help::group(a->objectName());  // the help's area (UI-107), translated
}

namespace palette {
QStringList remember(QStringList recent, const QString& id, int keep) {
  if (id.isEmpty()) return recent;
  recent.removeAll(id);
  recent.prepend(id);
  while (recent.size() > keep) recent.removeLast();
  return recent;
}
QStringList recent() { return QSettings().value("palette/recent").toStringList(); }
void noteRun(const QString& id) {
  if (id == "tools.commands" || id == "edit.undo" || id == "edit.redo") return;  // the palette itself; undo is one key away
  QSettings().setValue("palette/recent", remember(recent(), id));
}
}  // namespace palette

// ---------------------------------------------------------------- CommandPalette
namespace {
constexpr int kRecentRole = Qt::UserRole + 1;

// 0 when the query's letters are not all in the text in order. The query as typed from the start of a word comes above any
// scattered match (an Arabic label that keeps a Latin name, "ODA File Converter", must not beat Fit's keyword "fit"
// with f-i from File and the t of Converter); among those the whole text ("Fit" for fit) comes first, then a whole word
// ("Fit sheet"), then the start of a word ("Fitting"); then runs of letters and word starts score more.
constexpr int kWordHit = 1000, kWholeWord = 100, kWholeText = 200;
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
  if (qi != lq.size()) return 0;
  int best = 0;  // the best place the query starts a word at
  for (qsizetype at = lt.indexOf(lq); at >= 0; at = lt.indexOf(lq, at + 1)) {
    if (at > 0 && lt[at - 1] != ' ') continue;
    const qsizetype end = at + lq.size();
    best = std::max(best, lq.size() == lt.size() ? kWordHit + kWholeText : end == lt.size() || !lt[end].isLetterOrNumber() ? kWordHit + kWholeWord : kWordHit);
  }
  return score + best;
}

// Laid out left to right and mirrored for right-to-left languages: icon, name, summary (or what a command not available
// now needs), group, key.
class PaletteDelegate : public QStyledItemDelegate {
 public:
  QString query;
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
    const Tokens& t = theme::current();
    auto* a = static_cast<QAction*>(index.data(Qt::UserRole).value<void*>());
    if (!a) return;
    const bool sel = opt.state & QStyle::State_Selected;
    const QRect row = opt.rect;
    auto at = [&](const QRect& r) { return QStyle::visualRect(opt.direction, row, r); };
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (sel) p->fillRect(row, t.sel);
    else if (opt.state & QStyle::State_MouseOver) p->fillRect(row, t.bg4);
    QColor fg = sel ? t.onsel : (a->isEnabled() ? t.fg : t.fg3);
    int left = row.left() + 8, right = row.right() - 8;
    const QString iconName = a->data().toString();
    if (!iconName.isEmpty()) p->drawPixmap(at(QRect(left, row.top() + 6, 16, 16)), icons::pixmap(iconName, sel ? t.onsel : t.fg2, 16, p->device()->devicePixelRatioF()));
    left += 24;
    const QString sc = keys::text(keys::binding(a));  // the user's key, also while a sketch holds it
    if (!sc.isEmpty()) {
      const QFontMetrics mm(theme::mono(11));
      const int w = mm.horizontalAdvance(sc) + 10;
      const QRect key = at(QRect(right - w, row.top() + 6, w, 16));
      p->setPen(QPen(sel ? t.onsel : t.line, 1));
      p->setBrush(sel ? QColor(255, 255, 255, 40) : t.bg4);
      p->drawRoundedRect(key, 3, 3);
      p->setFont(theme::mono(11));
      p->setPen(sel ? t.onsel : t.fg2);
      p->drawText(key, Qt::AlignCenter, sc);
      right -= w + 10;
    }
    const bool recent = index.data(kRecentRole).toBool();
    const QString group = recent ? CommandPalette::tr("Recent") : opGroup(a);
    const QFont small = theme::ui(11);
    const int groupWidth = std::min(QFontMetrics(small).horizontalAdvance(group) + 4, 120);  // + rounding: never elided when it fits
    p->setFont(small);
    p->setPen(sel ? t.onsel : recent ? t.sel : t.fg3);
    const int align = Qt::AlignVCenter | (opt.direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft);
    p->drawText(at(QRect(right - groupWidth, row.top(), groupWidth, row.height())), align, QFontMetrics(small).elidedText(group, Qt::ElideRight, groupWidth));
    right -= groupWidth + 12;
    // The name: the letters the query found in bold (right-to-left text whole, its letters join).
    const QString text = a->text().remove('&');
    const QFont normal = theme::ui(13), bold = theme::ui(13, QFont::DemiBold);
    const int nameWidth = std::min(QFontMetrics(query.isEmpty() ? normal : bold).horizontalAdvance(text) + 4, right - left);
    const QRect name = at(QRect(left, row.top(), nameWidth, row.height()));
    p->setPen(fg);
    if (query.isEmpty() || text.isRightToLeft()) {
      p->setFont(normal);
      p->drawText(name, align, QFontMetrics(normal).elidedText(text, Qt::ElideRight, nameWidth));
    } else {
      QList<int> pos;
      fuzzyScore(text, query, &pos);
      int x = name.left();
      for (int i = 0; i < text.size() && x < name.right(); ++i) {
        const QFont& f = pos.contains(i) ? bold : normal;
        p->setFont(f);
        const QString ch = text.mid(i, 1);
        p->drawText(QRect(x, row.top(), 40, row.height()), Qt::AlignVCenter | Qt::AlignLeft, ch);
        x += QFontMetrics(f).horizontalAdvance(ch);
      }
    }
    left += nameWidth + 12;
    // What it does; for a command not available now, what it needs.
    const CommandHelp* h = help::find(a->objectName());
    const bool needs = !a->isEnabled() && h && !h->requirement.isEmpty();
    const QString note = needs ? help::requirement(*h) : h ? help::expand(h->summary) : QString();
    if (right - left > 40 && !note.isEmpty()) {
      p->setFont(small);
      p->setPen(sel ? QColor(t.onsel.red(), t.onsel.green(), t.onsel.blue(), 190) : needs ? t.amber : t.fg3);
      p->drawText(at(QRect(left, row.top(), right - left, row.height())), align, QFontMetrics(small).elidedText(note, Qt::ElideRight, right - left));
    }
    p->restore();
  }
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(100, 28); }
};
}  // namespace

CommandPalette::CommandPalette(const QList<QAction*>& actions, QWidget* parent) : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint), m_actions(actions) {
  setObjectName("overlay");
  setAttribute(Qt::WA_StyledBackground);
  // The list, and beside it the current command's card (UI-107): summary, keys, its clip, what it needs.
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(1, 1, 1, 1);
  row->setSpacing(0);
  auto* column = new QWidget(this);
  column->setFixedWidth(620);
  row->addWidget(column);
  auto* rule = new QFrame(this);
  rule->setFixedWidth(1);
  rule->setStyleSheet(QString("background: %1;").arg(theme::css(theme::current().line)));
  row->addWidget(rule);
  m_preview = new CommandPreview(CommandPreview::Size::Compact, this);
  m_preview->setFixedWidth(312);
  row->addWidget(m_preview);
  auto* layout = new QVBoxLayout(column);
  layout->setContentsMargins(0, 0, 0, 0);
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
  m_foot = new QLabel(this);
  m_foot->setObjectName("paletteFoot");
  m_foot->setContentsMargins(12, 6, 12, 6);
  layout->addWidget(m_foot);
  setFoot(QString(), false);
  connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::refill);
  connect(m_edit, &QLineEdit::returnPressed, this, &CommandPalette::runCurrent);
  connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { runCurrent(); });
  connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
    auto* a = it ? static_cast<QAction*>(it->data(Qt::UserRole).value<void*>()) : nullptr;
    m_preview->setCommand(a ? a->objectName() : QString(), a);
    setFoot(QString(), false);
  });
  m_edit->installEventFilter(this);
  refill(QString());
  m_edit->setFocus();
}

void CommandPalette::setFoot(const QString& text, bool warning) {
  m_foot->setText(text.isEmpty() ? tr("↑↓ navigate · Enter run · Esc close") : text);
  m_foot->setStyleSheet(QString("color: %1;").arg(theme::css(warning ? theme::current().amber : theme::current().fg3)));
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
  const QString query = filter.trimmed();
  static_cast<PaletteDelegate*>(m_list->itemDelegate())->query = query;
  m_list->clear();
  const QStringList recent = palette::recent();
  auto rank = [&recent](const QAction* a) { const qsizetype i = recent.indexOf(a->objectName()); return i < 0 ? recent.size() : i; };
  QList<QPair<int, QAction*>> scored;
  // A key typed as shown or as Qt writes it ("ctrl+alt+f"): the command that has it now comes first; with a '+' the
  // commands whose key starts so follow ("ctrl+alt" lists them). One plain character is how a name search starts: the
  // command bound to it ranks with the names that word starts, after them ('c': Combine, Copy, then Circle's key C).
  auto squeeze = [](QString s) { return s.remove(' ').toLower(); };
  const QString typedKey = squeeze(query);
  const int keyHit = typedKey.size() == 1 ? kWordHit : 3 * kWordHit;
  for (QAction* a : m_actions) {
    if (a->text().isEmpty() || a->isSeparator()) continue;
    int s = fuzzyScore(a->text().remove('&'), query, nullptr);
    for (const QKeySequence& key : keys::bindings(a)) {
      if (typedKey.isEmpty()) break;
      for (const QString& written : {squeeze(keys::plain(key)), squeeze(key.toString(QKeySequence::PortableText))}) {
        if (written == typedKey) s = std::max(s, keyHit);
        else if (typedKey.contains('+') && written.startsWith(typedKey)) s = std::max(s, 2 * kWordHit);
      }
    }
    for (const QString& keyword : a->property("commandKeywords").toStringList()) s = std::max(s, (fuzzyScore(keyword, query, nullptr) + 1) / 2);  // below a label match of its kind (a word hit above a scattered one)
    if (const CommandHelp* h = help::find(a->objectName())) {  // its help's keywords (the English name too), then its summary
      for (const QString& keyword : h->keywords) s = std::max(s, (fuzzyScore(keyword, query, nullptr) + 1) / 2);
      if (!s && help::matches(*h, query)) s = 1;
    }
    if (s > 0) scored << qMakePair(s, a);
  }
  // Best match first; among equals (all of them with nothing typed) the recent commands, newest first.
  std::stable_sort(scored.begin(), scored.end(), [&rank](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : rank(x.second) < rank(y.second); });
  for (const auto& [s, a] : scored) {
    auto* it = new QListWidgetItem(m_list);
    it->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(a)));
    it->setData(kRecentRole, rank(a) < recent.size());
    it->setToolTip(a->toolTip());
  }
  if (m_list->count() > 0) m_list->setCurrentRow(0);
}

void CommandPalette::runCurrent() {
  QListWidgetItem* it = m_list->currentItem();
  auto* a = it ? static_cast<QAction*>(it->data(Qt::UserRole).value<void*>()) : nullptr;
  if (!a) return;
  if (!a->isEnabled()) {  // stays open and says why
    const CommandHelp* h = help::find(a->objectName());
    setFoot(QString::fromUtf8("⚠  ") + (h && !h->requirement.isEmpty() ? help::requirement(*h) : tr("%1 is not available right now.").arg(a->text().remove('&'))), true);
    return;
  }
  palette::noteRun(a->objectName());
  accept();
  a->trigger();
}
