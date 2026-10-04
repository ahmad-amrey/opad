#pragma once
#include <QMap>
// The sketch editor: draws and constrains one sketch on its plane. It takes the left mouse button and plain
// keys from the viewport (SketchInput), keeps its own undo stack, solves after every change (a change the
// solver cannot satisfy is refused, so a sketch is never left over-constrained) and draws everything itself
// through one overlay object. Nothing reaches the document until the controller finishes the sketch.
#include <AIS_InteractiveObject.hxx>
#include <QLineEdit>
#include <QObject>
#include <QTimer>
#include <QPointer>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "AppDocument.hpp"
#include "DynamicInput.hpp"
#include "Viewport.hpp"
#include "GuidedTool.hpp"
#include "InputKeys.hpp"
#include "SketchKeys.hpp"
#include "SketchSnap.hpp"
#include "SnapMarkers.hpp"
#include "opad/design/sketch.hpp"
#include <QElapsedTimer>

class JobRunner;
class Job;
class SketchGeometryCache;
namespace opad::design {
struct CurveCuts;
}
class DimensionHandle;

class SketchEditor : public QObject, public SketchInput {
  Q_OBJECT
 public:
  // Widget px around a point or a constraint glyph that take a click or a drag: a 24 px target at any scale (UI-124);
  // curves keep the narrower tol() so the ones close together stay apart.
  static constexpr double kHandlePixels = 12;
  SketchEditor(AppDocument* doc, Viewport* viewport, JobRunner* jobs, QObject* parent = nullptr);
  ~SketchEditor() override;

  // `sketchId` empty: a new sketch. `geometry` is what it holds so far (empty object for a new one).
  void begin(const std::string& sketchId, const QString& name, const opad::json& plane, const opad::Frame& frame, const opad::json& geometry);
  void end();
  bool active() const { return m_active; }
  bool visible() const { return m_visible; }
  void setVisible(bool visible);
  const std::string& sketchId() const { return m_id; }
  QString name() const { return m_name; }
  opad::json plane() const { return m_plane; }
  const opad::Frame& frame() const { return m_frame; }
  opad::json geometry() const { return m_sk.to_json(); }
  opad::json geometryDelta() const { return opad::design::sketch_delta(m_initialGeometry, geometry()); }
  bool modified() const { return m_modified; }
  bool empty() const;  // nothing but the origin
  // Copies one entity per UI slice, then serializes on a worker. A changed sketch
  // invalidates the capture rather than mixing geometry from different edits.
  void captureRecovery(std::function<void(opad::json, const QString&)> done);
  void restoreRecovery(const opad::json& state);
  opad::json agentContext() const;

