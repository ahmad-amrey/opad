#pragma once
// Review > Annotate > Note (N) and Hand drawing (Shift+N). Started first, then asks for its target like the guided
// tools: the prompt bar names the step, the "annotation" tool panel holds the type, the pen and the text, and the
// target is highlighted in the view under a badge. Hand drawing: every stroke lies on the plane through the target's
// picked point that faces the camera when the stroke starts, so orbiting between strokes draws on another plane and
// the strokes build up a 2.5D sketch around the object. Session only: Save runs one "annotate" op (one Undo step),
// Cancel leaves the document as it was.
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <string>
#include <vector>

#include "opad/json.hpp"
#include "opad/scene.hpp"

class AppDocument;
class PromptBar;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class ToolPanel;
class Viewport;

class AnnotationEditor : public QObject {
  Q_OBJECT
 public:
  // panel: the one "annotation" ToolPanel; the editor fills its content and shows it.
  AnnotationEditor(AppDocument* doc, Viewport* viewport, ToolPanel* panel, QObject* parent, bool drawing);
  ~AnnotationEditor() override;
  void cancel();
  void undo();  // strokes only; the document's history waits until Save
  void redo();
  bool drawingMode() const { return m_drawingMode; }
  bool anchored() const { return m_anchored; }
  const opad::Ref& target() const { return m_anchor; }
  // Takes the viewport's current selection as the target when it is exactly one thing ("selected first, tool second").
  void adoptSelection(const std::vector<opad::Ref>& selection);

 protected:
  bool eventFilter(QObject* object, QEvent* event) override;

 private:
  void build();
  QWidget* buildHeading(QVBoxLayout* layout, const QString& title, QLabel** value = nullptr);
  bool anchorTo(opad::Ref target, bool hasPoint);
  void addPoint(const QPointF& point);
  void eraseAt(const QPointF& point);
  void finish();
  void detach();
  void remember(opad::json before);  // one undo step: the stroke list before a change
  void refresh();
  void refreshStrokes();
  void refreshPens();
  void refreshPrompt();
  void positionOverlays();
  void fitPanel(bool now = false);
  void chooseType(const std::string& type);
  void chooseColor(int index);
  void chooseWidth(int index);
  void chooseTool(bool eraser);
  QString targetName() const;
  bool mousePress(QEvent* event);
  bool mouseMove(QEvent* event);
  bool mouseRelease(QEvent* event);
  bool key(QObject* object, QEvent* event);

  QPointer<AppDocument> m_doc;
  QPointer<Viewport> m_viewport;
  QPointer<ToolPanel> m_panel;
  QPointer<QWidget> m_content;
  QPointer<PromptBar> m_prompt;
  QPointer<QFrame> m_badge;
  QLabel *m_badgeIcon = nullptr, *m_badgeText = nullptr;
  QScrollArea* m_scroll = nullptr;
  QWidget* m_sections = nullptr;
  QWidget* m_footerBar = nullptr;
  QPlainTextEdit* m_text = nullptr;
  QLabel *m_typeHint = nullptr, *m_footer = nullptr, *m_count = nullptr, *m_toolValue = nullptr, *m_colorValue = nullptr, *m_widthValue = nullptr;
  QPushButton* m_save = nullptr;
  QToolButton *m_undoButton = nullptr, *m_redoButton = nullptr, *m_clearButton = nullptr;
  QWidget* m_strokeList = nullptr;
  QScrollArea* m_strokeScroll = nullptr;
  std::vector<QToolButton*> m_types, m_colors, m_widths;
  QToolButton *m_pen = nullptr, *m_eraserButton = nullptr;
  opad::Frame m_frame;  // the stroke being drawn
  opad::Ref m_anchor;
  opad::json m_drawing;
  opad::json m_before;                     // the stroke list when the current gesture started
  std::vector<opad::json> m_undo, m_redo;  // stroke lists before / after each change
  std::string m_type = "note";
  bool m_drawingMode = true, m_active = true, m_dragging = false, m_anchored = false, m_eraser = false, m_picking = false;
  bool m_cubePress = false, m_erased = false, m_fitPending = false;
  int m_colorIndex = 0, m_widthIndex = 1;
  QPointF m_lastPoint, m_pickPress;
  size_t m_points = 0;
};
