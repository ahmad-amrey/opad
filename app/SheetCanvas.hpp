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
// sheet, Del deletes the selected views, Esc ends a placement or clears the selection. Nothing here measures or projects
// on the UI thread.
#include <QGraphicsView>
#include <QPointer>
#include <QTimer>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "opad/drawing/sheet.hpp"

class AppDocument;
class Job;
class JobRunner;
class QGraphicsItem;
class SheetPaperItem;
class SheetViewItem;
class SheetGuides;

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

 signals:
  void selectionChanged(const std::vector<std::string>& views);
  void cursorMoved(double x, double y, bool onPaper);  // paper mm
  void promptChanged(const QString& text);             // what a placement asks for; empty when none
  void partsArrived();                                 // a part of the sheet was shown
  void contextMenuRequested(const std::vector<std::string>& views, const QPoint& globalPos);
  void deleteRequested(const std::vector<std::string>& views);

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
  void drawBackground(QPainter* p, const QRectF& rect) override;
  void showEvent(QShowEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void scrollContentsBy(int dx, int dy) override;

 private:
  struct Part {
    enum Kind { Frames, Paper, View } kind = Frames;
    std::vector<opad::drawing::ViewFrame> frames;
    std::string id;
    std::shared_ptr<const opad::drawing::Display> display;
    std::array<double, 4> box{0, 0, 0, 0};     // the frame the view was drawn in (paper)
    std::array<double, 4> bounds{0, 0, 0, 0};  // the display's (measured on the worker)
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
};
