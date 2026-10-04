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
#include <Image_PixMap.hxx>
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
struct ObjectSnapState;
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
  virtual bool sketchKey(QKeyEvent* e) = 0;  // at the shortcut override: true = the sketch's key (no shortcut sees it)
  virtual bool sketchType(QKeyEvent*) { return false; }  // the press of a key that types a value (or Tab), taken above
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
  // G: the grid's visibility, one state outside sketches (view/grid) and one inside (sketch/grid, on unless hidden
  // there); gridShownChanged tells the G action which one it shows.
  void setGrid(bool on);
  void configureGrid(double spacing,double extent);
  void setSelectThrough(bool on) {m_selectThrough=on;}
  void UpdateRubberBand(const Graphic3d_Vec2i& from,const Graphic3d_Vec2i& to) override;
  // Grid snapping is one switch (F9, mirrored by the sketch panel): gridSnapChanged tells both. It does not depend on
  // the grid being shown (the snap marker shows the node); in a sketch or 2D mode the step follows the zoom.
  void setGridSnap(bool on);
  bool gridSnap() const { return m_gridSnap; }
  double gridStep() const;
  bool gridShown() const { return m_sketchInput ? m_sketchGrid : m_grid; }
  double gridShownStep() const { return m_gridShownStep; }  // as the sketch / 2D grid was last laid out
  // The sketch's own cursor (grid snapping: the drawing cursor jumps between nodes, SketchEditor draws it at the snapped
  // point). Asked for, the system pointer is blank over the view, but not over the view cube, during a camera gesture,
  // while a popup menu is open or the view is blocked; it is the system's own again off the view (the widget's cursor),
  // over a tool panel or an overlay on the view (they keep the arrow). ownCursor(): blank now, the editor draws its own.
  void setOwnCursor(bool on);
  bool ownCursor() const { return m_ownCursorShown; }
  // How far (widget pixels, y down) `world` moves to where lines `width` device pixels wide through it cover whole pixels
  // (a pixel's centre for an odd width, a corner for an even one): the drawn cursor is crisp there.
  QPointF pixelAlign(const opad::Vec3& world, int width) const;
  Bnd_Box benchGridBox() const;  // where the grid is drawn (OCCT's structure, world box), for benches
  bool benchGridEcho() const;    // OCCT's grid echo (a star on the node nearest the pointer) is on: it must not be
  QPointF benchGridOrigin() const { return {m_gridShownX, m_gridShownY}; }  // the sketch / 2D grid's origin as laid out
  Bnd_Box benchFitBox() const { return fitBounds(false); }  // what Fit frames, for benches
  opad::json circleInfo(const opad::Ref& ref) const;
  void setShadows(bool on);
  void setRenderQuality(int level);
  static int savedRenderQuality();
  static int savedSceneBackground();
  void setSceneBackground(int style);
  int sceneBackground() const { return m_sceneBackground; }
  QColor sceneBackgroundColor() const;  // its colour (the gradient's middle)
  // What a 2D drawing without a colour (DXF colour 7) is drawn in: light on a dark background, dark on a light one (UI-10).
  std::array<double, 3> drawingInk() const;
  // The 2D vocabulary (UI-118, ViewportDrawing.cpp): in a drawing-only scene or 2D mode, a hovered drawing entity reads as
  // "Line on Walls · 120 mm" (an object on its layer, never "body › edge 12") and is reported by hoverInfo.
  void setDrawingWords(bool on);
  bool drawingWords() const { return m_drawingWords; }
  static QString drawingWord(const std::string& type);  // "line" -> "Line", translated
  bool benchHover(const QPointF& widgetPos);  // the detection a mouse move here makes, and the hover after it (hidden windows never paint)
  void setHoverFade(bool enabled,double seconds);
  void resetHoverFade();
  void setCubeEdgesCorners(bool on);  // setting view/cubeEdgesCorners: off = only the cube's faces are views
  void setTwoDimensional(bool on);
  bool twoDimensional() const { return m_twoDimensional; }
  void setTracking(bool on);
  void setExtensionTracking(bool on);
  void setOrthographic(bool ortho);
  bool isOrthographic() const;
  void setSelectionFilter(SelFilter f);
  SelFilter selectionFilter() const { return m_filter; }

  void fitAll();
  void animateFitAll(double seconds = 0.35);  // the camera glides to what fitAll frames, also in a view that draws no frames
  bool cameraMoving() const;  // a camera animation (fit, cube, roll) is under way
  bool showsAll() const;  // every corner of what Fit All frames is inside the view
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
  QString benchCubePart(int dx, int dy);  // hover the cube this far from its centre (Qt points): "side", "edge", "corner" or ""
  std::string benchHeaviest() const;       // OPAD_BENCH_FILTER: the body with the most faces, the pick target
  void benchBand();                        // OPAD_BENCH_BAND: rubber band over the whole view in the current mode
  void benchSubShot(const QString& path);  // OPAD_BENCH_SUBSHOT: frame from behind the picked sub-shape (X-ray check)
  void benchClick(double fx, double fy);  // OPAD_BENCH_TOOL: a left click at this fraction of the view, as the mouse handlers deliver it
  void benchFlush();  // what the next frame does with the input the mouse handlers queued (picks, hover): a hidden window never paints
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
  void isolate(const std::vector<std::string>& ids, bool fit = true);  // empty = exit the mode; fit: frame them (a layer walk keeps the camera)
  bool isIsolated() const { return !m_isolated.empty(); }
  std::vector<std::string> isolatedNodes() const {return {m_isolated.begin(),m_isolated.end()};}
  int isolatedCount() const { return static_cast<int>(m_isolated.size()); }
  int displayedCount() const { return static_cast<int>(m_items.size()); }
  // What the view did with bodies since the document opened (benches: a sync re-meshes only the shapes that changed, a
  // moved part is relocated, never displayed again): bodies meshed (one per key), displayed, relocated in place.
  struct DisplayStats { int meshed = 0, displayed = 0, relocated = 0; };
  DisplayStats displayStats() const { return {m_meshCount.load(), m_displayCount, m_relocateCount}; }
  // Small parts hidden while the view moves (ViewportSmallParts.cpp): bodies whose box's longest side is under `mm` are
  // hidden (LookSource::Navigation) from the first frame the camera moves until it has been still for 300 ms, selected
  // ones excepted; 0 turns it off. A camera change outside a frame (benches) is reported with cameraMoving().
  void setSmallPartFilter(double mm);
  double smallPartFilter() const { return m_smallParts; }
  bool smallPartsHidden() const { return m_smallHidden; }
  int smallPartCount() const { return m_smallCount; }  // hidden by the last move
  void cameraMoving();
  // The colours a displayed body's shaded presentation fills its groups with (sRGB): one, or the body's own and each face
  // colour (UI-74). Benches check what is drawn with it.
  std::vector<std::array<double, 3>> drawnColors(const std::string& nodeId) const;
  std::vector<double> drawnTransparencies(const std::string& nodeId) const;  // the same groups' (front material)
  // Pictures on bodies (SVG images, canvases) decoded on workers so far (UI-71), and whether a displayed body shows one.
  int rastersDecoded() const { return m_rastersDecoded; }
  bool showsPicture(const std::string& nodeId) const;

  // Per-body looks (ViewportLooks.cpp, UI-121): each source owns one layer of deltas by node id (a component's covers the
  // bodies under it; the nearest entry wins), composed over the document's appearance in LookSource order (BodyLook.hpp)
  // and applied by a sliced job, aspects in place, never Redisplay. looksApplied() once every displayed body shows it.
  // A sketch (no component) takes the entries under its own id: visible, colour, opacity or ghost (faded lines), pickable.
  void setLookLayer(LookSource source, std::map<std::string, LookDelta> deltas);
  void clearLookLayer(LookSource source) { setLookLayer(source, {}); }
  // Ghosts can be picked as references, never selected (UI-33): on request (a sketch plane being chosen), and by
  // themselves while picks accumulate (guided tools, feature inputs) or sketch Project hovers body edges.
  void setGhostsPickable(bool on);
  bool ghostsPickable() const { return m_ghostsPickable || m_pickAccumulate || m_edgeHover; }
  BodyLook bodyLook(const std::string& body) const;  // as composed now (whether displayed yet or not); a sketch's too
  BodyLook shownLook(const std::string& body) const;  // as applied to the displayed body or sketch (the default look if none)
  QString hoverName(const std::string& node) const;   // the status text of a hovered node: "Lid (inactive)" for a ghost, "(locked)"
  const QString& hoverText() const { return m_hover; }  // the status text of what picking finds under the mouse now
  // The body drawn nearest under a point of the view (widget px), found as the orbit pivot is, whether it is picked or not
  // (a ghost, a locked body): "" when there is none. One pick of the navigation selector.
  std::string drawnAt(const QPointF& point);
  std::string ghostAt(const QPointF& point);  // drawnAt when that is a ghost, else ""
  bool looksPending() const { return m_lookJob != nullptr || !m_lookQueue.empty(); }
  // How far looks moved what is drawn (an exploded view): the displayed bodies off their place, and one node's offset
  // (a component's by its own entry). Measuring and annotating take the parts where they are drawn.
  std::unordered_map<std::string, opad::Vec3> shownOffsets() const;
  opad::Vec3 shownOffset(const std::string& node) const;
  opad::json benchLookState(const std::string& body) const;  // OPAD_BENCH_LOOKS: what AIS holds for a displayed body or sketch
  Graphic3d_ZLayerId throughLayer() const { return m_throughLayer; }  // where a canvas shown through the model is drawn
  std::string benchPickAt(int x, int y, opad::Vec3* at = nullptr);  // the body picking finds at this point of the view (device pixels), "" none
  bool benchBodyPoint(const std::string& body, int& x, int& y);  // a point of the view where picking finds this body
  void benchClickAt(int x, int y);  // a left click at this device pixel through the mouse handlers, then the frame's flush

  // Compare (ViewportCompare.cpp, UI-58): another version drawn with the model. Parts are bodies the model does not draw
  // as they are: ghosts of the other version (a removed body, a moved one's old place, a modified one's old geometry) and
  // the compared version's own bodies when it is not the session; arrows run from a moved body's old centre to its new
  // one, dashed. Never pickable; Fit frames the parts. A part's arrays come from the model's meshes (displayArrays) or
  // from a worker (BodyPrs::build); a part whose world placement is not rigid comes baked, at identity.
  struct ComparePart {
    std::string id;  // its node id in its version
    TopoDS_Shape shape;  // the prototype, meshed
    std::shared_ptr<const BodyPrs> prs;
    opad::Mat4 world;
    std::array<double, 3> color{0.5, 0.5, 0.5};
    double opacity = 1;
    bool visible = true;
    char view = 0;  // side by side: 0 in both views, 'A' only in A's (the side), 'B' only in this one
  };
  struct CompareArrow {
    opad::Vec3 from{0, 0, 0}, to{0, 0, 0};
    bool visible = true;
  };
  void setCompare(const std::vector<ComparePart>& parts, const std::vector<CompareArrow>& arrows, const QColor& arrowColor);
  // The same parts and arrows, in the same order, in other colours, opacities or visibility: aspects in place.
  void restyleCompare(const std::vector<ComparePart>& parts, const std::vector<CompareArrow>& arrows);
  void clearCompare();
  std::shared_ptr<const BodyPrs> displayArrays(const std::string& key) const;  // the arrays a displayed body key was drawn from; null: none
  void fitBox(const Bnd_Box& box);  // frames a world box as Fit does; void: Fit All
  opad::json benchCompareState() const;  // OPAD_BENCH_COMPARE: each part's id, whether drawn, colour, transparency; the arrows
  // Side by side: a second view of the same scene left of this one (it takes the left half of the parent), A's, whose
  // camera is this view's after every frame. Parts show where their `view` says; setSideHidden names the session's bodies
  // and sketches A's view leaves out (B's changes). Navigation over it (wheel, middle and right drags, the cube) drives
  // this view; it selects nothing. Its caption names A.
  void setSideBySide(bool on, const QString& caption = QString());
  bool sideBySide() const { return m_side != nullptr; }
  void setSideCaption(const QString& caption);
  void setSideHidden(const std::vector<std::string>& ids);
  QWidget* sideWidget() const;
  QImage grabSide();
  opad::json benchSideState();  // a frame as paint runs it, then: both cameras and sizes, the caption, where each part shows

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
  bool pickAccumulate() const { return m_pickAccumulate; }  // a guided tool or a design input owns the picks
  void deselectLast();      // one step back
  void keepLastSelected();  // a pick after the last step starts over from that pick
  bool lastPickPoint(opad::Vec3& p) const;  // where the last click hit the geometry
  bool lastClickHit() const { return m_hasLastPick; }  // false: the last click was on empty space
  void showPickMarkers(const std::vector<opad::Vec3>& points);  // numbered end markers, 1-based
  void showPreview(const opad::Vec3& a, const opad::Vec3& b, const QString& label);  // dashed hov line to the hovered candidate
  void clearPreview();
  // Object snap (UI-90, ViewportSnap.cpp): while a tool picks points (Vertex filter, a drawing's Points), the drawings' and
  // sketches' ends, midpoints, centres, quadrants, intersections and nearest points under the mouse, perpendicular and
  // tangent points from the point picked before, as the sketch offers them (its snap set, settings sketch/snap/<kind>): a
  // marker of the kind's shape, its name in the status bar, and a click picks the point (a Point ref). F3 switches it
  // (view/objectSnap). Each body's (sketch's) index is built on a worker when first needed.
  void setObjectSnap(bool on);
  // Who takes snapped points (the snap shows only then): a guided tool that picks free points (Distance, Bounding box,
  // Area) while in the Points filter, or a pick of its own in any filter (the plot window's corners: it reads snapAt
  // itself). None: what needs the entity it picks (Radius a circle's centre, the section's face, feature inputs).
  enum class SnapPicks { None, Points, Always };
  void setSnapPicks(SnapPicks picks);
  SnapPicks snapPicks() const { return m_snapPicks; }
  void setSnapFrom(const std::optional<opad::Vec3>& from);  // the point picked before (a tool's last pick), none
  bool objectSnap() const { return m_objectSnap; }
  bool snapAt(const QPointF& widgetPos, opad::Vec3& world, QString* kind = nullptr);  // any point consumer: false until indexed
  bool shownSnap(opad::Vec3& world, QString* kind = nullptr) const;                // the one the cursor shows now
  bool snapIndexesReady();  // asks for the missing indexes; true once every displayed drawing has one (or its indexing was cancelled)
  int snapIndexCount() const;  // the indexes kept (benches)
  static QString snapWord(const QString& kind);  // "endpoint" -> "Endpoint", translated
  bool benchSnap(const QPointF& widgetPos);  // the snap a mouse move here shows (hidden windows never paint)
  bool pointUnder(const QPointF& widgetPos, opad::Vec3& world);  // the frontmost displayed surface there (one BVH ray), false: none

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
  // Smart selection's candidate (UI-95, ViewportCandidates.cpp): what a click on its chip would select, in the candidate
  // amber, on top like the selection. Faces and edges are one object copied from the bodies' meshes (a sliced job when
  // there are many), whole bodies take the look compositor's candidate layer. Empty: nothing shown.
  void showCandidateRefs(const std::vector<opad::Ref>& refs);
  size_t candidateRefsShown() const { return m_candidateShown; }  // faces and edges drawn now (benches)
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
  size_t previewBodyCount() const { return m_previewBodies.size(); }  // bench checks: a feature preview is on screen
  void setPreparedPreview(const TopoDS_Shape& shape,std::shared_ptr<const BodyPrs> prs,const std::vector<std::string>& hidden);
  // Sketch editing.
  void beginSketchInput(SketchInput* input, const opad::Frame& frame, const std::string& hiddenSketch);
  void endSketchInput();
  bool sketching() const { return m_sketchInput != nullptr; }
  void lookAt(const opad::Frame& frame, bool fit = true, bool animate = true);  // camera along the plane normal, plane x to the right
  bool planePoint(const QPointF& widgetPos, const opad::Frame& frame, double& u, double& v) const;
  // Where the mouse met the hovered body at the last detection (ViewportReadout.cpp); false: no body under it.
  bool detectedPoint(opad::Vec3& p) const;
  bool benchDetect(int x, int y);  // OPAD_BENCH_STATUSBAR: the detection under this point (device pixels), kept as hovering does
  double pixelSize() const;                    // world units per widget pixel at the view's focus
  double displayScale() const { return viewScale().x(); }  // device pixels per widget point: overlay text, markers, lines
  // The width a line must be given to come out at least this many widget points wide: times the display scale and the
  // render scale (Studio quality renders 1.25 times as large and scales down, which made 1 px hairlines 0.8 px, dim and
  // blurred across two rows), rounded up to whole pixels since the driver rounds line widths.
  double lineWidth(double points = 1) const;
  double renderScale() const;  // the render's size over the view's (Studio quality: 1.25)
  opad::Vec3 viewDirection() const;            // unit direction the camera looks along (into the scene)
  QPoint widgetPoint(const opad::Vec3& world) const;
  // widgetPoint for a worker: a copy of the camera as it is now (fractional widget coordinates; empty before the view is up).
  std::function<QPointF(const opad::Vec3&)> projector() const;
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
  QRect cubeRect() const;  // around the view cube (widget points; empty while it is not shown): its faces for screen readers
  // Objects owned by an editor (the sketch being drawn, its dimensions): never pickable, drawn on top.
  void showOverlay(const Handle(AIS_InteractiveObject)& obj);
  void updateOverlay(const Handle(AIS_InteractiveObject)& obj);
  void removeOverlay(const Handle(AIS_InteractiveObject)& obj);
  void moveOverlay(const Handle(AIS_InteractiveObject)& obj, const gp_Trsf& to);  // its location only: nothing recomputed
  const Tokens& tokens() const { return m_tokens; }
  // Sketch "Project": lets body edges be hovered while the editor keeps the clicks, and hands over the edge
  // under the mouse (world coordinates).
  void setEdgeHover(bool on);
  bool hoveredEdge(TopoDS_Shape& edge) const;
  bool hoveredReference(opad::Ref& ref) const;
  bool referenceAt(const QPointF& point,opad::Ref& ref);
  bool originReferenceAt(const QPointF& point,opad::Ref& ref);
  // Drawing to sketch's preview (UI-29): segment and point arrays built on the worker, construction ones dashed; showing
  // them hands the arrays to the driver. previewSegments() counts what the preview draws.
  void setPreviewCurves(std::shared_ptr<const BodyPrs> curves,std::shared_ptr<const BodyPrs> construction,const std::vector<std::string>& hidden);
  size_t previewSegments() const;
  size_t previewParts() const { return m_previewBodies.size(); }
  void showBackdrop(const Handle(AIS_InteractiveObject)& obj);
  // A canvas dragged by its handles (CanvasEditor): drawn at `world` through its local transformation (no remesh; picking
  // and its selection glow follow; stretched out of its proportions, its one rectangle is rebuilt at that aspect) until
  // endPlacementPreview, or until a scene sync puts it where the document says. False: not displayed, or not placed rigidly.
  bool previewPlacement(const std::string& node, const opad::Mat4& world);
  void endPlacementPreview(const std::string& node);
  bool shownPlacement(const std::string& node, opad::Mat4& world) const;  // what it is drawn at now (benches)
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
  // The drawing entity under the mouse in the 2D vocabulary: drawing2d::entityInfo plus body and index; null when none.
  void hoverInfo(const opad::json& info);
  void contextMenuRequested(const QPoint& globalPos);
  void meshingProgress(int remaining);
  void isolationChanged();  // entered, left, or left because every isolated object was deleted
  void sectionDragged(const opad::Vec3& origin);  // the section plane's handle was dragged here
  void gridSnapChanged(bool on);
  void gridShownChanged(bool on);
  void ownCursorChanged(bool shown);  // the system pointer went blank (the editor draws its cursor) or came back
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
  // A right click on a body's face, edge, vertex or the body not selected selects it (Replace) before the menu: the menu is
  // about what is under the pointer. On what is selected, on nothing, or while a tool or an editor owns the picks: unchanged.
  void contextPick(const QPointF& at);
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
  void showGrid();  // gridShown() on screen
  void applyGridColors();  // faint lines from the theme in a sketch and 2D mode, OCCT's greys in 3D
  void applyOwnCursor();   // the system pointer blank or back, as setOwnCursor asked and what is under it allows
  double layoutStep() const;  // the sketch / 2D grid's step at this zoom (0: none)
  double planePixel() const;  // world units per pixel on the sketch's plane, the longer screen direction (a tilt)
  void placeGrid(double u, double v, double step, double extent);  // centred on (u, v) of the privileged plane
  // The box Fit All, Home and the load-time fit frame: displayed bodies, sketches, their images, a feature preview, Compare's parts
  // (never the grid, gizmos, overlays or annotations); the default grid square when there is nothing (void if !fallback).
  Bnd_Box fitBounds(bool fallback = true) const;
  void applySelectionFilter(SelFilter f);  // setSelectionFilter's work, also for the filter already set (re-activates)
  // 2D mode and sketches: the grid follows the view (its plane, the visible area, a spacing for the zoom), so it never ends.
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
    std::string raster;  // its picture (rasterKey), empty without one
    BodyLook look;      // as applied (ViewportLooks.cpp)
    bool rigid = true;  // the world placement is the object's local transformation (else baked into `located`)
    // A canvas stretched out of its picture's proportions (free aspect) is drawn rigidly too: `located` is its rectangle
    // `stretch` times as tall, `placement` the similarity left of its world matrix.
    double stretch = 1;
    gp_Trsf placement;  // its local transformation for `world` (rigid), before a look's offset
  };
  // Where a rigidly drawn body goes for `world`: its local transformation and, for a canvas, its stretch (1 in its
  // picture's proportions). False: `world` is neither rigid nor a stretched canvas's.
  static bool rigidPlacement(const opad::Node& n, const opad::Mat4& world, gp_Trsf& placement, double& stretch);
  // The item drawn at `world` without displaying it again: its location (with its look's offset), and a canvas whose stretch
  // changed gets its rectangle at the new one (one face, meshed here). False: it cannot be placed that way.
  bool relocate(Item& item, const opad::Node& n, const opad::Mat4& world);
  // looks (ViewportLooks.cpp)
  std::array<std::unordered_map<std::string, LookDelta>, kLookSources> m_lookLayers;
  bool m_ghostsPickable = false, m_edgeHover = false;
  void referencesChanged(bool wasPickable);  // ghostsPickable() may have changed: ghosts (de)activated by the look job
  // Canvases shown through the model (opad/canvas.hpp): after the model, no depth test, no depth written (initViewer).
  Graphic3d_ZLayerId m_throughLayer = Graphic3d_ZLayerId_Topmost;
  Job* m_lookJob = nullptr;
  std::deque<std::string> m_lookQueue;  // displayed bodies whose look may have changed, applied in this order
  std::unordered_set<std::string> m_lookQueued;
  BodyLook composeLook(const opad::Node& body) const;
  bool layered() const;
  // The AIS state of the look that differs from item.look: colour and opacity in place (SynchronizeAspects), erase or
  // display, (de)activate, Z layer, location (SetLocation: picking follows). True when it moved the body.
  bool applyLook(const std::string& id, Item& item, const BodyLook& look);
  void scheduleLooks();  // every displayed body checked again by the sliced job
  Handle(Prs3d_Drawer) m_drawingSelected, m_drawingHover;  // a drawing's highlights, shared (ViewportSettings.cpp)
  bool m_drawingWords = false;
  opad::json m_hoverInfo;
  void updateHover();  // after a frame's detection (paintEvent)
  void updateDrawingHighlights();
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
  bool m_sketchGrid = true;  // the grid in sketches (sketch/grid)
  bool m_ownCursorWanted = false, m_ownCursorShown = false, m_ownCursorAside = false;  // aside: over the cube, a camera gesture
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
  std::atomic<int> m_meshCount{0};
  int m_displayCount = 0, m_relocateCount = 0;
  double m_smallParts = 0;
  bool m_smallHidden = false;
  int m_smallCount = 0;
  QTimer m_smallTimer;
  Graphic3d_WorldViewProjState m_smallCamera;
  std::unordered_map<std::string, double> m_partSizes;  // body key -> its box's longest side (the cached view box)
  void restoreSmallParts();
  QTimer m_refineTimer;
  Graphic3d_WorldViewProjState m_refineCamera;
  Job* m_refineJob = nullptr;
  qint64 m_refineClock = 0;
  void scheduleRefinement();
  void refineVisible();
  std::set<std::string> m_meshing;
  std::set<std::string> m_meshSkipped;
  // A body's picture is decoded on a worker before the body is shown (80 ms for a 12 MP JPEG in displayBody), once per
  // picture: textures by rasterKey, null when it cannot be decoded (the frame is shown).
  std::map<std::string, Handle(Image_PixMap)> m_rasters;
  std::set<std::string> m_rasterDecoding;
  int m_rastersDecoded = 0;
  void decodeRaster(const opad::Node& n, const std::string& key);
  std::pair<int, int> m_lastSyncedSize{-1, -1};  // Qt size and display-scale stamp for syncWindowSize
  qreal m_cubeScale = 1.0;  // OCCT backing pixels per Qt point
  JobRunner* m_jobs = nullptr;
  Job* m_displayJob = nullptr;                    // in-flight sync(): bodies being added to the context
  QTimer m_syncTimer;
  Job* m_selJob = nullptr;                        // in-flight selectNodes
  Handle(SubHighlight) m_subHl;                   // every selected sub-shape, one object in the Topmost layer
  Handle(SubHighlight) m_candidateHl;             // showCandidateRefs' faces and edges
  Job* m_candidateJob = nullptr;
  size_t m_candidateShown = 0;
  std::vector<opad::Ref> m_candidateRefs;
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
  std::vector<std::pair<std::string, Handle(AIS_Shape)>> m_compareParts;  // ViewportCompare.cpp
  std::vector<char> m_compareViews;                                       // each part's `view`
  Handle(AIS_InteractiveObject) m_compareArrows;
  void styleComparePart(const Handle(AIS_Shape)& ais, const ComparePart& part);
  class SideView;  // side by side (ViewportCompare.cpp)
  SideView* m_side = nullptr;
  Handle(V3d_View) m_sideView;
  std::set<std::string> m_sideHidden;
  Graphic3d_WorldViewProjState m_sideCamera;
  QMargins m_hostMargins;  // the parent's own, given back when the side goes
  void layoutSide();
  void drawSide(bool full);  // its camera this view's; full: everything again, else the immediate layer only
  void applySideMasks();     // which view shows what (AIS view affinity), and the layers' boxes for the z range
  void maskSide(const std::string& id, const Handle(AIS_InteractiveObject)& ais);  // a session object made while side by side
  std::vector<Handle(AIS_InteractiveObject)> m_overlays;  // showOverlay's: Fit frames the finite ones (a drawing being placed)
  std::set<std::string> m_previewHidden;  // nodes whose own object is erased while the preview shows
  bool m_bodiesPickable = true;
  SketchInput* m_sketchInput = nullptr;
  opad::Frame m_sketchFrame;
  bool m_sketchDrag = false;

  QTimer m_timer;
  bool m_glide = false;  // animateFitAll: the timer advances the camera animation itself
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
  // object snap (ViewportSnap.cpp)
  bool m_objectSnap = true;
  SnapPicks m_snapPicks = SnapPicks::None;
  std::shared_ptr<ObjectSnapState> m_osnap;
  ObjectSnapState& snapState();
  std::string sketchSnapKey(const std::string& id);  // a displayed sketch's index key, new with each version of it
  bool objectSnapActive() const;
  void updateObjectSnap();                 // after the hover, every frame
  void pruneSnapIndexes();                 // in sync: drops the indexes of what the scene no longer has
  bool objectSnapPress(QMouseEvent* e);  // true: the press picks the shown snap
};
