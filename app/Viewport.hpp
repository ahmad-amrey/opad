#pragma once
#include "CursorWrap.hpp"
// The 3D viewport: OCCT AIS/V3d rendering inside a native Qt widget, driven by AIS_ViewController
// (navigation gestures, hover pre-highlight, click/rubber-band selection, view-cube animation).
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <V3d_View.hxx>
#include <SelectMgr_SelectionManager.hxx>
#include <V3d_Viewer.hxx>

#include <QImage>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "AppDocument.hpp"
#include "BodyLook.hpp"
#include "BodyShape.hpp"
#include "Theme.hpp"
#include "Tracking.hpp"

class JobRunner;
class Job;
class QKeyEvent;
class QNativeGestureEvent;

// Sketch editing (SketchEditor) takes the left mouse button and the keyboard while it is active; positions
// arrive in sketch-plane coordinates. Navigation (middle/right button, wheel, view cube) stays with the view.
class SketchInput {
 public:
  virtual ~SketchInput() = default;
  virtual void sketchPress(double u, double v, Qt::KeyboardModifiers mods) = 0;
  virtual void sketchMove(double u, double v, Qt::KeyboardModifiers mods, bool dragging) = 0;
  virtual void sketchRelease(double u, double v, Qt::KeyboardModifiers mods) = 0;
  virtual void sketchDoubleClick(double u, double v) = 0;
  virtual bool sketchKey(QKeyEvent* e) = 0;  // true = handled
  virtual void sketchLeave() {}               // the pointer left the view: nothing is hovered any more
};

class Viewport : public QWidget, protected AIS_ViewController {
  Q_OBJECT
 public:
  enum class NavPreset { Fusion, SolidWorks, Onshape, Blender };
  enum class Style { Shaded, ShadedEdges, Wireframe };
  enum class SelFilter { Body, Face, Edge, Vertex };

  explicit Viewport(AppDocument* doc, QWidget* parent = nullptr);
  ~Viewport() override;

  void setTokens(const Tokens& t);
  void setNavPreset(NavPreset p);
  NavPreset navPreset() const { return m_preset; }
  void setStyle(Style s);
  Style style() const { return m_style; }
  void setGrid(bool on);
  void configureGrid(double spacing,double extent);
  void setSelectThrough(bool on) {m_selectThrough=on;}
  void UpdateRubberBand(const Graphic3d_Vec2i& from,const Graphic3d_Vec2i& to) override;
  void setGridSnap(bool on) { m_gridSnap=on; }
  bool gridSnap() const { return m_gridSnap; }
  double gridStep() const { return m_gridStep; }
  Bnd_Box benchGridBox() const;  // where the grid is drawn (OCCT's structure, world box), for benches
  Bnd_Box benchFitBox() const { return fitBounds(false); }  // what Fit frames, for benches
  opad::json circleInfo(const opad::Ref& ref) const;
  void setShadows(bool on);
  void setRenderQuality(int level);
  static int savedRenderQuality();
  static int savedSceneBackground();
  void setSceneBackground(int style);
  void setHoverFade(bool enabled,double seconds);
  void resetHoverFade();
  void setTwoDimensional(bool on);
  bool twoDimensional() const { return m_twoDimensional; }
  void setTracking(bool on);
  void setExtensionTracking(bool on);
  void setOrthographic(bool ortho);
  bool isOrthographic() const;
  void setSelectionFilter(SelFilter f);
  SelFilter selectionFilter() const { return m_filter; }

  void fitAll();
  void requestRefinement() { m_refineTimer.start(); }  // zoom refinement without waiting for a frame (benches)
  bool benchLeave();  // OPAD_BENCH_LEAVE: hover a body, leave the view, nothing may stay highlighted
  void fitWhenReady();   // fit now if bodies are displayed, otherwise once the first meshes arrive
  void fitNodesWhenReady(std::vector<std::string> ids);
  void cancelMeshing();  // stop tessellating the remaining bodies (they stay hidden until resetMeshing)
  void resetMeshing();
  // A viewer document became editable: the same shapes under content keys. What is meshed and drawn carries over.
  void renameBodyKeys(const std::map<std::string, std::string>& keys);
  int skippedCount() const { return static_cast<int>(m_meshSkipped.size()); }
  void fitSelection();
  void fitNodes(const std::vector<std::string>& ids);
  void standardView(const QString& name);
  void home();
  void rollView(double degrees);  // animated turn about the view axis; positive = counter-clockwise on screen

