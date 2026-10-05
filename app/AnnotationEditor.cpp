#include "AnnotationEditor.hpp"

#include <QApplication>
#include <QBitmap>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

#include "AppDocument.hpp"
#include "GuidedTool.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "KeyText.hpp"
#include "Notes.hpp"
#include "Panels.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "Viewport.hpp"

namespace {
constexpr size_t kMaxStrokes = 128, kMaxPoints = 8192;  // the op's limits; the document checks them again
constexpr size_t kUndoDepth = 50;
constexpr int kStrokeRow = 24, kVisibleRows = 3;

using Paint = std::function<void(QPainter&)>;

QPixmap painted(QSize size, qreal dpr, const Paint& paint) {
  QPixmap pixmap(size * dpr);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  paint(painter);
  return pixmap;
}

// Every scale the app runs at, so swatches stay crisp at 125-200 %. `on` is the checked look.
QIcon paintedIcon(QSize size, const Paint& off, const Paint& on = {}) {
  QIcon icon;
  for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
    icon.addPixmap(painted(size, dpr, off), QIcon::Normal, QIcon::Off);
    if (on) icon.addPixmap(painted(size, dpr, on), QIcon::Normal, QIcon::On);
  }
  return icon;
}

void dot(QPainter& p, QPointF centre, double diameter, const QColor& color) {
  p.setPen(QPen(color.darker(160), 1));
  p.setBrush(color);
  p.drawEllipse(centre, diameter / 2, diameter / 2);
}

// A pen sample: a round-capped line at the stroke's width, rimmed so that white still shows on a light panel.
void sample(QPainter& p, QPointF from, QPointF to, double width, const QColor& color) {
  p.setPen(QPen(QColor(0, 0, 0, 70), width + 2, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(from, to);
  p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(from, to);
}

// A picker chip (type, tool, colour, width): checkable, never takes the focus from the view. `fit`: as wide as its
// label, the row's spare width shared out; otherwise the row is split evenly.
QToolButton* chip(QWidget* parent, const QString& name, int height, bool fit = false) {
  auto* button = new QToolButton(parent);
  button->setObjectName(name);
  button->setProperty("annotationChoice", true);
  button->setCheckable(true);
  button->setFocusPolicy(Qt::NoFocus);
  button->setFixedHeight(height);
  button->setSizePolicy(fit ? QSizePolicy::Expanding : QSizePolicy::Ignored, QSizePolicy::Fixed);
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  return button;
}

QToolButton* flatButton(QWidget* parent, const QString& name, const QString& icon, const QString& tip, int size = 24) {
  const Tokens& t = theme::current();
  auto* button = new QToolButton(parent);
  button->setObjectName(name);
  button->setProperty("annotationRole", "flat");
  button->setIcon(icons::icon(icon, t.fg2, t.bg4));
  button->setIconSize(QSize(size * 2 / 3, size * 2 / 3));
  button->setFixedSize(size, size);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(tip);
  return button;
}

// Cancel and Save carry their key, like the hints in the prompt bar. The labels sit in a layout, which a push
// button's own size hint knows nothing about (and a style sheet min-height stops the layout from setting a minimum).
class KeyButton : public QPushButton {
 public:
  using QPushButton::QPushButton;
  QSize sizeHint() const override { return layout() ? layout()->sizeHint().expandedTo(QPushButton::sizeHint()) : QPushButton::sizeHint(); }
  QSize minimumSizeHint() const override { return sizeHint(); }
};

QPushButton* keyButton(QWidget* parent, const QString& name, const QString& text, const QString& keys) {
  auto* button = new KeyButton(parent);
  button->setObjectName(name);
  button->setAccessibleName(text);
  auto* row = new QHBoxLayout(button);
  row->setContentsMargins(12, 0, 10, 0);
  row->setSpacing(8);
  auto* label = new QLabel(text, button);
  auto* key = new QLabel(keys, button);
  key->setProperty("annotationRole", "key");
  for (QLabel* l : {label, key}) {
    l->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(l);
  }
  return button;
}

QFrame* rule(QWidget* parent) {
  auto* line = new QFrame(parent);
  line->setProperty("annotationRole", "rule");
  line->setFixedHeight(1);
  return line;
}

// The eraser's reach (10 px around the pointer, see eraseAt) as the cursor.
QCursor eraserCursor(qreal dpr) {
  return QCursor(painted(QSize(24, 24), dpr, [](QPainter& p) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 150), 3));
    p.drawEllipse(QPointF(12, 12), 9.5, 9.5);
    p.setPen(QPen(Qt::white, 1.4));
    p.drawEllipse(QPointF(12, 12), 9.5, 9.5);
  }));
}

// The target's badge: a native child over the view like the prompt bar, rounded by a mask because it cannot be
// translucent.
class Badge : public QFrame {
 public:
  explicit Badge(QWidget* parent) : QFrame(parent) {
    setObjectName("annotationAnchorBadge");
    setProperty("annotationRole", "badge");
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

 protected:
  void resizeEvent(QResizeEvent* e) override {
    QFrame::resizeEvent(e);
    QBitmap mask(size());
    mask.fill(Qt::color0);
    QPainter p(&mask);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::color1);
    p.drawRoundedRect(QRectF(rect()), 4, 4);
    p.end();
    setMask(mask);
  }
};

