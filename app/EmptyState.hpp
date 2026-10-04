#pragma once
// The "no document" screen: drop zone, Open / Import / Recent, Browse-vs-Import explanation cards, recent list.
#include <QListWidget>
#include <QPushButton>
#include <QWidget>
#include <functional>

class EmptyState : public QWidget {
  Q_OBJECT
 public:
  explicit EmptyState(QWidget* parent = nullptr);
  void setRecent(const QStringList& paths);
  // A recent file's context menu (right click on the list or on Recent ▾'s entries): Open, Open file location, Copy path,
  // Remove from list (MainWindow::recentMenu).
  void setMenuBuilder(std::function<QMenu*(const QString& path, QWidget* parent)> build) { m_menu = std::move(build); }
  QListWidget* recentList() const { return m_recent; }  // benches

 signals:
  void openRequested();
  void importRequested();
  void recentChosen(const QString& path);
  void filesDropped(const QStringList& paths);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  QListWidget* m_recent;
  QPushButton* m_recentButton;
  QStringList m_paths;
  std::function<QMenu*(const QString&, QWidget*)> m_menu;
};
