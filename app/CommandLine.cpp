#include "CommandLine.hpp"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPainterPath>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "AreaController.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "Ribbon.hpp"
#include "SketchCommands.hpp"
#include "SketchEditor.hpp"
#include "Theme.hpp"
#include "Viewport.hpp"

OPAD_ICON_TABLE(commandLine, {"commandLine", R"(<rect x="3" y="5" width="18" height="14"/><path d="M7 10l3 2-3 2M12 15h5"/>)"});

// ---------------------------------------------------------------- CommandLine
CommandLine::CommandLine(QWidget* view) : QFrame(view), m_view(view) {
  setObjectName("commandLine");
  setAttribute(Qt::WA_NativeWindow);  // over the native 3D window
  setAttribute(Qt::WA_StyledBackground);
  setLayoutDirection(Qt::LeftToRight);  // coordinates and words as typed; the prompt's own text keeps its direction
  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(10, 4, 4, 4);
  column->setSpacing(2);
  m_log = new QLabel(this);
  m_log->setObjectName("commandLog");
  m_log->setTextFormat(Qt::RichText);
  m_log->hide();
  m_list = new QListWidget(this);
  m_list->setObjectName("commandCompletions");
  m_list->setFocusPolicy(Qt::NoFocus);  // the keyboard stays in the line; Up, Down and Tab choose
  m_list->setFrameShape(QFrame::NoFrame);
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list->hide();
  column->addWidget(m_list);
  column->addWidget(m_log);
  auto* row = new QHBoxLayout;
  row->setSpacing(6);
  m_prompt = new QLabel(this);
  m_prompt->setObjectName("commandPrompt");
  m_prompt->setTextFormat(Qt::PlainText);
  m_edit = new QLineEdit(this);
  m_edit->setObjectName("commandEdit");
  m_edit->setFrame(false);
  m_edit->setPlaceholderText(tr("Command or point: x,y  @dx,dy  @length<angle"));
  m_edit->setToolTip(tr("Type a command (L, C, REC, TR, O, M ...) or a point, then Enter · while a word is typed ↑/↓ choose among the commands listed and Tab completes · else ↑/↓ earlier entries · Enter on an empty line ends the step · Esc clears, then steps back"));
  m_edit->installEventFilter(this);
  m_match = new QLabel(this);
  m_match->setObjectName("commandMatch");
  m_close = new QToolButton(this);
  m_close->setObjectName("commandClose");
  // Its key as the user has it (Space by default), or its entry in the Sketch menu when it has none.
  auto closeTip = [this] {
    const QString key = keys::text(QStringLiteral("sketch.commandLine"));
    m_close->setToolTip(key.isEmpty() ? tr("Hide the command line (Sketch > Command line shows it again)") : tr("Hide the command line (%1 shows it again)").arg(key));
  };
  closeTip();
  connect(keys::notifier(), &keys::Notifier::changed, this, closeTip);
  m_close->setFixedSize(20, 20);
  m_close->setIconSize(QSize(12, 12));
  m_close->setFocusPolicy(Qt::NoFocus);
  row->addWidget(m_prompt);
  row->addWidget(m_edit, 1);
  row->addWidget(m_match);
  row->addWidget(m_close);
  column->addLayout(row);
  connect(m_close, &QToolButton::clicked, this, &CommandLine::closeRequested);
  connect(m_edit, &QLineEdit::textEdited, this, [this] { showMatch(true); });
  connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { submit(item->data(Qt::UserRole).toString()); });
  connect(theme::notifier(), &theme::Notifier::changed, this, &CommandLine::restyle);
  view->installEventFilter(this);
  restyle();
  hide();
}

