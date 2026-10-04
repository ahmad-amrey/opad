#include "GuidedTool.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QToolButton>
#include <QSignalBlocker>
#include <QTextLayout>
#include <QScrollBar>
#include <QStyle>
#include <algorithm>

#include "Icons.hpp"
#include "Theme.hpp"

QPixmap stepRing(StepState state, int number, qreal dpr) {
  const Tokens& t = theme::current();
  QPixmap pm(QSize(16, 16) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor ring = state == StepState::Pending ? t.fg3 : t.sel;
  p.setPen(QPen(ring, 1.5));
  p.setBrush(state == StepState::Done ? QBrush(t.sel) : QBrush(Qt::NoBrush));
  p.drawEllipse(QRectF(1, 1, 14, 14));
  if (state == StepState::Done) {
    p.setPen(QPen(t.onsel, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF({QPointF(4.6, 8.2), QPointF(7.1, 10.6), QPointF(11.4, 5.6)}));
  } else {
    p.setFont(theme::mono(10));
    p.setPen(ring);
    p.drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, QString::number(number));
  }
  return pm;
}

// ---------------------------------------------------------------- PromptBar
PromptBar::PromptBar(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setFixedHeight(32);
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void PromptBar::set(const QString& icon, const QString& title, const QList<ToolStep>& steps, const QString& hints) {
  m_icon = icon;
  m_title = title;
  m_steps = steps;
  m_hints = hints;
  resize(sizeHint());
  update();
}

QList<PromptBar::Piece> PromptBar::pieces() const {
  QFontMetrics title(theme::ui(13, QFont::Medium)), text(theme::ui(13)), hint(theme::mono(11));
  QList<Piece> out;
  out << Piece{Piece::Icon, {}, {}, 0, 16} << Piece{Piece::Title, m_title, {}, 0, title.horizontalAdvance(m_title)} << Piece{Piece::Rule, {}, {}, 0, 1};
  bool waitingSeen = false;
  for (int i = 0; i < m_steps.size(); ++i) {
    const ToolStep& s = m_steps[i];
    StepState state = !s.picked.isEmpty() ? StepState::Done : waitingSeen ? StepState::Pending : StepState::Waiting;
    if (state == StepState::Waiting) waitingSeen = true;
    if (i > 0) out << Piece{Piece::Arrow, QString::fromUtf8("›"), {}, 0, text.horizontalAdvance(QString::fromUtf8("›"))};
    const QString label = s.picked.isEmpty() ? s.label : s.picked;
    out << Piece{Piece::Ring, {}, state, i + 1, 16} << Piece{Piece::Text, label, state, 0, text.horizontalAdvance(label)};
  }
  out << Piece{Piece::Rule, {}, {}, 0, 1} << Piece{Piece::Hint, m_hints, {}, 0, hint.horizontalAdvance(m_hints)};
  return out;
}

QSize PromptBar::sizeHint() const {
  int w = 10;
  for (const Piece& p : pieces()) w += p.width + 8;
  return QSize(w + 2, 32);
}

void PromptBar::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setLayoutDirection(Qt::LeftToRight);  // absolute positions, mirrored by hand
  p.fillRect(rect(), t.vp);               // an overlay on the native viewport cannot be translucent
  p.setPen(QPen(t.sel, 1));
  p.setBrush(t.bg2);
  p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 3, 3);
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  QFontMetrics fm(theme::ui(13));
  const int baseline = (height() + fm.ascent() - fm.descent()) / 2;
  int x = 10;
  for (const Piece& piece : pieces()) {
    const int at = rtl ? width() - x - piece.width : x;
    switch (piece.kind) {
      case Piece::Icon: p.drawPixmap(at, 8, icons::pixmap(m_icon, t.sel, 16, devicePixelRatioF())); break;
      case Piece::Rule: p.setPen(QPen(t.line, 1)); p.drawLine(at, 8, at, height() - 8); break;
      case Piece::Ring: p.drawPixmap(at, 8, stepRing(piece.state, piece.number, devicePixelRatioF())); break;
      case Piece::Title:
        p.setFont(theme::ui(13, QFont::Medium));
        p.setPen(t.fg);
        p.drawText(at, baseline, piece.text);
        break;
      case Piece::Text:
        p.setFont(theme::ui(13));
        p.setPen(piece.state == StepState::Waiting ? t.fg : piece.state == StepState::Done ? t.fg2 : t.fg3);
        p.drawText(at, baseline, piece.text);
        break;
      case Piece::Arrow:
        p.setFont(theme::ui(13));
        p.setPen(t.fg3);
        p.drawText(at, baseline, rtl ? QString::fromUtf8("‹") : piece.text);
        break;
      case Piece::Hint:
        p.setFont(theme::mono(11));
        p.setPen(t.fg3);
        p.drawText(at, baseline, piece.text);
        break;
    }
    x += piece.width + 8;
  }
}