double round4(double v) { return std::round(v * 1e4) / 1e4; }  // 0.1 µm: plenty for markup, keeps the op short
}  // namespace

AnnotationEditor::AnnotationEditor(AppDocument* doc, Viewport* viewport, ToolPanel* panel, QObject* parent, bool drawing)
    : QObject(parent), m_doc(doc), m_viewport(viewport), m_panel(panel), m_drawingMode(drawing) {
  doc->annotationEditing = true;
  viewport->clearSelection();  // the target is shown by the editor, not as a selection
  m_drawing = {{"plane", opad::Frame().to_json()}, {"strokes", opad::json::array()}};
  build();
  m_prompt = new PromptBar(viewport);
  m_prompt->setObjectName("annotationPrompt");
  m_prompt->setAttribute(Qt::WA_NativeWindow);
  m_badge = new Badge(viewport);
  auto* badge = new QHBoxLayout(m_badge);
  badge->setContentsMargins(8, 4, 10, 4);
  badge->setSpacing(6);
  m_badgeIcon = new QLabel(m_badge);
  m_badgeText = new QLabel(m_badge);
  badge->addWidget(m_badgeIcon);
  badge->addWidget(m_badgeText);
  m_badge->hide();
  connect(doc, &AppDocument::changed, this, &AnnotationEditor::cancel);
  connect(doc, &AppDocument::aboutToReplace, this, &AnnotationEditor::cancel);
  connect(viewport, &Viewport::filterApplied, this, [this] { refreshPrompt(); });
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] { refreshPrompt(); });  // the filters' keys
  connect(viewport, &Viewport::notesMoved, this, &AnnotationEditor::positionOverlays);  // every camera move
  qApp->installEventFilter(this);
  chooseType("note");
  if (m_drawingMode) {
    chooseColor(0);
    chooseWidth(1);
    chooseTool(false);
    refreshStrokes();
  }
  refresh();
  m_panel->setHeader(m_drawingMode ? "pen" : "annotate", m_drawingMode ? tr("Hand drawing") : tr("Note"));
  fitPanel(true);  // opens at its size
  m_panel->anchorTo(QRect(viewport->mapToGlobal(QPoint(0, 0)), viewport->size()));
  m_panel->show();
  m_panel->raise();
  m_prompt->show();
  positionOverlays();
  viewport->setFocus();
}

AnnotationEditor::~AnnotationEditor() {
  detach();
  delete m_content.data();
  delete m_prompt.data();
  delete m_badge.data();
}