void CommandLine::restyle() {
  const Tokens& t = theme::current();
  setStyleSheet(QString("#commandLine { background: %1; border: 1px solid %2; border-radius: 5px; }"
                        "#commandPrompt { color: %3; font-size: 12px; }"
                        "#commandEdit { background: transparent; color: %4; font-family: '%5'; font-size: 12px; selection-background-color: %6; }"
                        "#commandMatch { color: %7; font-size: 11px; }"
                        "#commandLog { color: %7; font-family: '%5'; font-size: 11px; }"
                        "#commandCompletions { background: transparent; color: %4; font-family: '%5'; font-size: 12px; }"
                        "#commandCompletions::item { padding: 1px 2px; }"
                        "#commandCompletions::item:selected { background: %6; color: %4; }"
                        "#commandClose { background: transparent; border: none; }")
                    .arg(theme::css(t.bg2), theme::css(t.line), theme::css(t.fg2), theme::css(t.fg), theme::mono().family(), theme::css(t.selbg), theme::css(t.fg3)));
  auto palette = m_edit->palette();
  palette.setColor(QPalette::PlaceholderText, t.fg3);
  m_edit->setPalette(palette);
  m_close->setIcon(icons::icon("close", t.fg3));
  layoutLines();
}

void CommandLine::setPrompt(const QString& prompt, const QString& hints) {
  const QString shown = prompt.isEmpty() ? tr("Command:") : prompt + QStringLiteral(":");
  if (m_prompt->text() != shown) m_prompt->setText(shown);
  m_prompt->setToolTip(hints);
}

QString CommandLine::prompt() const { return m_prompt->text(); }

void CommandLine::log(const QString& line, bool problem) {
  if (line.isEmpty() || (!m_lines.isEmpty() && m_lines.back() == line)) return;
  m_lines << line;
  if (problem) m_problems << line;
  while (m_lines.size() > 50) m_lines.removeFirst();
  layoutLines();
}

void CommandLine::layoutLines() {
  const Tokens& t = theme::current();
  QStringList shown;
  for (int i = std::max(0, int(m_lines.size()) - kLines); i < m_lines.size(); ++i) {
    const QString text = m_lines[i].toHtmlEscaped();
    shown << (m_problems.contains(m_lines[i]) ? QString("<span style=\"color:%1\">%2</span>").arg(theme::css(t.red), text) : text);
  }
  m_log->setText(shown.join("<br>"));
  m_log->setVisible(!shown.isEmpty() && m_list->isHidden());
  place();
}

void CommandLine::remember(const QString& entry) {
  if (m_history.isEmpty() || m_history.back() != entry) m_history << entry;
  while (m_history.size() > 100) m_history.removeFirst();
  m_recall = int(m_history.size());
  m_draft.clear();
}

void CommandLine::showMatch(bool complete) {
  const std::string text = m_edit->text().toStdString();
  const auto* c = sketchcommands::find(text);
  m_match->setText(c ? QStringLiteral("→ ") + i18n::t(c->name) : QString());
  const auto found = complete ? sketchcommands::complete(text) : std::vector<sketchcommands::Completion>();
  m_list->clear();
  m_chosen = false;
  size_t pad = 0;
  for (const auto& f : found) pad = std::max(pad, f.word.size());
  for (const auto& f : found) {
    auto* item = new QListWidgetItem(QString::fromStdString(f.word).toUpper().leftJustified(int(pad) + 2) + i18n::t(f.command->name), m_list);
    item->setData(Qt::UserRole, QString::fromStdString(f.word));
  }
  if (!found.empty()) {
    m_list->setCurrentRow(0);
    m_list->ensurePolished();  // the style sheet's font and padding, before the rows are measured
    m_list->setFixedHeight(int(found.size()) * m_list->sizeHintForRow(0) + 2);
  }
  if (found.empty() && m_list->isHidden()) return;
  m_list->setVisible(!found.empty());
  m_log->setVisible(found.empty() && !m_log->text().isEmpty());  // the commands in the log's place while a word is typed
  place();
}

void CommandLine::submit(const QString& text) {
  m_edit->clear();
  showMatch();
  m_recall = int(m_history.size());
  emit entered(text);
}

void CommandLine::focusLine() {
  if (isHidden()) {
    show();
    place();
  }
  raise();
  if (!isActiveWindow()) window()->activateWindow();
  m_edit->setFocus(Qt::ShortcutFocusReason);
}

void CommandLine::place() {
  if (!m_view) return;
  const int width = std::min(kMaxWidth, std::max(240, m_view->width() - 2 * kMargin));
  layout()->invalidate();  // a line shown or hidden just now
  layout()->activate();
  const int height = std::max(30, layout()->heightForWidth(width) > 0 ? layout()->heightForWidth(width) : sizeHint().height());
  setGeometry(kMargin, m_view->height() - kMargin - height, width, height);
  if (isVisible()) raise();
}