// ---------------------------------------------------------------- ToolStepsPanel
namespace {
// Use the full row width for a selected name. Bound wrapping to two lines, retain
// the entity suffix when eliding, and expose the complete plain-text name on hover.
class SelectionNameLabel : public QLabel {
 public:
  explicit SelectionNameLabel(const QString& name, QWidget* parent) : QLabel(name, parent) {
    setTextFormat(Qt::PlainText);
    setToolTip("<qt>" + name.toHtmlEscaped() + "</qt>");
    setAccessibleName(name);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setMinimumWidth(0);
  }
  QSize sizeHint() const override { return QSize(380, heightForWidth(380)); }
  QSize minimumSizeHint() const override { return QSize(0, fontMetrics().height()); }
  int heightForWidth(int width) const override {
    return lines(width).size() * fontMetrics().height();
  }
 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::WindowText));
    int y = 0;
    for (const QString& line : lines(width())) {
      painter.drawText(QRect(0, y, width(), fontMetrics().height()), Qt::AlignLeading | Qt::AlignVCenter | Qt::TextSingleLine, line);
      y += fontMetrics().height();
    }
  }
 private:
  QStringList lines(int width) const {
    width = std::max(1, width);
    const QString name = text().simplified();  // embedded newlines must not defeat the height cap
    QTextLayout layout(name, font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine first = layout.createLine();
    if (!first.isValid()) { layout.endLayout(); return {QString()}; }
    first.setLineWidth(width);
    const int split = first.textLength();
    layout.endLayout();
    if (split >= name.size()) return {name};
    return {name.left(split).trimmed(), fontMetrics().elidedText(name.mid(split).trimmed(), Qt::ElideMiddle, width)};
  }
};
}  // namespace

