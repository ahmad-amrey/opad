#include "HelpWindows.hpp"

#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace help {
QStringList keyCaps(const QString& keys) {
  QStringList out;
  QString text = keys;
  while (!text.isEmpty()) {
    const qsizetype plus = text.indexOf('+', 1);  // a '+' key stays one cap
    if (plus < 0) { out << text; break; }
    out << text.left(plus);
    text = text.mid(plus + 1);
  }
  return out;
}

QList<KeyRow> mouseRows(const QString& preset) {
  const QString middle = QCoreApplication::translate("help", "Middle drag"), right = QCoreApplication::translate("help", "Right drag");
  QString orbit = "Shift+" + middle, pan = middle;  // Viewport::setNavPreset
  if (preset == "solidworks") orbit = middle, pan = "Ctrl+" + middle;
  else if (preset == "onshape") orbit = right, pan = middle;
  else if (preset == "blender") orbit = middle, pan = "Shift+" + middle;
  return {{QCoreApplication::translate("help", "Orbit"), orbit},
          {QCoreApplication::translate("help", "Pan"), pan},
          {QCoreApplication::translate("help", "Zoom at the cursor"), QCoreApplication::translate("help", "Wheel")},
          {QCoreApplication::translate("help", "Select"), QCoreApplication::translate("help", "Click")},
          {QCoreApplication::translate("help", "Select in a window"), QCoreApplication::translate("help", "Left drag")}};
}

QList<KeyGroup> keyGroups(const QList<QAction*>& actions, bool sketching, const QString& preset) {
  QList<KeyGroup> groups;
  for (QAction* a : actions) {
    if (!a || a->shortcut().isEmpty() || a->objectName().isEmpty()) continue;
    QString group = a->property("commandGroup").toString();
    if (group.isEmpty()) group = help::group(a->objectName());
    const CommandHelp* h = help::find(a->objectName());
    QString label = h && !h->title.isEmpty() ? h->title : a->text();
    label.remove('&').remove(QString::fromUtf8("…"));
    auto it = std::find_if(groups.begin(), groups.end(), [&](const KeyGroup& g) { return g.title == group; });
    if (it == groups.end()) it = groups.insert(groups.end(), KeyGroup{group, {}});
    it->rows << KeyRow{label, a->shortcut().toString(QKeySequence::NativeText)};
  }
  if (sketching)  // the sketch's keys first
    std::stable_partition(groups.begin(), groups.end(), [&](const KeyGroup& g) {
      return std::any_of(actions.begin(), actions.end(), [&](QAction* a) { return a && a->objectName().startsWith("sketch.") && a->property("commandGroup").toString() == g.title; });
    });
  groups << KeyGroup{QCoreApplication::translate("help", "Mouse"), mouseRows(preset)};
  groups << KeyGroup{QCoreApplication::translate("help", "In every tool"),
                     {{QCoreApplication::translate("help", "Step back, or leave the tool"), "Esc"},
                      {QCoreApplication::translate("help", "OK, or finish"), "Enter"},
                      {QCoreApplication::translate("help", "The full card of the button under the pointer"), "Shift"},
                      {QCoreApplication::translate("help", "Guide of the tool you are using"), "F1"}}};
  return groups;
}

QString problemReport(const QString& description, const QStringList& facts) {
  QString text = "What happened:\n" + (description.trimmed().isEmpty() ? QString("(not described)") : description.trimmed()) + "\n\nOPAD and this computer:\n";
  for (const QString& fact : facts) text += "- " + fact + "\n";
  return text;
}
}  // namespace help

// ---------------------------------------------------------------- ShortcutSheet
ShortcutSheet::ShortcutSheet(QWidget* parent) : QWidget(parent, Qt::Window) {
  setObjectName("shortcutSheet");
  setWindowTitle(tr("Shortcuts cheat sheet"));
  setAttribute(Qt::WA_StyledBackground);
  resize(1000, 680);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 12, 16, 12);
  v->setSpacing(10);
  auto* top = new QHBoxLayout();
  m_search = new QLineEdit(this);
  m_search->setObjectName("paletteInput");
  m_search->setPlaceholderText(tr("Search by name or key…"));
  m_search->setClearButtonEnabled(true);
  m_search->addAction(icons::icon("search", theme::current().fg3), QLineEdit::LeadingPosition);
  auto* edit = new QPushButton(tr("Change shortcuts…"), this);
  top->addWidget(m_search, 1);
  top->addWidget(edit);
  v->addLayout(top);
  auto* scroll = new QScrollArea(this);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  m_page = new QWidget(scroll);
  scroll->setWidget(m_page);
  v->addWidget(scroll, 1);
  connect(m_search, &QLineEdit::textChanged, this, &ShortcutSheet::setFilter);
  connect(edit, &QPushButton::clicked, this, &ShortcutSheet::editRequested);
  auto* again = new QShortcut(QKeySequence("Ctrl+/"), this);  // the key that opened it closes it, as Esc does
  connect(again, &QShortcut::activated, this, &QWidget::close);
}

