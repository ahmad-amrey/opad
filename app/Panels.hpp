#pragma once
// Secondary UI, styled per design_handoff_opad_desktop_ui: browser (F27), properties (F24), annotations (F32),
// section tab (F20), timeline (F28), measurement card (F23), command search (F30), shortcut editor (F31).
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QWidget>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "AppDocument.hpp"

// ---------------------------------------------------------------- dock header (28 px: title 500 fg2, float/close 16 px)
class QDockWidget;
class DockHeader : public QWidget {
  Q_OBJECT
 public:
  DockHeader(const QString& title, QDockWidget* dock);
  void setTitle(const QString& t);
  // QDockWidget places its content below the title bar's *size hint*, so it must match the fixed 28 px.
  QSize sizeHint() const override { return QSize(QWidget::sizeHint().width(), 28); }
  QSize minimumSizeHint() const override { return QSize(0, 28); }
 private:
  QLabel* m_title;
};

// ---------------------------------------------------------------- browser
class BrowserTree : public QTreeWidget {
  Q_OBJECT
 public:
  explicit BrowserTree(AppDocument* doc, QWidget* parent = nullptr);
  using QTreeWidget::indexFromItem;  // BrowserPanel selects in one batch through the selection model
 signals:
  void reparentRequested(const std::vector<std::string>& ids, const std::string& parent, int index);
  void eyeClicked(const std::string& id);
  void swatchClicked(const std::string& id);
 protected:
  void dropEvent(QDropEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override;
 private:
  AppDocument* m_doc;
};

class BrowserDelegate : public QStyledItemDelegate {
  Q_OBJECT
 public:
  explicit BrowserDelegate(AppDocument* doc, QObject* parent = nullptr) : QStyledItemDelegate(parent), m_doc(doc) {}
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(100, 28); }
  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
 private:
  AppDocument* m_doc;
};

class BrowserPanel : public QWidget {
  Q_OBJECT
 public:
  explicit BrowserPanel(AppDocument* doc, QWidget* parent = nullptr);
  std::vector<std::string> selectedIds() const;
  void setSelectedIds(const std::vector<std::string>& ids);
  void startRename(const std::string& id);
  void focusFilter();

 signals:
  void selectionChanged(const std::vector<std::string>& ids);
  void contextMenuRequested(const QPoint& globalPos, const std::vector<std::string>& ids);
  void fitRequested(const std::vector<std::string>& ids);

 public slots:
  void rebuild();

 private:
  void applyFilter();
  void updateBreadcrumb();
  QTreeWidgetItem* itemFor(const std::string& id) const;
  QTreeWidgetItem* build(const std::string& id, QTreeWidgetItem* parent, std::set<std::string>& expanded);

  AppDocument* m_doc;
  BrowserTree* m_tree;
  QLineEdit* m_filter;
  QLabel* m_breadcrumb;
  QLabel* m_empty;
  std::unordered_map<std::string, QTreeWidgetItem*> m_index;  // node id -> item, rebuilt with the tree
  bool m_updating = false;
};

// ---------------------------------------------------------------- properties
class PropertiesPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PropertiesPanel(QWidget* parent = nullptr);
  void showEntity(const QString& title, const QString& subtitle, const QString& id, const opad::json& props);
  void clear();
 signals:
  void faceChosen(int index);
 private:
  void addRow(const QString& key, const opad::json& v);
  QLabel* m_title;
  QLabel* m_id;
  QLabel* m_subtitle;
  QTreeWidget* m_table;
};

// ---------------------------------------------------------------- annotations
class AnnotationsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit AnnotationsPanel(AppDocument* doc, QWidget* parent = nullptr);
  std::string currentOpId() const { return m_current; }
 signals:
  void selectNode(const std::string& id);
  void addRequested();
  void resolveRequested(const std::string& opId);
  void restoreRequested(const std::string& opId);
 public slots:
  void rebuild();
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  AppDocument* m_doc;
  QComboBox* m_author;
  QComboBox* m_status;
  QLabel* m_count;
  QWidget* m_cards;
  std::string m_current;
};