void CommandLine::resizeEvent(QResizeEvent* e) {
  QFrame::resizeEvent(e);
  QPainterPath path;
  path.addRoundedRect(QRectF(rect()), 5, 5);
  setMask(QRegion(path.toFillPolygon().toPolygon()));  // a native child cannot be translucent
}

bool CommandLine::eventFilter(QObject* target, QEvent* event) {
  if (target == m_view) {
    if (event->type() == QEvent::Resize) place();
    return QFrame::eventFilter(target, event);
  }
  if (target != m_edit || (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)) return QFrame::eventFilter(target, event);
  // The line's own keys: no window shortcut sees them (Esc would be the sketch's at once, Enter nobody's).
  auto* key = static_cast<QKeyEvent*>(event);
  const bool press = event->type() == QEvent::KeyPress, empty = m_edit->text().isEmpty();
  if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
  switch (key->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      key->accept();
      if (press) submit(m_chosen && m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : m_edit->text());
      return true;
    case Qt::Key_Escape:
      key->accept();
      if (press) {
        if (empty) emit escaped();
        m_edit->clear();
        showMatch();
        m_recall = int(m_history.size());
      }
      return true;
    case Qt::Key_Tab:  // the command chosen, into the line
      if (m_list->isHidden() || !m_list->currentItem()) return false;
      key->accept();
      if (press) {
        m_edit->setText(m_list->currentItem()->data(Qt::UserRole).toString());
        showMatch();
      }
      return true;
    case Qt::Key_Up:
    case Qt::Key_Down: {
      key->accept();
      if (!press) return true;
      if (m_list->isVisible()) {  // through the commands listed
        const int row = m_list->currentRow() + (key->key() == Qt::Key_Up ? -1 : 1);
        if (row >= 0 && row < m_list->count()) m_list->setCurrentRow(row);
        m_chosen = true;
        return true;
      }
      if (m_recall >= int(m_history.size())) m_draft = m_edit->text();
      m_recall = sketchcommands::recall(m_recall, int(m_history.size()), key->key() == Qt::Key_Up);
      if (m_recall >= 0) m_edit->setText(m_recall < int(m_history.size()) ? m_history[m_recall] : m_draft);
      showMatch();
      return true;
    }
    case Qt::Key_Backspace:
      if (!empty) return false;
      key->accept();
      if (press && !key->isAutoRepeat()) emit undoPoint();  // held to clear the line, it stops at the line's start
      return true;
    default: return false;
  }
}

