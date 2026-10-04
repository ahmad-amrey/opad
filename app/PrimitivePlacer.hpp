#pragma once
// A new primitive placed in the view (TODO 11 P1), as its guide shows and as Fusion's Box, Cylinder, Sphere, Torus and Coil
// work: the panel opens with the defaults, and
//   1. Place: a click on an origin plane, a construction plane or a planar face writes Plane and Position X/Y (in that
//      plane's frame). The pointer shows where on the hovered plane it would go, snapped as a sketch's points are
//      (PlaneSnap: the sketches' and drawings' object snaps, the hovered face's corners, centres and midpoints, the plane's
//      origin, else the grid while Grid snapping is on; Alt: free), the snap's marker and name shown. Over nothing the plane
//      the panel holds takes the click (XY unless chosen; the origin plane facing the view when that one is edge-on).
//   2. Size: the pointer draws the footprint (FeatureSpec::footprint): a rectangle's length and width about the click (Centred
//      on, or from it as a corner), a diameter from it, a ring's diameter through the tube's middle; the outline follows at
//      once, the preview as fast as plans come; the pointer snaps as when placing (a snapped point's size is exact, others
//      are rounded at this zoom). The values show in boxes beside the pointer; Tab goes round them and a typed value (or one
//      typed into the panel) holds while the pointer sets the others. A click fixes them (a press, drag and release from the
//      first click does too). The pointer never sizes what the kernel refuses: a ring smaller than its section takes a
//      thinner section (until the section is typed), a coil stays wider than twice its section, a cone's base differs from
//      its top.
//   3. Section (a ring): the pointer's distance from the ring sets the tube's section diameter; a click fixes it.
//   4. Height (with a "height" input): the arrow the core places (feature_handles) is pulled or typed; Tab reaches the other
//      sizes (a cone's top diameter first).
// Enter is OK at any stage, with what is set (before any click: the panel's defaults at the XY origin). Esc steps back one
// stage, then closes the feature. The panel stays the precise editor and shows the same values; editing an existing
// primitive is the panel's alone.
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <map>
#include <set>

#include "DynamicInput.hpp"
#include "PlaneSnap.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"

class AppDocument;
class FeaturePanel;
class Job;
class JobRunner;
class ToolValues;

