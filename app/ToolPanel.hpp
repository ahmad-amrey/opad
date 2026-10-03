#pragma once
#include <QColor>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QToolButton>
#include <QWidget>
#include <algorithm>
#include <functional>

#include "Theme.hpp"

class QLabel;

// ---------------------------------------------------------------- floating tool panel
// Replaces the fixed right dock (handoff: "Floating tool panel"). A frameless tool window owned by the main
// window, so it sits over the OpenGL viewport and hides with it. Header 32 px (drag handle; double-click puts
// it back at its default place): icon, title 500, context fg3, pin, close. Anchored to the viewport's top-right
// corner: MainWindow calls anchorTo() whenever the viewport moves. Place and size persist per panel id.
class ToolPanel : public QWidget {
  Q_OBJECT
 public:
  ToolPanel(const QString& id, const QString& icon, QColor Tokens::* tint, const QString& title, QWidget* content, int preferredHeight, QWidget* owner);
  void setContext(const QString& text);
  void setEscapeHandler(std::function<void()> handler) { m_escapeHandler=std::move(handler); }
  void setHeader(const QString& icon, const QString& title);  // one panel serves every guided tool
  QWidget* content() const { return m_content; }
  void setPinnable(bool on);  // an editor's panel has no pin: it lives exactly as long as the editor
  // The default height for this content (header excluded). A panel the user has not sized follows it, never taller
  // than the viewport below its top, so it stays under the view cube.
  void setDefaultHeight(int contentHeight);
  void setDefaultWidth(int width) { m_defaultSize.setWidth(std::clamp(width, 280, 480) + 2 * kMargin); }  // a table's columns: 336 by default
  bool pinned() const { return m_pin->isChecked(); }
  bool userPlaced() const { return m_userPlaced; }
  void setDefaultTop(int top) { if (!m_userPlaced) m_offset.setY(top); }
  int bottom() const { return m_offset.y() + height() - 2 * kMargin; }  // in viewport coordinates
  void anchorTo(const QRect& viewportGlobal);
  // Opt-in content fitting for inspect results; other floating panels keep their saved sizing.
  void setContentSizeHint(std::function<QSize(int)> hint);
  void requestContentFit();
  static constexpr int kMargin = 6;  // translucent rim the shadow is painted in
  // Where the keyboard lives (the viewport, TODO 11 UI-05). A panel takes the keyboard only for a text field or a list
  // clicked; a click on its buttons, check boxes, combos, sliders or header leaves the window that has the keyboard active
  // (Windows) and, if the panel has it all the same (a text field typed into before), gives it back to `home` when the
  // click is done, so digits, Esc and the tools' keys go on reaching the tool, never a window shortcut through the panel.
  static void setKeyboardHome(QWidget* home);
  static bool keepsKeyboard(const QWidget* widget);  // a text field or a list, or a part of one: it types into its panel
  bool takesKeyboardAt(const QPoint& global) const;  // a click there activates the panel
 signals:
  void visibilityChanged(bool visible);
 protected:
#ifdef Q_OS_WIN
  bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;
#endif
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  bool event(QEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void showEvent(QShowEvent*) override { emit visibilityChanged(true); }
  void hideEvent(QHideEvent*) override { emit visibilityChanged(false); }
 private:
  friend class ToolPanelGrip;
  void userPlacedNow();  // after a drag or a grip resize: remember where the panel is
  void refreshIcons();
  std::function<void()> m_escapeHandler;
  QString m_id, m_iconName;
  QColor Tokens::* m_tint;  // header icon colour: sel for inspect tools, amber for annotations, fg2 for selection
  QLabel *m_icon, *m_name, *m_context;
  QWidget* m_content;
  QToolButton *m_pin, *m_close;
  QWidget* m_grip;
  QRect m_anchor;                    // the viewport, global
  QPoint m_offset{8, 186};           // frame's top-right corner: x px left of the viewport's right edge, y px below its top
  QSize m_defaultSize;
  std::function<QSize(int)> m_contentSizeHint;
  bool m_contentFitPending = false;
  bool m_userPlaced = false;
  bool m_dragging = false;
  bool m_resizing = false;
  QPoint m_dragFrom, m_posFrom;
};