  // Tools: select, line, rect, crect, circle, circle3, arc3, arcc, polygon, slot, point, spline, ellipse, fillet,
  // trim, mirror, dimension, and "c:<constraint>" (horizontal, vertical, coincident, parallel, perpendicular,
  // tangent, equal, concentric, midpoint, symmetric, collinear, fix).
  void setTool(const QString& tool);
  QList<ToolStep> toolSteps() const;
  // What was typed for `key`, else `fallback`; a fallback in millimetres ("2 mm") is offered in the document's unit.
  QString option(const QString& key, const QString& fallback = {}) const;
  void setOption(const QString& key, const QString& value) { m_options[key] = value; }
  QString dimensionText(const opad::design::SkConstraint& c) const;  // "R1 in", "fx: 12.5 mm", "(45°)"
  void applyTool();
  void previewTool();
  void invalidatePreview(bool keepOverlay = false);  // keepOverlay: the shown one stays until the next replaces it (live drags)
  void dropPreviewJob();  // a preview being computed is cancelled: a click or a key acts on the sketch as it is
  void scheduleToolPreview();
  bool placePrecise(const QString& u, const QString& v, int mode);  // false: not placed, the status says why
  // The editing keys and the panel's buttons (SketchKeys.hpp): Backspace / Undo point, Enter / Done, Esc (one rung of
  // the ladder) and Close tool (Esc until the tool is closed). Each returns whether it did something.
  sketchkeys::State keyState() const;
  bool undoPoint();
  bool done();
  bool escape();
  void closeTool();
  QString keyHints() const;  // what Backspace, Enter, Esc and Shift do now, for the prompt
  QString prompt(bool note = false) const;  // "<tool>: <the step that waits>" (the prompt bar's step), then the tool's note
  // The command line (UI-133, SketchCommands.hpp): a point or the tool's values typed into the step's boxes key by key as
  // over the view, then Enter; the reason when that did not work (nothing typed stays behind then), else empty.
  QString enter(const QString& text);
  bool closeChain();  // the polyline back to its first point
  // The clipboard (UI-129, SketchClipboard.cpp): the selected curves with their points and the constraints among them, about
  // a base point (the lower left of their extent; the copybase tool asks for one), as kClipMime, so another OPAD window
  // pastes them too; Cut deletes them after (one undo step). Paste reads the clipboard on a worker, then the paste tool
  // carries the curves by their base point (snaps apply; typed: X,Y, or @dx,dy from where they were copied) until a click
  // places them, one undo step, selected after.
  static constexpr const char* kClipMime = "application/x-opad+json";
  bool copySelection(bool cut);  // false: nothing to copy (the status says why)
  void copyWithBase();           // the copybase tool: the selection is copied about the point clicked next
  void paste();
  bool pasting() const { return m_tool == "paste" && m_clip != nullptr; }
  void toggleReference();
  void selectConnected();
  void selectType();
  void selectAll(bool invert = false);  // Ctrl+A: every curve that can be picked; invert: those not selected now (UI-111)
  const std::vector<int>& selected() const { return m_sel; }
  void deleteNode();
  void redefinePlane(const opad::json& plane, const opad::Frame& frame);
  QString tool() const { return m_tool; }
  void editSplineNode();
  void findOpenVertices();
  void insertSplineNode(double u,double v);
  void toggleConstruction();
  void deleteSelection();
  void fitSketch();
  void analyseSketch();
  bool busy() const {return m_editJob!=nullptr || !m_geometryJob.isNull();}
  bool canUndo() const { return !m_undo.empty() || m_tool!="select"; }
  bool canRedo() const { return !m_redo.empty(); }
  void undo();
  void redo();
  int dof() const { return m_solved.dof; }
  // What the overlay draws now, for benches: "points", "rings" (free points), "bigPoints" and "texts" (glyphs, labels).
  QMap<QString, QStringList> drawn() const;
  void bench(const QString& script);  // OPAD_BENCH_DESIGN: draws a dimensioned rectangle with a hole through the tool code paths
  void benchWorkflow();
  void benchPrimitives();
  void benchModify();
  void benchHandles();
  void benchDrag();
  void benchGrid();
  void benchLadder();
  void benchKeys();
  void benchShapes();
  void benchCrossLock();
  void benchSnaps();
  void benchSteps();
  void benchGridCursor();
  void benchCommandLine();
  void benchClipboard();
  void benchEdits();
  void benchApply();
  void benchPointer();
  void refreshSnap();  // a snap setting changed (Ortho, a snap kind): read again, the pointer's snap again where it is
  size_t settingsReads() const { return m_settingsReads; }  // benches: once per change, never per mouse move
  // Show constraints (UI-24, setting sketch/showConstraints): their badges and coincidence dots; off, only those in conflict
  // or selected show.
  bool showConstraints() const { return m_showConstraints; }
  void setShowConstraints(bool on);
  void benchConstraints();
  void benchLarge(const QString& output, opad::json metrics);

  // SketchInput
  void sketchPress(double u, double v, Qt::KeyboardModifiers mods) override;
  void sketchMove(double u, double v, Qt::KeyboardModifiers mods, bool dragging) override;
  void sketchLeave() override;
  void sketchRelease(double u, double v, Qt::KeyboardModifiers mods) override;
  void sketchDoubleClick(double u, double v) override;
  bool sketchKey(QKeyEvent* e) override;
  bool sketchType(QKeyEvent* e) override;

 protected:
  bool eventFilter(QObject* o, QEvent* e) override;  // Esc in the dimension field

 signals:
  void toolChanged(const QString& tool);
  void status(const QString& text);  // what the tool waits for, or why a change was refused
  void changed();                    // geometry, selection or undo state
  void workflowChanged();
  void hintsChanged();  // what Shift does changed (the pointer onto a guide or off it): keyHints() again

