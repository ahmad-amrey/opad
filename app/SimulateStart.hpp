#pragma once
// Simulate's start page, "What do you want to find out?" (the Start tab: a page in the viewport's place): a grid of checks
// (SimChecks), each a card with its goal, its picture (SimPictures), its question, one line of what it is for, how long it
// takes and whether it can run here (Ready / Needs <solver> / Coming), a box to describe it in your own words (the cards
// that match stay, best first), goal chips, the checks already made in this document on the left, and a way to the Advanced
// tools. It only shows and asks; the area does the work.
#include <QAbstractButton>
#include <QList>
#include <QWidget>

#include <string>
#include <vector>

#include "opad/json.hpp"

class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class CheckCard : public QAbstractButton {
  Q_OBJECT
 public:
  CheckCard(const QString& check, QWidget* parent);
  QString check() const { return m_check; }
  void setReadiness(const QString& chip, int state);  // simchecks::Readiness::State
  QString chip() const { return m_chip; }
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  bool hasHeightForWidth() const override { return false; }

 signals:
  void solverClicked();  // the Needs chip pressed

 protected:
  void paintEvent(QPaintEvent*) override;
  void enterEvent(QEnterEvent*) override;
  void leaveEvent(QEvent*) override;
  void mouseReleaseEvent(QMouseEvent* e) override;

 private:
  QRectF chipRect() const;
  QString m_check, m_chip;
  int m_state = 0;
  bool m_hover = false;
};

class SimulateStart : public QWidget {
  Q_OBJECT
 public:
  explicit SimulateStart(QWidget* parent = nullptr);
  void setEngines(const opad::json& engines);  // the chips: Ready, Needs ..., Coming
  // The document's checks: {study id, check id, its name, a short result or "Not run"}.
  struct Made {
    std::string study;
    QString check, name, result;
  };
  void setMade(const std::vector<Made>& made);
  void setQuery(const QString& text);
  void setGoal(const QString& goal);  // "" all, else "air", "heat", "strength", "movement"
  QStringList shown() const;           // the cards shown, in order (benches)
  QLineEdit* search() const { return m_search; }
  QList<CheckCard*> cards() const { return m_cards; }

 signals:
  void checkChosen(const QString& check);
  void madeChosen(const std::string& study, const QString& check);
  void solverRequested(const QString& solver);
  void advancedRequested();

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;

 private:
  void relayout();
  QLineEdit* m_search;
  QList<QPushButton*> m_chips;
  QString m_goal;
  QList<CheckCard*> m_cards;
  QWidget* m_grid;
  QGridLayout* m_gridLayout;
  QLabel* m_none;
  QListWidget* m_made;
  QLabel* m_madeTitle;
  opad::json m_engines;
  int m_columns = 0;
};
