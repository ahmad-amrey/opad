#pragma once
// Widgets of the components area (UI-34, ComponentsArea.cpp): the Move to component list and the opacity slider, both
// over the view instead of a dialog.
#include <QFrame>
#include <QTimer>
#include <QWidget>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QSlider;

// Move to component…: where the selection can go (the document root and every component not moving with it, in tree
// order), narrowed as one types (every word in the name or in the path above it), ↑↓ to choose, Enter or a click to
// move there, Esc to close. Where the selection is already is listed, marked, and not chosen.
class ComponentPicker : public QFrame {
  Q_OBJECT
 public:
  struct Entry {
    std::string id;        // "" the document root
    QString name, path;    // path: the components above it, "A › B"
    int depth = 0;
    bool current = false;  // the selection is there: listed, not chosen
  };
  explicit ComponentPicker(QWidget* parent);
  void setEntries(std::vector<Entry> entries);  // the filter cleared, the first one that can be chosen current
  void popup(const QPoint& global);             // there on the screen (kept on it), the filter focused
  QLineEdit* filter() const { return m_filter; }
  QListWidget* list() const { return m_list; }
  int shown() const;  // the rows the filter lets through
 signals:
  void chosen(const std::string& id);
 protected:
  bool eventFilter(QObject* object, QEvent* event) override;
 private:
  void refill();
  void choose();
  std::vector<Entry> m_entries;
  QLineEdit* m_filter;
  QListWidget* m_list;
};

// Opacity as a slider (10-100 %), in the right-click menu (QWidgetAction) and the Opacity command's popup: previewed()
// while it moves, committed() when it is let go, or once keys or the wheel have rested a moment.
class OpacitySlider : public QWidget {
  Q_OBJECT
 public:
  explicit OpacitySlider(double opacity, QWidget* parent = nullptr);
  QSlider* slider() const { return m_slider; }
  double value() const;
  void finish(bool keep);  // the menu or popup goes: what is pending written (keep) or dropped; finished() once
 signals:
  void previewed(double opacity);
  void committed(double opacity);
  void finished();
 private:
  void write();
  QSlider* m_slider;
  QLabel* m_value;
  QTimer m_rest;
  int m_written;
  bool m_done = false;
};

// The Opacity command's popup at the mouse: an OpacitySlider; Esc drops what is pending, any other way out writes it.
class OpacityPopup : public QFrame {
  Q_OBJECT
 public:
  OpacityPopup(double opacity, QWidget* parent);
  OpacitySlider* slider() const { return m_slider; }
  void popup(const QPoint& global);
 protected:
  void keyPressEvent(QKeyEvent* event) override;
  void hideEvent(QHideEvent* event) override;
 private:
  OpacitySlider* m_slider;
};
