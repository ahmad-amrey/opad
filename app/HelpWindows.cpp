#include "HelpWindows.hpp"

#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
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
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace help {
QString KeyRow::text() const {
  QStringList out;
  for (const QStringList& caps : keys) out << keys::joined(caps);
  return out.join(" / ");
}

KeyRow keyRow(const QString& label, const QList<QKeySequence>& sequences) {
  KeyRow row{label, {}, {}};
  QStringList portable;
  for (const QKeySequence& key : sequences)
    if (keys::pressable(key)) {
      row.keys << keys::caps(key);
      portable << key.toString(QKeySequence::PortableText);
    }
  row.search = portable.join(' ');
  return row;
}

// A fixed key or mouse modifier (named for the platform) and a mouse word: Shift + Middle drag.
static QStringList with(const QString& modifier, const QString& gesture) { return modifier.isEmpty() ? QStringList{gesture} : keys::fixedCaps(modifier) << gesture; }

QList<KeyRow> mouseRows(const QString& preset) {
  const QString middle = QCoreApplication::translate("help", "Middle drag"), right = QCoreApplication::translate("help", "Right drag");
  QStringList orbit = with("shift", middle), pan = with({}, middle);  // Viewport::setNavPreset
  if (preset == "solidworks") orbit = with({}, middle), pan = with("ctrl", middle);
  else if (preset == "onshape") orbit = with({}, right), pan = with({}, middle);
  else if (preset == "blender") orbit = with({}, middle), pan = with("shift", middle);
  return {{QCoreApplication::translate("help", "Orbit"), {orbit}, {}},
          {QCoreApplication::translate("help", "Pan"), {pan}, {}},
          {QCoreApplication::translate("help", "Zoom at the cursor"), {{QCoreApplication::translate("help", "Wheel")}}, {}},
          {QCoreApplication::translate("help", "Select"), {{QCoreApplication::translate("help", "Click")}}, {}},
          {QCoreApplication::translate("help", "Select in a window"), {{QCoreApplication::translate("help", "Left drag")}}, {}}};
}

QList<KeyGroup> keyGroups(const QList<QAction*>& actions, bool sketching, const QString& preset) {
  QList<KeyGroup> groups;
  QAction* helpCurrent = nullptr;
  for (QAction* a : actions) {
    if (a && a->objectName() == "help.current") helpCurrent = a;
    if (!a || a->objectName().isEmpty()) continue;
    const QList<QKeySequence> live = a->shortcuts();  // the keys that work now (a sketch holds some)
    if (std::none_of(live.begin(), live.end(), keys::pressable)) continue;
    QString group = a->property("commandGroup").toString();
    if (group.isEmpty()) group = help::group(a->objectName());
    const CommandHelp* h = help::find(a->objectName());
    QString label = h && !h->title.isEmpty() ? h->title : a->text();
    label.remove('&').remove(QString::fromUtf8("…"));
    auto it = std::find_if(groups.begin(), groups.end(), [&](const KeyGroup& g) { return g.title == group; });
    if (it == groups.end()) it = groups.insert(groups.end(), KeyGroup{group, {}});
    it->rows << keyRow(label, live);  // an alternate too (Redo: Ctrl+Y / Ctrl+Shift+Z)
  }
  if (sketching)  // the sketch's keys first
    std::stable_partition(groups.begin(), groups.end(), [&](const KeyGroup& g) {
      return std::any_of(actions.begin(), actions.end(), [&](QAction* a) { return a && a->objectName().startsWith("sketch.") && a->property("commandGroup").toString() == g.title; });
    });
  groups << KeyGroup{QCoreApplication::translate("help", "Mouse"), mouseRows(preset)};
  groups << KeyGroup{QCoreApplication::translate("help", "Without the mouse"),  // UI-124
                     {{QCoreApplication::translate("help", "Key tips on the ribbon's tabs and tools"), {keys::fixedCaps("alt")}, {}},
                      {QCoreApplication::translate("help", "Show or hide the browser's rows, suppress a timeline marker"), {{QCoreApplication::translate("help", "Space")}}, {}},
                      {QCoreApplication::translate("help", "The menu of a browser row or a timeline marker"),
                       {{QCoreApplication::translate("help", "Menu key")}, keys::caps(QKeySequence(QKeyCombination(Qt::ShiftModifier, Qt::Key_F10)))}, {}},
                      {QCoreApplication::translate("help", "Repeat the last tool, in the view while nothing runs"), {keys::fixedCaps("enter")}, {}}}};
  KeyGroup every{QCoreApplication::translate("help", "In every tool"),
                 {{QCoreApplication::translate("help", "Step back, or leave the tool"), {keys::fixedCaps("esc")}, {}},
                  {QCoreApplication::translate("help", "OK, or finish"), {keys::fixedCaps("enter")}, {}},
                  {QCoreApplication::translate("help", "The full card of the button under the pointer"), {keys::fixedCaps("shift")}, {}}}};
  // Help for this tool's key, as the user bound it; no row without one.
  if (helpCurrent && !keys::binding(helpCurrent).isEmpty())
    every.rows << keyRow(QCoreApplication::translate("help", "Guide of the tool you are using"), {keys::binding(helpCurrent)});
  groups << every;
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
  m_search->installEventFilter(this);
  connect(edit, &QPushButton::clicked, this, &ShortcutSheet::editRequested);
  m_again = new QShortcut(this);  // the key that opened it closes it, as Esc does: help.shortcuts', whatever the user bound
  connect(m_again, &QShortcut::activated, this, &QWidget::close);
  rebind();
  connect(keys::notifier(), &keys::Notifier::changed, this, &ShortcutSheet::rebind);
}

void ShortcutSheet::rebind() {
  const QKeySequence key = keys::binding(QStringLiteral("help.shortcuts"));
  m_again->setKey(key);
  m_again->setEnabled(!key.isEmpty());
}

QKeySequence ShortcutSheet::closeKey() const { return m_again->isEnabled() ? m_again->key() : QKeySequence(); }

void ShortcutSheet::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

// The key that opened it can be one that types ('?' by default): in the search field it closes the sheet while nothing is
// typed there, and is typed into the search after that.
bool ShortcutSheet::eventFilter(QObject* watched, QEvent* event) {
  if (watched == m_search && event->type() == QEvent::ShortcutOverride && m_search->text().isEmpty() &&
      keys::pressedBy(closeKey(), static_cast<QKeyEvent*>(event))) {
    event->ignore();  // not the field's: the shortcut map takes it
    return true;
  }
  return QWidget::eventFilter(watched, event);
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
      // The caps read left to right in every language ("Ctrl" before "F"): their own box, placed as a whole.
      auto* caps = new QWidget(row);
      caps->setLayoutDirection(Qt::LeftToRight);
      auto* ch = new QHBoxLayout(caps);
      ch->setContentsMargins(0, 0, 0, 0);
      ch->setSpacing(4);
      for (qsizetype i = 0; i < r.keys.size(); ++i) {
        if (i) ch->addWidget(new QLabel("/", caps), 0, Qt::AlignTop);  // between the alternates, never a cap
        for (const QString& cap : r.keys[i]) {
          auto* key = new QLabel(cap, caps);
          if (cap != keys::kThen) key->setObjectName("keycap");  // a multi-chord key's separator is plain text
          ch->addWidget(key, 0, Qt::AlignTop);
        }
      }
      h->addWidget(caps, 0, Qt::AlignTop);
      bv->addWidget(row);
      m_rows << Row{row, r.label + ' ' + r.text(), r.search, int(m_groups.size())};
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
    const bool show = std::all_of(words.begin(), words.end(), [&](const QString& w) { return r.text.contains(w, Qt::CaseInsensitive) || r.search.contains(w, Qt::CaseInsensitive); });
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
    const QString keys = r.keys.value(0).join(" + ");
    if (r.label == QCoreApplication::translate("help", "Orbit")) orbit = keys;
    else if (r.label == QCoreApplication::translate("help", "Pan")) pan = keys;
  }
  // Every key named is the user's now ({key:...}, help::expand): a command without one is named without it.
  return {
      {tr("Move around the view"),
       help::expand(tr("Orbit with %1, pan with %2 and zoom with the wheel, toward the cursor. Fit ({key:view.fit}) frames the selection or everything, "
                       "Home ({key:view.home}) goes home, and a click on the view cube turns the view to that side. View > Navigation preset sets the "
                       "mouse as other CAD programs do.")).arg(orbit, pan),
       "nav." + preset, QString()},
      {tr("Open a file"),
       tr("Open an OPAD document, or a STEP, IGES, STL, 3MF, OBJ, DXF, DWG or SVG file: other formats open read-only, at once. Hide, isolate, "
          "colour and cut what you see; Save turns the file into an editable OPAD document."),
       "file.open", "file.open"},
      {tr("Measure"),
       help::expand(tr("Start Distance ({key:inspect.distance}) and click two faces, edges, points or bodies: the distance and its X, Y and Z parts. "
                       "Angle ({key:inspect.angle}) measures angles, Radius ({key:inspect.radius}) radii and Bounding box ({key:inspect.bbox}) the "
                       "box around the selection; Pin ({key:inspect.pin}) keeps a result in the document.")),
       "inspect.distance", "inspect.distance"},
      {tr("Sketch"),
       help::expand(tr("In Design ({key:workspace.design}) choose New sketch, pick a plane or a flat face, then OK. Draw with Line ({key:sketch.line}), "
                       "Rectangle ({key:sketch.rect}) and Circle ({key:sketch.circle}), type lengths as you go, add dimensions with Dimension "
                       "({key:sketch.dimension}), then Finish sketch ({key:sketch.finish}).")),
       "design.sketch", "design.sketch"},
      {tr("Make it solid"),
       help::expand(tr("Extrude ({key:design.extrude}) pulls a closed profile into a solid: drag the arrow or type the distance. Fillets, shells, "
                       "patterns and the other features follow on the timeline, where every step can be changed later.")),
       "design.extrude", "design.extrude"},
      {tr("Find any command"),
       help::expand(tr("{press:tools.commands} and type what you want to do: every command with its key and, for most, an animated guide. Over a "
                       "button, Shift shows its full card; Help for this tool ({key:help.current}) opens the guide of the tool you are using, the "
                       "Shortcuts cheat sheet ({key:help.shortcuts}) lists every shortcut and the ? of a panel opens its guide.")),
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
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] { setPreset(m_preset); });  // the keys the lessons name
}

