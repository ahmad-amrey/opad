#pragma once
// The start page (UI-113), shown while no document is open: how to begin (Open, New document, a template, Clone when the
// version-control commands are there), the recent files as cards with a picture of each, and where to learn (Getting
// started, the tool guide, the cheat sheet). A card opens its file; its menu (right click, the Menu key) also opens the
// folder, copies the path or removes it from the list; a file that is gone is marked and offers Locate… instead. Without
// recent files the drop zone and the View / Edit cards take their place. Files dropped anywhere on the page open.
// The pictures come from opad-cli thumbnail (the renderer of the Explorer thumbnails, so a big file never loads in the app)
// on a worker, one file at a time, and are kept in the user cache by path, size and time; whether a file is still there is
// read on that worker too.
#include <QAbstractButton>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <functional>

class Job;
class JobRunner;
class QAction;
class QGridLayout;
class QLabel;
class QMenu;
class QPushButton;
class QVBoxLayout;

// Templates: New from a template is a new document set up a little (built in: millimetres, inches, a sketch begun at once)
// or an untitled copy of an .opad in the templates folder (setting files/templates; by default "templates" beside the
// settings: <data dir>/templates in a portable run).
namespace templates {
struct Entry {
  QString id;     // "builtin:mm", "builtin:in", "builtin:sketch", or the file's path
  QString title, icon;
};
QString folder();
QList<Entry> list();  // the built-in ones, then the folder's .opad files by name
}  // namespace templates

class RecentCard : public QAbstractButton {
  Q_OBJECT
 public:
  enum class State { Unknown, Present, Missing };
  RecentCard(const QString& path, QWidget* parent = nullptr);
  QString path() const { return m_path; }
  State state() const { return m_state; }
  bool hasPicture() const { return !m_picture.isNull(); }
  void setState(State state, const QDateTime& modified);
  void setPicture(const QImage& picture);
  QSize sizeHint() const override { return {kWidth, kHeight}; }
  static constexpr int kWidth = 176, kHeight = 182, kPicture = 116;
 signals:
  void menuRequested(const QPoint& global);

 protected:
  void paintEvent(QPaintEvent*) override;
  void contextMenuEvent(QContextMenuEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;  // Enter opens, the Menu key and Shift+F10 open the menu
  void focusInEvent(QFocusEvent* e) override;
  void focusOutEvent(QFocusEvent* e) override;
  void enterEvent(QEnterEvent*) override { update(); }
  void leaveEvent(QEvent*) override { update(); }

 private:
  void describe();  // accessible name and description, tooltip
  QString m_path;
  State m_state = State::Unknown;
  QDateTime m_modified;
  QImage m_picture;
  bool m_keyFocus = false;  // focus from the keyboard: the ring shows
};

class EmptyState : public QWidget {
  Q_OBJECT
 public:
  explicit EmptyState(QWidget* parent = nullptr);
  ~EmptyState() override;
  void setRecent(const QStringList& paths);
  QStringList recent() const { return m_paths; }
  void setJobs(JobRunner* jobs);  // the pictures and file states are read through it; until then the cards show their icons
  // Commands the page offers when the app has them: Learn's (help.start, help.reference, help.shortcuts) and Clone
  // (file.clone). Looked up again each time the page shows.
  void setCommands(std::function<QAction*(const QString&)> lookup);
  QList<RecentCard*> cards() const { return m_cards; }
  QMenu* cardMenu(RecentCard* card);  // what a card's menu offers (not shown; the caller shows or deletes it)
  QList<QPushButton*> templateButtons() const { return m_templateButtons; }
  QList<QPushButton*> learnButtons() const;  // the Learn commands shown
  QPushButton* cloneButton() const { return m_clone; }
  QWidget* dropZone() const { return m_zone; }
  void refresh();  // reads the files' states and pictures again (on a worker) and the templates folder
  bool reading() const { return m_job; }
  // A recent file's menu for the cards: the window's (File > Recent's, MainWindow::recentMenu), set once.
  void setMenuBuilder(std::function<QMenu*(const QString& path, QWidget* parent)> build) { m_menu = std::move(build); }

 signals:
  void openRequested();
  void newRequested();
  void recentChosen(const QString& path);
  void filesDropped(const QStringList& paths);
  void recentChanged(const QStringList& paths);  // an entry removed or located elsewhere: the list to keep
  void templateChosen(const QString& id);        // templates::Entry::id

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  void showEvent(QShowEvent* e) override;
  void hideEvent(QHideEvent* e) override;
  bool eventFilter(QObject* watched, QEvent* e) override;  // the cards' column count follows their room

 private:
  void restyle();
  void layoutCards();
  void fillTemplates();
  void fillCommands();
  void applyFile(int generation, const QString& path, bool exists, const QDateTime& modified, const QImage& picture);
  void removeRecent(const QStringList& paths);
  void locate(const QString& path);
  std::function<QAction*(const QString&)> m_lookup;
  JobRunner* m_jobs = nullptr;
  QPointer<Job> m_job;
  int m_generation = 0;
  QStringList m_paths;
  std::function<QMenu*(const QString&, QWidget*)> m_menu;
  QList<RecentCard*> m_cards;
  QWidget* m_grid;
  QGridLayout* m_gridLayout;
  QLabel* m_recentTitle;
  QPushButton* m_clearMissing;
  QWidget* m_zone;     // the big drop zone and the View / Edit cards: no recent files yet
  QWidget* m_dropHint;  // the slim one under the cards
  QVBoxLayout* m_templates;
  QList<QPushButton*> m_templateButtons;
  QList<QPair<QString, QPushButton*>> m_learnButtons;  // by command id
  QPushButton* m_clone;
  int m_columns = 0;
};
