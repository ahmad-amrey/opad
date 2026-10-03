#pragma once
// The Compare panel (UI-58), the content of the "compare" ToolPanel: which two versions (A, the one compared with; B, the
// one the view shows), how much of each to see (A <-> B), which kinds of change to show (legend chips with their counts
// and an eye each), the changes grouped by what changed (] and [ step through them) and a table of what the current one
// changed, before and after. CompareMode fills it and does what it asks.
#include <QAbstractButton>
#include <QString>
#include <QWidget>
#include <array>
#include <vector>

#include "Theme.hpp"
#include "opad/json.hpp"

class PanelFooter;
class QComboBox;
class QLabel;
class QSlider;
class QTableWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

struct CompareVersion {
  enum class Kind { None, Session, Saved, Git, Recovery, File };
  Kind kind = Kind::None;
  QString ref;     // Saved, File: the path; Git: the commit; Recovery: the snapshot's file
  QString label;   // as the pickers show it
  QString detail;  // their tooltip: the whole hash, author, time, path
  bool operator==(const CompareVersion& o) const { return kind == o.kind && ref == o.ref; }
};

// One legend entry: the state's mark and colour, its name, the count and an eye (checked: shown).
class LegendChip : public QAbstractButton {
  Q_OBJECT
 public:
  LegendChip(const char* state, QWidget* parent = nullptr);
  void setCount(int n);
  int count() const { return m_count; }
  QSize sizeHint() const override;
 protected:
  void paintEvent(QPaintEvent*) override;
 private:
  const theme::Cue* m_cue;
  QString m_state;
  int m_count = 0;
};

class ComparePanel : public QWidget {
  Q_OBJECT
 public:
  enum Category { Added, Removed, Modified, Moved, Unchanged, Categories };
  static QColor Tokens::* colour(Category c);  // the diff role, ghost for Unchanged
  explicit ComparePanel(QWidget* parent = nullptr);
  // The pickers' versions (both list the same ones, then Other file…) and the chosen pair.
  void setVersions(const std::vector<CompareVersion>& versions, int a, int b);
  void setBusy(const QString& status);    // reading or comparing: the list stays as it was
  void setFailed(const QString& error);
  void setResult(const opad::json& diff, const std::array<int, Categories>& counts);
  void clear();
  // The current change, by its index in the diff's changes: its row selected and scrolled to, its table shown. No signal.
  void setCurrent(int change);
  int current() const { return m_current; }
  std::vector<int> order() const { return m_order; }  // the changes in the list's order (] and [)
  bool shown(Category c) const;
  int emphasis() const;  // 0 = A only, 100 = B only
  // Overlay (B over A in one view, weighted by the emphasis) or side by side (A's view left of B's): no signal.
  void setSideBySide(bool on);
  bool sideBySide() const;
  QString status() const;
  QSize preferredSize(int width) const;
  // The list and the table as the panel has them, for other lists of changes (the Recovery offer, UI-59): makeList and
  // makeDetails set them up; listChanges fills the list with a diff's changes grouped by what changed, a row each (its
  // mark, colour and words; rows by change index, order the rows' order); fillDetails shows what one change changed.
  static QTreeWidget* makeList(QWidget* parent);
  static QTableWidget* makeDetails(QWidget* parent);
  static void listChanges(QTreeWidget* list, const opad::json& changes, std::vector<QTreeWidgetItem*>& rows, std::vector<int>& order);
  static void fillDetails(QTableWidget* table, const opad::json& change);
  // benches
  LegendChip* chip(Category c) const { return m_chips[c]; }
  QSlider* slider() const { return m_slider; }
  QTreeWidget* list() const { return m_list; }
  QTableWidget* details() const { return m_details; }
  PanelFooter* footer() const { return m_footer; }
  QComboBox* picker(int side) const { return side == 0 ? m_pickA : m_pickB; }
  QToolButton* layoutButton(bool sideBySide) const { return sideBySide ? m_sideBySide : m_overlay; }
 signals:
  void versionsChosen(int a, int b);  // indices into the versions
  void layoutChosen(bool sideBySide);
  void otherFileRequested(int side);  // 0 = A, 1 = B
  void swapRequested();
  void emphasisChanged(int value);
  void categoryToggled(int category, bool shown);
  void changeActivated(int change);  // a row clicked, or stepped to
  void stepRequested(int delta);     // Previous / Next
  void doneRequested();
  void contentResized();
 private:
  void picked(int side);
  QComboBox *m_pickA, *m_pickB;
  QToolButton* m_swap;
  QToolButton *m_overlay, *m_sideBySide;
  QWidget* m_weight;  // the slider's row
  QSlider* m_slider;
  std::array<LegendChip*, Categories> m_chips{};
  QLabel* m_summary;
  QLabel* m_status;
  QTreeWidget* m_list;
  QTableWidget* m_details;
  PanelFooter* m_footer;
  opad::json m_changes = opad::json::array();
  std::vector<int> m_order;
  std::vector<QTreeWidgetItem*> m_rows;  // by change index
  int m_current = -1;
  int m_versions = 0;
};