void ShortcutSheet::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

void ShortcutSheet::setGroups(const QList<help::KeyGroup>& groups) {
  delete m_page->layout();
  qDeleteAll(m_page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
  m_groups.clear();
  m_titles.clear();
  m_rows.clear();
  // Three columns, each group to the shortest one so far (rows + its header).
  auto* columns = new QHBoxLayout(m_page);
  columns->setContentsMargins(0, 0, 0, 0);
  columns->setSpacing(24);
  QList<QVBoxLayout*> column;
  QList<int> height;
  for (int i = 0; i < 3; ++i) {
    column << new QVBoxLayout();
    column.last()->setSpacing(2);
    columns->addLayout(column.last(), 1);
    height << 0;
  }
  for (const help::KeyGroup& g : groups) {
    const int c = int(std::min_element(height.begin(), height.end()) - height.begin());
    height[c] += int(g.rows.size()) + 3;
    auto* box = new QWidget(m_page);
    auto* bv = new QVBoxLayout(box);
    bv->setContentsMargins(0, 0, 0, 12);
    bv->setSpacing(2);
    auto* title = new QLabel(g.title.toUpper(), box);
    title->setObjectName("sectionHeader");
    bv->addWidget(title);
    for (const help::KeyRow& r : g.rows) {
      auto* row = new QWidget(box);
      auto* h = new QHBoxLayout(row);
      h->setContentsMargins(0, 2, 0, 2);
      h->setSpacing(4);
      auto* label = new QLabel(r.label, row);
      label->setWordWrap(true);
      h->addWidget(label, 1);
      for (const QString& cap : help::keyCaps(r.keys)) {
        auto* key = new QLabel(cap, row);
        key->setObjectName("keycap");
        h->addWidget(key, 0, Qt::AlignTop);
      }
      bv->addWidget(row);
      m_rows << Row{row, r.label + ' ' + r.keys, int(m_groups.size())};
    }
    column[c]->addWidget(box);
    m_groups << box;
    m_titles << g.title;
  }
  for (QVBoxLayout* l : column) l->addStretch(1);
  setFilter(m_search->text());
}

void ShortcutSheet::setFilter(const QString& text) {
  const QStringList words = text.split(' ', Qt::SkipEmptyParts);
  QList<bool> any(m_groups.size(), false);
  for (const Row& r : m_rows) {
    const bool show = std::all_of(words.begin(), words.end(), [&](const QString& w) { return r.text.contains(w, Qt::CaseInsensitive); });
    r.widget->setVisible(show);
    if (show) any[r.group] = true;
  }
  for (int i = 0; i < m_groups.size(); ++i) m_groups[i]->setVisible(any[i]);
  if (m_search->text() != text) m_search->setText(text);
}

QStringList ShortcutSheet::titles() const {
  QStringList out;
  for (int i = 0; i < m_groups.size(); ++i)
    if (!m_groups[i]->isHidden()) out << m_titles[i];
  return out;
}

QStringList ShortcutSheet::shown() const {
  QStringList out;
  for (const Row& r : m_rows)
    if (!r.widget->isHidden() && !m_groups[r.group]->isHidden()) out << r.text;
  return out;
}

// ---------------------------------------------------------------- GettingStarted
QList<GettingStarted::Lesson> GettingStarted::lessons(const QString& preset) {
  QString orbit, pan;
  for (const help::KeyRow& r : help::mouseRows(preset)) {
    const QString keys = help::keyCaps(r.keys).join(" + ");
    if (r.label == QCoreApplication::translate("help", "Orbit")) orbit = keys;
    else if (r.label == QCoreApplication::translate("help", "Pan")) pan = keys;
  }
  return {
      {tr("Move around the view"),
       tr("Orbit with %1, pan with %2 and zoom with the wheel, toward the cursor. F fits the selection or everything, H goes home, and a click "
          "on the view cube turns the view to that side. View > Navigation preset sets the mouse as other CAD programs do.").arg(orbit, pan),
       "nav." + preset, QString()},
      {tr("Open a file"),
       tr("Open an OPAD document, or a STEP, IGES, STL, 3MF, OBJ, DXF, DWG or SVG file: other formats open read-only, at once. Hide, isolate, "
          "colour and cut what you see; Save turns the file into an editable OPAD document."),
       "file.open", "file.open"},
      {tr("Measure"),
       tr("Press D and click two faces, edges or points: the distance and its X, Y and Z parts. A measures angles, R radii and B the bounding "
          "box; P pins a result to the document."),
       "inspect.distance", "inspect.distance"},
      {tr("Sketch"),
       tr("In Design (Ctrl+2) choose New sketch and click a plane or a flat face. Draw with Line (L), Rectangle (R) and Circle (C), type "
          "lengths as you go, add dimensions with D, then Finish sketch (Ctrl+Enter)."),
       "design.sketch", "design.sketch"},
      {tr("Make it solid"),
       tr("Extrude (E) pulls a closed profile into a solid: drag the arrow or type the distance. Fillets, shells, patterns and the other "
          "features follow on the timeline, where every step can be changed later."),
       "design.extrude", "design.extrude"},
      {tr("Find any command"),
       tr("Press S and type what you want to do: every command with its keys and an animated guide. Over a button, Shift shows its full "
          "card; F1 opens the guide of the tool you are using, Ctrl+/ lists every shortcut and the ? of a panel opens its guide."),
       "tools.commands", "tools.commands"},
  };
}

GettingStarted::GettingStarted(std::function<QAction*(const QString&)> lookup, std::function<void(const QString&)> run, QWidget* parent)
    : QWidget(parent, Qt::Window), m_lookup(std::move(lookup)), m_run(std::move(run)) {
  setObjectName("gettingStarted");
  setWindowTitle(tr("Getting started"));
  setAttribute(Qt::WA_StyledBackground);
  resize(860, 520);
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  m_list = new QListWidget(this);
  m_list->setObjectName("referenceList");
  m_list->setFixedWidth(240);
  m_list->setFrameShape(QFrame::NoFrame);
  h->addWidget(m_list);
  auto* rule = new QFrame(this);
  rule->setFixedWidth(1);
  rule->setObjectName("referenceRule");
  h->addWidget(rule);
  auto* right = new QWidget(this);
  auto* v = new QVBoxLayout(right);
  v->setContentsMargins(20, 16, 20, 16);
  v->setSpacing(10);
  m_title = new QLabel(right);
  m_title->setObjectName("panelTitle");
  m_text = new QLabel(right);
  m_text->setWordWrap(true);
  m_text->setObjectName("secondary");
  m_clip = new ClipView(QString(), right);
  m_clip->setFixedSize(480, 270);
  m_needs = new QLabel(right);
  m_needs->setWordWrap(true);
  auto* buttons = new QHBoxLayout();
  m_try = new QPushButton(tr("Try it"), right);
  m_try->setObjectName("primary");
  m_next = new QPushButton(tr("Next"), right);
  buttons->addStretch(1);
  buttons->addWidget(m_next);
  buttons->addWidget(m_try);
  v->addWidget(m_title);
  v->addWidget(m_text);
  v->addWidget(m_clip, 0, Qt::AlignHCenter);
  v->addWidget(m_needs);
  v->addStretch(1);
  v->addLayout(buttons);
  h->addWidget(right, 1);
  auto restyle = [this, rule] {
    rule->setStyleSheet(QString("background: %1;").arg(theme::css(theme::current().line)));
    m_needs->setStyleSheet(QString("color: %1;").arg(theme::css(theme::current().amber)));
  };
  restyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, restyle);
  connect(m_list, &QListWidget::currentRowChanged, this, &GettingStarted::refresh);
  connect(m_next, &QPushButton::clicked, this, [this] { m_list->setCurrentRow((m_list->currentRow() + 1) % m_list->count()); });
  connect(m_try, &QPushButton::clicked, this, [this] {
    const int i = m_list->currentRow();
    if (i >= 0 && i < m_lessons.size() && m_run && !m_lessons[i].command.isEmpty()) m_run(m_lessons[i].command);
  });
  setPreset("fusion");
}