 private:
  friend class SketchPanel;
  friend class ClipReplay;  // the help clips replayed into the tools (ClipReplayBench.cpp)
  opad::design::SolveOptions solveOptions() const;
  bool selectable(int id) const;
  void runSketchEdit(const QString& label,std::function<void(opad::design::Sketch&)> work);
  struct Snap;
  bool primitiveClick(const Snap& s);
  void finishPrimitive();
  // An arc slot's or a centre arc's sweep as the pointer went round its centre from the start (TODO 11 wave 3, P5): signed,
  // counter-clockwise positive, past half a turn too, so the slot or arc runs the way the pointer went (the slot always ran
  // counter-clockwise, the arc the shorter way); without a pointer that went round, the shorter way to (u, v).
  // trackSlotSweep keeps it as the pointer moves.
  double slotSweep(double u, double v) const;
  void trackSlotSweep();
  struct SlotSweep { double sweep = 0, last = 0, ou = 0, ov = 0, su = 0, sv = 0; };  // the sweep so far, the pointer's last angle, for these clicks
  std::optional<SlotSweep> m_slotSweep;
  opad::design::Sketch primitivePreview() const;
  opad::json primitiveOptions() const;
  void createText(double u,double v);
  bool modifyClick(double u,double v);
  bool applyModify();
  struct Clip {  // what the paste tool places: the clip, its base point, its curves as polylines about the base (a box when many)
    opad::json data;
    double bu = 0, bv = 0;
    size_t curves = 0;
    std::vector<std::vector<std::pair<double, double>>> outline;
  };
  std::shared_ptr<const Clip> m_clip;
  int m_clipRevision = 0, m_copies = 0;  // the paste read last, the copy made last
  bool copyFrom(const std::vector<int>& ids, double bu, double bv, bool cut);
  bool clipClick(const Snap& s);  // the paste and copybase tools' click
  struct Snap {
    // A constraint the click's new point gets with `ref` (UI-21): Midpoint of a line, Coincident on a second curve (an
    // intersection), Horizontal / Vertical with a tracked point or a circle's centre (a quadrant).
    struct Hold { opad::design::SkConstraint::Type type; int ref; };
    double u = 0, v = 0;
    int point = 0;     // an existing point to reuse
    int entity = 0;    // a curve the new point will lie on
    std::vector<Hold> holds;  // what else holds the new point there (with automatic constraints on)
    // The segment the click ends (the line tool), with `ref` (UI-23): Perpendicular to a line, Tangent to a circle or an
    // arc, a circle's centre Coincident on it (square to the circle).
    std::vector<Hold> segment;
    bool horizontal = false, vertical = false;  // from the step's last point (fromPoint)
    bool grid = false;  // on a grid node, or whole grid steps along the inference
    // What the pointer was pulled to, for the display: that object is highlighted and named beside the cursor.
    enum class Kind { None, Point, Midpoint, Quadrant, Intersection, Apparent, Perpendicular, Tangent, Curve, Extension, Aligned, Cross, Angle, Locked, Grid, Typed } kind = Kind::None;
    int target = 0, other = 0;  // the point (Point, Aligned, Angle), the crossing guides' points (Cross, Locked) or the curves (the others)
    int curve = 0;  // Cross, Locked: the curve a guide crosses there
    sketchsnap::Guide line;  // the guide or angle ray it lies on (onLine): what Shift locks onto
    bool onLine = false;
    int stops = 0, stop = -1;  // Locked: the stops along the line in reach, the one it is on (-1: none)
    int choices = 0, choice = 0;  // object snaps in reach of the pointer, the one shown (Shift taps go through them, UI-23)
    bool ortho = false;  // Locked: by Ortho (F8), not by Shift
    std::map<QString, std::pair<double, QString>> typed;  // the values typed for this click (mm, radians; as typed)
  };
  struct Hit {
    enum Kind { None, Point, Entity, Dimension } kind = None;
    int id = 0;
  };
  Snap snap(double u, double v, bool infer = true, bool grid = true) const;  // grid: grid snapping as switched (false: as if off)
  // Typed values (UI-16, SketchDynamicInput.cpp): the boxes of the step that waits (an option of the tool, or where the
  // next point goes), which keys type into them, and using what was typed (Enter, or a click: the typed values win, the
  // pointer gives the rest). A point's typed values hold the rubber band at once (typedPoint); '#', '@', ',' and '<' switch
  // between X/Y, ΔX/ΔY from the last point and length/angle from it (entryKey).
  QList<DynamicInput::Field> inputStage() const;
  bool typingKey(const QKeyEvent* e) const;
  bool appliesOnEnter() const;  // an option tool with what it applies to picked: Enter applies
  bool useTyped(const Snap* at = nullptr);
  bool pointTyped() const;  // a value typed that places the next point (not only an option of the tool)
  void updateInput();
  inputkeys::Entry entry() const;
  QString inputStep() const;                         // the step that waits: a chosen entry lasts while it does
  bool inputBase(double& u, double& v) const;        // the last point, what ΔX/ΔY and length/angle are measured from
  double angleReference() const;                     // what a typed angle is measured from (radians from X)
  void retype();                                     // the typed values evaluated, the rubber band moved to them
  Snap typedPoint(const Snap& pointer) const;        // where the next point goes: the typed values, the pointer the rest
  bool entryKey(int box, QChar c);                   // DynamicInput's key hook
  void forgetTyped();
  void setAngled(bool angled);  // the second box of a chamfer (a move): its angle to the first line (the move's direction)
  // A shape's own sizes (UI-17): the step's boxes (a rectangle's width and height after its first corner, a slot's width
  // after its centres), where they put the click, and what the click made keeps them as driving dimensions (setting
  // sketch/input/addDimensions, on by default) in the shape's own undo step. The rubber band reads them out as it goes.
  bool shaped() const;                                // the step that waits takes the shape's sizes
  QList<DynamicInput::Field> shapeFields() const;
  bool shapePoint(const Snap& pointer, double& u, double& v) const;
  int keepTyped(const Snap& s, const char* key, opad::design::SkConstraint::Type type, std::vector<int> refs, double scale = 1);  // 0: none
  void labelOff(int id, int line, double u, double v, double offset);  // its value beside the line, away from (u, v)
  void labelAt(int id, double u, double v);
  // A typed angle (`angle` the direction it made, radians from X): horizontal or vertical (`axis`: a line, or two points),
  // else against the line before (`line` after `previous`).
  void keepDirection(const Snap& s, const char* key, std::vector<int> axis, double angle, int line = 0, int previous = 0);
  void keepAligned(const Snap& s, std::vector<int> axis);  // the click's horizontal or vertical from the step's last point (UI-23)
  int referenceX();                                   // a fixed line along +X to hold an angle from the X axis against
  bool keepSweep(const Snap& s, int arc, int radius, double r);  // a typed sweep as the arc's length, radius dimension times it
  int pointAt(double u, double v) const;              // an existing point exactly there (typed values land on it)
  const opad::design::SkPoint* pointOf(int id) const;  // through the geometry cache's index (a mouse move never scans, UI-27)
  bool tangentStart(double& u, double& v, double& tu, double& tv) const;  // a tangent arc's line end and the way it leaves it
  std::vector<std::pair<double, double>> filletPreview(int corner) const;  // the fillet's arc at a corner (empty: none fits)
  double unitLength() const;                          // mm in one unit of the document
  // A size or an angle the rubber band reads out, at (u, v); an angle's arc about (cu, cv) of radius r > 0 from the direction
  // `from` through `sweep`, a size's leader from (fu, fv) to (tu, tv) where the rubber band does not draw it already.
  struct Readout {
    QString key;  // the box that takes it: that box sits there instead of the text (off (bu, bv) the way (bx, by))
    double u = 0, v = 0, bu = 0, bv = 0, bx = 0, by = 1;
    QString text;
    bool locked = false;  // typed: it holds
    double ox = 0, oy = 1, ext = 0;  // the way it sits off what it measures, how far it reaches that way (its padlock beyond)
    double cu = 0, cv = 0, r = 0, from = 0, sweep = 0;
    bool leader = false;
    double fu = 0, fv = 0, tu = 0, tv = 0;
  };
  std::vector<Readout> readouts() const;
  bool boxed(const QString& key) const;               // a box of the step takes that readout's value (and sits on it)
  QStringList transientTexts() const;                 // what the rubber band reads out (benches)
  size_t transientLocked() const;                     // segments drawn thick dashed: a Shift lock's line (benches)
  size_t transientCursor() const;                     // segments of the drawing cursor drawn (benches)
  // What the rubber band draws now, by kind (benches; the clip replay compares it with what its clip shows): the curves a
  // click makes ("line", "arc", "circle", "ellipse", "spline", "construction <kind>" for those it makes as construction, a
  // dashed guide among them), "line" for a straight band to the pointer, "outline" for each of a text's or a paste's
  // outlines, "trim" the piece a click removes, "extension" where an end runs to, "frame" a picture's, "measure" a distance.
  const std::map<QString, int>& rubberKinds() const { return m_rubberKinds; }
  std::map<QString, int> m_rubberKinds;
  bool cursorCrisp() const;                           // its arms lie on whole device pixels (benches)
  QStringList overlayTexts() const;                   // the texts the sketch's overlay draws (benches)
  size_t badgeTriangles() const;                      // the constraint badges' backs, two triangles each (benches)
  size_t coincidenceDots() const;                     // the dots drawn for coincidences, explicit and where curves meet (benches)
  size_t transientSolid(const QColor& c) const;       // rubber band and highlight segments in that colour (benches)
  size_t transientDashed(const QColor& c) const;      // dashed ones (a frame, a measure being taken)
  size_t sketchSolid(const QColor& c) const;          // the sketch's own curves' segments drawn in that colour (benches)
  bool drawsCursor() const;  // grid snapping: the editor draws the drawing cursor at the snapped point, the pointer is hidden
  std::optional<std::pair<double, double>> m_drawnCursor;  // where it was last drawn (none: not drawn), sketch coordinates
  bool m_inTransient = false;  // updateTransient is telling the viewport whether it draws the cursor
  size_t m_transientRedisplays = 0;  // the rubber band's overlay redisplayed (benches: not again for the same picture)
  std::optional<snapmarkers::Marker> m_marker;        // the marker drawn where the pointer snapped (none: a dot)
  double m_markerTurn = 0;                            // its turn on the screen (radians): an extension's follows its line
  Hit hitTest(double u, double v) const;
  double tol() const;  // pick distance in sketch units
  int pointFor(const Snap& s);           // reuse or create (with the on-curve constraint and the snap's holds)
  // The constraints a click at `s` adds, for the pictograms beside the pointer (UI-21): the new point's (it reuses a point,
  // lies on a curve, holds as the snap says) when the click that waits makes a point there, the segment's (the line tool).
  std::vector<snapmarkers::Glyph> snapGlyphs(const Snap& s) const;
  bool pointHere() const;    // the click that waits makes a point where it lands (else it sizes or passes a curve through)
  bool curveHere() const;    // the click that waits passes a curve through where it lands (a circle's rim): a point there lies on it
  bool alignsHere() const;   // the click's horizontal or vertical from the step's last point is kept as a constraint
  // The point the step's next click is measured from (UI-23): a polyline's or spline's last point, else the shape's last
  // click (an arc's end: its centre) for the tools whose next point has a direction from it; id: its point (-1: none).
  bool fromPoint(double& x, double& y, int& id) const;
  std::vector<snapmarkers::Glyph> m_glyphs;  // the pictograms drawn beside the pointer (benches)
  void begin_change();                   // snapshot for undo
  bool end_change(const QString& what);  // solve; false = refused and rolled back
  void cancel_change();
  void rebuild();                        // redraw the overlay
  void updateTransient();
  // The text tool's letters as they will land, relative to the insertion point (cached per text, font and height).
  const std::vector<std::vector<std::pair<double, double>>>& textPreview();
  QString m_textPreviewKey;
  std::vector<std::vector<std::pair<double, double>>> m_textPreview;
  bool prepareGeometry();
  void scheduleFill();
  void click(const Snap& s, Qt::KeyboardModifiers mods);
  void finishChain();
  bool applyConstraint(opad::design::SkConstraint::Type type, const std::vector<int>& ids, bool quiet);
  void constraintClick(const Hit& h);
  void dimensionClick(const Hit& h, double u, double v);
  void placeDimension(double u, double v);
  void editDimension(int id, bool fresh);
  void commitDimensionEdit();
  void filletAt(const Hit& h, double u, double v);
  void trimAt(const Hit& h, double u, double v);
  bool trimPiece(int id, double u, double v, QString& why);  // inside a change; false: nothing changed, why
  std::vector<std::pair<double, double>> trimPreview(int id, double u, double v) const;  // the piece trimAt would remove
  // Fence trim (UI-28): a drag with the trim tool; every piece the fence crosses goes, in one undo step.
  std::vector<std::tuple<int, double, double>> fenceHits(double au, double av, double bu, double bv) const;  // curve, where, in order
  int curveThrough(double u, double v) const;
  void fenceTrim(double au, double av, double bu, double bv);
  bool m_fencing = false, m_fenceMoved = false;
  double m_fenceU = 0, m_fenceV = 0, m_fenceToU = 0, m_fenceToV = 0;
  Qt::KeyboardModifiers m_fenceMods;
  // One-click extend (UI-28): where the end nearer (u, v) of the hovered line or arc would run to, as a polyline (empty:
  // nowhere); cached for the hovered curve and end while the sketch stays as it is.
  std::vector<std::pair<double, double>> extendPreview(int id, double u, double v);
  std::tuple<int, bool, int> m_extendKey{0, false, -1};
  std::vector<std::pair<double, double>> m_extendShown;
  // Where the other curves cross a spline or an ellipse the trim hovers (core curve_cuts, the kernel's, on what their samples
  // bring near it), and a line, a circle or an arc (splines and ellipses by their samples; the click asks the kernel), for
  // this model revision: a move over the same curve, or a fence over the same curves, looks them up.
  struct TrimCrossings;
  mutable std::map<int, std::shared_ptr<const opad::design::CurveCuts>> m_trimCuts;
  mutable std::map<int, std::shared_ptr<const TrimCrossings>> m_trimCrossings;
  mutable int m_trimCutsRevision = -1;
  bool mayCross(const opad::design::SkEntity& a, const opad::design::SkEntity& b) const;  // their samples come near; unsure: true
  // Dragged points snap and merge on drop (UI-28): the point or curve the dragged point is held to, kept on release (the
  // point merged into it, or the point put on the curve) when the sketch still solves.
  int m_dropPoint = 0, m_dropCurve = 0;
  double m_dropU = 0, m_dropV = 0;
  bool dropTarget(int dragged, double u, double v, double& x, double& y);  // sets m_dropPoint / m_dropCurve
  void mirrorSelection(int axisLine);
  void offsetSelection();
  void updateDimensionHandle();
  void referenceHover();
  void pickReference();
  // The reference a replayed press is on (ClipReplayBench.cpp: a clip's click on a body, found where the clip shows it), which
  // pickReference takes before the view's hover; none otherwise.
  std::optional<opad::Ref> m_replayReference;
  // The reference tools' sources (TODO 11 wave 3, P4): picked in the view or chosen in the panel, each a JSON reference;
  // a pick toggles one (a picked one again drops it), the preview shows them all and Enter or Apply adds them together.
  void toggleSource(const QString& source);
  QString sourceLabel(const QString& source) const;
  QStringList m_sources;
  // The picked sources in the model (edges, faces, vertices, bodies) shown as the view's selection while the tool runs, as
  // a feature's picks are; called by rebuild(), so every change of m_sources reaches the view.
  void showSources();
  QStringList m_sourcesShown;
  bool applyReference();
  bool applyImageTool();
  bool imageClick(double u,double v);
  // Transform image (TODO 11 wave 3, P5, as its guide shows): a press on a backdrop picture and a drag move it, the picture
  // following the pointer; the release keeps its new place (one undo step) and the panel's X and Y show it, the other
  // values wait for Enter or Apply as before. A press off the pictures does nothing.
  struct ImageDrag { int id = 0; size_t index = 0; double u = 0, v = 0, x = 0, y = 0, du = 0, dv = 0; bool moved = false; };
  std::optional<ImageDrag> m_imageDrag;
  int imageAt(double u, double v) const;  // the backdrop whose picture is under (u, v), the one the panel shows first; 0: none
  bool imagePress(double u, double v);
  void imageDragTo(double u, double v);
  void imageRelease();
  void imageFrame(int id, double du, double dv, std::vector<std::pair<double, double>>& corners) const;  // its corners, moved by (du, dv)
  // Insert image: the picture's size in pixels as shown (its file's header only, read once per file); invalid: unknown.
  QSizeF insertPicture();
  QString m_insertFile;
  QSizeF m_insertPicture;
  QString m_calibrateShown;  // the Known distance calibrate's two clicks put there (a typed one is not replaced)
  // A click with a modify tool on a curve (TODO 11 wave 3): it toggles the curve, or with the tool's "Select connected
  // chain on click" on (offset by default) the whole connected chain. Per tool: setting it for one left the others alone.
  bool chainOnClick() const;
  void pickCurve(int id);
  void applied();  // after an Apply that changed the sketch: the tool asks for its next curves, sources or line
  void refreshImages();
  std::vector<std::pair<double, double>> sampled(const opad::design::SkEntity& e) const;  // polyline of a curve, sketch coordinates
  double distanceTo(const opad::design::SkEntity& e, double u, double v) const;
  void labelPosition(const opad::design::SkConstraint& c, double& u, double& v) const;
  void toolPrompt();
  bool isFixedPoint(int id) const;