// ---------------------------------------------------------------- panel
void AnnotationEditor::build() {
  const Tokens& t = theme::current();
  QWidget* host = m_panel->content();
  if (!host->layout()) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
  }
  m_content = new QWidget(host);
  m_content->setObjectName(m_drawingMode ? "annotationDrawingEditor" : "annotationNoteEditor");
  host->layout()->addWidget(m_content);
  auto* outer = new QVBoxLayout(m_content);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  m_scroll = new QScrollArea(m_content);
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setWidgetResizable(true);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_sections = new QWidget(m_scroll);
  m_sections->setAutoFillBackground(false);
  m_scroll->setWidget(m_sections);
  outer->addWidget(m_scroll, 1);
  auto* sections = new QVBoxLayout(m_sections);
  sections->setContentsMargins(0, 0, 0, 0);
  sections->setSpacing(0);
  auto block = [&](QWidget** widget = nullptr) {
    auto* w = new QWidget(m_sections);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(10, 6, 10, 8);
    layout->setSpacing(6);
    sections->addWidget(w);
    if (widget) *widget = w;
    return layout;
  };

  // Type: one chip per tag, the tag's colour as its dot; four in a row, AI agent notes under them.
  auto* type = block();
  buildHeading(type, tr("TYPE"));
  auto* tags = new QHBoxLayout();
  auto* agent = new QHBoxLayout();
  tags->setSpacing(4);
  agent->setSpacing(4);
  for (size_t i = 0; i < notes::styles().size(); ++i) {
    const auto& style = notes::styles()[i];
    const std::string id = style.id;
    auto* button = chip(m_sections, "annotationType_" + QString::fromLatin1(style.id), 26, true);
    button->setText(i18n::t(style.label));
    const QColor color = t.*style.color;
    button->setIcon(paintedIcon(QSize(10, 10), [color](QPainter& p) { dot(p, QPointF(5, 5), 8, color); }));
    button->setIconSize(QSize(10, 10));
    if (i < 4) tags->addWidget(button);
    else agent->addWidget(button, 1);
    connect(button, &QToolButton::clicked, this, [this, id] { chooseType(id); });
    m_types.push_back(button);
  }
  agent->addStretch(1);  // about half the row, as wide as two of the chips above
  type->addLayout(tags);
  type->addLayout(agent);
  m_typeHint = new QLabel(m_sections);
  m_typeHint->setObjectName("secondary");
  m_typeHint->setWordWrap(true);
  type->addWidget(m_typeHint);
  sections->addWidget(rule(m_sections));

  if (m_drawingMode) {
    auto* pen = block();
    buildHeading(pen, tr("TOOL"), &m_toolValue);
    auto* tools = new QHBoxLayout();
    tools->setSpacing(4);
    m_pen = chip(m_sections, "annotationPen", 28);
    m_pen->setText(tr("Pen"));
    m_pen->setIcon(icons::icon("pen", t.fg2));
    m_eraserButton = chip(m_sections, "annotationEraser", 28);
    m_eraserButton->setText(tr("Eraser"));
    m_eraserButton->setIcon(icons::icon("eraser", t.fg2));
    for (QToolButton* b : {m_pen, m_eraserButton}) {
      b->setIconSize(QSize(16, 16));
      tools->addWidget(b);
    }
    connect(m_pen, &QToolButton::clicked, this, [this] { chooseTool(false); });
    connect(m_eraserButton, &QToolButton::clicked, this, [this] { chooseTool(true); });
    pen->addLayout(tools);
    pen->addSpacing(4);

    buildHeading(pen, tr("COLOUR"), &m_colorValue);
    auto* colours = new QHBoxLayout();
    colours->setSpacing(4);
    for (int i = 0; i < int(notes::pens().size()); ++i) {
      const auto& p = notes::pens()[i];
      auto* button = chip(m_sections, "annotationColor_" + QString::fromLatin1(p.id), 30);
      const QColor color = p.color;
      button->setIcon(paintedIcon(QSize(24, 24), [color](QPainter& q) { dot(q, QPointF(12, 12), 16, color); },
                                  [color](QPainter& q) {  // chosen: a ring around a smaller dot
                                    q.setBrush(Qt::NoBrush);
                                    q.setPen(QPen(color, 1.6));
                                    q.drawEllipse(QPointF(12, 12), 10.2, 10.2);
                                    dot(q, QPointF(12, 12), 13, color);
                                  }));
      button->setIconSize(QSize(24, 24));
      button->setToolTip(QString("%1 · %2").arg(i18n::t(p.label)).arg(i + 1));
      button->setAccessibleName(i18n::t(p.label));
      colours->addWidget(button);
      m_colors.push_back(button);
      connect(button, &QToolButton::clicked, this, [this, i] { chooseColor(i); });
    }
    pen->addLayout(colours);
    pen->addSpacing(4);

    buildHeading(pen, tr("WIDTH"), &m_widthValue);
    auto* widths = new QHBoxLayout();
    widths->setSpacing(4);
    for (int i = 0; i < int(notes::penWidths().size()); ++i) {
      const int w = notes::penWidths()[i];
      auto* button = chip(m_sections, "annotationWidth_" + QString::number(w), 28);
      button->setText(QString::number(w));
      button->setIconSize(QSize(26, 12));
      button->setToolTip(tr("%1 px · [ ] to step").arg(w));
      button->setAccessibleName(tr("%1 pixel stroke").arg(w));
      widths->addWidget(button);
      m_widths.push_back(button);
      connect(button, &QToolButton::clicked, this, [this, i] { chooseWidth(i); });
    }
    pen->addLayout(widths);
    sections->addWidget(rule(m_sections));

    auto* list = block();
    auto* head = new QHBoxLayout();
    head->setSpacing(2);
    m_count = new QLabel(m_sections);
    m_count->setObjectName("annotationStrokeCount");
    m_count->setProperty("annotationRole", "section");
    head->addWidget(m_count, 1);
    // The editor's own keys are the platform's Undo and Redo (keyPress: QKeySequence::Undo/Redo), named its way.
    m_undoButton = flatButton(m_sections, "annotationUndo", "rollLeft", tr("Undo stroke (%1)").arg(keys::fixedText("undo")));
    m_redoButton = flatButton(m_sections, "annotationRedo", "rollRight", tr("Redo stroke (%1)").arg(keys::fixedText("redo")));
    m_clearButton = flatButton(m_sections, "annotationClear", "delete", tr("Clear strokes"));
    for (QToolButton* b : {m_undoButton, m_redoButton, m_clearButton}) head->addWidget(b);
    list->addLayout(head);
    connect(m_undoButton, &QToolButton::clicked, this, &AnnotationEditor::undo);
    connect(m_redoButton, &QToolButton::clicked, this, &AnnotationEditor::redo);
    connect(m_clearButton, &QToolButton::clicked, this, [this] {
      if (m_dragging || m_drawing["strokes"].empty()) return;
      remember(m_drawing["strokes"]);
      m_drawing["strokes"] = opad::json::array();
      refreshStrokes();
      refresh();
    });
    m_strokeScroll = new QScrollArea(m_sections);
    m_strokeScroll->setFrameShape(QFrame::NoFrame);
    m_strokeScroll->setWidgetResizable(true);
    m_strokeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_strokeList = new QWidget(m_strokeScroll);
    m_strokeList->setAutoFillBackground(false);
    auto* rows = new QVBoxLayout(m_strokeList);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    m_strokeScroll->setWidget(m_strokeList);
    list->addWidget(m_strokeScroll);
    sections->addWidget(rule(m_sections));
  }

  QWidget* noteBlock = nullptr;
  auto* note = block(&noteBlock);
  buildHeading(note, tr("NOTE"));
  m_text = new QPlainTextEdit(m_sections);
  m_text->setObjectName("annotationText");
  m_text->setProperty("annotationRole", "text");
  m_text->setTabChangesFocus(true);
  if (m_drawingMode) {
    m_text->setFixedHeight(64);
  } else {  // no natural height of its own: 72 px, and whatever a taller panel gives it
    m_text->setMinimumHeight(72);
    m_text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
  }
  note->addWidget(m_text, 1);
  if (!m_drawingMode) sections->setStretchFactor(noteBlock, 1);  // a note panel sized taller gives the room to the text
  connect(m_text, &QPlainTextEdit::textChanged, this, &AnnotationEditor::refresh);

  outer->addWidget(rule(m_content));
  m_footerBar = new QWidget(m_content);
  auto* footer = new QHBoxLayout(m_footerBar);
  footer->setContentsMargins(10, 8, 10, 8);
  footer->setSpacing(6);
  m_footer = new QLabel(m_footerBar);
  m_footer->setObjectName("tertiary");
  m_footer->setMinimumWidth(0);
  m_footer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // elides into the room the buttons leave
  footer->addWidget(m_footer, 1);
