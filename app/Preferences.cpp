#include "Preferences.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTextDocumentFragment>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "Icons.hpp"
#include "Theme.hpp"

namespace preferences {
namespace {
QList<Page>& registry() {
  static QList<Page> list;
  return list;
}

// What the search reads on a widget: its text, title, choices and tooltip.
QStringList texts(const QWidget* w) {
  QStringList out;
  if (auto* l = qobject_cast<const QLabel*>(w)) out << QTextDocumentFragment::fromHtml(l->text()).toPlainText();
  if (auto* b = qobject_cast<const QAbstractButton*>(w)) out << QString(b->text()).remove('&');
  if (auto* g = qobject_cast<const QGroupBox*>(w)) out << g->title();
  if (auto* c = qobject_cast<const QComboBox*>(w))
    for (int i = 0; i < c->count(); ++i) out << c->itemText(i);
  if (!w->toolTip().isEmpty()) out << w->toolTip();
  out.removeAll(QString());
  return out;
}

void setMarked(QWidget* w, bool on) {
  if (w->property("prefMatch").toBool() == on) return;
  w->setProperty("prefMatch", on);
  w->style()->unpolish(w);
  w->style()->polish(w);
}

QWidget* g_from = nullptr;  // the control whose change is being said: it does not read itself back (typing "2." stays)
void say(QWidget* from, const QString& key) {
  QWidget* before = std::exchange(g_from, from);
  changed(key);
  g_from = before;
}

// The control reads `key` again (signals blocked) whenever another face changes it, or every setting is said to have.
void follow(QWidget* w, const QString& key, std::function<void()> read) {
  read();
  QObject::connect(notifier(), &Notifier::changed, w, [w, key, read](const QString& changed) {
    if (w == g_from || (!changed.isEmpty() && changed != key)) return;
    const QSignalBlocker block(w);
    read();
  });
}
}  // namespace

Notifier* notifier() {
  static Notifier* n = new Notifier;
  return n;
}

void changed(const QString& key) { emit notifier()->changed(key); }

void bind(QCheckBox* box, const QString& key, bool fallback) {
  follow(box, key, [box, key, fallback] { box->setChecked(QSettings().value(key, fallback).toBool()); });
  QObject::connect(box, &QCheckBox::toggled, box, [box, key](bool on) { QSettings().setValue(key, on); say(box, key); });
}

void bind(QSpinBox* box, const QString& key, int fallback) {
  follow(box, key, [box, key, fallback] { box->setValue(QSettings().value(key, fallback).toInt()); });
  QObject::connect(box, &QSpinBox::valueChanged, box, [box, key](int v) { QSettings().setValue(key, v); say(box, key); });
}

void bind(QDoubleSpinBox* box, const QString& key, double fallback) {
  follow(box, key, [box, key, fallback] { box->setValue(QSettings().value(key, fallback).toDouble()); });
  QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [box, key](double v) { QSettings().setValue(key, v); say(box, key); });
}

void bind(QComboBox* box, const QString& key, int fallback) {
  follow(box, key, [box, key, fallback] { box->setCurrentIndex(std::clamp(QSettings().value(key, fallback).toInt(), 0, std::max(0, static_cast<int>(box->count()) - 1))); });
  QObject::connect(box, &QComboBox::currentIndexChanged, box, [box, key](int i) { QSettings().setValue(key, i); say(box, key); });
}

void bind(QLineEdit* edit, const QString& key, const QString& fallback) {
  follow(edit, key, [edit, key, fallback] { if (!edit->hasFocus()) edit->setText(QSettings().value(key, fallback).toString()); });
  QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, key] { QSettings().setValue(key, edit->text().trimmed()); say(edit, key); });
}

void addPage(const Page& page) {
  QList<Page>& list = registry();
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Page& p) { return p.id == page.id; }), list.end());
  list << page;
}

QList<Page> pages() {
  QList<Page> out = registry();
  std::stable_sort(out.begin(), out.end(), [](const Page& a, const Page& b) { return a.order < b.order; });
  return out;
}

bool matches(const QStringList& texts, const QString& query) {
  const QString all = texts.join('\n');
  const QStringList words = query.simplified().split(' ', Qt::SkipEmptyParts);
  return std::all_of(words.begin(), words.end(), [&all](const QString& word) { return all.contains(word, Qt::CaseInsensitive); });
}

// ---------------------------------------------------------------- Form
Form::Form(QWidget* page) : m_page(page), m_layout(new QVBoxLayout(page)) {
  m_layout->setContentsMargins(16, 12, 16, 12);
  m_layout->setSpacing(6);
}

QFormLayout* Form::form() {
  if (!m_form) {
    m_form = new QFormLayout;
    m_form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    m_form->setLabelAlignment(Qt::AlignLeading | Qt::AlignVCenter);
    m_form->setContentsMargins(0, 0, 0, 6);
    m_layout->addLayout(m_form);
  }
  return m_form;
}