void GettingStarted::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

void GettingStarted::setPreset(const QString& preset) {
  const int keep = std::max(0, m_list->currentRow());
  m_lessons = lessons(preset);
  const QSignalBlocker quiet(m_list);
  m_list->clear();
  for (int i = 0; i < m_lessons.size(); ++i) m_list->addItem(QString("%1  %2").arg(i + 1).arg(m_lessons[i].title));
  m_list->setCurrentRow(std::min(keep, int(m_lessons.size()) - 1));
  refresh();
}

void GettingStarted::open(int lesson) {
  m_list->setCurrentRow(std::clamp(lesson, 0, int(m_list->count()) - 1));
  QWidget::show();
  raise();
  activateWindow();
}

int GettingStarted::current() const { return m_list->currentRow(); }

void GettingStarted::refresh() {
  const int i = m_list->currentRow();
  if (i < 0 || i >= m_lessons.size()) return;
  const Lesson& l = m_lessons[i];
  m_title->setText(l.title);
  m_text->setText(l.text);
  m_clip->setClip(clips::has(l.clip) ? l.clip : QString());
  m_clip->setVisible(clips::has(l.clip));
  disconnect(m_changed);
  m_action = l.command.isEmpty() || !m_lookup ? nullptr : m_lookup(l.command);
  m_try->setVisible(m_action != nullptr);
  // Not available now (no document yet): the button waits, and says what is missing.
  auto availability = [this] {
    const bool on = m_action && m_action->isEnabled();
    m_try->setEnabled(on);
    const CommandHelp* h = m_action ? help::find(m_action->objectName()) : nullptr;
    m_needs->setText(m_action && !on ? QString::fromUtf8("⚠  ") + (h && !h->requirement.isEmpty() ? help::requirement(*h) : tr("Not available right now.")) : QString());
    m_needs->setVisible(!m_needs->text().isEmpty());
  };
  if (m_action) m_changed = connect(m_action, &QAction::changed, this, availability);
  availability();
}