  void warmUp();  // create the OpenGL viewer now rather than on first paint
  void setBlocked(bool on);  // while a file loads: mouse input is ignored (the shade window covers the view)
  void benchShot(const QString& path);  // --bench-select with OPAD_BENCH_SHOT: hover the view cube, save a frame
  std::string benchHeaviest() const;       // OPAD_BENCH_FILTER: the body with the most faces, the pick target
  void benchBand();                        // OPAD_BENCH_BAND: rubber band over the whole view in the current mode
  void benchSubShot(const QString& path);  // OPAD_BENCH_SUBSHOT: frame from behind the picked sub-shape (X-ray check)
  void benchClick(double fx, double fy);  // OPAD_BENCH_TOOL: a left click at this fraction of the view, as the mouse handlers deliver it
  bool benchPicking();  // OPAD_BENCH_PICKING: circle discovery, locking, exact picks and orbit regression
  // OPAD_BENCH_TRACKING (ViewportTrackingBench.cpp): what is behind a face is never hovered, acquired or offered (UI-31)
  bool benchTracking(const QString& prefix, bool endsOnly = false);
  // OPAD_BENCH_CROSSLOCK (ViewportCrossLockBench.cpp): lock on one anchor, acquire another, take where they line up (UI-32)
  bool benchCrossLock(const QString& prefix);
  void benchPick();  // --bench-select: pick at the view centre through the context and log what it hit
  void setJobs(JobRunner* jobs);  // long operations (selection, mode switches) run through the app's JobRunner
  std::vector<opad::Ref> selection() const;
  // Highlights the given nodes' bodies as a sliced job; emits selectionApplied() when it has settled. Sets
  // that would take longer than ~0.5 s to highlight are shown as translucent boxes instead.
  void selectNodes(const std::vector<std::string>& ids);
  void clearSelection();  // emits selectionChanged() once the un-highlight has settled
  // Isolate mode: exactly these nodes' bodies are shown, whatever their visibility flags say, until
  // isolate({}) or until none of them exists any more (all deleted). isolationChanged() reports both.
  void isolate(const std::vector<std::string>& ids);  // empty = exit the mode
  bool isIsolated() const { return !m_isolated.empty(); }
  std::vector<std::string> isolatedNodes() const {return {m_isolated.begin(),m_isolated.end()};}
  int isolatedCount() const { return static_cast<int>(m_isolated.size()); }
  int displayedCount() const { return static_cast<int>(m_items.size()); }

  // Per-body looks (ViewportLooks.cpp, UI-121): each source owns one layer of deltas by node id (a component's covers the
  // bodies under it; the nearest entry wins), composed over the document's appearance in LookSource order (BodyLook.hpp)
  // and applied by a sliced job, aspects in place, never Redisplay. looksApplied() once every displayed body shows it.
  // A sketch (no component) takes the entries under its own id: visible, colour, opacity or ghost (faded lines), pickable.
  void setLookLayer(LookSource source, std::map<std::string, LookDelta> deltas);
  void clearLookLayer(LookSource source) { setLookLayer(source, {}); }
  void setGhostsPickable(bool on);  // feature inputs, sketch Project, measuring: ghosts can be picked as references
  bool ghostsPickable() const { return m_ghostsPickable; }
  BodyLook bodyLook(const std::string& body) const;  // as composed now (whether displayed yet or not); a sketch's too
  BodyLook shownLook(const std::string& body) const;  // as applied to the displayed body or sketch (the default look if none)
  QString hoverName(const std::string& node) const;   // the status text of a hovered node: "Lid (inactive)" for a ghost
  bool looksPending() const { return m_lookJob != nullptr || !m_lookQueue.empty(); }
  opad::json benchLookState(const std::string& body) const;  // OPAD_BENCH_LOOKS: what AIS holds for a displayed body or sketch
  std::string benchPickAt(int x, int y, opad::Vec3* at = nullptr);  // the body picking finds at this point of the view (device pixels), "" none
  bool benchBodyPoint(const std::string& body, int& x, int& y);  // a point of the view where picking finds this body

  // Section: the clip plane, and its gizmo (ViewportSection.cpp): the plane's outline over the model, edges only,
  // sized to the model's extent in the plane. A strip inside each side is a drag handle: hovering it shows a
  // two-headed arrow along the normal, dragging moves the plane and reports the new origin (sectionDragged).
  void setSection(bool enabled, const opad::Vec3& origin, const opad::Vec3& normal, bool caps = true);
  bool sectionEnabled() const { return m_sectionEnabled; }
  int sectionHover() const { return m_sectionHover; }      // side whose handle is under the mouse, -1 = none
  bool benchSectionHandle(QPointF& at) const;              // OPAD_BENCH_SECTION: a widget point on a handle strip
  void showMeasurement(const opad::json& result);
  void setMeasurementComponents(bool on);
  bool measurementComponents() const { return m_measureComponents; }
  bool measurementHasMultipleAxes() const;
  void clearDimension();
  void setMeasurementSelectionLocked(bool locked) { m_measureSelectionLocked = locked; }
  QStringList measurementCaptions() const { return m_measureCaptions; }  // the labels as drawn (benches)

