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
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <algorithm>
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

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include "AppDocument.hpp"
#include "BodyLook.hpp"
#include "BodyShape.hpp"
#include "ScrollInput.hpp"
#include "Theme.hpp"
#include "Tracking.hpp"
#include "ViewNav.hpp"

class JobRunner;
class Job;
class QMenu;
class QKeyEvent;
struct ObjectSnapState;
class QNativeGestureEvent;
class Toast;

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
  // Cad2D (UI-47): pan on the middle button, zoom on the wheel, no orbit (drafting, named after no product).
  enum class NavPreset { Fusion, SolidWorks, Onshape, Blender, Cad2D };
  // HiddenLine (UI-48): the faces in the background's colour with their edges and outlines, so what is behind is hidden;
  // HiddenEdges: the same with the hidden edges dashed and dim (ViewportEdges.cpp).
  enum class Style { Shaded, ShadedEdges, Wireframe, HiddenLine, HiddenEdges };
  enum class SelFilter { Body, Face, Edge, Vertex };

  explicit Viewport(AppDocument* doc, QWidget* parent = nullptr);
  ~Viewport() override;

  void setTokens(const Tokens& t);
  void setNavPreset(NavPreset p);
  NavPreset navPreset() const { return m_preset; }
  // A sliced job (UI-48), a body a step: recomputing every body at once froze the Engine for 0.5 s, its wireframes for longer.
  void setStyle(Style s);
  Style style() const { return m_style; }
  bool stylePending() const { return m_styleJob != nullptr; }
  // OPAD_BENCH_STYLES (ViewportStyleBench.cpp): each style applied in steps within the budget, the wireframe from the
  // worker's arrays, no refinement in it, hidden line hiding what is behind with outlines (UI-48)
  bool benchStyles(const QString& prefix, const std::function<void(const QString&)>& trigger);
  bool benchRefinedHighlight(const QString& prefix);  // OPAD_BENCH_REFINED_HIGHLIGHT (ViewportRefinedBench.cpp)
  // G: the grid's visibility, one state outside sketches (view/grid) and one inside (sketch/grid, on unless hidden
  // there); gridShownChanged tells the G action which one it shows.
  void setGrid(bool on);
  void configureGrid(double spacing,double extent);
  void setSelectThrough(bool on) {m_selectThrough=on;}
  // The window shown again (restored from the taskbar, uncovered): its native surface lost the frame and nothing in the
  // scene changed, so the next frame must draw everything, or the view stays black until the pointer moves over it.
  void exposedAgain();
  int exposeRedraws() const { return m_exposeRedraws; }  // benches
  int framesPainted() const { return m_framesPainted; }   // paint events that drew or showed a frame (benches, trace::traceFrames)
  QString frameState() const;  // shown, exposed, native window visible, blocked, size: for trace::traceFrames and benches
  // A native overlay over the view moved, shrank, went or changed its mask, or the system uncovered part of the view
  // (ViewOverlay.hpp): the next paint draws a whole frame (quick ones) or shows the last frame again, or the overlay's old
  // image stays there.
  void overlayUncovered();
  int overlayRepairs() const { return m_overlayRepairs; }  // paints that showed an uncovered part again (benches)
  int overlayReshows() const { return m_overlayReshows; }  // of those, the ones that drew no frame of their own (benches)
  void benchPaint() { paintEvent(nullptr); }               // a paint as the window makes it (a hidden window never paints)
  void benchFullFrameMs(qint64 ms) { m_fullFrameMs = ms; }  // a model whose frames are this slow (benches)
  // A press whose release went to another widget or window (an overlay's chip, a menu, a file dialog): the controller's
  // gesture (a rubber band following the pointer, whose next click selected everything in it) is dropped, nothing applied.
  int droppedGestures() const { return m_droppedGestures; }  // benches
  bool gestureHeld() const;  // the controller holds a pressed button now (benches)
  bool frameInvalidated() const;  // the next frame draws everything (benches)
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
  // Adaptive quality (UI-45, ViewportSettings.cpp): while the camera moves under a gesture, the wheel or an animation and a
  // frame at full quality takes longer than kSmoothFrameMs, Studio draws at 1.0x resolution without shadows and ray
  // tracing at half resolution; still for 350 ms, the view is drawn at full quality again. Setting view/adaptive.
  static constexpr qint64 kSmoothFrameMs = 20;
  void setAdaptiveQuality(bool on);
  bool adaptiveQuality() const { return m_adaptive; }
  bool degraded() const { return m_degraded; }
  qint64 fullFrameMs() const { return m_fullFrameMs; }  // the last frame drawn at full quality, ms
  // OPAD_BENCH_ORBITFPS (ViewportQualityBench.cpp): an orbit drag on a heavy model draws faster while it moves, at full
  // quality once still; one light casts shadows (UI-45)
  bool benchOrbitFps(const QString& prefix);
  static int savedRenderQuality();
  static int savedSceneBackground();
  void setSceneBackground(int style);
  int sceneBackground() const { return m_sceneBackground; }
  QColor sceneBackgroundColor() const;  // its colour (the gradient's middle; hidden line draws its faces in it)
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
  // roundFaces (with Edge, for an axis input, TODO 11 P3): cylindrical, conical and toroidal faces are picked too, beside the
  // edges, for the axis through them; other faces are not.
  void setSelectionFilter(SelFilter f, bool roundFaces = false);
  SelFilter selectionFilter() const { return m_filter; }
  bool roundFacesPickable() const { return m_roundFaces; }

  // Fit, Home and the standard views move the camera at once, or (animate, the commands) in a short animation on screen.
  void fitAll(bool animate = false);
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
  // Bodies to be shown that are not yet: waiting for their mesh or in the display queue (meshingProgress reports it).
  int remainingBodies() const { return static_cast<int>(m_waitingNodes + m_displayQueue.size()); }
  // The job that reports bodies streaming in (a load, Displaying bodies): the display pump runs as its child, so its
  // Cancel stops the pump too (UI-40). Null: the pump is a Background job.
  void setStreamJob(Job* job);
  int syncCount() const { return m_syncs; }  // full syncs so far (benches: one per document change, none per batch of meshes)
  int partialSyncCount() const { return m_partialSyncs; }  // syncs of the bodies under the nodes a change touched (UI-40)
  qint64 syncMs() const { return m_syncMs; }  // the time of both, and the UI thread's CPU time in them
  qint64 syncCpuMs() const { return m_syncCpuMs; }
  void fitSelection(bool animate = false);
  void fitNodes(const std::vector<std::string>& ids, bool animate = false);  // animate: the commands (browser, context menu)
  void standardView(const QString& name, bool animate = false);
  void home(bool animate = false);
  void rollView(double degrees);  // animated turn about the view axis; positive = counter-clockwise on screen; 2D too (twist)
  // Navigation staples (UI-47, ViewportNavigation.cpp).
  void startZoomWindow();  // the next left drag frames what to zoom into (a click zooms in twice there); Esc, right click cancel
  void cancelZoomWindow();
  bool zoomWindowActive() const { return m_zoomWindow; }
  bool previousView();  // the view the camera rested at before this one (ViewNav.hpp); false: none
  bool nextView();
  // The document's Home: the camera of its last live view op with "home" (ViewNavigation's Set current view as Home),
  // null when it has none (Home is then the iso view, fitted).
  opad::json homeCamera() const;
  bool customHome() const { return !homeCamera().is_null(); }
  void twistView(double degrees);  // the view turned this far about its axis from untwisted (2D view twist), animated
  double twistAngle() const;       // how far it is turned now, degrees
  void setAnimateViews(bool on);   // setting view/animate (default on)
  bool animateViews() const { return m_animateViews; }
  void benchAnimate(bool on) { m_forceAnimate = on; }  // benches: camera moves animate in a hidden window as on screen
  // Setting view/scrollInput (Preferences > Keyboard and mouse > Scroll wheel / trackpad): Automatic tells a wheel (zoom)
  // from a trackpad's fingers (pan, Shift orbits) by what the event carries (ScrollInput.hpp), or every scroll is one.
  // Never saved, it is the platform's assumption (a wheel on Windows and Linux, Automatic on macOS) and the first scroll
  // asks once (scrollInputQuestion). setScrollInput saves the choice.
  void setScrollInput(int mode);
  scrollinput::Mode scrollInput() const { return m_scrollInput; }
  // Benches: scrolls are told apart as on this platform ("xcb", "windows", ...; empty: the running one).
  void benchScrollPlatform(const QString& platform) { m_scrollPlatform = platform.toUtf8(); }

  // Startup (StartUp.hpp, UI-44): the OpenGL viewer is made by warmUp(), which the window calls once its shell has been
  // painted, and its first frame (the shaders) drawn by firstFrame() on a later turn; a show or paint of the view before
  // warmUp() makes nothing.
  void warmUp();
  void firstFrame();
  void setBlocked(bool on);  // while a file loads: mouse input is ignored (the shade window covers the view)
  bool blocked() const { return m_blocked; }
  const Job* pumpJob() const { return m_displayJob; }  // the display pump's job while it runs (benches)
  int pumpRuns() const { return m_pumpRuns; }          // pump jobs started so far (benches: one per stream, not per batch)
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
  // OPAD_BENCH_BIGDRAWING (ViewportDrawingBench.cpp): a drawing layer of 100,000 lines is picked in groups (UI-42): hover,
  // click, Ctrl+click, crossing and window boxes and selectRefs reach the right edges
  bool benchBigDrawing(const QString& prefix);
  // OPAD_BENCH_DRAWINGFILTERS (ViewportDrawingBench.cpp): in the Face filter every drawing layer is picked whole, in the
  // Vertex filter a big layer's ends in groups (UI-42): hover, click, a box and selectRefs reach the right ones
  bool benchDrawingFilter(const QString& prefix);
  // OPAD_BENCH_BOXSCAN (ViewportBoxBench.cpp): crossing and window boxes settle within 3 s and take exactly what is seen in
  // them (UI-43)
  bool benchBoxScan(const QString& prefix);
  // OPAD_BENCH_ORBITPIVOT (ViewportOrbitBench.cpp): the pivot of a press away from a big drawing is found run by run, fast,
  // and is the point a scan of every segment finds (UI-51)
  bool benchOrbitPivot(const QString& prefix);
  // OPAD_BENCH_WHEEL (ViewportWheelBench.cpp): nothing chosen, every scroll zooms (Windows, Linux) and the first one asks
  // once through a card (`cards`: the ones showing; `other` shows an unrelated toast, which never pushes it out);
  // Automatic: wheel notches and a high-resolution wheel's eighths of one from a device that says TouchPad zoom on xcb
  // (X11, XWayland) and pan elsewhere as before, fractions pan; and the setting, chosen through `choose` (Preferences),
  // overrides both ways; prefix (if any): <prefix>.card.png
  bool benchWheel(const std::function<bool(int)>& choose, const std::function<QList<Toast*>()>& cards, const std::function<void()>& other,
                  const QString& prefix);
  // OPAD_BENCH_CAVITYZOOM (ViewportZoomBench.cpp): wheel zoom at a face deep in an open box, in perspective and orthographic:
  // every notch gets closer, the face under the pointer stays there, unclipped and pickable, and perspective reaches inside
  bool benchCavityZoom(const QString& prefix);
  // OPAD_BENCH_TRANSPARENCY (ViewportViewBench.cpp): two translucent boxes overlap in the same colour whichever is
  // displayed last, in the rasterised qualities (UI-39)
  bool benchTransparency(const QString& prefix);
  // OPAD_BENCH_NAVIGATE (ViewportViewBench.cpp): zoom window, previous and next view, the CAD 2D preset, animated standard
  // views, fit and Home, a custom Home, the cube's menu and the 2D twist (UI-47)
  bool benchNavigation(const QString& prefix);
  // OPAD_BENCH_VIEWS (ViewsBench.cpp, help audit P7): the view commands as their guides show them, run through the window's
  // `trigger` with animations on as on screen: the seven standard views turn, Isometric is off in 2D mode, where a standard
  // view takes the grid to its plane; on a drawing in 2D mode Turn 90° left twists it and the grid stays in its plane.
  bool benchViews(const QString& prefix, const std::function<void(const QString&)>& trigger, const std::function<bool(const QString&)>& enabled);
  // OPAD_BENCH_HIGHLIGHT (ViewportViewBench.cpp): hover and selection roles in the current theme (UI-38): a body, its
  // face, edge and vertex hovered (white) and selected (hued, edges thicker in a halo), a body in the selection's own
  // colour outlined, the view cube's side in a standard view and its hover
  bool benchHighlight(const QString& prefix);
  QPointF cubeCentre() const;  // the view cube's centre, widget coordinates
  bool benchWireHighlight(const std::string& sketch, const QString& prefix);  // OPAD_BENCH_HIGHLIGHT: a sketch's wire in 3D
  // The longest displayBody so far, in wall and UI-thread CPU time (benches: no display step over 50 ms, UI-42).
  qint64 longestDisplay() const { return m_longestDisplay; }
  qint64 longestDisplayCpu() const { return m_longestDisplayCpu; }
  void benchPick();  // --bench-select: pick at the view centre through the context and log what it hit
  // Benches: a left click at a widget point as the mouse handlers deliver it (move, press, release and the frames that
  // handle them), with these modifiers held; then a plain move there (`held`: a move with them still held).
  void benchClickAt(const QPointF& at, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool held = false);
  void benchDoubleClickAt(const QPointF& at, Qt::KeyboardModifiers modifiers);  // press, release, double-click, release
  void benchHoverAt(const QPointF& at);  // a plain move there and the frame that handles it (the hover text follows)
  // The document changed: what the status said is under the pointer may be gone or renamed. Cleared; the next frame
  // says it again for whatever is still there.
  void clearHover();
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
  // ones excepted; 0 turns it off. A camera change outside a frame (benches) is reported with smallPartsCameraMoved().
  void setSmallPartFilter(double mm);
  double smallPartFilter() const { return m_smallParts; }
  bool smallPartsHidden() const { return m_smallHidden; }
  int smallPartCount() const { return m_smallCount; }  // hidden by the last move
  void smallPartsCameraMoved();
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
  // A point of the view (device pixels) where the pointer detects what `want` accepts: a candidate's id, else a body's
  // entity (its node, the detected sub-shape's kind and ordinal). `inside`: one whose eight neighbours detect it too (a
  // face), else any (a line).
  bool benchPickPoint(const std::function<bool(const std::string& candidate, const opad::Ref& entity)>& want, int& x, int& y, bool inside = true);

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
  // Bodies of this document drawn with some faces in colours of their own (Compare: a modified body's added and changed
  // faces), over the faces the file coloured. The arrays are built on a worker from the base mesh and drawn in place of
  // the zoom refinement's while the tint lasts. Node id -> colours; empty: none.
  void setFaceTints(std::map<std::string, std::shared_ptr<const opad::FaceColors>> tints);
  bool faceTintsPending() const {
    return std::any_of(m_faceTints.begin(), m_faceTints.end(), [](const auto& t) { return t.second.ais == nullptr; });
  }
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
  // OPAD_BENCH_COMPARE: the orbit pivot a press over the scene point `at` takes, in this view or (side) over A's view.
  opad::Vec3 benchOrbitPivot(const opad::Vec3& at, bool side);
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
  // The axes the current distance's ΔX, ΔY and ΔZ are drawn along: a component's (world <- component; UI-144), else world.
  void setMeasurementFrame(const gp_Trsf& toWorld);
  bool measurementHasMultipleAxes() const;
  void clearDimension();
  void setMeasurementSelectionLocked(bool locked) { m_measureSelectionLocked = locked; }
  QStringList measurementCaptions() const { return m_measureCaptions; }  // the labels as drawn (benches)

  // Select other (UI-128, ViewportSelectOther.cpp): everything picking finds under a point of the view in the current filter,
  // nearest first (bodies, faces, edges, vertices, feature candidates), never a face that only stands in front of edges and
  // vertices (UI-31's occluders) or an arc's centre finder. Alt+click lists them (selectOtherMenu): hovering a row hovers it
  // in the view, choosing one selects it as a click would (a guided tool takes it as its next pick). Tab and Shift+Tab hover
  // the next or previous one under the resting pointer in place, and a click there takes it. A plain press held still opens
  // the list as Alt+click does (pressHeld).
  struct PickCandidate { opad::Ref ref; std::string candidate; QString label; double depth = 0; };
  std::vector<PickCandidate> pickCandidates(const QPointF& at);  // widget coordinates; also what preview/choose index
  // nullptr: fewer than `fewest` things there (said in a tip when there is nothing); owned by the view, deleted once closed
  QMenu* selectOtherMenu(const QPointF& at, size_t fewest = 1);
  void previewPickCandidate(int index);  // -1: nothing hovered
  bool choosePickCandidate(int index);
  bool cycleHover(bool forward);
  // While the context menu of a right click in the view is open: where it was clicked (widget coordinates).
  bool contextMenuPoint(QPointF& at) const { at = m_contextAt; return m_inContextMenu; }

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
  TopoDS_Shape snapGlyph(const QString& kind, const opad::Vec3& at) const;  // the kind's marker about `at`, facing the camera
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
  void showCandidates(const std::vector<Candidate>& candidates);  // these and no others (those shown already stay as they are)
  void addCandidates(const std::vector<Candidate>& more);          // these too (a sliced job's slice)
  void clearCandidates();  // with the origin guide on, its planes come back
  // The origin guide (UI-51, an empty design document): the origin's axes (X red, Y green, Z blue, labelled, never
  // picked), its XY, XZ and YZ planes as candidates ({"base":"xy"}, ...) whenever nothing else shows
  // candidates, and the grid whatever its setting says.
  // grid: the grid too, whatever its setting (an empty document's ground); without (a model, Origin planes and axes), the
  // axes and planes alone, the planes shown but never picked: a face behind or on one would lose its clicks to it (New
  // sketch's plane step offers them as its own candidates).
  void setOriginGuide(bool on, bool grid = true);
  bool originGuide() const { return m_originGuide; }
  std::string hoveredCandidate() const;
  std::vector<std::string> selectedCandidates() const;  // in pick order
  // Makes the context selection exactly these (bodies, faces/edges/vertices by ordinal, candidates).
  void selectRefs(const std::vector<opad::Ref>& refs, const std::vector<std::string>& candidates = {});
  // X-ray highlight (view.xrayHighlight, setting view/xrayHighlight, on by default): the selection (bodies, their glows,
  // the selected sub-shapes) and the hover are drawn in the Topmost layer, through whatever is in front of them; off, in
  // the Top layer, hidden by what is in front (depth-tested). Applies at once (ViewportSettings.cpp).
  void setXrayHighlight(bool on);
  bool xrayHighlight() const { return m_xrayHighlight; }
  // A temporary override over the setting: the selection in Top while on, so the sketch drawn in Topmost stays over a
  // reference tool's picked body or face (SketchReference). Off again, the user's choice is back. Takes effect at the next
  // selection change (selectRefs).
  void suppressSelectionXray(bool on) { m_xraySuppressed = on; }
  bool selectionXray() const { return m_xrayHighlight && !m_xraySuppressed; }
  // Hover highlight (view.hoverHighlight, setting view/hoverHighlight, on by default): off, bodies, faces, edges, vertices
  // and design candidates are never drawn hovered; the hover is still detected (clicks, labels, hoverPoint, tracking) and
  // the selection is drawn as ever. The view cube and handles keep theirs.
  void setHoverHighlight(bool on);
  bool hoverHighlight() const { return m_hoverHighlight; }
  bool hoverDrawn() const;  // benches: a hover highlight of the model is in the frame now
  // OPAD_BENCH_HIGHLIGHT_KEYS (HighlightKeysBench.cpp): bodies one behind the other along the view; the switches run by
  // `trigger` (the window's commands): layers, frames and detection, logged as "bench: highlight keys: ..." lines.
  bool benchHighlightSwitches(const QString& prefix, const std::function<void(const QString&)>& trigger);
  void setBodiesPickable(bool on);  // off: only candidates can be picked (choosing a sketch plane, a profile)
  // Smart selection's candidate (UI-95, ViewportCandidates.cpp): what a click on its chip would select, in the candidate
  // amber, on top like the selection. Faces and edges are one object copied from the bodies' meshes (a sliced job when
  // there are many), whole bodies take the look compositor's candidate layer. Empty: nothing shown.
  void showCandidateRefs(const std::vector<opad::Ref>& refs);
  size_t candidateRefsShown() const { return m_candidateShown; }  // faces and edges drawn now (benches)
  // The design checks' findings on the model (ViewportChecks.cpp, help audit P8), until replaced or cleared: the print check's
  // faces tinted in the warning amber (overhangs) and the error red (thin walls), one object per colour copied from the
  // meshes the bodies are drawn with (a sliced job, a body a step), over everything like the selection's X-ray (TopOSD: also
  // over a body selected whole, which is in Topmost); the volume an interfering pair shares in the error red, over the pair.
  // Tints are made again when the bodies shown change (isolation, a look hiding or showing one, a body still being displayed
  // when they were asked for) or move in the view (an exploded view); the overlap is drawn where its pair is, while both are
  // drawn together. Never pickable, never framed by Fit.
  struct CheckTint {
    std::string body;
    std::vector<int> faces;  // face ordinals, or a mesh body's triangle ordinals (triangles: as the print check numbers them)
    bool error = false;      // a thin wall or a narrow face (red), else an overhang (amber)
    bool triangles = false;
  };
  void showCheckTints(const std::vector<CheckTint>& tints);
  void showOverlap(const TopoDS_Shape& shape, std::shared_ptr<const BodyPrs> prs, const std::vector<std::string>& pair = {});  // a null shape: none
  void clearCheckOverlays();
  opad::json benchCheckOverlays() const;  // the triangles of each tint, their colours and layers, the overlap's
  // Feature preview: these shapes (world coordinates, already meshed by the worker) are drawn in place of the
  // nodes they change; `hidden` nodes are not drawn at all (consumed tools, removed bodies).
  void setPreviewBodies(const std::vector<std::pair<std::string, TopoDS_Shape>>& shapes, const std::vector<std::string>& hidden);
  // The same, with arrays built on the worker (BodyPrs::build): displaying them walks no triangulation here.
  // `tint`: the operation's colour (Theme's preview roles; invalid: the selection colour), a changed body's own colour mixed
  // in; `transparency` (negative: the default); `xray`: drawn in the Topmost layer, through the bodies in front of it (a
  // cut's tool, the volume it removes).
  struct PreviewPart {
    std::string node;
    TopoDS_Shape shape;
    std::shared_ptr<const BodyPrs> prs;
    QColor tint = QColor();
    double transparency = -1;
    bool xray = false;
  };
  void setPreviewBodies(const std::vector<PreviewPart>& parts, const std::vector<std::string>& hidden);
  // Benches: the colour each preview body is drawn in and whether it is drawn through the model, in the parts' order.
  std::vector<std::pair<QColor, bool>> previewLooks() const { return m_previewLooks; }
  // Other arrays to draw for the preview bodies, in the parts' order (nullptr: their own): a handle drag's live
  // stretch while the exact preview is computed.
  void setPreviewDisplay(const std::vector<std::shared_ptr<const BodyPrs>>& arrays);
  void clearPreviewBodies();
  size_t previewBodyCount() const { return m_previewBodies.size(); }  // bench checks: a feature preview is on screen
  // Preview of bodies a feature moves as they are (a linked file Move moves as one): their own objects drawn moved by these
  // world motions, picking too, nothing meshed or copied; the rest back where they are. clearPreviewBodies ends it.
  void setPreviewMotion(const std::vector<std::pair<std::string, gp_Trsf>>& bodies);
  // Benches: how far the preview moves a body (identity: it does not).
  gp_Trsf previewMotion(const std::string& node) const;
  size_t previewMovedCount() const { return m_previewMotion.size(); }
  // Align sketch: the sketch drawn moved by a world motion (its lines and images, a location each: nothing rebuilt);
  // identity or another sketch puts the last one back. clearPreviewBodies ends it.
  void setPreviewSketch(const std::string& id, const gp_Trsf& motion);
  bool previewSketchMoved() const { return !m_previewSketch.empty(); }  // benches
  void setPreparedPreview(const TopoDS_Shape& shape,std::shared_ptr<const BodyPrs> prs,const std::vector<std::string>& hidden);
  // A preview stands in for the bodies it changes (they are erased, unpickable, and it takes no picks): holding Ctrl alone
  // over the view while `gate` allows it (a feature is open) shows those bodies as they are, pickable, the preview out of
  // the way, until Ctrl is released (the report "Ctrl shows the original to allow selecting more"). A Ctrl+click there
  // adds or takes back a pick, as everywhere. Previews made meanwhile wait for the release.
  void setPreviewPeekGate(std::function<bool()> gate) { m_peekGate = std::move(gate); }
  void setPreviewPeek(bool on);
  bool previewPeek() const { return m_previewPeek; }
  bool previewStandsIn(const std::string& node) const { return m_previewHidden.count(node) > 0; }  // benches
  bool previewShown() const;  // benches: a preview body is on screen
  // Sketch editing.
  void beginSketchInput(SketchInput* input, const opad::Frame& frame, const std::string& hiddenSketch);
  void endSketchInput();
  bool sketching() const { return m_sketchInput != nullptr; }
  // Camera along the plane normal, plane x to the right; animate: as the standard views (on screen, Animate view changes).
  void lookAt(const opad::Frame& frame, bool fit = true, bool animate = false);
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
  bool noteAnchor(const std::string& opId, QPoint& out) const;  // false: unknown, not measured yet, or behind the eye
  bool notesPending() const { return m_anchorJobs > 0; }  // anchors being measured on a worker
  int anchorsMeasured() const { return m_anchorsMeasured; }
  bool noteAnchorPoint(const std::string& opId, opad::Vec3& out) const;  // world, without a look's offset
  void setNoteLeaders(const std::map<std::string, QPoint>& ends, bool shown);  // shown=false: notes hidden, nothing drawn
  void setNoteTypeFilter(const std::string& type) { m_noteTypeFilter=type; }
  // The note / hand drawing editor (AnnotationEditor.cpp). Its target is drawn in the selection blue, tinted with a
  // dashed outline, on top of everything; the target's widget rectangle places the editor's badge.
  bool annotationPick(const QPointF& point, opad::Ref& target, bool& hit);  // body/face/edge/vertex; hit: point = where
  // Why a target could not be lit: its body is not shown (hidden, isolated away, hidden by a look), not drawn (still on its
  // way to the view, not a body, no viewer yet), or the face, edge or vertex it names is no longer in the body (it changed).
  enum class TargetMiss { None, Hidden, NotDrawn, Changed };
  // false: not in the view (`miss` says why). `add`: lit beside the ones already shown (every pick of a pinned measurement).
  bool showAnnotationTarget(const opad::Ref& target, opad::Vec3* centre = nullptr, bool add = false, TargetMiss* miss = nullptr);
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
  // What the pointer meets at `point` (widget px) as a click there would (TODO 11 P1, placing a primitive): a candidate's id,
  // else a body's face (where it is drawn: `face` is moved into the world); `at` where the pointer met it. False: nothing.
  // hoveredReference() names the face afterwards. `fresh`: a pick at `point` now; else what the last frame's hover detected
  // (one pick a frame, the view's own: a hover need not pay for a second).
  bool surfaceAt(const QPointF& point, std::string& candidate, TopoDS_Face& face, opad::Vec3& at, bool fresh = true);
  // The detected sub-shape for a worker to name (hoveredReference without its walk of the body, which a reopened document's
  // stock owners need): its body's node, the shape the view draws, the sub-shape in it, and its ordinal when the view knows
  // it (-1: opad::subshape_index(whole, sub) on the worker).
  bool hoveredSubShape(std::string& body, TopoDS_Shape& whole, TopoDS_Shape& sub, int& index) const;
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
  void zoomWindowChanged(bool active);
  void previewPeekChanged(bool on);  // Ctrl shows the bodies a preview stands in for (on), or the preview again
  void fitRequested();  // a double click of the middle button: the window fits everything (its Fit all)
  void cubeMenuRequested(const QPoint& globalPos);  // a right click on the view cube
  // The first scroll while Scroll wheel / trackpad was never chosen (Windows, Linux) has zoomed: ask once whether it was a
  // trackpad (PreferencesArea's card). Emitted once per start at most, never in a bench unless OPAD_BENCH_SCROLLASK is set.
  void scrollInputQuestion();

 public slots:
  void sync();
  void requestSync();  // coalesces mesh arrivals: at most one sync per 50 ms

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
  bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;  // trace::traceFrames: the native paint messages
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
  // Perspective zoom at the pointer (ViewportZoom.cpp): the wheel, a pinch, Ctrl+scroll and the zoom drag fly along the
  // pointer's ray towards what is drawn under it, whatever picks; the rest is OCCT's.
  void handleCameraActions(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view, const AIS_WalkDelta& walk) override;
  // Every hover and click pick of the controller: what lies behind a face is taken for nothing (dropOccluded).
  void contextLazyMoveTo(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view, const Graphic3d_Vec2i& point) override;

 private:
  // A right click on a body's face, edge, vertex or the body not selected selects it (Replace) before the menu: the menu is
  // about what is under the pointer. On what is selected, on nothing, or while a tool or an editor owns the picks: unchanged.
  void contextPick(const QPointF& at);
  int m_renderQuality = 1, m_sceneBackground = 0;
  bool m_adaptive = true, m_degraded = false;
  qint64 m_fullFrameMs = 0;
  QElapsedTimer m_wheelClock;  // the last wheel turn: zooming is navigating
  QTimer m_qualityTimer;       // still for this long: full quality again
  Graphic3d_WorldViewProjState m_qualityCamera;
  void degradeWhileNavigating();  // from every redraw: the camera moved
  void restoreQuality();
  void applyQuality();  // the rendering parameters of m_renderQuality, lowered while m_degraded
  void outlineBodies();  // silhouettes in Shaded + edges, none while m_degraded (only the flag: nothing recomputed)
  void updateDepthBias();
  bool m_twoDimensional = false;
  Handle(Graphic3d_Camera) m_threeDimensionalCamera;
  QPointF m_dragOffset, m_warpPosition;
  bool m_selectThrough=false, m_boxCrossing=false;
  Graphic3d_Vec2i m_boxStart,m_boxEnd;
  Job* m_boxJob=nullptr;
  // The view as drawn, for the box's visibility test (ViewportSelection.cpp, UI-43): the projection and the frame's depth.
  struct DepthImage;
  std::shared_ptr<const DepthImage> captureDepth();
  CursorWarpGate m_warpGate;
  void updateGridExtent();
  void alignGridPlane();  // the grid's plane: the principal plane 2D mode looks at (a standard view there changes it), else XY
  void showGrid();  // gridDrawn() on screen
  // Drawn: the G setting in force (gridShown) or the origin guide of an empty Design document (UI-51), whatever the setting.
  bool gridDrawn() const { return gridShown() || (m_originGuide && m_originGrid); }
  void applyGridColors();  // faint lines from the theme in a sketch and 2D mode, OCCT's greys in 3D
  void applyOwnCursor();   // the system pointer blank or back, as setOwnCursor asked and what is under it allows
  double layoutStep() const;  // the sketch / 2D grid's step at this zoom (0: none)
  double planePixel() const;  // world units per pixel on the sketch's plane, the longer screen direction (a tilt)
  void showOriginPlanes();
  bool m_originGuide = false, m_originPlanes = false;  // m_originPlanes: the candidates shown are the origin's
  bool m_originGrid = true;  // the origin guide draws the grid too (setOriginGuide)
  std::vector<Handle(AIS_InteractiveObject)> m_originAxes;
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
  // select other (ViewportSelectOther.cpp)
  bool selectOtherOwner(const Handle(SelectMgr_EntityOwner)& owner, opad::Ref& ref, std::string& candidate) const;
  bool detectOwner(const Handle(SelectMgr_EntityOwner)& owner);
  bool cycleKey(QEvent* e);  // Tab / Shift+Tab: cycleHover
  std::vector<Handle(SelectMgr_EntityOwner)> m_pickOwners;  // pickCandidates' owners, same order
  Graphic3d_Vec2i m_pickAt, m_cycledAt;
  bool m_hoverCycled = false;  // the hover was chosen (Tab, a list row): kept until the pointer moves, occluded or not
  bool m_selectOtherPress = false;  // an Alt+press: its release opens the list, once the double-click time has passed
  QTimer m_selectOtherTimer;
  QPointF m_selectOtherAt;
  QPoint m_selectOtherGlobal;
  // A plain left press held still for the platform's press-and-hold time opens the list too, when more than one thing is
  // under it: the controller forgets the press (no click, no rubber band) and its release is the view's.
  QTimer m_holdTimer;
  QPointF m_holdAt;
  bool m_holdPress = false;
  Qt::MouseButtons m_viewButtons = Qt::NoButton;  // pressed on the view itself (not on an overlay, a dialog or a menu)
  int m_exposeRedraws = 0, m_droppedGestures = 0, m_framesPainted = 0;
  bool m_uncovered = false;   // an overlay left part of the view since the last paint (overlayUncovered)
  bool m_frameDrawn = false;  // this paint's flush drew a frame (handleViewRedraw)
  int m_overlayRepairs = 0, m_overlayReshows = 0;
  bool m_topExposed = false;  // the window's surface as Qt last said (an Expose to shown again draws everything)
  void dropGesture();
  void pressHeld();
  QPointF m_contextAt;
  bool m_inContextMenu = false;
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
  // Objects drawn besides the document's bodies (Compare's parts) under navigation stand-ins of their own, so the orbit
  // pivot and the zoom point land on whatever is on screen: stand-in -> the object and its prototype's box.
  struct NavExtra {
    Handle(AIS_InteractiveObject) shown;
    Bnd_Box box;
  };
  std::map<const SelectMgr_SelectableObject*, NavExtra> m_navExtras;
  // What a navigation pick found when it is drawn in `view` (a body's or part's object; null otherwise), and its box.
  Handle(AIS_InteractiveObject) navDrawn(const SelectMgr_SelectableObject* picked, const Handle(V3d_View)& view, Bnd_Box* box = nullptr) const;
  Handle(V3d_View) navView() const;  // the view navigation picks in: A's after a press or wheel over it (side by side)
  bool m_navSide = false;            // the last press or wheel came from A's view
  // One perspective zoom step at a device pixel (ViewportZoom.cpp): the eye moves along the pixel's ray by a share of the
  // distance to what is drawn there (a body whatever the filter, a sketch's or a drawing's plane; nothing: the model's
  // middle), never less than a floor that keeps that surface in front of the near plane, so it never stalls and goes on
  // through an opening or past a surface reached.
  void zoomAlongRay(const Handle(V3d_View)& view, const Graphic3d_Vec2i& pixel, double delta);
  gp_Pnt centralOrbitPoint();
  bool nearestSurface(int x, int y, gp_Pnt& point);
  gp_Pnt orbitPoint(const Graphic3d_Vec2i& cursor);
  void focusCube();
  void updateCubeSide();  // the side the view looks straight at, drawn as selected (UI-38)
  void syncWindowSize();
  // look: a ghost's edges fade with it. True when its shaded presentation must be computed again (edges, hidden line): it
  // is flagged, and drawn again once shown in that mode; the other mode's is kept (no second upload switching back).
  bool applyStyle(const Handle(AIS_Shape)& ais, const BodyLook* look = nullptr);
  Job* m_styleJob = nullptr;
  // Hidden edges visible (ViewportEdges.cpp): every body's edges in world coordinates, built on a worker, drawn by two
  // objects: dashed and dim with no depth test (m_hiddenLayer, after the faces), solid against the faces' depth (m_seenLayer).
  Handle(AIS_InteractiveObject) m_edgesBehind, m_edgesSeen;
  Graphic3d_ZLayerId m_hiddenLayer = Graphic3d_ZLayerId_UNKNOWN, m_seenLayer = Graphic3d_ZLayerId_UNKNOWN;
  QTimer m_edgeTimer;
  unsigned m_edgeSerial = 0;
  void scheduleEdgeOverlay();  // after a change of the scene, a look or the style: built again shortly, or removed
  void buildEdgeOverlay();
  void clearEdgeOverlay();
 public:
  bool edgeOverlayShown() const { return !m_edgesSeen.IsNull(); }  // benches
  int outlinedBodies() const;  // benches: bodies drawn with their silhouettes
 private:
  void activateSelection(const Handle(AIS_Shape)& ais);
  bool drawingLayer(const Handle(AIS_InteractiveObject)& ais) const;  // a displayed drawing2d body (picked whole in the Face filter)
  void startMeshing(std::vector<std::string> keys);
  void displayBody(const std::string& id);
  // Objects of bodies no longer shown (UI-41): erased by retire(), removed from the context by removeRetired's background
  // job, a few per slice.
  struct Retired { Handle(AIS_Shape) ais; Handle(NavigationShape) navigation; };
  std::deque<Retired> m_retired;
  Job* m_retireJob = nullptr;
  void retire(const Item& item);
  void removeRetired();
  void finishSync(int pendingCount, bool added);
  void showShade(const std::vector<std::string>& ids);
  void refreshSubHighlight();   // rebuilds m_subHl from the context's selected faces/edges/vertices (sliced)
  void applySelectionLayers();  // selected bodies live in the Topmost layer (own depth buffer): X-ray through occluders
  QColor shownColor(const std::string& node) const;  // a displayed body's or sketch's colour as drawn; invalid if not shown
  GlowStyle glowStyle(const QColor& body, bool wholeBody) const;  // the selection over it (Highlight.hpp, UI-38)
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
  std::vector<Handle(AIS_InteractiveObject)> m_annotationTargets;  // what showAnnotationTarget lit
  std::vector<opad::Vec3> m_annotationCorners;  // the targets' boxes, world
  std::vector<Job*> m_targetJobs;               // a body target's tint, built on a worker when the body has no arrays
  std::map<std::string, NoteMark> m_notes;  // open notes by op id
  // Where each note is pinned (UI-03), by op id: measured once on a worker (opad::annotation_anchor) and again only when
  // its signature (the reference, the pinned node's body keys and placements) changes. ready && !found: nothing to pin to.
  struct NoteAnchor { size_t signature = 0; bool ready = false, found = false, queued = false; gp_Pnt at; };
  std::map<std::string, NoteAnchor> m_noteAnchors;
  size_t anchorSignature(const opad::Ref& ref) const;
  int m_anchorJobs = 0;                        // anchor jobs running
  int m_anchorsMeasured = 0;                   // anchors measured so far (benches: a sync must not measure again)
  unsigned long long m_notesRevision = ~0ull;  // the document revision the notes were laid out for (sync skips the same)
  Graphic3d_WorldViewProjState m_noteCamera;
  QSize m_noteSize;
  bool m_measureComponents = true;
  gp_Trsf m_measureFrame;
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
  bool m_roundFaces = false;  // setSelectionFilter(Edge, true): round faces picked beside the edges
  bool m_gridSnap=false;
  double m_gridStep=10;
  // view/gridSpacing (0 = automatic) and view/gridExtent, read once and when the grid settings change (configureGrid):
  // every sync's finish lays the 3D grid out again, and read them three times there.
  double m_gridSpacing=0, m_gridExtentSetting=100;
  double m_gridShownStep=0, m_gridShownExtent=0, m_gridShownX=0, m_gridShownY=0;  // the infinite grid as last laid out
  bool m_sketchGrid = true;  // the grid in sketches (sketch/grid)
  bool m_ownCursorWanted = false, m_ownCursorShown = false, m_ownCursorAside = false;  // aside: over the cube, a camera gesture
  bool m_grid = false, m_sectionEnabled = false, m_sectionCaps = true, m_initialised = false, m_needFit = false, m_warmed = false;
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
  // Display pump (UI-40): the mesh workers hand finished keys over (handOver, m_meshMu held: m_newlyMeshed, one queued
  // pumpMeshed per batch); the nodes that waited for them join m_displayQueue, which the display job works through.
  void handOver(const std::string& key);
  void pumpMeshed();
  void runPump();
  void streamSettled();
  std::vector<std::string> m_newlyMeshed;  // under m_meshMu
  bool m_pumpPosted = false;               // under m_meshMu
  std::unordered_map<std::string, std::vector<std::string>> m_waiting;  // body key -> nodes to show once it is meshed
  size_t m_waitingNodes = 0;
  std::deque<std::string> m_displayQueue;
  bool m_streamAdded = false;  // bodies displayed since the stream last settled completely
  unsigned m_pumpSteps = 0;
  int m_pumpRuns = 0;
  QElapsedTimer m_streamFit;   // the last fit while streaming
  QPointer<Job> m_streamJob;
  int m_syncs = 0, m_partialSyncs = 0;
  qint64 m_longestDisplay = 0, m_longestDisplayCpu = 0;
  qint64 m_syncMs = 0, m_syncCpuMs = 0;
  unsigned long long m_syncedRevision = 0;  // the document revision the last sync saw: the next one may take its change set
  Job* m_displayJob = nullptr;                    // the display pump's job while it runs
  QTimer m_syncTimer;
  Job* m_selJob = nullptr;                        // in-flight selectNodes
  Handle(SubHighlight) m_subHl;                   // every selected sub-shape, one object in the Topmost layer
  bool m_xrayHighlight = true;                    // the selection and the hover in Topmost (else Top: setXrayHighlight)
  bool m_xraySuppressed = false;                  // suppressSelectionXray: the selection in Top for now, whatever the setting
  bool m_hoverHighlight = true;                   // setHoverHighlight
  bool m_hoverHidden = false;                     // the model's hover highlight was taken out of the frame (hover off)
  bool hoverSuppressed(const Handle(AIS_InteractiveObject)& object) const;  // hover off and object one of the model's
  void applyHighlightLayers();                    // the highlight styles' layers as setXrayHighlight wants them
  Handle(SubHighlight) m_candidateHl;             // showCandidateRefs' faces and edges
  Job* m_candidateJob = nullptr;
  Handle(SubHighlight) m_checkTints[2];           // showCheckTints: overhangs, thin walls
  std::array<size_t, 2> m_tintTriangles{0, 0};
  Bnd_Box m_tintBoxes[2];                         // where they are (benches)
  Job* m_tintJob = nullptr;
  std::vector<CheckTint> m_tintWanted;            // what showCheckTints was last asked for
  bool m_tintAgain = false;                       // a body they colour was not shown yet, or the bodies shown changed: at finishSync
  void buildCheckTints();
  Handle(AIS_Shape) m_overlap;                    // showOverlap
  std::vector<std::string> m_overlapPair;         // the bodies it lies in
  void placeOverlap();  // with its pair's explode offset, or not drawn while they are apart or one is not drawn (no redraw)
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
  // The scene's visible sketches as wire objects. `sameGeometry`: the change could not touch any sketch's geometry or plane
  // (AppDocument::lastChange), so a sketch drawn already is only checked for being shown.
  void syncSketches(bool sameGeometry = false);
  // What a sketch's wire was built from, compared rather than printed: a dump of the Engine's big sketch took 5-8 ms a sync.
  struct SketchStamp {
    opad::json geometry, frame;
    bool matches(const opad::SketchItem& s) const { return frame == s.frame.to_json() && geometry == s.geometry; }
  };
  struct SketchWire {
    Handle(AIS_Shape) ais;
    std::shared_ptr<BodyPrs> prs;
    SketchStamp stamp;  // geometry + frame it was built from
    std::vector<Handle(AIS_InteractiveObject)> backdrops;
    BodyLook look;  // as applied (ViewportLooks.cpp); colour = the selection blue it is drawn in
  };
  std::map<std::string, SketchWire> m_sketchWires;
  // A sketch's look: the layers' entries under its own id over the sketch blue; faded lines are blended towards the
  // background (line aspects ignore alpha). Applied in place like a body's, images included.
  BodyLook sketchLook(const std::string& id) const;
  void applySketchLook(SketchWire& wire, const BodyLook& look);
  struct PreparedSketch { SketchStamp stamp; TopoDS_Shape shape; std::shared_ptr<BodyPrs> prs; std::vector<Handle(AIS_InteractiveObject)> backdrops; bool ready=false; };
  std::map<std::string,std::shared_ptr<PreparedSketch>> m_preparedSketches;
  std::string m_hiddenSketch;  // being edited: the editor draws it
  std::vector<std::pair<std::string, Handle(AIS_Shape)>> m_candidates;
  Handle(AIS_Shape) displayCandidate(const Candidate& c);
  std::vector<Handle(AIS_Shape)> m_pointMarks;  // markPickedPoints
  std::vector<Handle(AIS_Shape)> m_previewBodies;
  std::map<const AIS_InteractiveObject*, int> m_previewModes;  // each preview's display mode, to show it again after a peek
  bool m_previewPeek = false;
  std::function<bool()> m_peekGate;
  void standIn(const std::string& node);  // a preview stands in for it: erased (not while peeking), its glow gone
  void showOriginal(const std::string& node);  // as it is: displayed and pickable again
  void showPreviewPart(const Handle(AIS_Shape)& ais, int mode);  // displayed, or kept back while peeking
  std::vector<std::pair<QColor, bool>> m_previewLooks;  // previewLooks
  std::vector<std::pair<std::string, Handle(AIS_Shape)>> m_compareParts;  // ViewportCompare.cpp
  std::vector<char> m_compareViews;                                       // each part's `view`
  std::vector<Handle(AIS_InteractiveObject)> m_compareNav;                // the parts' navigation stand-ins (m_navExtras)
  Handle(AIS_InteractiveObject) m_compareArrows;
  struct FaceTint {
    std::shared_ptr<const opad::FaceColors> colors;
    const AIS_InteractiveObject* ais = nullptr;  // drawn on this object (null: its arrays are being built)
    std::string key;
  };
  std::map<std::string, FaceTint> m_faceTints;  // setFaceTints
  unsigned m_faceTintSerial = 0;
  bool faceTinted(const std::string& id) const { return m_faceTints.count(id) > 0; }
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
  std::unordered_map<std::string, gp_Trsf> m_previewMotion;  // setPreviewMotion: bodies drawn moved by these
  std::string m_previewSketch;                                // setPreviewSketch: the sketch drawn moved, and its objects' own places
  std::vector<std::pair<Handle(AIS_InteractiveObject), gp_Trsf>> m_previewSketchWas;
  // Where an item is drawn: its look's offset after the preview's motion after its placement (rigid; else in `located`).
  gp_Trsf drawnAt(const std::string& id, const std::array<double, 3>& offset, bool rigid, const gp_Trsf& placement) const;
  void placeItem(const std::string& id, Item& item);  // its object (and orbit pivot, glow) where drawnAt says
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
  scrollinput::Mode m_scrollInput = scrollinput::Mode::Automatic;
  bool m_scrollAsk = false;
  QByteArray m_scrollPlatform;  // benches
  QByteArray scrollPlatform() const;  // the running one, or the bench's
  void readScrollInput();  // m_scrollInput and m_scrollAsk from the settings
  scrollinput::Scroll scrollOf(const QWheelEvent* e) const;  // what isTrackpad and panStep look at
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
  // navigation (ViewportNavigation.cpp, UI-47)
  bool m_zoomWindow = false, m_zoomDrag = false, m_cubeMenu = false, m_animateViews = true, m_forceAnimate = false;
  QPointF m_zoomFrom, m_zoomTo;
  void showZoomBand();
  void finishZoomWindow();
  ViewHistory m_history;
  QTimer m_settleTimer;  // the camera has rested: settleView
  Graphic3d_WorldViewProjState m_settleCamera;
  void settleView();
  ViewState viewState() const;
  void goTo(const ViewState& state);
  bool animationsShown() const;  // only on screen: a hidden window (benches) has no frames to run them, moves at once
  // `move` changes the camera at once; animated, the camera is put back and animated to where it led.
  void moveCamera(bool animate, double seconds, const std::function<void()>& move);
  void animateCamera(const Handle(Graphic3d_Camera)& end, double seconds);
  void finishAnimation();  // a running camera animation jumps to its end
  gp_Dir naturalUp() const;
  bool applyHomeCamera();  // the camera to the document's Home (homeCamera); false: none to go to
  bool zoomWindowKey(QObject* object, QEvent* e);  // Esc leaves the zoom window before anything else sees it
};
