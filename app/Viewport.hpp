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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "AppDocument.hpp"
#include "BodyShape.hpp"
#include "Theme.hpp"

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
  opad::json circleInfo(const opad::Ref& ref) const;
  void setShadows(bool on);
  void setRenderQuality(int level);
  static int savedRenderQuality();
  void setSceneBackground(int style);
  void setHoverFade(bool enabled,double seconds);
  void setTwoDimensional(bool on);
  bool twoDimensional() const { return m_twoDimensional; }
  void setTracking(bool on);
  void setExtensionTracking(bool on);
  void setOrthographic(bool ortho);
  bool isOrthographic() const;
  void setSelectionFilter(SelFilter f);
  SelFilter selectionFilter() const { return m_filter; }

  void fitAll();
  void fitWhenReady();   // fit now if bodies are displayed, otherwise once the first meshes arrive
  void fitNodesWhenReady(std::vector<std::string> ids);
  void cancelMeshing();  // stop tessellating the remaining bodies (they stay hidden until resetMeshing)
  void resetMeshing();
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

  // Guided tools (distance, angle, ...: the tool asks for one pick per step). While accumulating, a plain click
  // adds to the selection (or takes a picked item out again) instead of replacing it, so selection() is the
  // tool's ordered pick list.
  void setPickAccumulate(bool on, bool retainPicks = false);
  void deselectLast();      // one step back
  void keepLastSelected();  // a pick after the last step starts over from that pick
  bool lastPickPoint(opad::Vec3& p) const;  // where the last click hit the geometry
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
    bool strong = false;  // drawn more solid (construction planes among faint origin planes)
  };
  void showCandidates(const std::vector<Candidate>& candidates);
  void clearCandidates();
  std::vector<std::string> selectedCandidates() const;  // in pick order
  // Makes the context selection exactly these (bodies, faces/edges/vertices by ordinal, candidates).
  void selectRefs(const std::vector<opad::Ref>& refs, const std::vector<std::string>& candidates = {});
  void setBodiesPickable(bool on);  // off: only candidates can be picked (choosing a sketch plane, a profile)
  // Feature preview: these shapes (world coordinates, already meshed by the worker) are drawn in place of the
  // nodes they change; `hidden` nodes are not drawn at all (consumed tools, removed bodies).
  void setPreviewBodies(const std::vector<std::pair<std::string, TopoDS_Shape>>& shapes, const std::vector<std::string>& hidden);
  void clearPreviewBodies();
  void setPreparedPreview(const TopoDS_Shape& shape,std::shared_ptr<const BodyPrs> prs,const std::vector<std::string>& hidden);
  // Sketch editing.
  void beginSketchInput(SketchInput* input, const opad::Frame& frame, const std::string& hiddenSketch);
  void endSketchInput();
  bool sketching() const { return m_sketchInput != nullptr; }
  void lookAt(const opad::Frame& frame, bool fit = true, bool animate = true);  // camera along the plane normal, plane x to the right
  bool planePoint(const QPointF& widgetPos, const opad::Frame& frame, double& u, double& v) const;
  double pixelSize() const;                    // world units per widget pixel at the view's focus
  QPoint widgetPoint(const opad::Vec3& world) const;
  // Notes: NoteCards places one card per open note and tells the view where each pointer ends (widget
  // coordinates); notesMoved() follows every camera move or scene change so it can place them again.
  bool noteAnchor(const std::string& opId, QPoint& out) const;  // false: unknown, or behind the eye
  void setNoteLeaders(const std::map<std::string, QPoint>& ends, bool shown);  // shown=false: notes hidden, nothing drawn
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

 private:
  int m_renderQuality = 0, m_sceneBackground = 1;
  void updateDepthBias();
  bool m_twoDimensional = false;
  Handle(Graphic3d_Camera) m_threeDimensionalCamera;
  QPointF m_dragOffset, m_warpPosition;
  bool m_selectThrough=false, m_boxCrossing=false;
  Graphic3d_Vec2i m_boxStart,m_boxEnd;
  Job* m_boxJob=nullptr;
  CursorWarpGate m_warpGate;
  void updateGridExtent();
  gp_Pnt drawingOrbitPoint();
  void updateTracking();
  void clearTracking();
  bool m_trackingEnabled = true, m_haveTrackingAnchor = false, m_trackingLocked = false;
  bool m_extensionEnabled = true, m_shiftHeld = false;
  QElapsedTimer m_shiftClock;
  int m_inferenceChoice = 0;
  struct TrackingAnchor { gp_Pnt point; gp_Vec direction; bool hasDirection; };
  struct TrackingCandidate { gp_Pnt anchor, point; gp_Vec direction; bool intersection = false; gp_Pnt secondAnchor; };
  std::vector<TrackingAnchor> m_trackingAnchors;
  std::vector<TrackingCandidate> m_trackingCandidates;
  TrackingCandidate m_lockedTracking;
  bool inferenceKey(QKeyEvent* event);
  void refreshCenterStyles();
  bool m_trackingDirty = false;
  QPointF m_trackingCursor;
  gp_Pnt m_trackingAnchor;
  gp_Vec m_trackingDirection, m_trackingLockDirection;
  bool m_trackingHasDirection = false;
  Handle(AIS_Shape) m_trackingGuide;
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
  };
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
  gp_Pnt orbitPoint(const Graphic3d_Vec2i& cursor);
  void focusCube();
  void syncWindowSize();
  void applyStyle(const Handle(AIS_Shape)& ais);
  void activateSelection(const Handle(AIS_Shape)& ais);
  void startMeshing(std::vector<std::string> keys);
  void displayBody(const std::string& id);
  void finishSync(int pendingCount, bool added);
  void showShade(const std::vector<std::string>& ids);
  void refreshSubHighlight();   // rebuilds m_subHl from the context's selected faces/edges/vertices (sliced)
  void applySelectionLayers();  // selected bodies live in the Topmost layer (own depth buffer): X-ray through occluders
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
  Graphic3d_WorldViewProjState m_measureCamera;
  QSize m_measureSize;
  struct NoteMark { gp_Pnt at; std::string style; };
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
  bool m_grid = false, m_sectionEnabled = false, m_sectionCaps = true, m_initialised = false, m_needFit = false;
  std::vector<std::string> m_fitNodesOnSync;
  bool m_flushingViewEvents = false, m_repaintAfterFlush = false;
  opad::Vec3 m_sectionOrigin{0, 0, 0}, m_sectionNormal{0, 0, 1};
  std::set<std::string> m_isolated;

  std::mutex m_meshMu;
  const void* m_activeCache = nullptr;  // the document's shape cache the mesh bookkeeping refers to
  std::set<std::string> m_meshed;                                // meshed and presentation built (m_prs)
  std::map<std::string, std::shared_ptr<BodyPrs>> m_prs;         // per key, built on the worker, consumed by displayBody
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
  };
  std::map<std::string, SketchWire> m_sketchWires;
  struct PreparedSketch { std::string stamp; TopoDS_Shape shape; std::shared_ptr<BodyPrs> prs; std::vector<Handle(AIS_InteractiveObject)> backdrops; bool ready=false; };
  std::map<std::string,std::shared_ptr<PreparedSketch>> m_preparedSketches;
  std::string m_hiddenSketch;  // being edited: the editor draws it
  std::vector<std::pair<std::string, Handle(AIS_Shape)>> m_candidates;
  std::vector<Handle(AIS_Shape)> m_previewBodies;
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
  const AIS_InteractiveObject* m_hoverFadeObject=nullptr;
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