ToolStepsPanel::ToolStepsPanel(QWidget* parent) : QWidget(parent) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  m_scroll = new QScrollArea(this);
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setWidgetResizable(true);
  m_scroll->setMinimumSize(0, 0);
  m_scroll->viewport()->installEventFilter(this);
  m_body = new QWidget(m_scroll);
  m_scroll->setWidget(m_body);
  m_body->setAutoFillBackground(false);
  outer->addWidget(m_scroll, 1);
  auto* layout = new QVBoxLayout(m_body);
  layout->setContentsMargins(0, 8, 0, 0);
  layout->setSpacing(0);
  auto* steps = new QWidget(this);
  m_stepRows = new QVBoxLayout(steps);
  m_stepRows->setContentsMargins(8, 0, 8, 8);
  m_stepRows->setSpacing(2);
  layout->addWidget(steps);
  auto* rule = new QFrame(this);
  rule->setFixedHeight(1);
  rule->setObjectName("toolRule");
  layout->addWidget(rule);

  auto* summary = new QWidget(this);
  auto* sl = new QVBoxLayout(summary);
  sl->setContentsMargins(12, 8, 12, 8);
  sl->setSpacing(2);
  auto* head = new QHBoxLayout();
  m_title = new QLabel(summary);
  m_title->setObjectName("panelTitle");
  m_state = new QLabel(summary);
  m_state->setObjectName("tertiary");
  m_state->setFont(theme::mono(11));
  head->addWidget(m_title, 1);
  head->addWidget(m_state);
  sl->addLayout(head);
  m_subtitle = new QLabel(summary);
  m_subtitle->setObjectName("secondary");
  m_subtitle->setWordWrap(true);
  sl->addWidget(m_subtitle);
  layout->addWidget(summary);
  m_error = new QLabel(this);
  m_error->setObjectName("toolError");
  m_error->setWordWrap(true);
  m_error->setTextFormat(Qt::PlainText);
  m_error->setContentsMargins(12, 0, 12, 8);
  m_error->hide();
  layout->addWidget(m_error);

  m_anchorRow = new QWidget(this);
  auto* anchorLayout = new QHBoxLayout(m_anchorRow);
  anchorLayout->setContentsMargins(12, 4, 12, 4);
  anchorLayout->setSpacing(8);
  auto* anchorLabel = new QLabel(tr("Anchors"), m_anchorRow);
  anchorLabel->setObjectName("secondary");
  m_anchors = new QComboBox(m_anchorRow);
  m_anchors->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  m_anchors->setToolTip(tr("Choose which points on the selected edges define the measurement."));
  anchorLayout->addWidget(anchorLabel);
  anchorLayout->addWidget(m_anchors, 1);
  layout->addWidget(m_anchorRow);
  m_anchorRow->hide();
  connect(m_anchors, qOverload<int>(&QComboBox::currentIndexChanged), this, &ToolStepsPanel::anchorChanged);

  m_modeRow = new QWidget(this);
  m_modeRow->setObjectName("toolModes");
  auto* modeLayout = new QHBoxLayout(m_modeRow);
  modeLayout->setContentsMargins(12, 4, 12, 4);
  modeLayout->setSpacing(4);
  m_modes = new QButtonGroup(this);
  m_modes->setExclusive(true);
  connect(m_modes, &QButtonGroup::idClicked, this, &ToolStepsPanel::modeChanged);
  layout->addWidget(m_modeRow);
  m_modeRow->hide();

  m_frameRow = new QWidget(this);
  auto* frameLayout = new QHBoxLayout(m_frameRow);
  frameLayout->setContentsMargins(12, 4, 12, 4);
  frameLayout->setSpacing(8);
  auto* frameLabel = new QLabel(tr("Coordinates"), m_frameRow);
  frameLabel->setObjectName("secondary");
  m_frames = new QComboBox(m_frameRow);
  m_frames->setObjectName("toolFrame");
  m_frames->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  m_frames->setToolTip(tr("Give the measured points and Δ in world axes, or in the axes of the component the first pick lies in."));
  frameLayout->addWidget(frameLabel);
  frameLayout->addWidget(m_frames, 1);
  layout->addWidget(m_frameRow);
  m_frameRow->hide();
  connect(m_frames, qOverload<int>(&QComboBox::currentIndexChanged), this, &ToolStepsPanel::frameChanged);

  m_components = new QCheckBox(tr("Show ΔX, ΔY, ΔZ arrows"), this);
  m_components->setChecked(true);
  m_components->setToolTip(tr("Signed components from point 1 to point 2 along the X, Y and Z axes: the world's, or the component's when its coordinates are chosen. Red X, green Y, blue Z."));
  m_components->hide();
  auto* componentRow = new QHBoxLayout();
  componentRow->setContentsMargins(12, 4, 12, 4);
  componentRow->addWidget(m_components);
  layout->addLayout(componentRow);
  connect(m_components, &QCheckBox::toggled, this, &ToolStepsPanel::componentsChanged);

  m_grid = new QTreeWidget(this);
  m_grid->setColumnCount(2);
  m_grid->setHeaderHidden(true);
  m_grid->header()->setSectionResizeMode(0, QHeaderView::Fixed);
  m_grid->header()->setSectionResizeMode(1, QHeaderView::Fixed);
  m_grid->header()->setStretchLastSection(false);
  m_grid->setTextElideMode(Qt::ElideNone);
  m_grid->setColumnWidth(0, 128);
  m_grid->setIndentation(0);
  m_grid->setRootIsDecorated(false);
  m_grid->setSelectionMode(QAbstractItemView::NoSelection);
  m_grid->setFocusPolicy(Qt::NoFocus);
  auto* gridRow = new QHBoxLayout();  // the grid lines stop 12 px short of the panel's edges, like the summary text
  gridRow->setContentsMargins(12, 0, 12, 0);
  gridRow->addWidget(m_grid);
  layout->addLayout(gridRow);

  // Earlier results of the session (UI-144): title, value, Copy and Pin.
  m_historyBox = new QWidget(this);
  auto* historyLayout = new QVBoxLayout(m_historyBox);
  historyLayout->setContentsMargins(12, 10, 12, 4);
  historyLayout->setSpacing(4);
  auto* historyTitle = new QLabel(tr("Earlier results"), m_historyBox);
  historyTitle->setObjectName("secondary");
  historyLayout->addWidget(historyTitle);
  m_history = new QTreeWidget(m_historyBox);
  m_history->setObjectName("toolHistory");
  m_history->setColumnCount(3);
  m_history->setHeaderHidden(true);
  m_history->setIndentation(0);
  m_history->setRootIsDecorated(false);
  m_history->setSelectionMode(QAbstractItemView::NoSelection);
  m_history->setFocusPolicy(Qt::NoFocus);
  m_history->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_history->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_history->header()->setStretchLastSection(false);
  m_history->header()->setSectionResizeMode(0, QHeaderView::Fixed);
  m_history->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_history->header()->setSectionResizeMode(2, QHeaderView::Fixed);
  m_history->setColumnWidth(2, 60);
  historyLayout->addWidget(m_history);
  layout->addWidget(m_historyBox);
  m_historyBox->hide();
  layout->addStretch(1);

  // Copy on the leading side; Clear (Esc: measure again) and Pin to document (P), which keeps the panel open.
  m_footer = new PanelFooter(this);
  m_copy = m_footer->addSecondary(tr("Copy"));
  m_copy->setToolTip(tr("Copy measurement values"));
  m_footer->setCancel(tr("Clear"));
  m_footer->setPrimary(tr("Pin to document"), QStringLiteral("P"));
  m_footer->setKeysStayWithWindow(true);  // Esc / P / Enter stay with the main window
  outer->addWidget(m_footer);
  m_footer->hide();
  connect(m_footer, &PanelFooter::cancelled, this, &ToolStepsPanel::clearRequested);
  connect(m_footer, &PanelFooter::accepted, this, &ToolStepsPanel::pinRequested);
  connect(m_copy, &QPushButton::clicked, this, [this] {
    QStringList lines;
    for (int i = 0; i < m_grid->topLevelItemCount(); ++i) {
      const auto* row = m_grid->topLevelItem(i);
      QString value = row->text(1);
      value.remove(QChar(0x202A)); value.remove(QChar(0x202C));
      lines << row->text(0) + "\t" + value;
    }
    QApplication::clipboard()->setText(lines.join('\n'));
  });

  auto restyle = [this, rule] {
    const Tokens& t = theme::current();
    rule->setStyleSheet(QString("background: %1;").arg(theme::css(t.line)));
    m_error->setStyleSheet(QString("color: %1;").arg(theme::css(t.error)));
    m_modeRow->setStyleSheet(QString("QPushButton { border: 1px solid %1; border-radius: 3px; padding: 3px 8px; background: %2; color: %3; }"
                                     "QPushButton:hover { background: %4; } QPushButton:checked { background: %5; border-color: %6; color: %7; }")
                                 .arg(theme::css(t.line), theme::css(t.bg2), theme::css(t.fg2), theme::css(t.bg3), theme::css(t.selbg), theme::css(t.sel), theme::css(t.fg)));
    m_history->setStyleSheet(QString("QTreeWidget::item { border-bottom: 1px solid %1; }").arg(theme::css(t.line)));
    m_grid->setStyleSheet(QString("QTreeWidget::item { border-bottom: 1px solid %1; }").arg(theme::css(t.line)));
    QList<QPair<QString, QString>> rows;
    for (int i = 0; i < m_grid->topLevelItemCount(); ++i) {
      auto* row = m_grid->topLevelItem(i);
      QString value = row->text(1);
      value.remove(QChar(0x202A)); value.remove(QChar(0x202C));
      rows << qMakePair(row->text(0), value);
    }
    setResult(rows);
    if (!m_historyRows.isEmpty()) setHistory(QList<ToolHistoryRow>(m_historyRows), m_historyPin);
  };
  restyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, restyle);
}