  AppDocument* m_doc;
  Viewport* m_viewport;
  JobRunner* m_jobs;
  bool m_active = false, m_modified = false, m_visible = true;
  std::string m_id;
  QString m_name;
  opad::json m_plane;
  opad::json m_initialGeometry;
  opad::json m_cameraBefore,m_sectionBefore;
  std::string m_imagesStamp;int m_imageRevision=0;Job* m_imageJob=nullptr;
  std::vector<Handle(AIS_InteractiveObject)> m_imagePrs;
  QMap<QString,QString> m_options;
  bool m_panelFieldsDirty = false;
  int m_session=0,m_modelRevision=0;
  Job* m_editJob=nullptr;
  bool m_previewRequested=false,m_previewComputing=false;
  QTimer m_toolPreviewTimer;
  QPointer<DimensionHandle> m_dimensionHandle;
  QPointer<DynamicInput> m_input;
  int m_offsetAnchor = 0;  // the selected curve the offset arrow sits on: the last one hovered
  int m_previewRevision=0;
  std::shared_ptr<opad::design::Sketch> m_toolPreview;
  Handle(AIS_InteractiveObject) m_toolPreviewOverlay;
  // Shows it over everything, the X-ray layer too: a picked source's highlight (an edge right above its projection, a
  // face or a body over its outline, seen square to the sketch) and a hover must not hide the preview of what it gives.
  void showToolPreview();
  opad::design::SolveResult m_previewSolved;
  QString m_selectionFilter = "all",m_constraintFilter;
  std::set<int> m_conflicts;
  std::vector<std::tuple<int,double,double>> m_glyphHits;  // each constraint badge: its constraint and centre (picking, hover)
  std::vector<std::tuple<int,double,double>> m_coincidentDots;  // each coincidence drawn: its constraint and point
  std::vector<int> m_joinDots;  // points two curves end on (a coincidence they share, no constraint): drawn as its dot; sorted
  bool m_showConstraints = true;
  void pixelAxes(double& rx, double& ry, double& ux, double& uy) const;  // one screen pixel right and up, in sketch coordinates
  bool m_boxSelecting = false;
  bool m_undoPending=false;
  double m_boxU=0,m_boxV=0;
  opad::Frame m_frame;
  opad::design::Sketch m_sk;
  opad::design::SolveResult m_solved;
  struct History {opad::design::Sketch geometry;opad::json plane;opad::Frame frame;};
  std::vector<History> m_undo,m_redo;
  opad::json m_beforePlane;opad::Frame m_beforeFrame;
  opad::design::Sketch m_before;  // the state a change started from
  bool m_inChange = false;