void Form::section(const QString& title, const QString& note) {
  m_form = nullptr;
  auto* heading = new QLabel(title, m_page);
  heading->setObjectName("prefSection");
  QFont f = heading->font();
  f.setBold(true);
  heading->setFont(f);
  if (m_layout->count()) m_layout->addSpacing(8);
  m_layout->addWidget(heading);
  if (!note.isEmpty()) this->note(note);
}

QLabel* Form::note(const QString& text) {
  auto* label = new QLabel(text, m_page);
  label->setObjectName("tertiary");
  label->setWordWrap(true);
  form()->addRow(label);
  return label;
}

QCheckBox* Form::check(const QString& key, const QString& label, bool fallback, std::function<void(bool)> apply) {
  auto* box = new QCheckBox(label, m_page);
  box->setObjectName(key);
  bind(box, key, fallback);
  if (apply) QObject::connect(box, &QCheckBox::toggled, box, apply);
  form()->addRow(box);
  return box;
}

QCheckBox* Form::option(QAction* command, const QString& label) {
  auto* box = new QCheckBox(label, m_page);
  box->setObjectName(command->objectName());
  box->setChecked(command->isChecked());
  QObject::connect(box, &QCheckBox::toggled, command, [command](bool on) { if (command->isChecked() != on) command->setChecked(on); });
  QObject::connect(command, &QAction::toggled, box, [box](bool on) { QSignalBlocker block(box); box->setChecked(on); });
  form()->addRow(box);
  return box;
}

QSpinBox* Form::integer(const QString& key, const QString& label, int fallback, int min, int max, const QString& suffix, std::function<void(int)> apply) {
  auto* box = new QSpinBox(m_page);
  box->setObjectName(key);
  box->setRange(min, max);
  box->setSuffix(suffix);
  box->setKeyboardTracking(false);  // typed values apply on Enter or leaving the box: "200" never passes through 2 (undo steps drop for good)
  bind(box, key, fallback);
  if (apply) QObject::connect(box, &QSpinBox::valueChanged, box, apply);
  form()->addRow(label, box);
  return box;
}

QDoubleSpinBox* Form::number(const QString& key, const QString& label, double fallback, double min, double max, int decimals, const QString& suffix,
                             std::function<void(double)> apply) {
  auto* box = new QDoubleSpinBox(m_page);
  box->setObjectName(key);
  box->setRange(min, max);
  box->setDecimals(decimals);
  box->setSuffix(suffix);
  box->setKeyboardTracking(false);
  bind(box, key, fallback);
  if (apply) QObject::connect(box, &QDoubleSpinBox::valueChanged, box, apply);
  form()->addRow(label, box);
  return box;
}

QComboBox* Form::choice(const QString& key, const QString& label, const QStringList& items, int fallback, std::function<void(int)> apply) {
  auto* box = new QComboBox(m_page);
  box->setObjectName(key);
  box->addItems(items);
  bind(box, key, fallback);
  if (apply) QObject::connect(box, &QComboBox::currentIndexChanged, box, apply);
  form()->addRow(label, box);
  return box;
}

QLineEdit* Form::text(const QString& key, const QString& label, const QString& fallback, std::function<void(const QString&)> apply) {
  auto* edit = new QLineEdit(m_page);
  edit->setObjectName(key);
  edit->setMinimumWidth(220);
  bind(edit, key, fallback);
  if (apply) QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, apply] { apply(edit->text().trimmed()); });
  form()->addRow(label, edit);
  return edit;
}

QPushButton* Form::button(const QString& label, std::function<void()> fn, const QString& name) {
  auto* b = new QPushButton(label, m_page);
  b->setObjectName(name);
  b->setAutoDefault(false);
  QObject::connect(b, &QPushButton::clicked, b, fn);
  auto* row = new QHBoxLayout;
  row->addWidget(b);
  row->addStretch();
  form()->addRow(row);
  return b;
}

void Form::row(const QString& label, QWidget* field) {
  field->setParent(m_page);
  form()->addRow(label, field);
}

void Form::finish() { m_layout->addStretch(1); }
}  // namespace preferences