void ToolStepsPanel::setSteps(const QList<ToolStep>& steps, const QString& hover) {
  const Tokens& t = theme::current();
  m_nameWidth = 0;
  while (QLayoutItem* it = m_stepRows->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  bool waitingSeen = false;
  for (int i = 0; i < steps.size(); ++i) {
    const ToolStep& s = steps[i];
    StepState state = !s.picked.isEmpty() ? StepState::Done : waitingSeen ? StepState::Pending : StepState::Waiting;
    if (state == StepState::Waiting) waitingSeen = true;
    auto* row = new QFrame(this);
    row->setObjectName("stepRow");
    row->setStyleSheet(state == StepState::Waiting
                           ? QString("QFrame#stepRow { background: %1; border: 1px solid %2; border-radius: 3px; } QLabel { background: transparent; }").arg(theme::css(t.selbg), theme::css(t.sel))
                           : QString("QFrame#stepRow { border: 1px solid transparent; } QLabel { background: transparent; }"));
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(6, 5, 8, 5);
    l->setSpacing(8);
    auto* ring = new QLabel(row);
    ring->setPixmap(stepRing(state, i + 1, devicePixelRatioF()));
    ring->setFixedSize(16, 16);
    auto* label = new QLabel(s.label, row);
    label->setStyleSheet(QString("color: %1;").arg(theme::css(state == StepState::Pending ? t.fg3 : t.fg)));
    const QString name = state == StepState::Done ? s.picked : state == StepState::Waiting && !hover.isEmpty() ? tr("hover: %1").arg(hover) : QString();
    auto* target = new SelectionNameLabel(name, row);
    target->setFont(theme::mono(11));
    target->setStyleSheet(QString("color: %1;").arg(theme::css(state == StepState::Waiting ? t.sel : t.fg2)));
    target->setVisible(!name.isEmpty());
    m_nameWidth = std::max(m_nameWidth, target->fontMetrics().horizontalAdvance(name.simplified()));
    auto* text = new QVBoxLayout();
    text->setSpacing(3);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    text->addWidget(label);
    text->addWidget(target);
    l->addWidget(ring, 0, Qt::AlignTop);
    l->addLayout(text, 1);
    m_stepRows->addWidget(row);
  }
  emit contentSizeChanged();
}

void ToolStepsPanel::setSummary(const QString& title, const QString& subtitle, const QString& state) {
  m_title->setText(title);
  m_subtitle->setText(subtitle);
  m_state->setText(state);
  const bool visible=!title.isEmpty()||!subtitle.isEmpty()||!state.isEmpty();
  m_title->parentWidget()->setVisible(visible);
  if(auto* rule=findChild<QFrame*>("toolRule"))rule->setVisible(visible);
  emit contentSizeChanged();
}

void ToolStepsPanel::setError(const QString& text) {
  if (m_error->text() == text && m_error->isVisibleTo(this) == !text.isEmpty()) return;
  m_error->setText(text);
  m_error->setVisible(!text.isEmpty());
  emit contentSizeChanged();
}

void ToolStepsPanel::setResult(const QList<QPair<QString, QString>>& rows) {
  const Tokens& t = theme::current();
  m_grid->clear();
  m_keyWidth = 48;
  m_valueWidth = 80;
  for (const auto& [key, value] : rows) {
    auto* row = new QTreeWidgetItem(m_grid);
    row->setText(0, key);
    row->setForeground(0, t.fg2);
    row->setText(1, QChar(0x202A) + value + QChar(0x202C));  // values keep their order in a right-to-left UI
    row->setFont(1, theme::mono(12));
    row->setToolTip(0, key);
    row->setToolTip(1, value);
    row->setSizeHint(0, QSize(0, 30));
    if (key.startsWith(QChar(0x0394)) && key.size() == 2) {
      const QColor c = key.endsWith('X') ? t.red : key.endsWith('Y') ? t.green : t.dark ? QColor("#76b5ff") : QColor("#2067be");
      row->setForeground(0, c);
      row->setForeground(1, c);
    } else if (m_grid->topLevelItemCount() == 1) {
      QFont font = theme::mono(15); font.setBold(true);
      row->setFont(1, font);
      row->setForeground(1, t.sel);
      row->setSizeHint(0, QSize(0, 38));
    }
    m_keyWidth = std::max(m_keyWidth, QFontMetrics(m_grid->font()).horizontalAdvance(key) + 16);
    m_valueWidth = std::max(m_valueWidth, QFontMetrics(row->font(1)).horizontalAdvance(value) + 20);
  }
  m_keyWidth = std::min(m_keyWidth, 140);
  m_valueWidth = std::min(m_valueWidth, 1200);
  m_grid->setVisible(!rows.isEmpty());
  sizeResults(std::max(1, m_scroll->viewport()->width()));
  emit contentSizeChanged();
}

void ToolStepsPanel::setFooter(bool visible, bool canPin) {
  m_footer->setVisible(visible);
  m_footer->setPrimaryEnabled(canPin);
  emit contentSizeChanged();
}

void ToolStepsPanel::setComponentsState(bool visible, bool checked) {
  const QSignalBlocker blocker(m_components);
  m_components->setChecked(checked);
  m_components->setVisible(visible);
  emit contentSizeChanged();
}

void ToolStepsPanel::setAnchorOptions(const QStringList& labels, int current) {
  const QSignalBlocker blocker(m_anchors);
  m_anchors->clear();
  m_anchors->addItems(labels);
  if (!labels.isEmpty()) m_anchors->setCurrentIndex(std::clamp(current, 0, static_cast<int>(labels.size()) - 1));
  m_anchorRow->setVisible(labels.size() > 1);
  emit contentSizeChanged();
}

void ToolStepsPanel::setModeOptions(const QStringList& labels, int current) {
  if (labels != m_modeLabels) {
    for (QAbstractButton* b : m_modes->buttons()) {
      m_modes->removeButton(b);
      delete b;
    }
    m_modeLabels = labels;
    for (int i = 0; i < labels.size(); ++i) {
      auto* b = new QPushButton(labels[i], m_modeRow);
      b->setCheckable(true);
      b->setFocusPolicy(Qt::NoFocus);
      b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      m_modes->addButton(b, i);
      m_modeRow->layout()->addWidget(b);
    }
  }
  if (QAbstractButton* b = m_modes->button(current)) {
    const QSignalBlocker blocker(m_modes);
    b->setChecked(true);
  }
  m_modeRow->setVisible(!labels.isEmpty());
  emit contentSizeChanged();
}

void ToolStepsPanel::setFrameOptions(const QStringList& labels, int current) {
  const QSignalBlocker blocker(m_frames);
  QStringList now;
  for (int i = 0; i < m_frames->count(); ++i) now << m_frames->itemText(i);
  if (now != labels) {
    m_frames->clear();
    m_frames->addItems(labels);
  }
  if (!labels.isEmpty()) m_frames->setCurrentIndex(std::clamp(current, 0, static_cast<int>(labels.size()) - 1));
  m_frameRow->setVisible(labels.size() > 1);
  emit contentSizeChanged();
}

void ToolStepsPanel::setHistory(const QList<ToolHistoryRow>& rows, bool canPin) {
  const Tokens& t = theme::current();
  m_historyRows = rows;
  m_historyPin = canPin;
  m_history->clear();
  int titleWidth = 48;
  for (int i = 0; i < rows.size(); ++i) {
    titleWidth = std::max(titleWidth, QFontMetrics(m_history->font()).horizontalAdvance(rows[i].title) + 16);
    auto* item = new QTreeWidgetItem(m_history);
    item->setText(0, rows[i].title);
    item->setForeground(0, t.fg2);
    item->setText(1, QChar(0x202A) + rows[i].value + QChar(0x202C));
    item->setToolTip(1, rows[i].value);
    item->setFont(1, theme::mono(11));
    item->setSizeHint(0, QSize(0, 26));
    auto* actions = new QWidget(m_history);
    auto* box = new QHBoxLayout(actions);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);
    auto button = [&](const QString& icon, const QString& tip, bool enabled) {
      auto* b = new QToolButton(actions);
      b->setIcon(icons::themed(icon, 16));
      b->setToolTip(tip);
      b->setAutoRaise(true);
      b->setEnabled(enabled);
      b->setFocusPolicy(Qt::NoFocus);
      box->addWidget(b);
      return b;
    };
    QToolButton* copy = button("copy", tr("Copy this result"), true);
    QToolButton* pin = button("pin", rows[i].pinned ? tr("Pinned to the document") : tr("Pin this result to the document"), canPin && !rows[i].pinned);
    copy->setObjectName("historyCopy");
    pin->setObjectName("historyPin");
    connect(copy, &QToolButton::clicked, this, [this, i] { emit historyCopyRequested(i); });
    connect(pin, &QToolButton::clicked, this, [this, i] { emit historyPinRequested(i); });
    m_history->setItemWidget(item, 2, actions);
  }
  m_history->setColumnWidth(0, std::min(titleWidth, 180));
  const int shown = std::min<int>(rows.size(), 5);
  m_history->setFixedHeight(shown * std::max(26, rows.isEmpty() ? 0 : m_history->sizeHintForRow(0)) + 2 * m_history->frameWidth() + 2);
  m_historyBox->setVisible(!rows.isEmpty());
  emit contentSizeChanged();
}

