// Notes (annotations) as cards: the four tags a note can carry and how each is drawn, the card itself (used by the
// Annotations panel and over the viewport), the layer that keeps one card beside every open note's anchor, and
// the dialog a note is written in.
#pragma once

#include <QDialog>
#include <QFrame>
#include <QPoint>
#include <map>
#include <string>
#include <vector>

#include "Theme.hpp"
#include "opad/json.hpp"

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
}  // namespace notes

struct NoteInfo {
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
 signals:
  void pressed();
  void resolveRequested(const std::string& opId);
  void restoreRequested(const std::string& opId);
  void styleRequested(const std::string& opId, const std::string& style);
 protected:
  void mousePressEvent(QMouseEvent* e) override;
 private:
  NoteInfo m_note;
};

// The cards over the viewport. They are native child widgets of the viewport (like the chips and the prompt); the
// pointer from the anchor to its card is drawn by the viewport, which is told where each pointer ends.
class NoteCards : public QObject {
  Q_OBJECT
 public:
  NoteCards(AppDocument* doc, Viewport* viewport, QObject* parent);
  void setShown(bool on);  // Show notes / Hide notes: off leaves nothing in the view, the panel still lists them
  bool shown() const { return m_shown; }
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
