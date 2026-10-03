#pragma once
// Help > Tool guide (UI-107/108, design notes B §4 "where clips appear" 3 and 4). Every command's help, searched
// by title, keywords or summary and listed by area; the selected command shows its card: icon, title, key caps,
// summary, the animated clip with its steps (a click on a step loops it), details, and in amber what it needs when it
// is not available now. CommandPreview is that card alone; the command palette shows it, compact, beside its list.
#include <QPointer>
#include <QWidget>
#include <functional>

class ClipView;
class QAction;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;

class CommandPreview : public QWidget {
  Q_OBJECT
 public:
  enum class Size { Compact, Full };  // the palette's pane (288 px clip, no steps or details) or the reference's
  explicit CommandPreview(Size size, QWidget* parent = nullptr);
  // The command's card; `action` (may be null) gives the icon, the live shortcut and whether it is available now.
  void setCommand(const QString& id, QAction* action);
  QString command() const { return m_id; }
  ClipView* clip() const { return m_clip; }  // hidden for a command without a clip
  QListWidget* steps() const { return m_steps; }  // Full: "All steps" then the clip's steps; a click loops one
  bool showsRequirement() const;

 private:
  void refresh();
  Size m_size;
  QString m_id;
  QPointer<QAction> m_action;
  QMetaObject::Connection m_changed;
  QLabel *m_icon, *m_title, *m_summary, *m_details, *m_requirement;
  QHBoxLayout* m_keys;
  ClipView* m_clip;
  QListWidget* m_steps = nullptr;
};

class CommandReference : public QWidget {
  Q_OBJECT
 public:
  // `lookup`: the QAction of a command id (MainWindow::action), or null; a record whose command this build does not
  // have (one of another build or of a branch not merged yet) is not listed. Without a lookup every record is.
  explicit CommandReference(std::function<QAction*(const QString&)> lookup, QWidget* parent = nullptr);
  // Shows the window at that command (the filter is cleared when it hides it); empty keeps the one shown.
  void open(const QString& id = QString());
  QString current() const;
  void setFilter(const QString& text);
  QStringList shown() const;  // the command ids listed now, in order
  CommandPreview* preview() const { return m_preview; }

 protected:
  bool eventFilter(QObject* o, QEvent* e) override;  // Up and Down in the search field walk the list

 private:
  void refill();
  std::function<QAction*(const QString&)> m_lookup;
  QLineEdit* m_search;
  QTreeWidget* m_list;
  CommandPreview* m_preview;
};