#ifdef Q_OS_MACOS
  const QString saveKeys = QString::fromUtf8("⌘↵");
#else
  const QString saveKeys = QString::fromUtf8("Ctrl+↵");
#endif
  auto* cancel = keyButton(m_footerBar, "annotationCancel", tr("Cancel"), QStringLiteral("Esc"));
  m_save = keyButton(m_footerBar, "annotationSave", tr("Save"), saveKeys);
  m_save->setProperty("annotationRole", "primary");
  footer->addWidget(cancel);
  footer->addWidget(m_save);
  outer->addWidget(m_footerBar);
  connect(cancel, &QPushButton::clicked, this, &AnnotationEditor::cancel);
  connect(m_save, &QPushButton::clicked, this, &AnnotationEditor::finish);
}

QWidget* AnnotationEditor::buildHeading(QVBoxLayout* layout, const QString& title, QLabel** value) {
  auto* row = new QWidget(m_sections);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(6);
  auto* label = new QLabel(title, row);
  label->setProperty("annotationRole", "section");
  h->addWidget(label, 1);
  if (value) {
    *value = new QLabel(row);
    (*value)->setProperty("annotationRole", "value");
    h->addWidget(*value);
  }
  layout->addWidget(row);
  return row;
}

// The panel is as tall as its content (the viewport permitting) unless the user sized it. A later fit waits a turn
// of the event loop: nested layouts take a changed size (the stroke list) from posted layout requests.
void AnnotationEditor::fitPanel(bool now) {
  if (!now) {
    if (m_fitPending) return;
    m_fitPending = true;
    QTimer::singleShot(0, this, [this] {
      m_fitPending = false;
      fitPanel(true);
    });
    return;
  }
  if (!m_active || !m_panel || !m_sections || !m_footerBar) return;
  QLayout* layout = m_sections->layout();
  layout->activate();
  const int width = std::max(1, m_panel->width() - 2 * ToolPanel::kMargin - 2);
  const int sections = layout->hasHeightForWidth() ? layout->heightForWidth(width) : layout->sizeHint().height();
  m_panel->setDefaultHeight(sections + 1 + m_footerBar->sizeHint().height());
}

// ---------------------------------------------------------------- state
QString AnnotationEditor::targetName() const {
  if (!m_anchored) return {};
  if (m_anchor.kind != opad::Ref::Kind::Body)
    return QString("%1 %2").arg(i18n::t(opad::Ref::kind_name(m_anchor.kind))).arg(m_anchor.index);
  return QFontMetrics(theme::ui(12)).elidedText(m_doc->nodeName(m_anchor.body), Qt::ElideMiddle, 180);
}

void AnnotationEditor::chooseType(const std::string& type) {
  m_type = notes::style(type).id;
  for (size_t i = 0; i < m_types.size(); ++i) m_types[i]->setChecked(m_type == notes::styles()[i].id);
  m_typeHint->setText(m_type == "ok"        ? tr("Confirms that this part of the design is right.")
                      : m_type == "warning" ? tr("Something to check before it becomes a problem.")
                      : m_type == "issue"   ? tr("A problem that needs a change.")
                      : m_type == "ai_agent" ? tr("Note written for AI agents that read this design.")
                                             : tr("General note pinned to the anchor point."));
  refresh();
}

void AnnotationEditor::chooseColor(int index) {
  m_colorIndex = std::clamp(index, 0, int(notes::pens().size()) - 1);
  for (int i = 0; i < int(m_colors.size()); ++i) m_colors[i]->setChecked(i == m_colorIndex);
  refreshPens();
}

void AnnotationEditor::chooseWidth(int index) {
  m_widthIndex = std::clamp(index, 0, int(notes::penWidths().size()) - 1);
  for (int i = 0; i < int(m_widths.size()); ++i) m_widths[i]->setChecked(i == m_widthIndex);
  refreshPens();
}

void AnnotationEditor::chooseTool(bool eraser) {
  m_eraser = eraser;
  m_pen->setChecked(!eraser);
  m_eraserButton->setChecked(eraser);
  if (m_anchored) m_viewport->setCursor(eraser ? eraserCursor(m_viewport->devicePixelRatioF()) : QCursor(Qt::CrossCursor));
  refreshPens();
  refreshPrompt();
}