// ---------------------------------------------------------------- section
class SectionPanel : public QWidget {
  Q_OBJECT
 public:
  explicit SectionPanel(AppDocument* doc, QWidget* parent = nullptr);
  bool enabled() const { return m_enabled; }
  opad::Vec3 origin() const;
  opad::Vec3 normal() const;
  bool caps() const;
 signals:
  void planeChanged();
  void saveRequested(const QString& name, const opad::Vec3& origin, const opad::Vec3& normal);
  void enabledChanged(bool on);
 public slots:
  void setEnabled(bool on);
  void flip();
  void setFromFace(const opad::Vec3& origin, const opad::Vec3& normal);
  void rebuild();
  void applyNamed(const std::string& id);
 private:
  void emitChange();
  AppDocument* m_doc;
  bool m_enabled = false;
  int m_axis = 2;
  bool m_flip = false;
  bool m_pick = false;
  opad::Vec3 m_pickOrigin{0, 0, 0}, m_pickNormal{0, 0, 1};
  QList<QToolButton*> m_axisButtons;
  QSlider* m_slider;
  QLineEdit* m_value;
  QToolButton* m_flipButton;
  QToolButton* m_capButton;
  QListWidget* m_named;
  QLabel* m_state;
};

// ---------------------------------------------------------------- measurement card (F23)
class MeasureCard : public QFrame {
  Q_OBJECT
 public:
  explicit MeasureCard(QWidget* parent = nullptr);
  void setResult(const opad::json& result, const QStringList& targets);
 signals:
  void pinRequested();
  void clearRequested();
 private:
  QLabel* m_title;
  QLabel* m_value;
  QLabel* m_deltas;
  QLabel* m_targets;
};

// ---------------------------------------------------------------- viewport chips
class ViewportChips : public QWidget {
  Q_OBJECT
 public:
  explicit ViewportChips(QWidget* parent = nullptr);
  void set(const QString& mode, const QString& projection, const QString& section);
 private:
  QLabel* m_mode;
  QLabel* m_proj;
  QLabel* m_section;
};

// ---------------------------------------------------------------- timeline
class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(AppDocument* doc, QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(400, 48); }
  QSize minimumSizeHint() const override { return QSize(100, 48); }
  void setCurrentOp(const std::string& id);
  std::string currentOp() const { return m_current; }
  void step(int delta);
  QString describe(const opad::Op& op) const;

 signals:
  void opClicked(const std::string& opId);
  void contextRequested(const std::string& opId, const QPoint& globalPos);

 public slots:
  void rebuild();

 protected:
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;

 private:
  int indexAt(const QPoint& p) const;
  QRect markerRect(int i) const;
  bool isUnresolved(const std::string& opId) const;
  AppDocument* m_doc;
  std::set<std::string> m_deleted, m_unresolved;
  int m_hover = -1;
  std::string m_current;
  QRect m_prevBtn, m_nextBtn;
};

// ---------------------------------------------------------------- command search (F30)
class CommandPalette : public QDialog {
  Q_OBJECT
 public:
  CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr);
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  void refill(const QString& filter);
  void runCurrent();
  QList<QAction*> m_actions;
  QLineEdit* m_edit;
  QListWidget* m_list;
};

class ShortcutEditor : public QDialog {
  Q_OBJECT
 public:
  ShortcutEditor(const QList<QAction*>& actions, QWidget* parent = nullptr);
  void accept() override;
 private:
  QList<QAction*> m_actions;
  QTreeWidget* m_tree;
};

// ---------------------------------------------------------------- progress strip (bottom of the window)
// Shown in the status bar for any operation that takes time. Carries a phase bar and, for multi-phase
// work like opening a file, an overall bar too, plus a Cancel button.
class QProgressBar;
class QPushButton;
class ProgressStrip : public QWidget {
  Q_OBJECT
 public:
  explicit ProgressStrip(QWidget* parent = nullptr);
  void begin(const QString& title, bool twoBars);
  void setPhase(const QString& text, int percent);  // percent < 0: indeterminate
  void setOverall(int percent);
  void finish();
 signals:
  void cancelRequested();
 private:
  void setTitle(const QString& text);
  QLabel* m_title;
  QProgressBar* m_phaseBar;
  QLabel* m_phasePct;
  QLabel* m_overallLabel;
  QProgressBar* m_overallBar;
  QLabel* m_overallPct;
  QPushButton* m_cancel;
};

QString opTypeIcon(const std::string& type);
QString opGroup(const QAction* a);
