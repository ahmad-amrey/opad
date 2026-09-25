// Notes (annotations) as cards: the tags a note can carry and how each is drawn, the card itself (used by the
// Annotations panel and over the viewport), the layer that keeps one card beside every open note's anchor, and
// the dialog a note is written in.
#pragma once

#include <QDialog>
#include <QFrame>
#include <QPoint>
#include <QPointer>
#include <map>
#include <functional>
#include <string>
#include <vector>

#include "Theme.hpp"
#include "opad/json.hpp"
#include "opad/scene.hpp"

class AppDocument;
class QPlainTextEdit;
class QToolButton;
class Viewport;

namespace notes {
// One tag = one look, shared by the card (border, icon) and the pointer in the viewport (colour, line type, width).
struct Style {
  const char* id;     // the op's "style" value
  const char* label;  // goes through i18n::t
  const char* icon;
  QColor Tokens::* color;
  Qt::PenStyle line;
  double width;
};
const std::vector<Style>& styles();
const Style& style(const std::string& id);  // an unknown id is "note"
void makeDraggable(QWidget* card, QWidget* handle, std::function<void()> moved = {});
}  // namespace notes

struct NoteInfo {
  bool measurement=false;
  QString value;
  std::string id, by, ts, text, style, body;
  QString target;          // "Clamp block › face 12"
  QString state = "open";  // open | unresolved | resolved
  opad::json comments = opad::json::array();
};

class NoteCard : public QFrame {
  Q_OBJECT
 public:
  NoteCard(const NoteInfo& note, QWidget* parent, AppDocument* doc = nullptr);
  const NoteInfo& note() const { return m_note; }
  void enableDragging();
 signals:
  void pressed();
  void resolveRequested(const std::string& opId);
  void restoreRequested(const std::string& opId);
  void styleRequested(const std::string& opId, const std::string& style);
  void moved();
 protected:
  void mousePressEvent(QMouseEvent* e) override;
 private:
  NoteInfo m_note;
  QWidget* m_dragHandle = nullptr;
};

// The cards over the viewport. They are native child widgets of the viewport (like the chips and the prompt); the
// pointer from the anchor to its card is drawn by the viewport, which is told where each pointer ends.
class NoteCards : public QObject {
  Q_OBJECT
 public:
  NoteCards(AppDocument* doc, Viewport* viewport, QObject* parent);
  void setShown(bool on);  // Show notes / Hide notes: off leaves nothing in the view, the panel still lists them
  bool shown() const { return m_shown; }
  void setTypeFilter(const std::string& type);
 signals:
  void pressed(const std::string& opId, const std::string& body);
  void resolveRequested(const std::string& opId);
  void styleRequested(const std::string& opId, const std::string& style);
 public slots:
  void rebuild();  // the document changed: one card per open note
  void layout();   // the camera or an anchor moved
 private:
  AppDocument* m_doc;
  Viewport* m_viewport;
  bool m_shown = true;
  std::vector<NoteCard*> m_cards;
  std::map<std::string, QPoint> m_positions;
  std::string m_type;
};

class NoteDialog : public QDialog {
  Q_OBJECT
 public:
  NoteDialog(const QString& where, QWidget* parent);
  QString text() const;
  std::string style() const { return m_style; }
 private:
  void pick(const std::string& style);
  QPlainTextEdit* m_text;
  std::vector<QToolButton*> m_tags;
  std::string m_style;
};

// Session-only editor. A completed drawing is one annotation/Undo step.
class HandDrawing : public QObject {
  Q_OBJECT
 public:
  HandDrawing(AppDocument* doc, Viewport* viewport, QObject* parent);
  ~HandDrawing() override;
  void cancel();
 protected:
  bool eventFilter(QObject* object,QEvent* event) override;
 private:
  void addPoint(const QPointF& point);
  void finish();
  void detach();
  QPointer<AppDocument> m_doc;
  QPointer<Viewport> m_viewport;
  QPointer<QFrame> m_panel;
  QPlainTextEdit* m_text;
  class QComboBox* m_type;
  class QComboBox* m_color;
  class QComboBox* m_width;
  class QLabel* m_hint;
  opad::Frame m_frame;
  opad::Ref m_anchor;
  opad::json m_drawing;
  bool m_active=true,m_dragging=false;
  int m_previousFilter=0;
  QPointF m_lastPoint;
  size_t m_points=0;
};
