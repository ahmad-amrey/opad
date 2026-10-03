#include "AssemblyWidgets.hpp"

#include <QCheckBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QScreen>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

#include "Icons.hpp"
#include "Theme.hpp"

namespace {
constexpr int kEntryRole = Qt::UserRole + 1;  // the index of the row's entry; -1: the "nothing matches" row
constexpr int kRowHeight = 28;

// At a point of the screen, kept on it; a right-to-left UI opens leftwards from it.
void placeAt(QWidget* w, const QPoint& global) {
  w->adjustSize();
  QRect r(global, w->size());
  if (w->layoutDirection() == Qt::RightToLeft) r.moveRight(global.x());
  if (const QScreen* screen = QGuiApplication::screenAt(global)) {
    const QRect area = screen->availableGeometry();
    r.moveRight(std::min(r.right(), area.right()));
    r.moveBottom(std::min(r.bottom(), area.bottom()));
    r.moveLeft(std::max(r.left(), area.left()));
    r.moveTop(std::max(r.top(), area.top()));
  }
  w->move(r.topLeft());
}

// A row: the component icon indented by depth (flat while filtering), the name, and at the far end the path above it in
// the dimmer text, or "current" where the selection is.
class PickerDelegate : public QStyledItemDelegate {
 public:
  PickerDelegate(const std::vector<ComponentPicker::Entry>* entries, QObject* parent) : QStyledItemDelegate(parent), m_entries(entries) {}
  bool flat = false;
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
    const Tokens& t = theme::current();
    const int i = index.data(kEntryRole).toInt();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const bool rtl = opt.direction == Qt::RightToLeft;
    const QRect r = opt.rect.adjusted(8, 0, -8, 0);
    if (i < 0 || i >= static_cast<int>(m_entries->size())) {  // nothing matches
      p->setFont(theme::ui(12));
      p->setPen(t.fg3);
      p->drawText(r, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), index.data(Qt::DisplayRole).toString());
      p->restore();
      return;
    }
    const ComponentPicker::Entry& e = (*m_entries)[static_cast<size_t>(i)];
    const bool sel = (opt.state & QStyle::State_Selected) && !e.current;
    if (sel) p->fillRect(opt.rect, t.sel);
    else if ((opt.state & QStyle::State_MouseOver) && !e.current) p->fillRect(opt.rect, t.bg4);
    const QColor fg = sel ? t.onsel : e.current ? t.fg3 : t.fg, dim = sel ? t.onsel : t.fg3;
    const int indent = flat ? 0 : 14 * e.depth;
    auto x = [&](int from, int width) { return rtl ? r.right() - from - width + 1 : r.left() + from; };  // a span along the row
    p->drawPixmap(x(indent, 16), r.top() + 6, icons::pixmap(e.id.empty() ? "doc" : "component", sel ? t.onsel : e.current ? t.fg3 : t.fg2, 16, p->device()->devicePixelRatioF()));
    const QString tail = e.current ? ComponentPicker::tr("current") : e.path;
    const QFont nameFont = theme::ui(13), tailFont = theme::ui(11);
    const int start = indent + 24, room = r.width() - start;
    const int nameW = std::min(QFontMetrics(nameFont).horizontalAdvance(e.name) + 4, tail.isEmpty() ? room : room * 2 / 3);  // + rounding
    p->setFont(nameFont);
    p->setPen(fg);
    p->drawText(QRect(x(start, nameW), r.top(), nameW, r.height()), Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft),
                QFontMetrics(nameFont).elidedText(e.name, Qt::ElideRight, nameW));
    const int tailW = room - nameW - 12;
    if (!tail.isEmpty() && tailW > 24) {
      p->setFont(tailFont);
      p->setPen(dim);
      p->drawText(QRect(x(start + nameW + 12, tailW), r.top(), tailW, r.height()), Qt::AlignVCenter | (rtl ? Qt::AlignLeft : Qt::AlignRight),
                  QFontMetrics(tailFont).elidedText(tail, Qt::ElideLeft, tailW));
    }
    p->restore();
  }
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(100, kRowHeight); }
 private:
  const std::vector<ComponentPicker::Entry>* m_entries;
};
}  // namespace

