#pragma once
class QScrollBar;
// Secondary UI, styled per design_handoff_opad_desktop_ui: browser (F27), properties (F24), annotations (F32),
// section tab (F20), timeline (F28), command search (F30), shortcut editor (F31).
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>
#include <functional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "AppDocument.hpp"
#include "Theme.hpp"
#include "ShortcutEditor.hpp"

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

// ---------------------------------------------------------------- floating tool panel
// Replaces the fixed right dock (handoff: "Floating tool panel"). A frameless tool window owned by the main
// window, so it sits over the OpenGL viewport and hides with it. Header 32 px (drag handle; double-click puts
// it back at its default place): icon, title 500, context fg3, pin, close. Anchored to the viewport's top-right
// corner: MainWindow calls anchorTo() whenever the viewport moves. Place and size persist per panel id.
class ToolPanel : public QWidget {
  Q_OBJECT
 public:
  ToolPanel(const QString& id, const QString& icon, QColor Tokens::* tint, const QString& title, QWidget* content, int preferredHeight, QWidget* owner);
  void setContext(const QString& text);
  void setEscapeHandler(std::function<void()> handler) { m_escapeHandler=std::move(handler); }
  void setHeader(const QString& icon, const QString& title);  // one panel serves every guided tool
  QWidget* content() const { return m_content; }
  void setPinnable(bool on);  // an editor's panel has no pin: it lives exactly as long as the editor
  // The default height for this content (header excluded). A panel the user has not sized follows it, never taller
  // than the viewport below its top, so it stays under the view cube.
  void setDefaultHeight(int contentHeight);
  bool pinned() const { return m_pin->isChecked(); }
  bool userPlaced() const { return m_userPlaced; }
  void setDefaultTop(int top) { if (!m_userPlaced) m_offset.setY(top); }
  int bottom() const { return m_offset.y() + height() - 2 * kMargin; }  // in viewport coordinates
  void anchorTo(const QRect& viewportGlobal);
  // Opt-in content fitting for inspect results; other floating panels keep their saved sizing.
  void setContentSizeHint(std::function<QSize(int)> hint);
  void requestContentFit();
  static constexpr int kMargin = 6;  // translucent rim the shadow is painted in
 signals:
  void visibilityChanged(bool visible);
 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void showEvent(QShowEvent*) override { emit visibilityChanged(true); }
  void hideEvent(QHideEvent*) override { emit visibilityChanged(false); }
 private:
  friend class ToolPanelGrip;
  void userPlacedNow();  // after a drag or a grip resize: remember where the panel is
  void refreshIcons();
  std::function<void()> m_escapeHandler;
  QString m_id, m_iconName;
  QColor Tokens::* m_tint;  // header icon colour: sel for inspect tools, amber for annotations, fg2 for selection
  QLabel *m_icon, *m_name, *m_context;
  QWidget* m_content;
  QToolButton *m_pin, *m_close;
  QWidget* m_grip;
  QRect m_anchor;                    // the viewport, global
  QPoint m_offset{8, 186};           // frame's top-right corner: x px left of the viewport's right edge, y px below its top
  QSize m_defaultSize;
  std::function<QSize(int)> m_contentSizeHint;
  bool m_contentFitPending = false;
  bool m_userPlaced = false;
  bool m_dragging = false;
  bool m_resizing = false;
  QPoint m_dragFrom, m_posFrom;
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
  void selectIds(const std::vector<std::string>& ids);  // like a click: selects and emits selectionChanged
  void selectParent();  // replaces the selection by its parents
  void expandAll();
  void collapseAll();  // everything but the document row
  void scrollToSelected();
  void setViewerMode(bool on);  // no rename or drag-to-reparent (colours stay: a view setting)
  void setEditedSketch(const std::string& id,const QString& name,bool visible);

