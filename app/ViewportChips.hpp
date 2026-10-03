#pragma once
#include <QLabel>
#include <QToolButton>
#include <QWidget>

// ---------------------------------------------------------------- viewport chips
class ViewportChips : public QWidget {
  Q_OBJECT
 public:
  explicit ViewportChips(QWidget* parent = nullptr);
  void set(const QString& mode, const QString& projection, const QString& section, const QString& isolate, bool twoDimensional = false);
  // Viewer mode: "Viewer · read-only" first in the row, then Save to edit. An empty file hides them.
  void setViewer(const QString& file);
 signals:
  void leaveTwoDimensional();  // the 2D mode card was clicked
  void saveToEditRequested();
 protected:
  bool eventFilter(QObject* object, QEvent* event) override;
 private:
  QLabel* m_mode;
  QLabel* m_proj;
  QLabel* m_twoD;
  QLabel* m_section;
  QLabel* m_isolate;
  QLabel* m_viewer;
  QToolButton* m_saveToEdit;
};