// The heads' read-outs and the width samples, drawn in the chosen colour.
void AnnotationEditor::refreshPens() {
  if (!m_drawingMode || !m_widthValue) return;
  const auto& pen = notes::pens()[m_colorIndex];
  m_toolValue->setText(m_eraser ? tr("Eraser · E") : tr("Pen · B"));
  m_colorValue->setText(QString("%1 · %2").arg(i18n::t(pen.label)).arg(m_colorIndex + 1));
  m_widthValue->setText(tr("%1 px  [ ]").arg(notes::penWidths()[m_widthIndex]));
  const QColor color = pen.color;
  for (size_t i = 0; i < m_widths.size(); ++i) {
    const double w = notes::penWidths()[i];
    m_widths[i]->setIcon(paintedIcon(QSize(26, 12), [w, color](QPainter& p) { sample(p, QPointF(6, 6), QPointF(20, 6), w, color); }));
  }
}

void AnnotationEditor::remember(opad::json before) {
  m_undo.push_back(std::move(before));
  if (m_undo.size() > kUndoDepth) m_undo.erase(m_undo.begin());
  m_redo.clear();
}

void AnnotationEditor::undo() {
  if (!m_active || !m_drawingMode || m_dragging || m_undo.empty()) return;
  m_redo.push_back(m_drawing["strokes"]);
  m_drawing["strokes"] = std::move(m_undo.back());
  m_undo.pop_back();
  refreshStrokes();
  refresh();
}

void AnnotationEditor::redo() {
  if (!m_active || !m_drawingMode || m_dragging || m_redo.empty()) return;
  m_undo.push_back(m_drawing["strokes"]);
  m_drawing["strokes"] = std::move(m_redo.back());
  m_redo.pop_back();
  refreshStrokes();
  refresh();
}

void AnnotationEditor::refresh() {
  if (!m_active || !m_save) return;
  const bool text = !m_text->toPlainText().trimmed().isEmpty();
  const bool strokes = m_drawingMode && !m_drawing["strokes"].empty();
  const bool needsText = !m_drawingMode || m_type == "ai_agent";  // an agent needs the request in words
  m_save->setEnabled(m_anchored && (!m_drawingMode || strokes) && (!needsText || text));
  const QString name = targetName();
  m_footer->setText(!m_anchored                ? tr("Anchor required")
                    : m_drawingMode && !strokes ? tr("Draw a stroke to save")
                    : needsText && !text        ? (m_type == "ai_agent" ? tr("Describe the request to save") : tr("Write the note to save"))
                                                : tr("Pinned to %1").arg(name));
  m_panel->setContext(!m_anchored ? tr("no anchor yet") : m_drawingMode ? tr("%1 · camera plane").arg(name) : name);
  m_text->setPlaceholderText(!m_anchored ? tr("Pick an anchor point first, or start typing")
                             : m_type == "ai_agent" ? tr("Describe what the agent should do...")
                                                    : tr("Write a note..."));
  if (m_anchored) {
    m_badgeIcon->setPixmap(icons::pixmap(m_drawingMode ? "plane" : "pin", theme::current().onsel, 16, m_badge->devicePixelRatioF()));
    m_badgeText->setText(m_drawingMode ? tr("Plane · %1 · camera-facing").arg(name) : tr("Note · %1").arg(name));
    m_badge->adjustSize();
  }
  refreshPrompt();
}

void AnnotationEditor::refreshPrompt() {
  if (!m_active || !m_prompt) return;
  const auto filter = m_viewport->selectionFilter();
  const QString select = filter == Viewport::SelFilter::Body   ? tr("Select a body")
                         : filter == Viewport::SelFilter::Face ? tr("Select a face")
                         : filter == Viewport::SelFilter::Edge ? tr("Select an edge")
                                                               : tr("Select a vertex");
  const QString next = !m_drawingMode ? tr("Write the note") : m_eraser ? tr("Erase strokes") : tr("Draw strokes");
  const QString filters = keys::span({"select.bodies", "select.faces", "select.edges", "select.vertices"}), cancel = tr("%1 cancel").arg(keys::fixedText("esc"));
  const QString hints = !m_anchored    ? (filters.isEmpty() ? cancel : cancel + QStringLiteral(" · ") + tr("%1 change filter").arg(filters))
                        : m_drawingMode ? tr("Orbit for a new plane · B pen · E eraser · 1–4 colour · [ ] width")
                                        : tr("Click another object to move the note · Esc cancel");
  m_prompt->set(m_drawingMode ? "pen" : "annotate", m_drawingMode ? tr("Hand drawing") : tr("Note"),
                {{select, m_anchored ? targetName() : QString()}, {next, QString()}}, hints);
  positionOverlays();
}

