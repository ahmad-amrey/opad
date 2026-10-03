#pragma once
// The drawing sheet editor (TODO 11 UI-78): a QGraphicsView over one sheet in paper millimetres (scene x = paper x, scene
// y = paper height - paper y), painted through the QPainter backend of the PDF and PNG output (drawing::paint), so the
// screen shows what prints. The paper stays white with black ink in both themes; the surround, hover and selection
// follow the theme.
// A sheet's parts come from a worker (AppDocument::readAsync, a job in the status bar): the views' frames first (layout),
// then the paper (template, title block, the sheet's own notes), then each view: a draft (PolyAlgo on meshes) when it was
// never projected as it is now, then its final linework with its dimensions. A part replaces what the canvas showed of it;
// until then the old one stays, moved to its new frame, marked "Updating…". A view with many primitives is drawn from a
// picture rendered on a worker at the zoom in use, rendered again 350 ms after the zoom stopped.
// Mouse: the wheel zooms about the cursor, the middle button (or Space with the left one) pans, a click selects a view
// (Ctrl adds), a drag on empty paper selects with a rectangle, a drag on a view moves it with the views projected from it:
// a base or pictorial view anywhere, its centre snapping to the other views' centres (guides shown) and to whole
// millimetres; a projected view only away from or towards its parent (its gap); one edit op on release. Keys: F fits the
// sheet, Del deletes the selected views, Esc ends a placement or clears the selection. The pointer snaps to what the
// sheet draws (snap.hpp: an index per part, built on the worker). Nothing here measures or projects on the UI thread.
// Annotations (UI-79): a click on an item (its box, worked out on the worker) selects it, a drag moves its text or symbol
// (one edit), Del deletes it; items that cannot be measured any more are reported (dangling). A tool (SheetInteraction)
// gets the mouse and keys first; it picks model edges through pickAt (the snap under the pointer traced back to the
// projection's body and edge) and shows what it would add through setPreview.
#include <QGraphicsView>
#include <QPointer>
#include <QTimer>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "opad/drawing/sheet.hpp"
#include "opad/drawing/snap.hpp"
#include "Jobs.hpp"

class AppDocument;
class Job;
class JobRunner;
class QGraphicsItem;
class SheetPaperItem;
class SheetViewItem;
class SheetGuides;

// What a tool on the canvas gets first (SheetAnnotate.hpp); false lets the canvas handle it.
class SheetInteraction {
 public:
  virtual ~SheetInteraction() = default;
  virtual bool mousePress(QMouseEvent*, const QPointF&) { return false; }
  virtual bool mouseMove(QMouseEvent*, const QPointF&) { return false; }
  virtual bool mouseRelease(QMouseEvent*, const QPointF&) { return false; }
  virtual bool keyPress(QKeyEvent*) { return false; }
  virtual bool wantsKey(QKeyEvent*) { return false; }  // a key the canvas takes before the window's shortcuts
  virtual bool active() const { return false; }         // a tool runs: no context menu, no view or item picking
};

// A pick on a view: the snap under the pointer and the projection curve it lies on, as the core takes it
// (drawing::pick_reference: node, edge or face, snap, at in sheet paper mm).
struct SheetPick {
  std::string view;
  opad::json pick;
  opad::drawing::Vec2 at{0, 0};  // sheet paper mm
  bool circle = false, line = false;
  opad::drawing::SnapKind kind = opad::drawing::SnapKind::Nearest;
  opad::drawing::Curve curve;  // the picked curve on the sheet (paper mm), to highlight it
};

class SheetCanvas : public QGraphicsView {
  Q_OBJECT
 public:
  SheetCanvas(AppDocument* doc, JobRunner* jobs, QWidget* parent = nullptr);
  ~SheetCanvas() override;
  // Edits (moves, new views) go through this: the window's own command path, which waits while the document is read;
  // `then` gets the command's result, or null when it was refused.
  using Runner = std::function<void(const std::string& command, const opad::json& args, std::function<void(const opad::json&)> then)>;
  void setRunner(Runner runner) { m_runner = std::move(runner); }

  void setSheet(const std::string& id);  // shows it, fitted the first time (empty: none)
  const std::string& sheet() const { return m_sheet; }
  void refresh();     // the document changed: from the scene at once, then from a worker (now when shown, else when shown next)
  // A command waits for the document: the worker stops at its next step and none starts until resume() (then the sheet is
  // drawn again). Counted: as many resumes as pauses.
  void pause();
  void resume();
  void fitSheet();
  std::vector<std::string> selectedViews() const;
  void selectViews(const std::vector<std::string>& ids);  // as the browser selected them; emits nothing
  // A new view placed with the mouse: a base view of an orientation (front, top, right, iso, ...) or a view projected from
  // `parent` to the side the cursor is on. Its size comes from a worker; done(true) once it was added, done(false) when
  // cancelled.
  void placeBase(const std::string& orient, std::function<void(bool)> done = {});
  void placeProjected(const std::string& parent, std::function<void(bool)> done = {});
  void cancelPlacement();
  bool placing() const { return m_place.active; }

