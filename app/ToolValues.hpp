#pragma once
// A tool's typed values outside the sketch (TODO 11 UI-122): a feature panel, the section, the drawing placer. While the
// tool has boxes (`fields`), a value key (a digit, the keypad's too, the point, a comma, a sign, a bracket) or Tab typed
// over the view or one of its tool panels, not into a text field, goes into them and never to a window shortcut (the
// filters' 1-4, the styles' 5-7): the sketch's contract (DynamicInput). The boxes show beside the pointer once a key
// starts them; what is typed reaches the tool at once (`edited`), Esc undoes the edit, then puts the old values back,
// then is the tool's (`escape`); Enter is the tool's `commit`. A handle with boxes of its own (the extrude's arrow) takes
// the keys while it shows.
#include <QList>
#include <QObject>
#include <QPointer>
#include <functional>

#include "DynamicInput.hpp"

class DimensionHandle;
class QKeyEvent;

class ToolValues : public QObject {
  Q_OBJECT
 public:
  ToolValues(QWidget* view, QObject* parent);
  std::function<QList<DynamicInput::Field>()> fields;  // the boxes now (option boxes, live = the value); empty: no keys taken
  static DynamicInput::Field box(const QString& key, const QString& label, const QString& value);  // such a box
  std::function<void(const QString& key, const QString& value)> edited;  // typed into a box, or the value before put back
  std::function<void()> commit;                                          // Enter in a box
  std::function<void()> escape;                                          // Esc with nothing typed left
  void setHandle(DimensionHandle* handle);  // takes the keys while it shows
  DynamicInput* input() const { return m_input; }
  bool takes(const QKeyEvent* key) const;  // a key that types into the tool now
  void refresh();                          // the fields again (which show, their values)
  void reset();                            // the tool ended: typed values forgotten, the boxes hidden
 protected:
  bool eventFilter(QObject* target, QEvent* event) override;
 private:
  void type(const QKeyEvent* key);
  void show();
  QWidget* m_view;
  DynamicInput* m_input;
  QPointer<DimensionHandle> m_handle;
};