// ---------------------------------------------------------------- PreferencesDialog
PreferencesDialog::PreferencesDialog(QWidget* window) : QDialog(window) {
  setObjectName("preferences");
  setWindowTitle(tr("Preferences"));
  resize(820, 600);
  const Tokens& t = theme::current();
  const QColor tint = t.sel;
  setStyleSheet(QString("*[prefMatch=\"true\"] { background: rgba(%1,%2,%3,70); border-radius: 3px; }").arg(tint.red()).arg(tint.green()).arg(tint.blue()));
  auto* layout = new QVBoxLayout(this);
  m_search = new QLineEdit(this);
  m_search->setObjectName("preferencesSearch");
  m_search->setPlaceholderText(tr("Search settings"));
  m_search->setClearButtonEnabled(true);
  layout->addWidget(m_search);
  auto* body = new QHBoxLayout;
  layout->addLayout(body, 1);
  m_list = new QListWidget(this);
  m_list->setObjectName("preferencesPages");
  m_list->setIconSize({18, 18});
  m_list->setFixedWidth(210);
  body->addWidget(m_list);
  m_stack = new QStackedWidget(this);
  body->addWidget(m_stack, 1);
  m_none = new QLabel(tr("No setting matches the search."), this);
  m_none->setObjectName("tertiary");
  m_none->setAlignment(Qt::AlignCenter);
  m_none->hide();
  body->addWidget(m_none, 1);
  for (const preferences::Page& p : preferences::pages()) {
    QWidget* page = p.build ? p.build() : new QWidget;
    page->setObjectName("page." + p.id);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // notes wrap to the width instead
    scroll->setWidget(page);
    m_stack->addWidget(scroll);
    auto* item = new QListWidgetItem(icons::themed(p.icon, 18), p.title, m_list);
    item->setData(Qt::UserRole, p.id);
    m_ids << p.id;
    QStringList all{p.title};
    all << p.keywords;
    for (QWidget* w : page->findChildren<QWidget*>()) all << preferences::texts(w);
    m_texts << all;
  }
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto* applies = new QLabel(tr("Changes apply at once."), this);
  applies->setObjectName("tertiary");
  auto* footer = new QHBoxLayout;
  footer->addWidget(applies, 1);
  footer->addWidget(buttons);
  layout->addLayout(footer);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0) m_stack->setCurrentIndex(row);
    mark();
  });
  connect(m_search, &QLineEdit::textChanged, this, &PreferencesDialog::filter);
  m_list->setCurrentRow(0);
}


PreferencesDialog* PreferencesDialog::open(QWidget* window, const QString& page, const QString& focus) {
  auto* dialog = window->findChild<PreferencesDialog*>("preferences", Qt::FindDirectChildrenOnly);
  if (!dialog) dialog = new PreferencesDialog(window);
  if (!focus.isEmpty()) dialog->m_search->clear();  // the control asked for must be on a page that shows
  if (!page.isEmpty()) dialog->setPage(page);
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
  if (QWidget* page = dialog->m_stack->currentWidget(); page && !focus.isEmpty())
    if (auto* control = page->findChild<QWidget*>(focus)) {
      control->setFocus(Qt::OtherFocusReason);
      preferences::setMarked(control, true);
      if (auto* scroll = qobject_cast<QScrollArea*>(page)) scroll->ensureWidgetVisible(control);
    }
  return dialog;
}

void PreferencesDialog::setPage(const QString& id) {
  const int row = m_ids.indexOf(id);
  if (row < 0) return;
  if (m_list->item(row)->isHidden()) m_search->clear();
  m_list->setCurrentRow(row);
}

bool PreferencesDialog::event(QEvent* e) {
  if (e->type() == QEvent::WindowActivate) preferences::changed({});  // a setting written elsewhere while it was behind
  return QDialog::event(e);
}

QString PreferencesDialog::page() const { return m_ids.value(m_list->currentRow()); }
QStringList PreferencesDialog::pageIds() const { return m_ids; }

QStringList PreferencesDialog::visiblePages() const {
  QStringList out;
  for (int i = 0; i < m_list->count(); ++i)
    if (!m_list->item(i)->isHidden()) out << m_ids[i];
  return out;
}

QWidget* PreferencesDialog::pageWidget(const QString& id) const {
  const int row = m_ids.indexOf(id);
  auto* scroll = row < 0 ? nullptr : qobject_cast<QScrollArea*>(m_stack->widget(row));
  return scroll ? scroll->widget() : nullptr;
}

void PreferencesDialog::setSearch(const QString& text) { m_search->setText(text); }

QStringList PreferencesDialog::marked() const {
  QStringList out;
  if (QWidget* page = pageWidget(this->page()))
    for (QWidget* w : page->findChildren<QWidget*>())
      if (w->property("prefMatch").toBool()) out << preferences::texts(w);
  return out;
}

// The pages with a match stay in the list; the one shown moves to the first of them when it has none.
void PreferencesDialog::filter() {
  const QString query = m_search->text();
  int first = -1;
  for (int i = 0; i < m_list->count(); ++i) {
    const bool shown = preferences::matches(m_texts[i], query);
    m_list->item(i)->setHidden(!shown);
    if (shown && first < 0) first = i;
  }
  m_none->setVisible(first < 0);
  m_stack->setVisible(first >= 0);
  if (first >= 0 && m_list->item(m_list->currentRow())->isHidden()) m_list->setCurrentRow(first);
  mark();
}

// On the page shown: every row whose own text has all the words, and the field of a label that has them.
void PreferencesDialog::mark() {
  const QString query = m_search->text().simplified();
  for (int i = 0; i < m_stack->count(); ++i) {
    QSet<QWidget*> on;
    const QList<QWidget*> all = pageWidget(m_ids[i])->findChildren<QWidget*>();
    if (i == m_list->currentRow() && !query.isEmpty())
      for (QWidget* w : all)
        if (const QStringList own = preferences::texts(w); !own.isEmpty() && preferences::matches(own, query)) {
          on << w;
          if (auto* label = qobject_cast<QLabel*>(w); label && label->buddy()) on << label->buddy();
        }
    for (QWidget* w : all) preferences::setMarked(w, on.contains(w));
  }
}