void ToolStepsPanel::sizeResults(int width) {
  const bool manyRows = m_grid->topLevelItemCount() > 8;
  m_grid->setVerticalScrollBarPolicy(manyRows ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
  const int available = std::max(1, width - 26 - 2 * m_grid->frameWidth()
                                   - (manyRows ? m_grid->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0));
  m_grid->setColumnWidth(0, m_keyWidth);
  m_grid->setColumnWidth(1, std::max(m_valueWidth, available - m_keyWidth));
  int height = 2 * m_grid->frameWidth() + 4;
  // Ordinary inspect results fit completely. Future longer result lists scroll.
  for (int i = 0; i < std::min(8, m_grid->topLevelItemCount()); ++i)
    height += std::max({m_grid->sizeHintForRow(i), m_grid->topLevelItem(i)->sizeHint(0).height(), QFontMetrics(m_grid->topLevelItem(i)->font(1)).height() + 10});
  if (m_keyWidth + m_valueWidth > available) height += m_grid->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
  m_grid->setFixedHeight(height);
}

QSize ToolStepsPanel::preferredSize(int width) {
  const int preferredWidth = std::clamp(std::max({380, m_nameWidth + 56, m_keyWidth + m_valueWidth + 28,
                                                m_footer->minimumSizeHint().width()}), 380, 560);
  if (width <= 0) return QSize(preferredWidth, 0);
  m_body->ensurePolished();
  const int contentWidth = std::max(1, width - (m_scroll->verticalScrollBar()->isVisible()
                                     ? m_scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0));
  sizeResults(contentWidth);
  m_body->layout()->invalidate();
  m_body->layout()->activate();
  const int bodyHeight = std::max(m_body->layout()->totalMinimumSize().height(), m_body->layout()->totalHeightForWidth(contentWidth));
  return QSize(preferredWidth, bodyHeight + (m_footer->isHidden() ? 0 : m_footer->height()) + 4);
}

void ToolStepsPanel::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (!m_grid) return;
  sizeResults(std::max(1, width() - (m_scroll->verticalScrollBar()->isVisible() ? m_scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0)));
  emit contentSizeChanged();
}

bool ToolStepsPanel::eventFilter(QObject* object, QEvent* event) {
  if (object == m_scroll->viewport() && event->type() == QEvent::Resize && m_grid) {
    sizeResults(std::max(1, m_scroll->viewport()->width()));
    emit contentSizeChanged();
  }
  return QWidget::eventFilter(object, event);
}
