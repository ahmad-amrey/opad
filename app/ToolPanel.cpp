#include "ToolPanel.hpp"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "Icons.hpp"
#include "Theme.hpp"

// ---------------------------------------------------------------- ToolPanel
// Right-anchored panels grow into the viewport from their bottom-left corner.
class ToolPanelGrip : public QWidget {
 public:
  explicit ToolPanelGrip(ToolPanel* panel) : QWidget(panel), m_panel(panel) {
    setFixedSize(12, 12);
    setCursor(Qt::SizeBDiagCursor);
  }
 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setPen(QPen(theme::current().fg3, 1));
    p.drawLine(1, 3, 8, 10);
    p.drawLine(1, 7, 4, 10);
  }
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return;
    m_from = e->globalPosition().toPoint();
    m_geometry = m_panel->geometry();
    m_panel->m_resizing = true;
    m_limit = m_panel->maximumSize();
    if (QScreen* screen = QGuiApplication::screenAt(m_geometry.center())) {
      QRect area = screen->availableGeometry();
      if (!m_panel->m_anchor.isEmpty()) area = area.intersected(m_panel->m_anchor).adjusted(8, 8, -8, -8);
      if (!area.isEmpty()) m_limit = m_limit.boundedTo(QSize(m_geometry.right() + 1 - area.left(), area.bottom() + 1 - m_geometry.top()));
    }
    m_limit = m_limit.expandedTo(m_panel->minimumSize());
    e->accept();
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (!m_panel->m_resizing || !(e->buttons() & Qt::LeftButton)) return;
    const QPoint d = e->globalPosition().toPoint() - m_from;
    const QSize size = (m_geometry.size() + QSize(-d.x(), d.y())).expandedTo(m_panel->minimumSize()).boundedTo(m_limit);
    m_panel->setGeometry(QRect(QPoint(m_geometry.right() + 1 - size.width(), m_geometry.top()), size));
    e->accept();
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton || !m_panel->m_resizing) return;
    finishResize();
    e->accept();
  }
  bool event(QEvent* e) override {
    if (e->type() == QEvent::UngrabMouse && m_panel->m_resizing) finishResize();
    return QWidget::event(e);
  }
 private:
  void finishResize() {
    m_panel->m_resizing = false;
    if (m_panel->geometry() != m_geometry) m_panel->userPlacedNow();
  }
  ToolPanel* m_panel;
  QPoint m_from;
  QRect m_geometry;
  QSize m_limit;
};

