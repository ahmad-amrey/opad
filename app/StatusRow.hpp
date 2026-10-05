#pragma once
// The status bar's leading end (UI-08): the document's path and the chips beside it (git, an area's), one widget that
// never collapses. QStatusBar hides its normal widgets while a message shows and gives the stretching hover text all the
// room it wants, which cut the path to nothing and the git chip to one letter. The row is its first *permanent* widget
// instead, so it keeps its place and its width; StatusBar paints no message of its own and the window shows messages in
// a label of their own between the row and the hover text (MainWindow::buildStatusBar).
//   PathChip: the path, elided in the middle beyond kMaxWidth or when room is short (never below kMinWidth or its whole
//   text: messages and the hover text keep room too), the whole of it in the tooltip; a click (either button) opens its
//   menu, which areas fill (menuRequested: Open file location, ...).
//   StatusRow::addChip: a chip after the path (AreaServices::addStatusChip), always at its size hint.
#include <QLabel>
#include <QStatusBar>
#include <QWidget>

class QHBoxLayout;
class QMenu;

class StatusBar : public QStatusBar {
  Q_OBJECT
 public:
  using QStatusBar::QStatusBar;
 protected:
  void paintEvent(QPaintEvent*) override;  // the panel only: the window's message label shows currentMessage()
};

// A line of text cut at its end ("…") to the room it gets, from the leading end: the message beside the row.
class ElidedLabel : public QLabel {
  Q_OBJECT
 public:
  using QLabel::QLabel;
 protected:
  void paintEvent(QPaintEvent*) override;
};

class PathChip : public QWidget {
  Q_OBJECT
 public:
  explicit PathChip(QWidget* parent = nullptr);
  void setText(const QString& text);  // as shown: "C:\…\model.opad", "Read-only: …", "unsaved document"
  QString text() const { return m_text; }
  void setFile(const QString& file);  // the file it names ("": none, no menu)
  QString file() const { return m_file; }
  bool elided() const;  // the text does not fit: shown cut in the middle
  QMenu* menu(QWidget* parent);  // what a click opens (areas' entries; benches)
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  static constexpr int kMinWidth = 160, kMaxWidth = 560, kMargin = 12;
 signals:
  void menuRequested(QMenu* menu);  // add entries for file()
 protected:
  bool event(QEvent* e) override;  // the tooltip: the whole path first when it is cut
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void enterEvent(QEnterEvent*) override;
  void leaveEvent(QEvent*) override;
 private:
  QString m_text, m_file;
  bool m_hover = false;
};

class StatusRow : public QWidget {
  Q_OBJECT
 public:
  explicit StatusRow(QWidget* parent = nullptr);
  PathChip* path() const { return m_path; }
  void addChip(QWidget* chip);  // after the path and the chips added before; never squeezed below its size hint
 private:
  QHBoxLayout* m_row;
  PathChip* m_path;
};
