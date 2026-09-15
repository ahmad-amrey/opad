#pragma once
// The "no document" screen: drop zone, Open / Import / Recent, Browse-vs-Import explanation cards, recent list.
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

class EmptyState : public QWidget {
  Q_OBJECT
 public:
  explicit EmptyState(QWidget* parent = nullptr);
  void setRecent(const QStringList& paths);

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
};