// ---------------------------------------------------------------- ComponentPicker
ComponentPicker::ComponentPicker(QWidget* parent) : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
  setObjectName("overlay");  // the command palette's look (Theme.cpp)
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(420);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(1, 1, 1, 1);
  layout->setSpacing(0);
  m_filter = new QLineEdit(this);
  m_filter->setObjectName("paletteInput");
  m_filter->setPlaceholderText(tr("Move to… (type to filter components)"));
  m_filter->addAction(icons::icon("reparent", theme::current().fg3), QLineEdit::LeadingPosition);
  m_list = new QListWidget(this);
  m_list->setObjectName("paletteList");
  m_list->setItemDelegate(new PickerDelegate(&m_entries, m_list));
  m_list->setMouseTracking(true);
  m_list->setFocusPolicy(Qt::NoFocus);  // the keys stay with the filter
  layout->addWidget(m_filter);
  layout->addWidget(m_list);
  auto* foot = new QLabel(tr("↑↓ choose · Enter move · Esc close"), this);
  foot->setObjectName("tertiary");
  foot->setContentsMargins(12, 6, 12, 6);
  layout->addWidget(foot);
  connect(m_filter, &QLineEdit::textChanged, this, &ComponentPicker::refill);
  connect(m_filter, &QLineEdit::returnPressed, this, &ComponentPicker::choose);
  connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    m_list->setCurrentItem(item);
    choose();
  });
  m_filter->installEventFilter(this);
}

void ComponentPicker::setEntries(std::vector<Entry> entries) {
  m_entries = std::move(entries);
  m_list->setFixedHeight(kRowHeight * static_cast<int>(std::clamp<size_t>(m_entries.size(), 1, 10)) + 4);
  if (m_filter->text().isEmpty()) refill();
  else m_filter->clear();  // refills
}

void ComponentPicker::popup(const QPoint& global) {
  placeAt(this, global);
  show();
  raise();
  m_filter->setFocus();
}

int ComponentPicker::shown() const {
  int n = 0;
  for (int i = 0; i < m_list->count(); ++i) n += m_list->item(i)->data(kEntryRole).toInt() >= 0;
  return n;
}

void ComponentPicker::refill() {
  const QStringList words = m_filter->text().split(' ', Qt::SkipEmptyParts);
  static_cast<PickerDelegate*>(m_list->itemDelegate())->flat = !words.isEmpty();
  m_list->clear();
  QListWidgetItem* first = nullptr;
  for (size_t i = 0; i < m_entries.size(); ++i) {
    const Entry& e = m_entries[i];
    const QString text = e.name + ' ' + e.path;
    if (!std::all_of(words.begin(), words.end(), [&](const QString& w) { return text.contains(w, Qt::CaseInsensitive); })) continue;
    auto* item = new QListWidgetItem(e.name, m_list);
    item->setData(kEntryRole, static_cast<int>(i));
    item->setToolTip(e.path.isEmpty() ? e.name : e.path + QString::fromUtf8(" › ") + e.name);
    if (e.current) item->setFlags(Qt::NoItemFlags);  // shown, never chosen
    else if (!first) first = item;
  }
  if (m_list->count() == 0) {
    auto* none = new QListWidgetItem(tr("No component matches \"%1\"").arg(m_filter->text().trimmed()), m_list);
    none->setData(kEntryRole, -1);
    none->setFlags(Qt::NoItemFlags);
  }
  if (first) m_list->setCurrentItem(first);
}

void ComponentPicker::choose() {
  const QListWidgetItem* item = m_list->currentItem();
  const int i = item && (item->flags() & Qt::ItemIsEnabled) ? item->data(kEntryRole).toInt() : -1;
  if (i < 0 || i >= static_cast<int>(m_entries.size()) || m_entries[static_cast<size_t>(i)].current) return;
  const std::string id = m_entries[static_cast<size_t>(i)].id;
  hide();
  emit chosen(id);
}

bool ComponentPicker::eventFilter(QObject* object, QEvent* event) {
  if (object == m_filter && event->type() == QEvent::KeyPress) {
    const int key = static_cast<QKeyEvent*>(event)->key();
    if (key == Qt::Key_Escape) {
      hide();
      return true;
    }
    const int step = key == Qt::Key_Down ? 1 : key == Qt::Key_Up ? -1 : key == Qt::Key_PageDown ? 8 : key == Qt::Key_PageUp ? -8 : 0;
    if (step) {  // to the next row that can be chosen that way, as far as there is one
      int row = m_list->currentRow(), to = row;
      for (int i = row + (step > 0 ? 1 : -1), left = std::abs(step); i >= 0 && i < m_list->count() && left > 0; i += step > 0 ? 1 : -1)
        if (m_list->item(i)->flags() & Qt::ItemIsEnabled) to = i, --left;
      if (to != row) m_list->setCurrentRow(to);
      return true;
    }
  }
  return QFrame::eventFilter(object, event);
}