  // Guided tools (distance, angle, ...: the tool asks for one pick per step). While accumulating, a plain click
  // adds to the selection (or takes a picked item out again) instead of replacing it, so selection() is the
  // tool's ordered pick list.
  void setPickAccumulate(bool on, bool retainPicks = false);
  void deselectLast();      // one step back
  void keepLastSelected();  // a pick after the last step starts over from that pick
  bool lastPickPoint(opad::Vec3& p) const;  // where the last click hit the geometry
  bool lastClickHit() const { return m_hasLastPick; }  // false: the last click was on empty space
  void showPickMarkers(const std::vector<opad::Vec3>& points);  // numbered end markers, 1-based
  void showPreview(const opad::Vec3& a, const opad::Vec3& b, const QString& label);  // dashed hov line to the hovered candidate
  void clearPreview();

  // ---- design (ViewportDesign.cpp)
  // Things a feature input can pick that are not part of a body: sketch regions, sketch points and lines,
  // origin planes and axes, construction planes. Shown translucent, picked like anything else; ids are the
  // caller's. While candidates are shown, selection() still reports body picks and selectedCandidates() these.
  struct Candidate {
    std::string id;
    TopoDS_Shape shape;
    bool strong = false;
    std::shared_ptr<BodyPrs> presentation;  // drawn more solid (construction planes among faint origin planes)
  };
  void showCandidates(const std::vector<Candidate>& candidates);
  void clearCandidates();
  std::string hoveredCandidate() const;
  std::vector<std::string> selectedCandidates() const;  // in pick order
  // Makes the context selection exactly these (bodies, faces/edges/vertices by ordinal, candidates).
  void selectRefs(const std::vector<opad::Ref>& refs, const std::vector<std::string>& candidates = {});
  void setBodiesPickable(bool on);  // off: only candidates can be picked (choosing a sketch plane, a profile)
  // Feature preview: these shapes (world coordinates, already meshed by the worker) are drawn in place of the
  // nodes they change; `hidden` nodes are not drawn at all (consumed tools, removed bodies).
  void setPreviewBodies(const std::vector<std::pair<std::string, TopoDS_Shape>>& shapes, const std::vector<std::string>& hidden);
  // The same, with arrays built on the worker (BodyPrs::build): displaying them walks no triangulation here.
  struct PreviewPart { std::string node; TopoDS_Shape shape; std::shared_ptr<const BodyPrs> prs; };
  void setPreviewBodies(const std::vector<PreviewPart>& parts, const std::vector<std::string>& hidden);
  // Other arrays to draw for the preview bodies, in the parts' order (nullptr: their own): a handle drag's live
  // stretch while the exact preview is computed.
  void setPreviewDisplay(const std::vector<std::shared_ptr<const BodyPrs>>& arrays);
  void clearPreviewBodies();
  void setPreparedPreview(const TopoDS_Shape& shape,std::shared_ptr<const BodyPrs> prs,const std::vector<std::string>& hidden);
  // Sketch editing.
  void beginSketchInput(SketchInput* input, const opad::Frame& frame, const std::string& hiddenSketch);
  void endSketchInput();
  bool sketching() const { return m_sketchInput != nullptr; }
  void lookAt(const opad::Frame& frame, bool fit = true, bool animate = true);  // camera along the plane normal, plane x to the right
  bool planePoint(const QPointF& widgetPos, const opad::Frame& frame, double& u, double& v) const;
  double pixelSize() const;                    // world units per widget pixel at the view's focus
  double displayScale() const { return viewScale().x(); }  // device pixels per widget point: overlay text, markers, lines
  opad::Vec3 viewDirection() const;            // unit direction the camera looks along (into the scene)
  QPoint widgetPoint(const opad::Vec3& world) const;
  // Notes: NoteCards places one card per open note and tells the view where each pointer ends (widget
  // coordinates); notesMoved() follows every camera move or scene change so it can place them again.
  bool noteAnchor(const std::string& opId, QPoint& out) const;  // false: unknown, or behind the eye
  void setNoteLeaders(const std::map<std::string, QPoint>& ends, bool shown);  // shown=false: notes hidden, nothing drawn
  void setNoteTypeFilter(const std::string& type) { m_noteTypeFilter=type; }
  // The note / hand drawing editor (AnnotationEditor.cpp). Its target is drawn in the selection blue, tinted with a
  // dashed outline, on top of everything; the target's widget rectangle places the editor's badge.
  bool annotationPick(const QPointF& point, opad::Ref& target, bool& hit);  // body/face/edge/vertex; hit: point = where
  bool showAnnotationTarget(const opad::Ref& target, opad::Vec3* centre = nullptr);  // false: not in the view
  void clearAnnotationTarget();
  QRect annotationTargetRect() const;  // null while nothing is shown
  opad::Frame annotationCameraPlane(const opad::Vec3& origin) const;  // through origin, facing the camera
  void previewAnnotationDrawing(const opad::json& drawing);
  bool cubeAt(const QPointF& point);  // the view cube is under the mouse: a left press there belongs to the cube
  // Objects owned by an editor (the sketch being drawn, its dimensions): never pickable, drawn on top.
  void showOverlay(const Handle(AIS_InteractiveObject)& obj);
  void updateOverlay(const Handle(AIS_InteractiveObject)& obj);
  void removeOverlay(const Handle(AIS_InteractiveObject)& obj);
  const Tokens& tokens() const { return m_tokens; }
  // Sketch "Project": lets body edges be hovered while the editor keeps the clicks, and hands over the edge
  // under the mouse (world coordinates).
  void setEdgeHover(bool on);
  bool hoveredEdge(TopoDS_Shape& edge) const;
  bool hoveredReference(opad::Ref& ref) const;
  bool referenceAt(const QPointF& point,opad::Ref& ref);
  bool originReferenceAt(const QPointF& point,opad::Ref& ref);
  void setPreviewCurves(const TopoDS_Shape& shape,std::shared_ptr<const BodyPrs> prs,const std::vector<std::string>& hidden);
  void showBackdrop(const Handle(AIS_InteractiveObject)& obj);
  opad::json sectionState() const;
  void restoreSection(const opad::json& state);
  void benchDesignShot(const QString& path);  // OPAD_BENCH_DESIGN: fit, redraw, dump the 3D frame