void AnnotationEditor::refreshStrokes() {
  if (!m_active || !m_drawingMode) return;
  const auto& strokes = m_drawing["strokes"];
  const qreal dpr = m_viewport->devicePixelRatioF();
  m_points = 0;
  for (const auto& s : strokes) m_points += s["points"].size();
  m_count->setText(tr("STROKES · %1").arg(strokes.size()));
  m_undoButton->setEnabled(!m_undo.empty());
  m_redoButton->setEnabled(!m_redo.empty());
  m_clearButton->setEnabled(!strokes.empty());
  auto* rows = static_cast<QVBoxLayout*>(m_strokeList->layout());
  while (QLayoutItem* item = rows->takeAt(0)) {
    if (QWidget* w = item->widget()) {  // a row's own button may be the one being clicked: later
      w->hide();
      w->deleteLater();
    }
    delete item;
  }
  if (strokes.empty()) {
    auto* empty = new QLabel(tr("Drag on the model to draw. Orbit between strokes to draw on another plane."), m_strokeList);
    empty->setObjectName("tertiary");
    empty->setWordWrap(true);
    rows->addWidget(empty);
  }
  for (size_t i = 0; i < strokes.size(); ++i) {
    const auto& stroke = strokes[i];
    const auto& points = stroke["points"];
    double length = 0;
    for (size_t p = 1; p < points.size(); ++p)
      length += std::hypot(points[p][0].get<double>() - points[p - 1][0].get<double>(), points[p][1].get<double>() - points[p - 1][1].get<double>());
    const std::string color = stroke.value("color", std::string("blue"));
    const int width = stroke.value("width", 2);
    const notes::Pen* pen = &notes::pens()[2];
    for (const auto& p : notes::pens())
      if (color == p.id) pen = &p;
    auto* row = new QWidget(m_strokeList);
    row->setFixedHeight(kStrokeRow);
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(8);
    auto* index = new QLabel(QString::number(i + 1), row);
    index->setProperty("annotationRole", "index");
    index->setFixedWidth(16);
    auto* mark = new QLabel(row);
    const QColor tint = pen->color;
    mark->setPixmap(painted(QSize(18, 10), dpr, [tint, width](QPainter& p) { sample(p, QPointF(3, 5), QPointF(15, 5), std::min(width, 4), tint); }));
    auto* label = new QLabel(tr("Pen · %1 · %2 px").arg(i18n::t(pen->label)).arg(width), row);
    auto* size = new QLabel(units::format(units::Kind::Length, length, units::toDisplay(units::Kind::Length, length) < 10 ? 1 : 0), row);
    size->setProperty("annotationRole", "value");
    auto* remove = flatButton(row, "annotationRemoveStroke", "close", tr("Delete stroke %1").arg(i + 1), 20);
    remove->setProperty("strokeIndex", int(i));
    line->addWidget(index);
    line->addWidget(mark);
    line->addWidget(label, 1);
    line->addWidget(size);
    line->addWidget(remove);
    connect(remove, &QToolButton::clicked, this, [this, i] {
      if (m_dragging || i >= m_drawing["strokes"].size()) return;
      remember(m_drawing["strokes"]);
      m_drawing["strokes"].erase(i);
      refreshStrokes();
      refresh();
    });
    rows->addWidget(row);
  }
  const int shown = std::min<int>(int(strokes.size()), kVisibleRows);
  m_strokeScroll->setFixedHeight(strokes.empty() ? 34 : shown * kStrokeRow + (shown - 1) * rows->spacing());
  QTimer::singleShot(0, m_strokeScroll, [scroll = m_strokeScroll] { scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); });
  m_viewport->previewAnnotationDrawing(m_drawing);
  fitPanel();
}

void AnnotationEditor::positionOverlays() {
  if (!m_active || !m_viewport || !m_prompt) return;
  m_prompt->move(std::max(8, (m_viewport->width() - m_prompt->width()) / 2), 44);
  m_prompt->raise();
  const QRect rect = m_viewport->annotationTargetRect();
  if (m_anchored && !rect.isNull() && m_viewport->rect().intersects(rect)) {
    m_badge->adjustSize();
    const int x = std::clamp(rect.center().x() - m_badge->width() / 2, 8, std::max(8, m_viewport->width() - m_badge->width() - 8));
    const int y = std::clamp(rect.top() - m_badge->height() - 10, 84, std::max(84, m_viewport->height() - m_badge->height() - 8));
    m_badge->move(x, y);
    m_badge->show();
    m_badge->raise();
  } else {
    m_badge->hide();
  }
}

// ---------------------------------------------------------------- target
void AnnotationEditor::adoptSelection(const std::vector<opad::Ref>& selection) {
  if (m_anchored || selection.size() != 1) return;
  const opad::Ref& ref = selection.front();
  if (ref.kind == opad::Ref::Kind::Point || ref.kind == opad::Ref::Kind::Center) return;
  anchorTo(ref, false);
}

bool AnnotationEditor::anchorTo(opad::Ref target, bool hasPoint) {
  opad::Vec3 centre{0, 0, 0};
  if (!m_doc->scene.node(target.body) || !m_viewport->showAnnotationTarget(target, &centre)) return false;
  if (!hasPoint) target.point = centre;
  // Drawing: the left button draws from now on, so the bodies stop being picked; the view cube stays live.
  if (!m_anchored && m_drawingMode) m_viewport->setBodiesPickable(false);
  m_anchor = target;
  m_anchored = true;
  m_drawing["plane"] = m_viewport->annotationCameraPlane(m_anchor.point).to_json();
  if (m_drawingMode) {
    m_viewport->setCursor(m_eraser ? eraserCursor(m_viewport->devicePixelRatioF()) : QCursor(Qt::CrossCursor));
  } else {  // what comes next is typing: into the note, not into the window's one-key shortcuts
    m_panel->activateWindow();
    m_text->setFocus();
  }
  refresh();
  return true;
}

