#pragma once
// The AI activity panel's content and the status bar's AI chip (AgentBridge owns both; AgentBridge.cpp fills them).
//
// The panel is a plain-language feed of what an AI assistant did: one row per call that updates in place from running to
// done, reads folded into one "Looked over the model" row, a task (a transaction) as one group row with its steps under it,
// what needs the user as an amber row with one button. Rows say only what OPAD observed (AgentSentences.hpp); hovering one
// tints its bodies in the view, Show frames them, Undo is offered only while its step is the next one Undo takes back.
// Over the feed, the connection strip (who is connected, what it may do, whether it works); empty, a state that says what
// to do next; under it, Zoom to its changes and Stop. It never opens by itself.
#include <QDateTime>
#include <QFrame>
#include <QPixmap>
#include <QPointer>
#include <QWidget>
#include <functional>
#include <string>
#include <vector>

#include "Theme.hpp"

class QCheckBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

struct ActivityEvent {
  // Phase: a heading in the assistant's own words (set_phase), the rows after it are its work.
  enum Kind { Divider, Running, Look, Reads, Change, Group, Note, Result, NeedsYou, Quiet, Phase };
  enum State { Active, Done, Staged, Applied, Dropped, Undone, Gone };
  quint64 id = 0;
  Kind kind = Running;
  State state = Active;
  QString icon;              // Icons.cpp name
  QString text, second;      // the sentence and its second line
  QString tag;               // trailing tag on the second line: "Undone", "No longer in the design"
  QStringList children;      // a fold's lookups, a group's steps
  std::vector<std::string> subjects;  // bodies: hover tint, Show, Select
  std::string op;            // the step it made (Undo, Show in timeline)
  std::vector<std::string> ops;  // every step its commit made (a task's several): the timeline's Show in AI activity finds it
  QString client;            // the assistant's short name (a tag on the row while several are connected)
  QString action, actionText;  // a needs-you row's button: "allowEdit", "save"
  QString details;           // raw text (Details, Copy details)
  QDateTime at = QDateTime::currentDateTime();
  qint64 ms = -1;            // how long it took (the tooltip only)
  QPixmap thumb;             // a look's picture, scaled on the worker (96 x 64)
  QPixmap picture;           // the same, up to 480 px wide (the popup)
  bool issue = false;        // a finding: the icon in amber
  int reads = 0;             // a fold's lookups
  bool expanded = false;
  bool canUndo = false, canShow = true;  // asked again whenever the document changes
  bool stale = false;        // a row of a document that went: no Show, no Undo, never asked again
};

class ActivityRow;

class ActivityPanel : public QWidget {
  Q_OBJECT
 public:
  struct Strip {
    bool shown = false;
    bool active = true;  // the mark in the AI hue, else dim
    bool amber = false;  // line 1 in amber (waiting for the user)
    QString line1, line2, pill, link;  // pill: the access ("Can edit", "Look only"); link: "Resume", "Save as OPAD to edit"
    bool operator==(const Strip&) const = default;
  };
  struct Empty {
    QString title, text, button, prompt, example;  // prompt: a card with Copy; example: a dim line
  };
  explicit ActivityPanel(QWidget* parent = nullptr);
  void post(const ActivityEvent& event);  // a new row, or the row with this id updated
  ActivityEvent* event(quint64 id);
  const std::vector<ActivityEvent>& events() const { return m_events; }
  void remove(quint64 id);
  void clear();
  void setStrip(const Strip& strip);
  const Strip& strip() const { return m_stripState; }
  void setEmpty(const Empty& empty);
  void setBusy(bool working);  // Stop is the primary while something runs or a preview waits
  void setFollow(bool on);
  void setMultiple(bool on);   // several assistants: each row carries its name
  void refreshRows();          // canUndo / canShow / tags changed
  void scrollToEnd();
  // Show on a change row saved the camera: that row's button reads Back until the camera moves (0: none).
  void setShowing(quint64 id);
  quint64 showing() const { return m_showing; }
  void reveal(quint64 id);     // scrolled into view and focused (its actions show): the timeline's Show in AI activity
  quint64 revealed() const { return m_revealed; }  // the row reveal() went to last (benches)
  QString actionsOf(quint64 id) const;  // the row's action buttons' texts, "|"-joined (benches: "Show|Undo", "Back|Undo")
  QStringList lines() const;   // the rows as text, oldest first (benches, Copy activity as text)
  QPushButton* stopButton() const { return m_stop; }
  QCheckBox* followBox() const { return m_follow; }
  static constexpr int kMaxRows = 100;
 signals:
  void hovered(quint64 id);  // 0: off every row
  void activated(quint64 id, const QString& action);  // show, back, undo, timeline, select, copy, dismiss, notNow, discard, or a needs-you action
  void stopRequested();
  void followToggled(bool on);
  void stripMenuRequested(const QPoint& global);
  void accessMenuRequested(const QPoint& global);
  void stripLinkActivated();
  void emptyActionRequested();
  void promptCopied(const QString& prompt);
 protected:
  bool event(QEvent* e) override;
 private:
  friend class ActivityRow;
  void restyle();
  void styleStrip();
  void updatePage();
  ActivityRow* rowFor(quint64 id) const;
  std::vector<ActivityEvent> m_events;
  std::vector<ActivityRow*> m_rows;
  QFrame* m_strip;
  QLabel *m_stripIcon, *m_line1, *m_line2;
  QToolButton *m_pill, *m_more, *m_link;
  QStackedWidget* m_pages;
  QWidget* m_empty;
  QLabel *m_emptyIcon, *m_emptyTitle, *m_emptyText, *m_emptyExample;
  QFrame* m_promptCard;
  QLabel* m_prompt;
  QPushButton *m_emptyButton, *m_copy;
  QScrollArea* m_scroll;
  QWidget* m_list;
  QVBoxLayout* m_listLayout;
  QCheckBox* m_follow;
  QPushButton* m_stop;
  Strip m_stripState;
  bool m_multiple = false;
  quint64 m_showing = 0, m_revealed = 0;
};

// The status bar's AI chip: the mark, a word or two, an optional glyph (look only: an eye; needs you: a warning) and a count
// of changes made while the panel was closed. No animation: "working" is a word.
class AgentChip : public QWidget {
  Q_OBJECT
 public:
  struct State {
    QString text, tip;
    QColor Tokens::* mark = &Tokens::fg2;
    QColor Tokens::* label = &Tokens::fg;
    QString glyph;     // "eye", "warning" or empty
    bool glyphFirst = false;
    int count = 0;     // unseen changes
  };
  explicit AgentChip(QWidget* parent = nullptr);
  void setState(const State& state);
  const State& state() const { return m_state; }
  QString text() const { return m_state.text; }
  QSize sizeHint() const override;
 signals:
  void clicked(const QPoint& global);  // also the Menu key and a right click
 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;
  void enterEvent(QEnterEvent*) override;
  void leaveEvent(QEvent*) override;
 private:
  State m_state;
  bool m_hover = false;
};