// ---------------------------------------------------------------- ProblemReport
ProblemReport::ProblemReport(const QStringList& facts, const QString& issueUrl, QWidget* parent) : QDialog(parent), m_facts(facts), m_url(issueUrl) {
  setObjectName("problemReport");
  setWindowTitle(tr("Report a problem"));
  resize(640, 560);
  auto* v = new QVBoxLayout(this);
  v->setSpacing(8);
  auto* intro = new QLabel(tr("Say what you did, what you expected and what happened instead. OPAD adds the facts below; nothing is sent by "
                              "itself: copy or save the report and attach it to your message, or open the issue page and paste it there."), this);
  intro->setWordWrap(true);
  v->addWidget(intro);
  v->addWidget(new QLabel(tr("What happened?"), this));
  m_description = new QPlainTextEdit(this);
  m_description->setObjectName("problemDescription");
  m_description->setPlaceholderText(tr("For example: I opened a STEP file, pressed Fit, and the view went empty."));
  v->addWidget(m_description, 2);
  v->addWidget(new QLabel(tr("OPAD and this computer"), this));
  auto* info = new QPlainTextEdit(facts.join('\n'), this);
  info->setObjectName("problemFacts");
  info->setReadOnly(true);
  info->setFont(theme::mono(11));
  v->addWidget(info, 1);
  m_status = new QLabel(this);
  m_status->setObjectName("tertiary");
  v->addWidget(m_status);
  auto* buttons = new QHBoxLayout();
  auto* copyButton = new QPushButton(tr("Copy report"), this);
  copyButton->setObjectName("primary");
  auto* saveButton = new QPushButton(tr("Save report…"), this);
  auto* pageButton = new QPushButton(tr("Open the issue page"), this);
  pageButton->setToolTip(m_url);
  pageButton->setVisible(!m_url.isEmpty());
  auto* closeButton = new QPushButton(tr("Close"), this);
  buttons->addWidget(pageButton);
  buttons->addStretch(1);
  buttons->addWidget(saveButton);
  buttons->addWidget(copyButton);
  buttons->addWidget(closeButton);
  v->addLayout(buttons);
  connect(copyButton, &QPushButton::clicked, this, &ProblemReport::copy);
  connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(saveButton, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save report"), "opad-problem.txt", tr("Text file (*.txt)"));
    if (path.isEmpty()) return;
    QFile f(path);
    const bool ok = f.open(QIODevice::WriteOnly | QIODevice::Text) && f.write(report().toUtf8()) >= 0;
    m_status->setText(ok ? tr("Saved %1").arg(QDir::toNativeSeparators(path)) : tr("Could not write %1").arg(QDir::toNativeSeparators(path)));
  });
  connect(pageButton, &QPushButton::clicked, this, [this] {  // the page gets the title; the report goes on the clipboard to paste
    copy();
    QUrl url(m_url);
    QUrlQuery query;
    const QString first = m_description->toPlainText().section('\n', 0, 0).trimmed().left(80);
    query.addQueryItem("title", first.isEmpty() ? QString("Problem report") : first);
    url.setQuery(query);
    QDesktopServices::openUrl(url);
  });
}

QString ProblemReport::report() const { return help::problemReport(m_description->toPlainText(), m_facts); }

void ProblemReport::copy() {
  QApplication::clipboard()->setText(report());
  m_status->setText(tr("The report is on the clipboard."));
}