// ---------------------------------------------------------------- drawing
void AnnotationEditor::addPoint(const QPointF& point) {
  if (m_points >= kMaxPoints) return;
  auto& points = m_drawing["strokes"].back()["points"];
  if (!points.empty() && (point - m_lastPoint).manhattanLength() < 2) return;
  double u, v;
  if (!m_viewport->planePoint(point, m_frame, u, v)) return;
  points.push_back({round4(u), round4(v)});
  ++m_points;
  m_lastPoint = point;
  m_viewport->previewAnnotationDrawing(m_drawing);
}

// Removes the stroke nearest the pointer, within 10 px on screen.
void AnnotationEditor::eraseAt(const QPointF& point) {
  auto& strokes = m_drawing["strokes"];
  double best = 100;
  size_t hit = strokes.size();
  for (size_t i = 0; i < strokes.size(); ++i) {
    const auto frame = opad::Frame::from_json(strokes[i].value("plane", m_drawing["plane"]));
    const auto& points = strokes[i]["points"];
    QPointF previous;
    for (size_t p = 0; p < points.size(); ++p) {
      const QPointF at = m_viewport->widgetPoint(frame.to_world(points[p][0].get<double>(), points[p][1].get<double>()));
      if (p > 0) {
        const QPointF ab = at - previous;
        const double n = QPointF::dotProduct(ab, ab);
        const double s = n > 0 ? std::clamp(QPointF::dotProduct(point - previous, ab) / n, 0.0, 1.0) : 0.0;
        const QPointF d = point - (previous + ab * s);
        if (const double distance = QPointF::dotProduct(d, d); distance < best) {
          best = distance;
          hit = i;
        }
      }
      previous = at;
    }
  }
  if (hit == strokes.size()) return;
  strokes.erase(hit);
  m_erased = true;
  refreshStrokes();
  refresh();
}

void AnnotationEditor::finish() {
  if (!m_active || m_dragging || !m_save->isEnabled()) return;
  const QString text = m_text->toPlainText().trimmed();
  // Drawn on a part an exploded view has moved: stored where the part is in the model (the view adds its offset back).
  const opad::Vec3 moved = m_anchor.body.empty() ? opad::Vec3{0, 0, 0} : m_viewport->shownOffset(m_anchor.body);
  auto back = [&moved](opad::Vec3 p) { return opad::Vec3{p[0] - moved[0], p[1] - moved[1], p[2] - moved[2]}; };
  opad::Ref anchor = m_anchor;
  opad::json drawing = m_drawing;
  if (moved != opad::Vec3{0, 0, 0}) {
    anchor.point = back(anchor.point);
    if (m_drawingMode && drawing.is_object()) {
      if (drawing.contains("plane") && drawing["plane"].contains("origin")) drawing["plane"]["origin"] = back(drawing["plane"]["origin"].get<opad::Vec3>());
      if (drawing.contains("strokes"))
        for (auto& stroke : drawing["strokes"])
          if (stroke.contains("plane") && stroke["plane"].contains("origin")) stroke["plane"]["origin"] = back(stroke["plane"]["origin"].get<opad::Vec3>());
    }
  }
  opad::json args = {{"anchor", anchor.to_json()}, {"text", (text.isEmpty() ? tr("Hand drawing") : text).toStdString()}, {"style", m_type}};
  if (m_drawingMode) args["drawing"] = drawing;
  try {
    m_doc->run("annotate", args);  // one op, one Undo step; the document's change ends this editor
    cancel();
  } catch (const std::exception& e) {
    m_footer->setText(i18n::t(e.what()));
  }
}

void AnnotationEditor::cancel() {
  if (!m_active) return;
  detach();
  deleteLater();
}

void AnnotationEditor::detach() {
  if (!m_active) return;
  m_active = false;
  m_dragging = false;
  qApp->removeEventFilter(this);
  if (m_doc) m_doc->annotationEditing = false;
  if (m_viewport) {
    m_viewport->previewAnnotationDrawing(nullptr);
    m_viewport->clearAnnotationTarget();
    m_viewport->setBodiesPickable(true);
    m_viewport->unsetCursor();
  }
  if (m_prompt) m_prompt->hide();
  if (m_badge) m_badge->hide();
  if (m_content) m_content->hide();  // deleted with the editor; a following editor fills the same panel
  if (m_panel) m_panel->hide();
}

// ---------------------------------------------------------------- input
// The view's left button while the editor runs: a click picks the target (a note can be moved by picking again),
// a drag draws. The view cube keeps its clicks and drags, and the other buttons navigate, so every stroke can be
// drawn from another side. The keys: Esc, Ctrl+Enter, and the pen keys once a drawing has its target.
bool AnnotationEditor::eventFilter(QObject* object, QEvent* event) {
  if (!m_active || !m_viewport || !m_content) return false;
  switch (event->type()) {
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress: return key(object, event);
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: return object == m_viewport && mousePress(event);  // quick strokes arrive as double clicks
    case QEvent::MouseMove: return object == m_viewport && mouseMove(event);
    case QEvent::MouseButtonRelease: return object == m_viewport && mouseRelease(event);
    case QEvent::Wheel: return object == m_viewport && m_dragging;
    case QEvent::Resize:
      if (object == m_viewport) positionOverlays();
      return false;
    default: return false;
  }
}