class PrimitivePlacer : public QObject {
  Q_OBJECT
 public:
  enum class Stage { Off, Place, Size, Section, Height, Done };
  PrimitivePlacer(AppDocument* doc, Viewport* view, JobRunner* jobs, FeaturePanel* form, ToolValues* values, QObject* parent);
  ~PrimitivePlacer() override;
  std::function<std::vector<Viewport::Candidate>()> planes;  // the origin and construction planes a click can take
  // The document and the scene as they are now, for a worker (the controller's plan copies: made once per document state).
  std::function<std::pair<std::shared_ptr<const opad::Document>, std::shared_ptr<const opad::Scene>>()> copies;
  void start(const opad::design::FeatureSpec& spec);  // a new placed primitive: its panel is open with the defaults
  void stop();                                        // the panel's flow from here on (the plane box clicked, the feature left)
  bool active() const { return m_stage != Stage::Off; }
  Stage stage() const { return m_stage; }
  bool escape();               // one stage back; false at the first (the feature closes)
  bool tracking() const;       // the pointer sets a size now: preview as fast as plans come
  bool previewShown() const;   // a body to preview: not while the plane is picked, nor before the pointer gave a size
  bool arrowShown() const;     // the value arrow (the height) may show
  void showPlanes();           // the planes to click again (after a pick box of the panel took the view)
  QList<DynamicInput::Field> fields() const;  // the boxes beside the pointer while it sizes (empty: the panel's)
  QStringList arrowExtras() const;            // the inputs Tab reaches from the arrow's box, in order
  void typed(const QString& key);             // a box typed into or emptied: that size holds, or follows the pointer again
  void inputsChanged();                       // the panel changed: a size typed there holds
  QString prompt() const;                     // what it waits for (the status bar)
  int guideStep() const;                      // the guide's step for the stage, of guideCount()
  int guideCount() const;
  // What a click does, for benches and the keyboard: the plane `plane` (resolved to `frame`) clicked at (u, v) in that frame;
  // the footprint fixed with the sizes as they are.
  void placeAt(const opad::json& plane, const opad::Frame& frame, double u, double v, bool exact = false);  // exact: a snapped point, not rounded
  void fixSize();
  bool markerShown(opad::Vec3* at = nullptr) const;  // where a click would place it now (sizing: a snapped point's marker)
  QString snapKind() const { return m_markerKind; }   // what the marker snapped to (PlaneSnap: "endpoint", "center", ..., "grid"; "" none)
  bool outlineShown() const { return !m_outline.IsNull(); }  // the footprint drawn on the plane while the pointer sizes it
  QPointF lastPointer() const { return m_last; }
 signals:
  void stageChanged();
  void status(const QString& text);
 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
 private:
  struct Hit {
    bool ok = false, frameKnown = false, snapped = false;  // snapped: onto a point of the model (exact, not rounded)
    opad::json plane;     // what the plane input gets ({"face": ...} once resolved)
    opad::Frame frame;    // that plane's frame (frameKnown), else the face's plane through `at` (to draw the marker)
    opad::Vec3 at{0, 0, 0}, seen{0, 0, 0};  // seen: the point snapped to, where it is
    double u = 0, v = 0;  // in `frame` when frameKnown
    QString kind;         // what it snapped to (PlaneSnap::Result::kind)
    QString why;          // why it cannot be clicked (a curved face)
  };
  Hit hitAt(const QPointF& pos, bool free, bool fresh);  // fresh: picked now (a click), else the last frame's hover
  void hover(const QPointF& pos, bool free);
  void click(const QPointF& pos, bool free);
  void sizeFrom(const QPointF& pos, bool free, bool fresh = false);
  void enter(Stage stage);
  void showMarker(const Hit& hit);
  void showSnap(const PlaneSnap::Result& snap);  // sizing: the snapped point's marker, else none
  void clearMarker();
  void drawOutline();
  void clearOutline();
  void setStatus(const QString& text);
  QString withSnap(const QString& text, const QString& kind) const;  // "Endpoint · <text>" when snapped
  double gridOr(double fallback, bool free) const;  // the grid step while it snaps, else the fallback
  double value(const QString& key, double fallback) const;  // an input as the feature gets it, in mm
  double parsed(const QString& text, double fallback) const;
  QString sizeText(double mm, bool exact) const;
  QStringList sizeKeys() const;  // what the pointer sets in this stage
  bool ring() const;
  bool hasHeight() const;
  void write(const std::vector<std::pair<QString, opad::json>>& values);

  AppDocument* m_doc;
  Viewport* m_view;
  JobRunner* m_jobs;
  FeaturePanel* m_form;
  ToolValues* m_values;
  const opad::design::FeatureSpec* m_spec = nullptr;
  Stage m_stage = Stage::Off;
  opad::Frame m_frame;            // the plane's frame (Position X/Y are in it)
  double m_cu = 0, m_cv = 0;      // the click in it: the centre (or a box's corner)
  QPointF m_placedAt, m_press, m_last;
  bool m_lastFree = false;        // Alt held at the last move
  bool m_placeExact = false;      // the click snapped to a point of the model: its position is written exactly
  bool m_sized = false;           // the pointer gave this stage's size
  bool m_down = false, m_writing = false;
  Stage m_pressStage = Stage::Off;
  std::set<QString> m_locked;     // typed sizes: the pointer leaves them
  std::map<QString, QString> m_written;  // what was last written into each size, by the placer or the defaults
  QPointer<Job> m_job;            // a face's frame being resolved
  int m_serial = 0;
  struct Release { bool pending = false; QPointF at; bool free = false; } m_release;  // let go while the face resolved
  std::vector<Viewport::Candidate> m_planes;
  Handle(AIS_InteractiveObject) m_marker, m_outline;
  bool m_markerShown = false;
  opad::Vec3 m_markerAt{0, 0, 0};
  QString m_status, m_markerKind;
  PlaneSnap m_snap;
  QString m_sectionText;          // a ring's section as the sizing of its diameter began: what it goes back to when there is room
};