  opad::json cameraJson() const;
  opad::Frame cameraPlane() const;
  void setCameraJson(const opad::json& j);
  QImage grabImage();

  QPaintEngine* paintEngine() const override { return nullptr; }

 signals:
  void notesMoved();
  void selectionChanged();
  void measurementAnchorPicked(int side, const opad::Vec3& point);
  void selectionApplied();  // a selectNodes() call has been applied (highlight or shade) and selection() reflects it
  void subHighlightApplied();  // the highlight of a sub-shape selection has been built and displayed
  void filterApplied();     // a setSelectionFilter() call has reached every displayed body
  void hoverChanged(const QString& text);
  void hoverPoint(bool valid, const opad::Vec3& point);  // with hoverChanged: where the mouse met the hovered entity
  void contextMenuRequested(const QPoint& globalPos);
  void meshingProgress(int remaining);
  void isolationChanged();  // entered, left, or left because every isolated object was deleted
  void sectionDragged(const opad::Vec3& origin);  // the section plane's handle was dragged here
  void looksApplied();  // a setLookLayer (or a scene change under one) has reached every displayed body

 public slots:
  void sync();
  void requestSync();  // coalesces mesh arrivals: at most one sync per 50 ms

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void showEvent(QShowEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;
  // An orbit gesture of the navigation preset pressed in 2D mode: a short hint instead of a silent pan.
  bool orbitGesture(unsigned buttonsAndFlags) const;
  void twoDimensionalHint(const QPoint& global);
  qint64 m_twoDHintShown = 0;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  bool eventFilter(QObject* object, QEvent* e) override;
  bool event(QEvent* e) override;  // sketching: plain keys reach the editor before the window's shortcuts
  void keyPressEvent(QKeyEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void OnSelectionChanged(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override;
  // Timed when OPAD_TRACE is set: a slow frame is either picking under the mouse or the redraw itself.
  gp_Pnt GravityPoint(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override;
  void handleSelectionPoly(const Handle(AIS_InteractiveContext)& ctx,const Handle(V3d_View)& view) override;
  void handleMoveTo(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override;
  void handleViewRedraw(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override;
  // Every hover and click pick of the controller: what lies behind a face is taken for nothing (dropOccluded).
  void contextLazyMoveTo(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view, const Graphic3d_Vec2i& point) override;

 private:
  int m_renderQuality = 1, m_sceneBackground = 0;
  void updateDepthBias();
  bool m_twoDimensional = false;
  Handle(Graphic3d_Camera) m_threeDimensionalCamera;
  QPointF m_dragOffset, m_warpPosition;
  bool m_selectThrough=false, m_boxCrossing=false;
  Graphic3d_Vec2i m_boxStart,m_boxEnd;
  Job* m_boxJob=nullptr;
  CursorWarpGate m_warpGate;
  void updateGridExtent();
  void placeGrid(double u, double v, double step, double extent);  // centred on (u, v) of the privileged plane
  // The box Fit All, Home and the load-time fit frame: displayed bodies, sketches, their images and a feature preview
  // (never the grid, gizmos, overlays or annotations); the default grid square when there is nothing (void if !fallback).
  Bnd_Box fitBounds(bool fallback = true) const;
  void applySelectionFilter(SelFilter f);  // setSelectionFilter's work, also for the filter already set (re-activates)
  // 2D mode: the grid follows the view (its plane, the visible area, a spacing for the zoom), so it never ends.
  void updateInfiniteGrid(bool force);
  gp_Pnt drawingOrbitPoint(const QPointF* cursor=nullptr,bool* found=nullptr);
  gp_Pnt drawingPlanePoint(const QPointF& cursor,bool& found);
  gp_Pnt nearestCurvePoint(const QPointF& cursor,bool& found,double& distance);
  // Tracking and extension (ViewportTracking.cpp), only while a tool takes points (m_pickAccumulate). Occlusion-aware
  // (UI-31): the vertex or line end under the pointer becomes an anchor after kTrackingDwellMs on it when it can be seen,
  // resting on an anchor again releases it, a camera move drops the anchors it hides, and a guide point that lies behind
  // a face is not offered.
  void updateTracking();
  void clearTracking();
  void dwellAnchor();          // acquisition and release under the pointer
  void showTrackingAnchors();  // a small cross on each anchor
  void pruneTracking();        // from handleViewRedraw: the camera moved
  // False when a displayed, non-ghost body's face lies in front of `p` by more than slackPx pixels (the navigation
  // selector, one ray from p towards the eye; section clipping honoured) or, for bodies other than `own` (the point's,
  // whose faces seen edge-on reach its pixel), covers its pixel 8 px in front; always true while selecting through.
  bool pointVisible(const gp_Pnt& p, const std::string& own = {}, double slackPx = 3) const;
  bool detectedPoint(gp_Pnt& p) const;  // where the pointer met the detected owner
  // A detected occluder, or a detected edge or vertex whose point is behind a face (Edge and Vertex modes): cleared.
  bool dropOccluded();
  void moveTo(const Graphic3d_Vec2i& at);  // the context's MoveTo, then dropOccluded
  static constexpr int kTrackingDwellMs = 350;
  bool m_trackingEnabled = true, m_extensionEnabled = true;
  // Shift over the guides (Tracking.hpp): held = locked while held, a tap with one guide or a double tap = locked until
  // a click or Esc. Cross lock (UI-32): anchors are still acquired while locked, and the locked line snaps to where it
  // lines up with another anchor (m_crossChoice among those in reach, m_crossings of them; a tap on a sticky lock cycles).
  tracking::ShiftLock m_shift;
  QElapsedTimer m_shiftClock;  // the key's time base, started at the first press
  int m_inferenceChoice = 0, m_crossChoice = 0, m_crossings = 0;
  bool m_trackingCross = false, m_eatEscape = false;  // the point shown is where two guides cross (an X); Esc unlocked
  struct TrackingAnchor { gp_Pnt point; gp_Vec direction; bool hasDirection; std::string body; };  // body: whose vertex or edge
  struct TrackingCandidate { gp_Pnt anchor, point; gp_Vec direction; bool intersection = false; gp_Pnt secondAnchor; std::string body; };
  std::vector<TrackingAnchor> m_trackingAnchors;  // at most 6, oldest first
  std::vector<TrackingCandidate> m_trackingCandidates;
  TrackingCandidate m_lockedTracking, m_tapLock;  // m_tapLock: what the last tap held, for a double tap's lock
  bool inferenceKey(QKeyEvent* event);
  bool trackingEscape(QEvent* event);  // Esc on a tracking lock unlocks it and goes no further (not to the tool)
  void refreshCenterStyles();
  bool m_trackingDirty = false, m_trackingShown = false;
  QPointF m_trackingCursor;
  TrackingAnchor m_dwellAnchor{};      // under the pointer since m_dwellClock
  bool m_dwelling = false, m_dwellDone = false;
  QElapsedTimer m_dwellClock;
  QTimer m_dwellTimer;                 // a paint once the dwell is over, the pointer resting
  Graphic3d_WorldViewProjState m_trackingCamera;
  Handle(AIS_Shape) m_trackingGuide, m_trackingGuideBehind, m_anchorMarks;  // the guide where seen, where behind a face
  std::string m_trackingMarker, m_snapClick;
  bool m_ctrlCenterPick=false;
  void setCenterPicking(bool on,const QPointF& position);
  struct Item {
    Handle(AIS_Shape) ais;
    std::string key;
    opad::Mat4 world;
    std::array<double, 3> color;
    double opacity;
    TopoDS_Shape located;
    Handle(NavigationShape) navigation;
    BodyLook look;      // as applied (ViewportLooks.cpp)
    bool rigid = true;  // the world placement is the object's local transformation (else baked into `located`)
  };
  // looks (ViewportLooks.cpp)
  std::array<std::unordered_map<std::string, LookDelta>, kLookSources> m_lookLayers;
  bool m_ghostsPickable = false;
  Job* m_lookJob = nullptr;
  std::deque<std::string> m_lookQueue;  // displayed bodies whose look may have changed, applied in this order
  std::unordered_set<std::string> m_lookQueued;
  BodyLook composeLook(const opad::Node& body) const;
  bool layered() const;
  // The AIS state of the look that differs from item.look: colour and opacity in place (SynchronizeAspects), erase or
  // display, (de)activate, Z layer, location (SetLocation: picking follows). True when it moved the body.
  bool applyLook(const std::string& id, Item& item, const BodyLook& look);
  void scheduleLooks();  // every displayed body checked again by the sliced job
  void initViewer();
  void trackpadScroll(const QPointF& position, const QPointF& delta, bool orbit);
  void finishTrackpadScroll();
  void zoomAt(const QPointF& position, qreal scaleFactor);
  bool handleNativeGesture(QNativeGestureEvent* event);
  Handle(AIS_Shape) centerMarker(const opad::Ref& ref, const gp_Pnt& point);
  void discoverCenter();
  void clearCenters();
  bool navigationPoint(const Graphic3d_Vec2i& cursor, gp_Pnt& point);
  gp_Pnt centralOrbitPoint();
  bool nearestSurface(int x, int y, gp_Pnt& point);
  gp_Pnt orbitPoint(const Graphic3d_Vec2i& cursor);
  void focusCube();
  void syncWindowSize();
  void applyStyle(const Handle(AIS_Shape)& ais, const BodyLook* look = nullptr);  // look: a ghost's edges fade with it
  void activateSelection(const Handle(AIS_Shape)& ais);
  void startMeshing(std::vector<std::string> keys);
  void displayBody(const std::string& id);
  void finishSync(int pendingCount, bool added);
  void showShade(const std::vector<std::string>& ids);
  void refreshSubHighlight();   // rebuilds m_subHl from the context's selected faces/edges/vertices (sliced)
  void applySelectionLayers();  // selected bodies live in the Topmost layer (own depth buffer): X-ray through occluders
  void markPickedPoints();      // a filled dot on each picked point candidate
  void clearShade();
  double deflectionFor(const std::string& key);
  QPointF viewScale() const;  // OCCT view coordinates per Qt widget point
  Graphic3d_Vec2i devicePos(const QPointF& p) const;
  void updateAnnotations();
  void noteCameraMoved();
  void updateClipPlanes();
  // section gizmo (ViewportSection.cpp)
  void updateSectionGizmo();   // rebuilds the outline from the plane and the model's extent; drops it when off
  void refreshSectionGizmo();  // re-displays the object from the kept outline and the hover state
  int sectionHandleAt(const QPointF& widgetPos, double& t) const;  // side 0-3 under the mouse (t: where along it), -1
  bool sectionDragDelta(const QPointF& from, const QPointF& to, double& along) const;
  void setSectionHover(int side, double t);
  bool sectionMousePress(QMouseEvent* e);    // true = the gizmo took the event
  bool sectionMouseMove(QMouseEvent* e);
  bool sectionMouseRelease(QMouseEvent* e);
  void applyTokens();
  void refreshMeasurement(bool force = false);
  int measurementAnchorAt(const QPointF& position) const;
  void requestRedraw() { update(); }
  // After a change made through the context without an update (Display/Redisplay/selection with
  // theToUpdateViewer=false): the view must be invalidated, or FlushViewEvents finds nothing to redraw and
  // the change only shows on the next orbit.
  void redrawScene() { if (m_initialised) m_view->Invalidate(); requestRedraw(); }

  AppDocument* m_doc;
  Tokens m_tokens;
  Handle(V3d_Viewer) m_viewer;
  Handle(V3d_View) m_view;
  Handle(AIS_InteractiveContext) m_ctx;
  Handle(AIS_ViewCube) m_cube;
  Handle(SelectMgr_ViewerSelector) m_navSelector;
  Handle(SelectMgr_SelectionManager) m_navSelection;
  std::map<const SelectMgr_SelectableObject*, std::string> m_navNodes;
  struct CenterMarker { opad::Ref ref; gp_Pnt point; Handle(AIS_Shape) ais; };
  std::map<std::string, CenterMarker> m_centers;
  std::map<const AIS_InteractiveObject*, std::string> m_centerObjects;
  std::string m_activeCenter;
  bool m_centerLocked = false;
  std::map<std::string, Item> m_items;
  std::map<const AIS_InteractiveObject*, std::string> m_nodeOf;
  std::vector<Handle(AIS_InteractiveObject)> m_labels;
  std::vector<Handle(AIS_InteractiveObject)> m_dimension;
  opad::json m_measurement;
  QStringList m_measureCaptions;
  Graphic3d_WorldViewProjState m_measureCamera;
  QSize m_measureSize;
  struct NoteMark { gp_Pnt at; std::string style; opad::json drawing; std::string node; };  // node: what it is pinned to
  gp_Vec lookOffset(const std::string& node) const;  // how far a look moved it (an exploded part): its notes follow
  std::string m_noteTypeFilter;
  Handle(AIS_InteractiveObject) m_drawingPreview;
  Handle(AIS_InteractiveObject) m_annotationTarget;
  std::vector<opad::Vec3> m_annotationCorners;  // the target's box, world
  Job* m_targetJob = nullptr;                   // a body target's tint, built on a worker when the body has no arrays
  std::map<std::string, NoteMark> m_notes;  // open notes by op id
  Graphic3d_WorldViewProjState m_noteCamera;
  QSize m_noteSize;
  bool m_measureComponents = true;
  struct MeasurementAnchor { int side; opad::Vec3 point; };
  std::vector<MeasurementAnchor> m_measureAnchors;  // same candidates for drawing and hit testing
  bool m_measureSelectionLocked = false, m_measureAnchorPress = false, m_retainToolPicks = false;
  std::vector<Handle(AIS_InteractiveObject)> m_pickMarkers, m_preview;
  opad::Vec3 m_lastPick{0, 0, 0};
  bool m_hasLastPick = false;
  Handle(Graphic3d_ClipPlane) m_sectionPlane;
  Handle(AIS_InteractiveObject) m_sectionGizmo;    // SectionGizmo (ViewportSection.cpp)
  std::array<opad::Vec3, 4> m_sectionCorners{};    // the outline, for the widget-space hit test
  bool m_sectionHasPlane = false, m_sectionDrag = false;
  int m_sectionHover = -1;                         // side whose handle strip is under the mouse
  double m_sectionHoverT = 0;                      // where along that side (0..1) the arrow sits
  QPointF m_sectionDragFrom;
  opad::Vec3 m_sectionDragOrigin{0, 0, 0};

  NavPreset m_preset = NavPreset::Fusion;
  Style m_style = Style::ShadedEdges;
  SelFilter m_filter = SelFilter::Body;
  bool m_gridSnap=false;
  double m_gridStep=10;
  double m_gridSpacing=0;  // view/gridSpacing (0 = automatic), read when the grid settings change
  double m_gridShownStep=0, m_gridShownExtent=0, m_gridShownX=0, m_gridShownY=0;  // the infinite grid as last laid out
  bool m_grid = false, m_sectionEnabled = false, m_sectionCaps = true, m_initialised = false, m_needFit = false;
  std::vector<std::string> m_fitNodesOnSync;
  bool m_flushingViewEvents = false, m_repaintAfterFlush = false;
  opad::Vec3 m_sectionOrigin{0, 0, 0}, m_sectionNormal{0, 0, 1};
  std::set<std::string> m_isolated;

  std::mutex m_meshMu;
  const void* m_activeCache = nullptr;  // the document's shape cache the mesh bookkeeping refers to
  std::set<std::string> m_meshed;                                // meshed and presentation built (m_prs)
  std::map<std::string, std::shared_ptr<BodyPrs>> m_prs;         // per key, built on the worker, consumed by displayBody
  // Zoom refinement (ViewportRefine.cpp): finer drawing arrays per key for bodies seen close up, bounded in total.
  struct Refined { double deflection = 0; std::shared_ptr<const BodyPrs> prs; qint64 used = 0; };
  std::map<std::string, Refined> m_refined;
  QTimer m_refineTimer;
  Graphic3d_WorldViewProjState m_refineCamera;
  Job* m_refineJob = nullptr;
  qint64 m_refineClock = 0;
  void scheduleRefinement();
  void refineVisible();
  std::set<std::string> m_meshing;
  std::set<std::string> m_meshSkipped;
  std::pair<int, int> m_lastSyncedSize{-1, -1};  // Qt size and display-scale stamp for syncWindowSize
  qreal m_cubeScale = 1.0;  // OCCT backing pixels per Qt point
  JobRunner* m_jobs = nullptr;
  Job* m_displayJob = nullptr;                    // in-flight sync(): bodies being added to the context
  QTimer m_syncTimer;
  Job* m_selJob = nullptr;                        // in-flight selectNodes
  Handle(SubHighlight) m_subHl;                   // every selected sub-shape, one object in the Topmost layer
  std::map<const AIS_InteractiveObject*,Handle(SubHighlight)> m_bodyGlows;
  Job* m_bodyGlowJob=nullptr;
  Job* m_subJob = nullptr;                        // in-flight refreshSubHighlight
  Job* m_filterJob = nullptr;                     // in-flight setSelectionFilter
  std::vector<Handle(AIS_Shape)> m_selApplied;    // objects selectNodes highlighted through the context
  std::vector<Handle(AIS_Shape)> m_shade;         // translucent boxes standing in for a large selection
  std::vector<std::string> m_shadeBodies;         // the bodies those boxes represent (reported by selection())
  bool m_notifyWhenApplied = false;               // clearSelection(): emit selectionChanged once settled
  std::shared_ptr<std::atomic<bool>> m_meshCancel = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> m_alive;

  // design
  void syncSketches();  // the scene's visible sketches as wire objects
  struct SketchWire {
    Handle(AIS_Shape) ais;
    std::shared_ptr<BodyPrs> prs;
    std::string stamp;  // geometry + frame it was built from
    std::vector<Handle(AIS_InteractiveObject)> backdrops;
    BodyLook look;  // as applied (ViewportLooks.cpp); colour = the selection blue it is drawn in
  };
  std::map<std::string, SketchWire> m_sketchWires;
  // A sketch's look: the layers' entries under its own id over the sketch blue; faded lines are blended towards the
  // background (line aspects ignore alpha). Applied in place like a body's, images included.
  BodyLook sketchLook(const std::string& id) const;
  void applySketchLook(SketchWire& wire, const BodyLook& look);
  struct PreparedSketch { std::string stamp; TopoDS_Shape shape; std::shared_ptr<BodyPrs> prs; std::vector<Handle(AIS_InteractiveObject)> backdrops; bool ready=false; };
  std::map<std::string,std::shared_ptr<PreparedSketch>> m_preparedSketches;
  std::string m_hiddenSketch;  // being edited: the editor draws it
  std::vector<std::pair<std::string, Handle(AIS_Shape)>> m_candidates;
  std::vector<Handle(AIS_Shape)> m_pointMarks;  // markPickedPoints
  std::vector<Handle(AIS_Shape)> m_previewBodies;
  std::vector<Handle(AIS_InteractiveObject)> m_overlays;  // showOverlay's: Fit frames the finite ones (a drawing being placed)
  std::set<std::string> m_previewHidden;  // nodes whose own object is erased while the preview shows
  bool m_bodiesPickable = true;
  SketchInput* m_sketchInput = nullptr;
  opad::Frame m_sketchFrame;
  bool m_sketchDrag = false;

  QTimer m_timer;
  QTimer m_trackpadEndTimer;
  enum class TrackpadMode { None, Pan, Orbit };
  TrackpadMode m_trackpadMode = TrackpadMode::None;
  QPointF m_trackpadCursor, m_trackpadAnchor;
  bool m_nativePinching = false;
  QString m_hover;
  void trackHoverFade();
  void updateHoverFade();
  QTimer m_hoverFadeTimer;
  QElapsedTimer m_hoverAge;
  Handle(AIS_InteractiveObject) m_hoverFadeObject;
  bool m_hoverFadeRestorePending=false;
  bool m_hoverFadeEnabled=true;
  double m_hoverFadeSeconds=5;
  const Standard_Transient* m_hoverOwner = nullptr;  // owner m_hover was built for (identity only, never dereferenced)
  QPoint m_pressPos;
  bool m_rightPress = false;
  bool m_blocked = false;
  bool m_cubeClick = false;       // the current left press is on the view cube: its click is not a selection change
  bool m_pickAccumulate = false;  // guided tools: left click toggles (XOR)
  bool m_cubeGesture = false;  // this left press started on the view cube: dragging orbits instead of rubber-banding
};
