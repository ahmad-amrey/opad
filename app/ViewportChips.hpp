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
  // Viewer mode: "Viewer · read-only" first in the row, then Save to edit; a read-only OPAD `document`: "Read-only", then
  // Save a copy to edit. An empty file hides them.
  void setViewer(const QString& file, bool document = false);
  // The display style and projection chips, 3D settings that a drawing in 2D mode has no use for (UI-118).
  void setDisplayChips(bool shown);
  // A feature area's chip after the built-in ones (a QLabel named "chip", or "chipSel" for a mode the view is in; a
  // QToolButton "chipAction"), e.g. the active component or an exploded view. The area shows, hides and words it; the row
  // fits itself to it. The row is a native child of the viewport, so the chip is opaque and never translucent.
  void addChip(QWidget* chip);
  QLabel* isolationChip() const { return m_isolate; }  // benches: "Isolated · n bodies  ×", clicked to end it
 signals:
  void leaveTwoDimensional();  // the 2D mode card was clicked
  void exitIsolation();        // the isolation card (its ×) was clicked
  void saveToEditRequested();
 protected:
  bool eventFilter(QObject* object, QEvent* event) override;
  bool event(QEvent* event) override;
 private:
  void keyTexts();  // the tooltips that name keys, as bound now (keys::notifier)
  bool m_areaChips = false, m_document = false;
  QLabel* m_mode;
  QLabel* m_proj;
  QLabel* m_twoD;
  QLabel* m_section;
  QLabel* m_isolate;
  QLabel* m_viewer;
  QToolButton* m_saveToEdit;
};
