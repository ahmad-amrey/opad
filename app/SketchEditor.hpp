#pragma once
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
#include <set>

#include "AppDocument.hpp"
#include "Viewport.hpp"
#include "GuidedTool.hpp"
#include "SketchKeys.hpp"
#include "opad/design/sketch.hpp"

class JobRunner;
class Job;
class SketchGeometryCache;
class DimensionHandle;

class SketchEditor : public QObject, public SketchInput {
  Q_OBJECT
 public:
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
  QString option(const QString& key, const QString& fallback = {}) const { return m_options.value(key, fallback); }
  void applyTool();
  void previewTool();
  void invalidatePreview(bool keepOverlay = false);  // keepOverlay: the shown one stays until the next replaces it (live drags)
  void scheduleToolPreview();
  void placePrecise(const QString& u, const QString& v, int mode);
  // The editing keys and the panel's buttons (SketchKeys.hpp): Backspace / Undo point, Enter / Done, Esc (one rung of
  // the ladder) and Close tool (Esc until the tool is closed). Each returns whether it did something.
  sketchkeys::State keyState() const;
  bool undoPoint();
  bool done();
  bool escape();
  void closeTool();
  QString keyHints() const;  // what Backspace, Enter and Esc do now, for the prompt
  void toggleReference();
  void selectConnected();
  void selectType();
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
  void bench(const QString& script);  // OPAD_BENCH_DESIGN: draws a dimensioned rectangle with a hole through the tool code paths
  void benchWorkflow();
  void benchPrimitives();
  void benchModify();
  void benchHandles();
  void benchDrag();
  void benchGrid();
  void benchLadder();
  void benchLarge(const QString& output, opad::json metrics);

  // SketchInput
  void sketchPress(double u, double v, Qt::KeyboardModifiers mods) override;
  void sketchMove(double u, double v, Qt::KeyboardModifiers mods, bool dragging) override;
  void sketchLeave() override;
  void sketchRelease(double u, double v, Qt::KeyboardModifiers mods) override;
  void sketchDoubleClick(double u, double v) override;
  bool sketchKey(QKeyEvent* e) override;

 protected:
  bool eventFilter(QObject* o, QEvent* e) override;  // Esc in the dimension field

 signals:
  void toolChanged(const QString& tool);
  void status(const QString& text);  // what the tool waits for, or why a change was refused
  void changed();                    // geometry, selection or undo state
  void workflowChanged();

 private:
  friend class SketchPanel;
  opad::design::SolveOptions solveOptions() const;
  bool selectable(int id) const;
  void runSketchEdit(const QString& label,std::function<void(opad::design::Sketch&)> work);
  bool primitiveClick(double u,double v);
  void finishPrimitive();
  opad::design::Sketch primitivePreview() const;
  opad::json primitiveOptions() const;
  void createText(double u,double v);
  bool modifyClick(double u,double v);
  bool applyModify();
  struct Snap {
    double u = 0, v = 0;
    int point = 0;     // an existing point to reuse
    int entity = 0;    // a curve the new point will lie on
    bool horizontal = false, vertical = false;  // relative to the previous click
    bool grid = false;  // on a grid node, or whole grid steps along the inference
    // What the pointer was pulled to, for the display: that object is highlighted and named beside the cursor.
    enum class Kind { None, Point, Midpoint, Quadrant, Intersection, Curve, Extension, Aligned, Cross, Angle, Locked, Grid } kind = Kind::None;
    int target = 0, other = 0;  // the point (Point, Aligned, Angle), the crossing guides' points (Cross) or the curves (the others)
    int curve = 0;  // Cross: the curve a guide crosses there
  };
  struct Hit {
    enum Kind { None, Point, Entity, Dimension } kind = None;
    int id = 0;
  };
  Snap snap(double u, double v, bool infer = true) const;
  Hit hitTest(double u, double v) const;
  double tol() const;  // pick distance in sketch units
  int pointFor(const Snap& s);           // reuse or create (with the on-curve constraint)
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
  std::vector<std::pair<double, double>> trimPreview(int id, double u, double v) const;  // the piece trimAt would remove
  void mirrorSelection(int axisLine);
  void offsetSelection();
  void updateDimensionHandle();
  void projectHovered();
  void referenceHover();
  void pickReference();
  bool applyReference();
  bool applyImageTool();
  bool imageClick(double u,double v);
  void refreshImages();
  std::vector<std::pair<double, double>> sampled(const opad::design::SkEntity& e) const;  // polyline of a curve, sketch coordinates
  double distanceTo(const opad::design::SkEntity& e, double u, double v) const;
  void labelPosition(const opad::design::SkConstraint& c, double& u, double& v) const;
  QString dimensionText(const opad::design::SkConstraint& c) const;
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
  int m_offsetAnchor = 0;  // the selected curve the offset arrow sits on: the last one hovered
  int m_previewRevision=0;
  std::shared_ptr<opad::design::Sketch> m_toolPreview;
  Handle(AIS_InteractiveObject) m_toolPreviewOverlay;
  opad::design::SolveResult m_previewSolved;
  QString m_selectionFilter = "all",m_constraintFilter;
  std::set<int> m_conflicts;
  std::vector<std::tuple<int,double,double>> m_glyphHits;
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
  Snap m_cursor;
  bool m_haveCursor = false;
  int m_trackingPoint = 0;
  bool m_inferenceLocked = false;
  double m_lockX = 0, m_lockY = 0, m_lockDx = 1, m_lockDy = 0;
  // dragging with the select tool
  bool m_dragging = false, m_dragMoved = false;
  bool m_dragPending=false,m_dragReleased=false;double m_dragNextU=0,m_dragNextV=0;
  Hit m_dragHit;
  double m_dragU = 0, m_dragV = 0;
  std::vector<std::pair<int, std::pair<double, double>>> m_dragStart;  // point -> where it was
  bool m_dragGrid = false;double m_dragGridU = 0, m_dragGridV = 0;  // the grid node the dragged geometry snapped to

  Handle(AIS_InteractiveObject) m_prs;  // a SketchPrs (SketchEditor.cpp)
  Handle(AIS_InteractiveObject) m_transientPrs;
  std::shared_ptr<SketchGeometryCache> m_geometry;
  QPointer<Job> m_geometryJob;
  int m_geometryRevision=0,m_fillRevision=0;
  QLineEdit* m_dimEdit = nullptr;
  int m_dimEditing = 0;
  bool m_dimFresh = false;
  double m_samplePixelSize=0;
  QTimer m_fillTimer;
  Job* m_fillJob = nullptr;
  std::vector<opad::Vec3> m_fill;  // triangles of the closed regions, world coordinates
};