// ---------------------------------------------------------------- OpacitySlider
OpacitySlider::OpacitySlider(double opacity, QWidget* parent) : QWidget(parent) {
  setObjectName("opacitySlider");
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(12, 4, 12, 4);
  row->setSpacing(10);
  auto* label = new QLabel(tr("Opacity"), this);
  m_slider = new QSlider(Qt::Horizontal, this);
  m_slider->setRange(10, 100);
  m_slider->setSingleStep(5);
  m_slider->setPageStep(10);
  m_slider->setValue(std::clamp(qRound(opacity * 100), 10, 100));
  m_slider->setMinimumWidth(150);
  m_value = new QLabel(this);
  m_value->setFont(theme::mono(11));
  m_value->setMinimumWidth(QFontMetrics(m_value->font()).horizontalAdvance(tr("%1 %").arg(100)) + 4);
  m_value->setAlignment(Qt::AlignVCenter | Qt::AlignTrailing);
  row->addWidget(label);
  row->addWidget(m_slider, 1);
  row->addWidget(m_value);
  m_written = m_slider->value();
  m_rest.setSingleShot(true);
  m_rest.setInterval(400);
  auto show = [this] { m_value->setText(tr("%1 %").arg(m_slider->value())); };
  show();
  connect(m_slider, &QSlider::valueChanged, this, [this, show] {
    show();
    emit previewed(value());
    if (!m_slider->isSliderDown()) m_rest.start();  // a key or the wheel: written once it rests
  });
  connect(m_slider, &QSlider::sliderReleased, this, [this] {
    m_rest.stop();
    write();
  });
  connect(&m_rest, &QTimer::timeout, this, &OpacitySlider::write);
}

double OpacitySlider::value() const { return m_slider->value() / 100.0; }

void OpacitySlider::write() {
  if (m_done || m_slider->value() == m_written) return;
  m_written = m_slider->value();
  emit committed(value());
}

void OpacitySlider::finish(bool keep) {
  if (m_done) return;
  m_rest.stop();
  if (keep) write();
  m_done = true;
  emit finished();
}

// ---------------------------------------------------------------- OpacityPopup
OpacityPopup::OpacityPopup(double opacity, QWidget* parent) : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
  setObjectName("overlay");
  setAttribute(Qt::WA_StyledBackground);
  setAttribute(Qt::WA_DeleteOnClose);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(1, 6, 1, 6);
  m_slider = new OpacitySlider(opacity, this);
  layout->addWidget(m_slider);
  setFixedWidth(320);
}

void OpacityPopup::popup(const QPoint& global) {
  placeAt(this, global);
  show();
  m_slider->slider()->setFocus();
}

void OpacityPopup::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) m_slider->finish(false);
  if (event->key() != Qt::Key_Return && event->key() != Qt::Key_Enter && event->key() != Qt::Key_Escape) return QFrame::keyPressEvent(event);
  close();
}

void OpacityPopup::hideEvent(QHideEvent* event) {
  m_slider->finish(true);
  QFrame::hideEvent(event);
}

// ---------------------------------------------------------------- ActivateToggle
ActivateToggle::ActivateToggle(QWidget* parent) : QFrame(parent) {
  setObjectName("activateToggle");
  setLayoutDirection(QGuiApplication::layoutDirection());  // the browser's rows are left to right; this reads as the UI does
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(8, 4, 10, 4);
  m_box = new QCheckBox(tr("Activate"), this);
  m_box->setFocusPolicy(Qt::NoFocus);  // the name being typed keeps the keys
  m_box->setToolTip(tr("Make it the active component: what is made next goes into it. The next new component starts the same way."));
  row->addWidget(m_box);
  connect(m_box, &QCheckBox::toggled, this, &ActivateToggle::toggled);
  hide();
}

void ActivateToggle::showFor(QWidget* editor, bool on) {
  if (m_editor) m_editor->removeEventFilter(this);
  m_editor = editor;
  if (!editor) return hide();
  {
    const QSignalBlocker quiet(m_box);
    m_box->setChecked(on);
  }
  editor->installEventFilter(this);
  connect(editor, &QObject::destroyed, this, [this](QObject* gone) {
    if (!m_editor || m_editor.data() == gone) hide();
  });
  place();
  show();
  raise();
}

void ActivateToggle::place() {
  if (!m_editor || !parentWidget()) return;
  adjustSize();
  const QRect e = m_editor->geometry();  // in the rows' coordinates, as this
  const int room = parentWidget()->height();
  const int y = e.bottom() + 3 + height() <= room ? e.bottom() + 3 : e.top() - 3 - height();
  move(std::clamp(e.right() + 1 - width(), 0, std::max(0, parentWidget()->width() - width())), y);
}

void ActivateToggle::paintEvent(QPaintEvent*) {  // over the rows: an overlay's look, painted (a style sheet's background is not, here)
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(t.line);
  p.setBrush(t.bg3);
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
}

bool ActivateToggle::eventFilter(QObject* object, QEvent* event) {
  if (object == m_editor) {
    if (event->type() == QEvent::Move || event->type() == QEvent::Resize) place();
    else if (event->type() == QEvent::HideToParent) hide();  // closed (hidden, then deleted later), or its row out of view
    else if (event->type() == QEvent::ShowToParent) place(), show();
  }
  return QFrame::eventFilter(object, event);
}