 signals:
  void selectionChanged(const std::vector<std::string>& ids);
  void contextMenuRequested(const QPoint& globalPos, const std::vector<std::string>& ids);
  void fitRequested(const std::vector<std::string>& ids);
  void autoHideChanged(bool on);
  void sketchActivated(const std::string& sketchId);  // double-click on a sketch row: edit it
  void editedSketchVisibilityRequested();

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
  QToolButton *m_parentBtn, *m_locateBtn, *m_expandBtn, *m_collapseBtn;
  QLabel* m_empty;
  std::unordered_map<std::string, QTreeWidgetItem*> m_index;  // node id -> item, rebuilt with the tree
  bool m_updating = false;
  bool m_viewer = false;
  std::string m_editedSketch;
  QString m_editedName;
  bool m_editedVisible=true;
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
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  void addRow(const QString& key, const opad::json& v);
  void fill();
  opad::json m_props;      // what is shown, kept to lay the rows out again when the width changes
  int m_filledWidth = -1;  // value column width the rows were laid out for
  bool m_splitVectors = false;  // some vector did not fit on one line at that width
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
  void typeFilterChanged(const std::string& type);
  void resolveRequested(const std::string& opId);
  void restoreRequested(const std::string& opId);
  void styleRequested(const std::string& opId, const std::string& style);  // re-tag: an edit op
 public slots:
  void rebuild();
 private:
  AppDocument* m_doc;
  QComboBox* m_author;
  QComboBox* m_type;
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
  bool pickRange(double& dmin, double& dmax) const;  // the model's extent along the picked normal
  opad::Vec3 normal() const;
  bool caps() const;
  bool picking() const { return m_pick; }  // "Pick face" is active: the next planar face picked in the view sets the plane
  void beginPick();                          // what the "Pick face" button does
 signals:
  void planeChanged();
  void pickRequested();  // the user wants to pick a face: switch the select filter to faces
  void saveRequested(const QString& name, const opad::Vec3& origin, const opad::Vec3& normal);
  void enabledChanged(bool on);
 public slots:
  void setEnabled(bool on);
  void flip();
  void setFromFace(const opad::Vec3& origin, const opad::Vec3& normal);
  void setOrigin(const opad::Vec3& origin);  // the plane was dragged in the view: move the slider to it (clamped to the model)
  void rebuild();
  void applyNamed(const std::string& id);
 private:
  void emitChange();
  void setAlong(double along);  // slider from a distance along the axis (or the picked normal)
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

// ---------------------------------------------------------------- timeline
class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(AppDocument* doc, QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(400, 48); }
  QSize minimumSizeHint() const override { return QSize(100, 48); }
  void setCurrentOp(const std::string& id);
  std::string currentOp() const { return m_current; }
  // The op an open sketch or feature editor changes: marked, and what follows it dimmed, since the edit applies from
  // that point in the history.
  void setEditingOp(const std::string& id);
  void step(int delta);
  std::vector<std::string> shownOps() const;  // the ops drawn as markers, in order (benches)
  QString describe(const opad::Op& op) const;

 signals:
  void opClicked(const std::string& opId);
  void opActivated(const std::string& opId);  // double-click: edit a feature or a sketch
  void contextRequested(const std::string& opId, const QPoint& globalPos);

 public slots:
  void rebuild();

 protected:
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;

 private:
  void updateScrollRange();
  void ensureCurrentVisible();
  QScrollBar* m_scroll;
  QString iconFor(const opad::Op& op) const;
  int indexAt(const QPoint& p) const;
  QRect markerRect(int i) const;
  bool isUnresolved(const std::string& opId) const;
  AppDocument* m_doc;
  std::set<std::string> m_deleted, m_unresolved;
  std::vector<size_t> m_shown;  // indices into doc.ops drawn as markers (see timelineShows)
  int m_hover = -1;             // marker index (into m_shown)
  std::string m_current, m_editing;
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

// ---------------------------------------------------------------- loading shade
// While a file loads, a frameless translucent tool window (owned by the main window, so it stays above it and
// hides with it) covers the workspace: docks and the OpenGL viewport alike, which a child widget could not
// do. It darkens everything, shows a spinner and swallows mouse input; shortcuts are held back by MainWindow.
class LoadShade : public QWidget {
  Q_OBJECT
 public:
  explicit LoadShade(QWidget* owner);
  void place(const QRect& globalArea, const QPoint& spinnerCentreGlobal);  // cover this screen area, spinner here
 protected:
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
 private:
  QTimer m_timer;
  int m_angle = 0;
  QPoint m_spinner;  // local
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
 protected:
  void resizeEvent(QResizeEvent* e) override;
 private:
  void setTitle(const QString& text);
  QString m_fullTitle;
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