  QString m_tool = "select";
  std::vector<Snap> m_clicks;      // of the running tool
  std::vector<int> m_chain;        // points of the polyline / spline being drawn
  size_t m_chainUndoStart=0;
  std::vector<int> m_picked;       // constraint / dimension tool picks
  int m_polygonSides = 6;
  opad::design::SkConstraint m_pendingDim;  // picked, waiting for its place
  bool m_placingDim = false;
  std::vector<int> m_sel;
  std::set<int> m_dangling;
  Hit m_hover;
  Snap m_cursor;   // where the next point goes (the typed values applied)
  Snap m_pointer;  // where the pointer put it
  bool m_haveCursor = false;
  std::map<QString, double> m_typedValues;  // the point's typed values that evaluate (mm, radians)
  std::optional<inputkeys::Entry> m_entry;  // switched by a prefix, for the step m_entryStep
  QString m_entryStep;
  bool m_angleRelative = false;  // setting sketch/input/angleRelative: a polyline's typed angles from its last segment
  bool m_circleRadius = false;   // setting sketch/input/circleRadius: a circle's box takes its radius, not its diameter
  // Cross-locking (UI-19): points acquired by resting on them (oldest first, at most 6) add their guides; Shift locks the
  // pointer onto the guide it is on (or the way from the last point to it) while held, a tap until a click or Esc; then
  // Shift taps go through the stops along the line (stop: the one shown, counted from where the pointer was, su sv).
  std::vector<int> m_tracked;
  int m_dwellPoint = 0;  // the point the pointer rests on
  QTimer m_dwellTimer;
  struct Lock { sketchsnap::Guide line; bool horizontal = false, vertical = false, sticky = false; int stop = -1; double su = 0, sv = 0; std::vector<Snap::Hold> holds; };
  std::optional<Lock> m_lock;
  bool m_shiftDown = false, m_shiftUsed = false, m_shiftSpent = false, m_unstick = false, m_inView = false;
  // The snap and solver settings, read once (UI-27: a mouse move read them a dozen times): when a sketch opens and when one
  // changes (refreshSnap: the snaps page, F8, F9, F11, F12).
  struct Settings {
    bool endpoint = true, midpoint = true, center = true, quadrant = true, intersection = true, apparent = true, perpendicular = true, tangent = true,
         nearest = true, angle = true, inference = true, extensions = true, tracking = true, ortho = false;
    double angleStep = 15, tolerance = 1e-8;
    int iterations = 100;
  } m_settings;
  void readSettings();
  size_t m_settingsReads = 0;  // benches: a mouse move reads nothing
  int m_snapChoice = 0;  // the object snap shown when several are in reach (Shift taps, UI-23), counted from (m_choiceU, m_choiceV)
  double m_choiceU = 0, m_choiceV = 0;
  bool m_cyclePending = false;  // Shift went down over several object snaps: a tap shows the next, a hold locks
  QElapsedTimer m_shiftClock;
  double m_lastU = 0, m_lastV = 0;
  Qt::KeyboardModifiers m_lastMods;
  bool placing() const;  // the tool places points: snapping, tracking and the lock apply
  // Grid snapping applies to the step: a point placed (placing(), a tangent arc's end), never a pick (trim, dimension,
  // constraints, the modify tools: they hit-test where the pointer is).
  bool gridPoints() const;
  bool lockOn();         // locks onto what the pointer is on now; false: nothing to lock onto
  void unlock();
  void shiftKey(bool pressed);
  void altKey(bool pressed);  // Alt frees the point (no snap, no grid): the cursor drawn and the click agree without a move
  void resnap();         // the pointer's snap again where it is (a point acquired, a lock taken or let go)
  // The pointer did not move but the view or what is over it did (a zoom, a camera gesture or a menu ended): the snap
  // again where the pointer is now. False: it is not over the sketch (off the view, over an overlay, a drag runs).
  bool followPointer();
  void noteHints();      // hintsChanged when what Shift does changed
  sketchkeys::Shift m_shiftHint = sketchkeys::Shift::None;
  // dragging with the select tool
  bool m_dragging = false, m_dragMoved = false;
  bool m_dragPending=false,m_dragReleased=false;double m_dragNextU=0,m_dragNextV=0;Qt::KeyboardModifiers m_dragNextMods;
  Hit m_dragHit;
  double m_dragU = 0, m_dragV = 0;
  std::vector<std::pair<int, std::pair<double, double>>> m_dragStart;  // point -> where it was
  bool m_dragGrid = false;double m_dragGridU = 0, m_dragGridV = 0;  // the grid node the dragged geometry snapped to
  double m_dragCursorU = 0, m_dragCursorV = 0;  // the drawing cursor while it snaps: on that node (a point), else the hand by whole steps
  // A drag at (u, v) with grid snapping (Alt: free): the grabbed point (a curve's first one) on a node and the rest by as
  // much (su, sv), or a rim by whole steps of radius (r > 0); sets m_dragGrid*, m_dragCursor* and returns whether it snaps.
  bool dragSnap(double u, double v, Qt::KeyboardModifiers mods, double& su, double& sv, double& r);

  Handle(AIS_InteractiveObject) m_prs;  // a SketchPrs (SketchEditor.cpp)
  Handle(AIS_InteractiveObject) m_transientPrs;
  std::shared_ptr<SketchGeometryCache> m_geometry;
  QPointer<Job> m_geometryJob;
  int m_geometryRevision=0,m_fillRevision=0;
  QLineEdit* m_dimEdit = nullptr;
  int m_dimEditing = 0;
  bool m_dimFresh = false;
  QString m_dimShown;  // what the value box started from: committed as it is, it changes nothing (a unit's rounding)
  double m_samplePixelSize=0;
  QTimer m_fillTimer;
  Job* m_fillJob = nullptr;
  std::vector<opad::Vec3> m_fill;  // triangles of the closed regions, world coordinates
};
