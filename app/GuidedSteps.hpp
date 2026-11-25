#pragma once
// The frame of a guided panel taken step by step (Simulate's checks, Thermal setup): an optional header (a goal label in its
// colour over a title), the steps as tabs (a step done gets a check mark), each step's page scrolling by itself, and under it
// "Step n of m", Back and Next (its text per step: "Next: how much air", the last step's hidden or taken by the caller).
// Debounce: a change of the pages written a moment later, once (typing a value writes once), and at once when asked to
// (a page switch, closing the panel).
#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTimer;
class QVBoxLayout;

class GuidedSteps : public QWidget {
  Q_OBJECT
 public:
  // `prefix` names the parts for benches and the stylesheet: <prefix>Steps (the tabs), <prefix>Back, <prefix>Next,
  // <prefix>StepText.
  explicit GuidedSteps(const QString& prefix, QWidget* parent = nullptr);
  // The goal label (upper case, in `color`) over the title; both empty: no header.
  void setHeader(const QString& goal, const QColor& color, const QString& title);
  QString title() const;
  // A page: its tab's text and an intro under it (empty: none); returns its layout to fill (it ends in a stretch the caller
  // need not add).
  QVBoxLayout* addPage(const QString& tab, const QString& intro = {});
  QWidget* page(int i) const;  // the scrolling page's content
  void setIntro(int i, const QString& text);
  int count() const;
  int current() const;
  void setCurrent(int i);
  void setDone(int i, bool done);  // a check mark on its tab
  bool done(int i) const;
  void setNextText(int i, const QString& text);  // empty: "Next"
  void setLastHasNext(bool on);                  // the last step's Next shown too (it then does what nextAtEnd says)
  void setNextEnabled(bool on);
  QTabBar* tabs() const { return m_tabs; }
  QPushButton* back() const { return m_back; }
  QPushButton* next() const { return m_next; }
  QLabel* stepText() const { return m_stepText; }
  QHBoxLayout* footer() const { return m_footer; }  // buttons of the caller's before Back (Save a report)

 signals:
  void stepShown(int i);  // the tab or Back/Next changed the page (also setCurrent)
  void nextAtEnd();       // Next pressed on the last step (setLastHasNext)

 private:
  void show(int i);
  void nav(int i);  // Back, Next and the step text for page i
  QLabel *m_goal, *m_title, *m_stepText;
  QTabBar* m_tabs;
  QStackedWidget* m_pages;
  QPushButton *m_back, *m_next;
  QHBoxLayout* m_footer;
  std::vector<QString> m_names, m_nextTexts;
  std::vector<bool> m_done;
  std::vector<QLabel*> m_intros;
  bool m_lastHasNext = false;
};

class Debounce : public QObject {
  Q_OBJECT
 public:
  Debounce(int ms, std::function<void()> fn, QObject* parent);
  void poke();         // run in `ms`, the wait starting again
  void flush();        // pending: run now
  void cancel();
  bool pending() const;

 private:
  QTimer* m_timer;
  std::function<void()> m_fn;
};