  // Annotations (UI-79).
  void setInteraction(SheetInteraction* interaction) { m_interaction = interaction; }
  std::optional<SheetPick> pickAt(const QPointF& scene) const;  // a model edge or face of a view under the pointer
  std::string viewUnder(const QPointF& scene) const;            // the view whose frame holds the point, else empty
  void setPreview(std::shared_ptr<const opad::drawing::Display> preview);  // drawn over the sheet in sheet paper mm
  const std::shared_ptr<const opad::drawing::Display>& preview() const;
  void setPrompt(const QString& text) { emit promptChanged(text); }
  const std::vector<opad::drawing::ViewFrame>& frames() const { return m_frames; }  // as last laid out by the worker
  const opad::drawing::ViewFrame* frame(const std::string& view) const;
  // A worker that reads the document; while the sheet's own worker holds it, that one stops for it and resumes afterwards.
  using ReadWork = std::function<void(const opad::Document&, const opad::Scene&, Progress)>;
  void read(const QString& title, ReadWork work, std::function<void(bool ok, const QString& error)> done);
  std::vector<std::string> selectedItems() const { return m_selItems; }
  void selectItems(const std::vector<std::string>& ids);  // as the browser selected them; emits nothing
  std::string itemAt(const QPointF& scene) const;         // the smallest annotation whose box holds the point
  std::optional<QRectF> itemBox(const std::string& id) const;  // scene
  std::map<std::string, QString> dangling() const;        // items of the shown sheet that cannot be measured: why
  void benchDragItem(const std::string& id, opad::drawing::Vec2 delta);  // as a drag on the item (one edit)

  // State for benches and the status bar.
  struct ViewState {
    std::string id;
    QRectF frame;      // scene
    QRectF linework;   // scene: what its display draws
    bool draft = false, final = false, picture = false;  // picture: drawn from a picture of its current linework
    int prims = 0;
    QString error;
  };
  std::vector<ViewState> viewStates() const;
  int paperPrims() const;
  bool paperPictured() const;  // the paper drawn from a picture of its current drawing
  bool busy() const { return m_job || m_again || m_rendering > 0 || m_render.isActive(); }  // a part or a picture is still coming
  double pixelsPerMm() const { return transform().m11(); }
  QPointF toScene(const opad::drawing::Vec2& paper) const { return {paper[0], m_paperH - paper[1]}; }
  opad::drawing::Vec2 toPaper(const QPointF& scene) const { return {scene.x(), m_paperH - scene.y()}; }
  SheetViewItem* viewItem(const std::string& id) const;
  int draftsShown() const { return m_draftsShown; }  // draft parts shown since the sheet was set
  std::vector<std::pair<std::string, bool>> partLog;  // the views' parts in the order they were shown (id, draft)
  // Drags a view by `delta` paper mm through the same path as the mouse (snapping off): one edit, as on release.
  void benchDrag(const std::string& id, opad::drawing::Vec2 delta);
  void placeAt(opad::drawing::Vec2 paper);  // a click at that point while placing (the mouse, benches)
  // Snaps on what the sheet draws (ends, midpoints, centres, quadrants, crossings, the nearest point of a line), within 8
  // pixels: the pointer shows the one it would take and the cursor readout gives its point. kinds: SnapKind bits, 0 off.
  void setSnapKinds(unsigned kinds);
  unsigned snapKinds() const { return m_snapKinds; }
  std::optional<opad::drawing::Snap> snapAt(const QPointF& scene) const;  // its point in paper mm
  const std::optional<opad::drawing::Snap>& hoverSnap() const { return m_hoverSnap; }
  static QString snapName(opad::drawing::SnapKind kind);

 signals:
  void selectionChanged(const std::vector<std::string>& views);
  void cursorMoved(double x, double y, bool onPaper, const QString& snap);  // paper mm; snap: what the point snapped to
  void promptChanged(const QString& text);             // what a placement asks for; empty when none
  void partsArrived();                                 // a part of the sheet was shown
  void contextMenuRequested(const std::vector<std::string>& views, const QPoint& globalPos);  // views or items
  void deleteRequested(const std::vector<std::string>& views);  // views and items
  void itemSelectionChanged(const std::vector<std::string>& items);
  void danglingChanged();