bool AnnotationEditor::key(QObject* object, QEvent* event) {
  auto* widget = qobject_cast<QWidget*>(object);
  if (!widget || !m_panel) return false;
  if (widget != m_viewport && widget != m_panel && !m_panel->isAncestorOf(widget)) return false;
  auto* e = static_cast<QKeyEvent*>(event);
  const int k = e->key();
  const auto modifiers = e->modifiers() & ~Qt::KeypadModifier;
  const bool typing = widget == m_text || m_text->isAncestorOf(widget);
  const bool save = (modifiers & Qt::ControlModifier) && (k == Qt::Key_Return || k == Qt::Key_Enter);
  const bool history = e->matches(QKeySequence::Undo) || e->matches(QKeySequence::Redo);
  const bool penKey = (modifiers == Qt::NoModifier && (k == Qt::Key_B || k == Qt::Key_E || (k >= Qt::Key_1 && k <= Qt::Key_4)))
                      || k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight;
  const bool drawingKey = m_drawingMode && m_anchored && !typing && (penKey || history);
  if (k != Qt::Key_Escape && !save && !drawingKey) return false;
  if (event->type() == QEvent::ShortcutOverride) {  // ours, not the window's one-key shortcuts
    event->accept();
    return true;
  }
  if (k == Qt::Key_Escape) cancel();
  else if (save) finish();
  else if (e->matches(QKeySequence::Undo)) undo();
  else if (e->matches(QKeySequence::Redo)) redo();
  else if (k == Qt::Key_B) chooseTool(false);
  else if (k == Qt::Key_E) chooseTool(true);
  else if (k >= Qt::Key_1 && k <= Qt::Key_4) chooseColor(k - Qt::Key_1);
  else chooseWidth(m_widthIndex + (k == Qt::Key_BracketRight ? 1 : -1));
  return true;
}

bool AnnotationEditor::mousePress(QEvent* event) {
  auto* e = static_cast<QMouseEvent*>(event);
  if (e->button() != Qt::LeftButton) return m_dragging;  // navigation, except in the middle of a stroke
  if (m_dragging || e->buttons() != Qt::LeftButton) return true;
  m_cubePress = m_viewport->cubeAt(e->position());
  if (m_cubePress) return false;  // another view from the cube: the next stroke gets another plane
  m_viewport->setFocus();
  if (!m_anchored || !m_drawingMode) {
    m_picking = true;
    m_pickPress = e->position();
    return true;
  }
  m_before = m_drawing["strokes"];
  if (m_eraser) {
    m_erased = false;
    m_dragging = true;
    eraseAt(e->position());
    return true;
  }
  if (m_drawing["strokes"].size() >= kMaxStrokes || m_points >= kMaxPoints) {
    m_footer->setText(tr("Drawing limit reached: save it and start another"));
    return true;
  }
  m_frame = m_viewport->annotationCameraPlane(m_anchor.point);  // this stroke's plane: through the target, facing the camera now
  m_drawing["strokes"].push_back({{"plane", m_frame.to_json()},
                                  {"color", notes::pens()[m_colorIndex].id},
                                  {"width", notes::penWidths()[m_widthIndex]},
                                  {"points", opad::json::array()}});
  m_dragging = true;
  addPoint(e->position());
  return true;
}

bool AnnotationEditor::mouseMove(QEvent* event) {
  auto* e = static_cast<QMouseEvent*>(event);
  if (m_cubePress) return false;
  if (m_dragging) {
    if (m_eraser) eraseAt(e->position());
    else addPoint(e->position());
    return true;
  }
  return m_picking;  // otherwise hover: the object under the mouse while picking, the cube while drawing
}

bool AnnotationEditor::mouseRelease(QEvent* event) {
  auto* e = static_cast<QMouseEvent*>(event);
  if (e->button() != Qt::LeftButton) return m_dragging;
  if (m_cubePress) {
    m_cubePress = false;
    return false;
  }
  if (m_picking) {
    m_picking = false;
    if ((e->position() - m_pickPress).manhattanLength() > 5) return true;  // a drag is not a pick
    opad::Ref target;
    bool hit = false;
    if (m_viewport->annotationPick(e->position(), target, hit) && anchorTo(target, hit)) return true;
    if (!m_anchored) m_footer->setText(tr("Nothing here: pick a body, face, edge or vertex"));
    return true;
  }
  if (!m_dragging) return m_anchored && m_drawingMode;  // its press was ours too (at the stroke limit)
  m_dragging = false;
  bool changed = m_erased;
  if (!m_eraser) {
    addPoint(e->position());
    auto& strokes = m_drawing["strokes"];
    changed = strokes.back()["points"].size() >= 2;
    if (!changed) strokes.erase(strokes.size() - 1);  // a click is not a stroke
  }
  if (changed) remember(std::move(m_before));
  m_before = nullptr;
  refreshStrokes();
  refresh();
  return true;
}