void GettingStarted::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

void GettingStarted::setPreset(const QString& preset) {
  const int keep = std::max(0, m_list->currentRow());
  m_preset = preset;
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

// ---------------------------------------------------------------- CoachCard
CoachCard::CoachCard(QWidget* viewport) : QFrame(viewport) {
  setObjectName("coachCard");
  setAttribute(Qt::WA_NativeWindow);  // over the native 3D window
  setAttribute(Qt::WA_StyledBackground);
  setFocusPolicy(Qt::NoFocus);
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(10, 10, 8, 10);
  row->setSpacing(14);
  m_clip = new ClipView(clips::has("design.extrude") ? "design.extrude" : QString(), this);
  m_clip->setFixedSize(256, 144);
  row->addWidget(m_clip, 0, Qt::AlignTop);
  auto* v = new QVBoxLayout();
  v->setSpacing(6);
  // Title, then the links and the close button: Getting started, Don't show again, x.
  auto* head = new QHBoxLayout();
  head->setSpacing(4);
  auto* title = new QLabel(tr("Start your design"), this);
  title->setObjectName("panelTitle");
  head->addWidget(title, 1);
  auto button = [this](const QString& command, const QString& label, const QString& name) {
    auto* b = new QPushButton(label, this);
    b->setProperty("command", command);
    b->setObjectName(name);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    if (!command.isEmpty()) connect(b, &QPushButton::clicked, this, [this, command] { emit run(command); });
    return b;
  };
  head->addWidget(button("help.start", tr("Getting started"), "coachLink"));
  m_never = button(QString(), tr("Don't show again"), "coachLink");
  head->addWidget(m_never);
  m_close = new QToolButton(this);
  m_close->setObjectName("toastClose");
  m_close->setToolTip(tr("Hide for this document"));
  m_close->setFixedSize(20, 20);
  m_close->setIconSize(QSize(12, 12));
  m_close->setFocusPolicy(Qt::NoFocus);
  head->addWidget(m_close);
  v->addLayout(head);
  auto* text = new QLabel(tr("Sketch on a plane, draw a closed profile, then extrude it into a solid. Or start from a box, or bring in a file."), this);
  text->setObjectName("secondary");
  text->setWordWrap(true);
  v->addWidget(text);
  m_steps = new QLabel(this);
  m_steps->setObjectName("tertiary");
  v->addWidget(m_steps);
  rekey();
  connect(keys::notifier(), &keys::Notifier::changed, this, &CoachCard::rekey);
  v->addStretch(1);
  auto* buttons = new QHBoxLayout();
  buttons->setSpacing(6);
  buttons->addWidget(button("design.sketch", tr("New sketch"), "primary"));
  buttons->addWidget(button("design.box", tr("Box"), QString()));
  buttons->addWidget(button("file.import", tr("Import…"), QString()));
  buttons->addStretch(1);
  v->addLayout(buttons);
  row->addLayout(v, 1);
  setFixedWidth(720);
  connect(m_close, &QToolButton::clicked, this, [this] { hide(); emit dismissed(); });
  connect(m_never, &QPushButton::clicked, this, [this] { hide(); emit neverAgain(); });
  restyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, &CoachCard::restyle);
  adjustSize();
}

// The tools of the first steps with the user's keys (a tool without one is named alone).
void CoachCard::rekey() {
  m_steps->setText(help::expand(tr("1  New sketch    2  Line ({key:sketch.line}), Rectangle ({key:sketch.rect}), Circle ({key:sketch.circle})    "
                                   "3  Extrude ({key:design.extrude})")));
}

QString CoachCard::steps() const { return m_steps->text(); }

QPushButton* CoachCard::button(const QString& command) const {
  for (auto* b : findChildren<QPushButton*>())
    if (b->property("command").toString() == command) return b;
  return nullptr;
}

void CoachCard::restyle() {
  const Tokens& t = theme::current();
  setStyleSheet(QString("QFrame#coachCard { background: %1; border: 1px solid %2; border-radius: 6px; }\n"
                        "QFrame#coachCard QLabel { background: transparent; }\n"
                        "QPushButton#coachLink { background: transparent; border: none; padding: 2px 4px; color: %3; }\n"
                        "QPushButton#coachLink:hover { color: %4; }\n")
                    .arg(theme::css(t.bg2), theme::css(t.line), theme::css(t.dark ? t.sel.lighter(130) : t.sel), theme::css(t.fg)));
  m_close->setIcon(icons::icon("close", t.fg2));
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