// ---------------------------------------------------------------- the area
// Shown while a sketch is open (setting sketch/commandLine, on by default; its close button turns it off, Space or its
// ribbon button turns it on again and gives it the keyboard). Words run commands, anything else goes to the sketch.
class SketchCommandLine : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    CommandInfo info;
    info.id = "sketch.commandLine", info.label = tr("Command line"), info.icon = "commandLine", info.key = QKeySequence(Qt::Key_Space);
    info.keywords = {tr("type a command"), tr("coordinates"), tr("keyboard")};
    info.editsDocument = true;  // a sketch command, as the built-in ones (MainWindow::isEditAction)
    services().addCommand(info, [this] { open(); });
  }
  void ready() override {
    m_line = new CommandLine(services().viewport());
    SketchEditor* sketch = services().design()->sketch();
    for (auto signal : {&SketchEditor::changed, &SketchEditor::workflowChanged, &SketchEditor::hintsChanged}) connect(sketch, signal, this, [this] { refresh(); });
    connect(sketch, &SketchEditor::toolChanged, this, [this] { refresh(); });
    // What the sketch says besides its prompt (a refusal, a value that waits) goes into the lines above the input.
    connect(sketch, &SketchEditor::status, this, [this, sketch](const QString& text) {
      if (sketch->active() && !text.isEmpty() && text != sketch->prompt(true)) m_line->log(text, m_entering);
    });
    connect(m_line, &CommandLine::entered, this, [this](const QString& text) { run(text); });
    connect(m_line, &CommandLine::escaped, this, [this, sketch] {
      if (!sketch->escape()) services().viewport()->setFocus();  // nothing left to step back from: the keyboard back to the view
      refresh();
    });
    connect(m_line, &CommandLine::undoPoint, this, [sketch] { sketch->undoPoint(); });
    connect(m_line, &CommandLine::closeRequested, this, [this] {
      QSettings().setValue("sketch/commandLine", false);
      services().viewport()->setFocus();
      refresh();
    });
    qApp->installEventFilter(this);
    refresh();
  }
  void workspaceChanged(const QString&) override { refresh(); }
  // Space is the view's: a widget with the keyboard keeps its own (the browser's tree, a list, a button), so the window
  // shortcut sees only a Space typed over the view (a Tool window's keys reach its owner's shortcuts too).
  bool eventFilter(QObject* target, QEvent* event) override {
    if (event->type() != QEvent::ShortcutOverride || !services().design()->sketch()->active()) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    auto* widget = qobject_cast<QWidget*>(target);
    QWidget* view = services().viewport();
    if (key->key() != Qt::Key_Space || key->modifiers() != Qt::NoModifier || !widget || widget == view || widget->isAncestorOf(view)) return false;  // the view's, as it goes up
    key->accept();  // delivered to the widget as a key press, not as the shortcut
    return false;
  }
  void positionOverlays(const QRect&) override {
    if (m_line) m_line->place();
  }

 private:
  void open() {
    QSettings().setValue("sketch/commandLine", true);
    refresh();
    if (m_line->isVisible()) m_line->focusLine();
  }
  void refresh() {
    if (!m_line) return;
    SketchEditor* sketch = services().design()->sketch();
    const bool on = sketch->active() && sketch->visible() && QSettings().value("sketch/commandLine", true).toBool();
    if (on) m_line->setPrompt(sketch->tool() == "select" ? QString() : sketch->prompt(), sketch->keyHints());
    if (on != m_line->isVisible()) {
      m_line->setVisible(on);
      if (on) m_line->place();
    }
  }
  void run(const QString& text) {
    SketchEditor* sketch = services().design()->sketch();
    if (!sketch->active()) return;
    const QString line = text.trimmed();
    if (line.isEmpty()) {  // the step's Enter; with no tool running, the last command again
      if (sketch->tool() != "select") sketch->done();
      else if (!m_last.isEmpty()) return run(m_last);
    } else {
      m_line->remember(line);
      m_line->log(m_line->prompt() + QStringLiteral(" ") + line);
      m_entering = true;
      if (const auto* c = sketchcommands::find(line.toStdString())) command(*c);
      else if (const QString problem = sketch->enter(line); !problem.isEmpty())
        m_line->log(sketchcommands::word(line.toStdString()) ? tr("Unknown command: %1").arg(line) : problem, true);
      m_entering = false;
    }
    refresh();
    if (m_line->isVisible()) m_line->focusLine();  // typed here: the typing goes on here (a value box took the keyboard meanwhile)
  }
  void command(const sketchcommands::Command& c) {
    SketchEditor* sketch = services().design()->sketch();
    const QString id = QString::fromLatin1(c.id);
    m_last = id;
    auto trigger = [this](const char* action) {
      if (QAction* a = services().action(action); a && a->isEnabled()) a->trigger();
      else m_line->log(tr("Not available now"), true);
    };
    if (id == "undo") sketch->undo();
    else if (id == "redo") sketch->redo();
    else if (id == "delete") sketch->deleteSelection();
    else if (id == "close") {
      if (!sketch->closeChain()) m_line->log(tr("Close needs a polyline of two segments or more"), true);
    } else if (id == "fit") sketch->fitSketch();
    else if (id == "finish") trigger("sketch.finish");
    else if (id == "cancel") trigger("sketch.cancel");
    else if (id == "copyclip") trigger("edit.copy");
    else if (id == "cut") trigger("edit.cut");
    else if (id == "paste") trigger("edit.paste");
    else sketch->setTool(id);
  }
  QPointer<CommandLine> m_line;
  QString m_last;  // the last command: Enter on an empty line runs it again
  bool m_entering = false;  // an entry runs: what the sketch says now is about it
};
OPAD_AREA(SketchCommandLine)