ToolPanel::ToolPanel(const QString& id, const QString& icon, QColor Tokens::* tint, const QString& title, QWidget* content, int preferredHeight, QWidget* owner)
    : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint), m_id(id), m_iconName(icon), m_tint(tint), m_content(content) {
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setWindowTitle(title);
  auto* escape=new QShortcut(QKeySequence(Qt::Key_Escape),this);
  escape->setContext(Qt::WidgetWithChildrenShortcut);
  connect(escape,&QShortcut::activated,this,[this] { if(m_escapeHandler) m_escapeHandler(); else hide(); });
  const int m = kMargin;
  setMinimumSize(280 + 2 * m, 120 + 2 * m);
  setMaximumWidth(480 + 2 * m);
  m_defaultSize = QSize(336 + 2 * m, preferredHeight + 2 * m);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(m + 1, m + 1, m + 1, m + 1);  // shadow rim + the 1 px border
  outer->setSpacing(0);
  auto* header = new QWidget(this);  // takes no mouse events itself: a press on it reaches the panel and drags it
  header->setFixedHeight(32);
  auto* h = new QHBoxLayout(header);
  h->setContentsMargins(8, 0, 6, 0);
  h->setSpacing(8);
  m_icon = new QLabel(header);
  m_icon->setFixedSize(16, 16);
  auto* name = m_name = new QLabel(title, header);
  name->setObjectName("toolPanelTitle");
  m_context = new QLabel(header);
  m_context->setObjectName("toolPanelContext");
  m_context->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // truncates instead of widening the panel
  m_pin = new QToolButton(header);
  m_pin->setCheckable(true);
  m_pin->setToolTip(tr("Keep open"));
  m_close = new QToolButton(header);
  m_close->setToolTip(tr("Close  (Esc)"));
  for (QToolButton* b : {m_pin, m_close}) {
    b->setObjectName("dockButton");
    b->setIconSize(QSize(16, 16));
    b->setFixedSize(20, 20);
    b->setFocusPolicy(Qt::NoFocus);
  }
  h->addWidget(m_icon);
  h->addWidget(name);
  h->addWidget(m_context, 1);
  h->addWidget(m_pin);
  h->addWidget(m_close);
  outer->addWidget(header);
  outer->addSpacing(1);  // the header's bottom line (painted)
  content->setParent(this);
  outer->addWidget(content, 1);
  m_grip = new ToolPanelGrip(this);

  QSettings settings;
  const QString key = "panels/" + m_id;
  m_pin->setChecked(settings.value(key + "/pinned", false).toBool());
  if (settings.contains(key + "/offset")) {
    m_offset = settings.value(key + "/offset").toPoint();
    m_userPlaced = true;
  }
  resize(settings.value(key + "/size", m_defaultSize).toSize());

  connect(m_close, &QToolButton::clicked, this, &QWidget::hide);
  connect(m_pin, &QToolButton::toggled, this, [this, key](bool on) {
    QSettings().setValue(key + "/pinned", on);
    refreshIcons();
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { refreshIcons(); update(); });
  refreshIcons();
}

void ToolPanel::refreshIcons() {
  const Tokens& t = theme::current();
  m_icon->setPixmap(icons::pixmap(m_iconName, t.*m_tint, 16, devicePixelRatioF()));
  m_pin->setIcon(icons::icon("pin", m_pin->isChecked() ? t.sel : t.fg2));
  m_close->setIcon(icons::icon("close", t.fg2));
}

void ToolPanel::setHeader(const QString& icon, const QString& title) {
  m_iconName = icon;
  m_name->setText(title);
  setWindowTitle(title);
  refreshIcons();
}

void ToolPanel::setContext(const QString& text) {
  m_context->setText(text);
  m_context->setToolTip(text);
}

void ToolPanel::setPinnable(bool on) {
  m_pin->setVisible(on);
  if (!on) m_pin->setChecked(false);
}

void ToolPanel::setDefaultHeight(int contentHeight) {
  const int height = contentHeight + 35 + 2 * kMargin;  // header 32, its line 1, border 2
  if (m_defaultSize.height() == height) return;
  m_defaultSize.setHeight(height);
  if (isVisible() && !m_userPlaced && !m_anchor.isEmpty()) anchorTo(m_anchor);
}

void ToolPanel::anchorTo(const QRect& viewportGlobal) {
  m_anchor = viewportGlobal;
  if (m_resizing) return;  // content fitting must not fight a live grip drag
  const int m = kMargin;
  auto target = [&] { return QPoint(m_anchor.right() + 1 - m_offset.x() - (width() - m), m_anchor.top() + m_offset.y() - m); };
  if (m_contentSizeHint) {
    QScreen* screen = QGuiApplication::screenAt(m_anchor.center());
    if (!screen) screen = this->screen();
    if (!screen) return;
    QRect area = m_anchor.intersected(screen->availableGeometry()).adjusted(8, 8, -8, -8);
    if (area.width() < 1 || area.height() < 1) area = screen->availableGeometry();
    // Logical pixels: Qt handles DPI. Clamp saved sizes and positions as well as
    // new ones, including a removed monitor or a viewport smaller than the defaults.
    const QSize maximum(std::max(1, std::min(560 + 2 * m + 2, area.width())),
                        std::max(1, std::min(720 + 2 * m + 2, area.height())));
    setMinimumSize(std::min(360 + 2 * m + 2, maximum.width()), std::min(160, maximum.height()));
    setMaximumSize(maximum);
    const int inset = 2 * m + 2;
    const int preferredWidth = m_contentSizeHint(0).width() + inset;
    const int w = std::clamp(m_userPlaced ? width() : preferredWidth, minimumWidth(), maximumWidth());
    const int wantedHeight = m_contentSizeHint(std::max(1, w - inset)).height() + inset + 33;
    const int h = std::clamp(m_userPlaced ? std::max(height(), wantedHeight) : wantedHeight, minimumHeight(), maximumHeight());
    resize(w, h);
    QPoint pos = target();
    pos.setX(std::clamp(pos.x(), area.left(), area.right() + 1 - width()));
    pos.setY(std::clamp(pos.y(), area.top(), area.bottom() + 1 - height()));
    move(pos);
    return;
  }
  // A remembered place that is on no screen any more (monitor gone) falls back to the default one.
  if (m_userPlaced && !QGuiApplication::screenAt(target() + QPoint(width() / 2, m + 16))) {
    m_userPlaced = false;
    m_offset = QPoint(8, 186);
  }
  if (!m_userPlaced) resize(m_defaultSize.width(), std::min(m_defaultSize.height(), std::max(120, m_anchor.height() - 194) + 2 * m));
  move(target());
}

void ToolPanel::setContentSizeHint(std::function<QSize(int)> hint) {
  m_contentSizeHint = std::move(hint);
  // The scrollable content, rather than the outer layout's minimum hint, owns
  // overflow when available space is smaller than a normal panel.
  if (m_contentSizeHint) layout()->setSizeConstraint(QLayout::SetNoConstraint);
  requestContentFit();
}

void ToolPanel::requestContentFit() {
  if (m_resizing || m_contentFitPending) return;
  m_contentFitPending = true;
  QTimer::singleShot(0, this, [this] {
    m_contentFitPending = false;
    if (isVisible() && !m_anchor.isEmpty()) anchorTo(m_anchor);
  });
}

void ToolPanel::userPlacedNow() {
  const int m = kMargin;
  m_userPlaced = true;
  m_offset = QPoint(m_anchor.right() + 1 - (x() + width() - m), y() + m - m_anchor.top());
  QSettings settings;
  settings.setValue("panels/" + m_id + "/offset", m_offset);
  settings.setValue("panels/" + m_id + "/size", size());
}

void ToolPanel::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  const int m = kMargin;
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF frame = QRectF(rect()).adjusted(m, m, -m, -m);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0, 0, 0, 9));  // shadow 0 2 6: stacked rims, densest next to the frame
  for (int i = m; i >= 1; --i) p.drawRoundedRect(frame.adjusted(-i, 2 - i, i, std::min(i + 2, m)), 4 + i, 4 + i);
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg2);
  p.drawRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
  p.setRenderHint(QPainter::Antialiasing, false);
  p.drawLine(m + 1, m + 33, width() - m - 2, m + 33);
}

void ToolPanel::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  m_grip->move(kMargin + 1, height() - kMargin - 1 - m_grip->height());
  m_grip->raise();
}

void ToolPanel::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton || e->position().y() > kMargin + 33) return QWidget::mousePressEvent(e);
  m_dragging = true;
  m_dragFrom = e->globalPosition().toPoint();
  m_posFrom = pos();
}

void ToolPanel::mouseMoveEvent(QMouseEvent* e) {
  if (m_dragging) move(m_posFrom + e->globalPosition().toPoint() - m_dragFrom);
}

void ToolPanel::mouseReleaseEvent(QMouseEvent*) {
  if (m_dragging && pos() != m_posFrom) userPlacedNow();
  m_dragging = false;
}

void ToolPanel::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->position().y() > kMargin + 33) return;
  m_dragging = false;
  m_userPlaced = false;
  m_offset = QPoint(8, 186);
  QSettings settings;
  settings.remove("panels/" + m_id + "/offset");
  settings.remove("panels/" + m_id + "/size");
  anchorTo(m_anchor);
}

void ToolPanel::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) { if(m_escapeHandler) m_escapeHandler(); else hide(); }
  else QWidget::keyPressEvent(e);
}