 protected:
  bool event(QEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void keyReleaseEvent(QKeyEvent* e) override;
  void contextMenuEvent(QContextMenuEvent* e) override;
  void leaveEvent(QEvent* e) override;
  void drawBackground(QPainter* p, const QRectF& rect) override;
  void showEvent(QShowEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void scrollContentsBy(int dx, int dy) override;

 private:
  // An annotation as a click finds it (paper mm): its texts' and fills' boxes and its lines, within its bounds.
  struct ItemHit {
    std::string id;
    std::array<double, 4> bounds{0, 0, 0, 0};
    std::vector<std::array<double, 4>> boxes;
    std::vector<std::array<opad::drawing::Vec2, 2>> lines;
  };
  struct Part {
    enum Kind { Frames, Paper, View } kind = Frames;
    std::vector<opad::drawing::ViewFrame> frames;
    std::string id;
    std::shared_ptr<const opad::drawing::Display> display;
    std::array<double, 4> box{0, 0, 0, 0};     // the frame the view was drawn in (paper)
    std::array<double, 4> bounds{0, 0, 0, 0};  // the display's (measured on the worker)
    std::shared_ptr<const opad::drawing::SnapIndex> snaps;  // its curves' snaps (built on the worker)
    std::shared_ptr<const opad::drawing::ViewGeometry> geometry;  // a view's projection (its curves come first)
    std::vector<ItemHit> items;  // annotations drawn in it, to pick them
    std::vector<std::pair<std::string, std::string>> dangling;         // items it could not measure: why
    bool draft = false;
  };
  struct Outbox {
    std::mutex mu;
    std::vector<Part> parts;
  };
  struct Drag {
    bool active = false, moved = false;
    std::string id;
    QPointF start;                              // scene
    std::map<SheetViewItem*, QPointF> origins;  // the items that move and where they were
    QPointF delta;                              // scene, constrained
  };
  struct ItemDrag {
    bool active = false, moved = false;
    std::string id;
    QPointF start, delta;  // scene
  };
  struct Placement {
    bool active = false, projected = false, sized = false;
    std::string orient, parent, side;
    std::map<std::string, std::array<double, 2>> sizes;  // side (or "") -> frame size, paper mm
    QRectF ghost;                                       // scene
    opad::json source;                                  // a base view: what the sheet's first base view draws
    std::function<void(bool)> done;
    QPointer<Job> job;
  };
  void start();         // the worker for the current sheet
  void drain();         // shows the parts the worker has sent
  void apply(Part& part);
  void syncFromScene(); // view items for the scene's views of the sheet (O(records))
  void dropViews(const std::set<std::string>& keep);  // the view items not kept
  void setPaperSize(double w, double h);
  void renderLater();   // big views drawn again for the zoom in use, once it stops
  void renderPictures();
  void zoomBy(double factor, const QPointF& viewPos);
  QRectF sceneBox(const std::array<double, 4>& paper) const;
  std::vector<SheetViewItem*> family(SheetViewItem* item) const;  // it and every view projected from it, deep
  QPointF snapCentre(const QPointF& centre, const std::vector<SheetViewItem*>& moving, bool grid);
  void commitDrag();
  void updatePlacement(const QPointF& scenePos);
  void emitSelection();
  void hover(const QPointF& scene);  // the snap under the pointer and the cursor readout
  void commitItemDrag();
  void updateItemMarks();  // the selected items' boxes on the guides

  AppDocument* m_doc;
  JobRunner* m_jobs;
  Runner m_runner;
  QGraphicsScene* m_scene;
  SheetPaperItem* m_paper;
  SheetGuides* m_guides;
  std::map<std::string, SheetViewItem*> m_views;
  std::string m_sheet;
  double m_paperW = 420, m_paperH = 297;
  bool m_fitted = false, m_dirty = false, m_again = false, m_space = false, m_panning = false, m_quietSelection = false;
  QPoint m_panLast;
  QPointer<Job> m_job;
  std::shared_ptr<Outbox> m_outbox;
  QTimer m_poll, m_render;
  Drag m_drag;
  Placement m_place;
  int m_draftsShown = 0, m_paused = 0, m_rendering = 0;
  unsigned m_snapKinds = opad::drawing::kAllSnaps;
  std::optional<opad::drawing::Snap> m_hoverSnap;
  SheetInteraction* m_interaction = nullptr;
  std::vector<opad::drawing::ViewFrame> m_frames;
  std::vector<std::string> m_selItems;
  ItemDrag m_itemDrag;
  std::map<std::string, std::vector<ItemHit>> m_items;  // part ("" the paper) -> items
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> m_dangling;          // part -> dangling items
};
