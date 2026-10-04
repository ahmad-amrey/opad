#pragma once
// Exploded views (UI-36), the assembly area's second half (core: opad/explode.hpp). Session state, never written while
// it changes: the spec (root, levels, mode, spacing, keep/split, groups, manual drags), the distance t and whether it is
// on. Its units are laid out on a worker from the boxes the view already holds; every change of t moves the bodies
// through the Explode layer of looks (Viewport::setLookLayer: a translation per body, picking follows), draws the trail
// lines and places the drag handle and triad (ExplodeDrag.cpp), all O(units). The Explode panel edits it; the browser
// marks what moves together while the panel is open; the chips row says how exploded the view is; Save as view writes
// a `view` op with the explode object, Update view an `edit` of it, and choosing such a view (the panel, View > Named
// views) shows it again.
// A sketch or feature edit collapses the view and opens it again afterwards. Guided tools measure where the parts are
// drawn (MainWindow::runToolMeasure through Viewport::shownOffsets).
#include <AIS_InteractiveObject.hxx>
#include <QElapsedTimer>
#include <QPointF>
#include <QPointer>
#include <QTimer>

#include <array>
#include <functional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "opad/explode.hpp"

class DimensionHandle;
class ExplodePanel;
namespace browser {
struct Row;
struct Decoration;
}  // namespace browser
class Job;
class PromptBar;
class QLabel;
class QMenu;
class Viewport;
class ToolPanel;

class Explode : public AreaController {
  Q_OBJECT
 public:
  explicit Explode(AreaServices& services);
  ~Explode() override;
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void selectionChanged(const SelectionContext& selection) override;
  void documentChanged(bool replaced) override;
  void positionOverlays(const QRect& viewport) override;

  // What the panel, the commands, the browser and the benches drive.
  void open();             // the panel; the explode on (the active component's when no view was chosen)
  void setOn(bool on);     // off: the parts go back together, then the view lets go of them
  void setT(double t);     // the distance, at once
  // Animated (default: the spec's duration for the whole way); frame: then Fit All if the parts left the view.
  void playTo(double t, double seconds = -1, bool frame = false);
  void play();             // Play / Pause: out, or back when out
  void pause();            // where it is (a collapse under way ends at once)
  void setLevels(int levels);
  void setRule(const std::string& component, opad::ExplodeRule rule);
  void group(const std::vector<std::string>& ids);
  void ungroup(const std::string& id);
  void resetDrags();
  std::string saveView(const QString& name);  // a view op with the explode; its id ("" when refused)
  void updateView();                          // an edit op of the view it came from
  void loadView(const std::string& id);

  bool isOn() const { return m_on; }
  double t() const { return m_t; }
  bool playing() const { return m_tick.isActive(); }
  bool layingOut() const { return m_job != nullptr || m_relayout; }
  bool measuring() const { return m_measure != nullptr; }  // the parts' tight boxes, on a worker
  bool exactBoxes() const { return m_exact; }  // the units come from tight boxes, as opad-cli lays them out
  const opad::ExplodeSpec& spec() const { return m_spec; }
  const std::vector<opad::ExplodeUnit>& units() const { return m_units; }
  int unitOf(const std::string& id) const;  // explode_unit_of over the current units
  ToolPanel* panel() const { return m_panel; }
  ExplodePanel* form() const { return m_form; }
  QLabel* chip() const { return m_chip; }
  PromptBar* hint() const { return m_hint; }  // the first-use hint over the view
  DimensionHandle* handle() const { return m_handle; }
  int dragUnit() const { return m_dragUnit; }
  size_t trailCount() const { return m_trailCount; }
  const std::string& viewId() const { return m_viewId; }
  // The drag triad on the selected part (ExplodeDrag.cpp): the DimensionHandle's arrow is its first axis, two arrows the
  // axes square to it, a square in the middle moves the part in the view's plane; a press on the part drags it too.
  bool triadShown() const { return m_triadShown; }
  QPointF triadPoint(int part) const;  // a widget point on a part: 0 the square, 1-2 an arrow (benches)
  const std::array<opad::Vec3, 2>& triadAxes() const { return m_triadAxes; }
  bool dragging() const { return m_drag.part >= 0; }
  static std::array<opad::Vec3, 2> crossAxes(const opad::Vec3& axis);  // the world axes least along it, made square

 signals:
  void laidOut();  // new units are on screen (benches)
  void moved();    // the parts moved (a tick, a drag)

 protected:
  bool eventFilter(QObject* object, QEvent* event) override;

 private:
  void edit(const std::function<void(opad::ExplodeSpec&)>& change, bool relayout = true);
  void layout();       // the units for the spec, on a worker
  void measureBoxes(const std::vector<std::string>& keys);  // their tight boxes on a worker, then layout() again
  void apply();        // offsets at t to the view, trails, handle, chip
  void showTrails(const std::vector<opad::Vec3>& moves);
  void placeHandle();  // on the selected unit, where it is drawn
  void refreshPanel();
  void checkRules(const SelectionContext& selection);  // Keep together / Explode its parts checked as the selected components are
  void refreshChip();
  void tick();
  void fitIfOutside();
  void designState();  // a sketch or feature edit collapses the view, its end opens it again
  void showHint();
  void hideHint(bool seen);  // seen: the user did what it says, it is not shown again (setting hints/explode)
  opad::Vec3 dragAxis(const opad::ExplodeUnit& unit) const;
  std::vector<std::string> members(int unit) const;  // what selecting a unit selects
  void placeTriad(bool shown, const opad::Vec3& at, const opad::Vec3& axis);
  int triadPart(const QPointF& at) const;  // -1 none, 0 the square, 1-2 an arrow
  void beginDrag(int part, const QPointF& at);
  void dragTo(const QPointF& at);
  void endDrag();
  bool dragEvent(QEvent* event);  // the viewport's mouse events; true: taken
  void decorate(const browser::Row& row, browser::Decoration& d);  // what moves together, while the panel is open
  bool belowRoot(const std::string& id) const;

  opad::ExplodeSpec m_spec;
  std::vector<opad::ExplodeUnit> m_units;
  std::vector<std::pair<std::string, int>> m_sketchUnits;  // sketches that move with a unit
  std::unordered_map<std::string, int> m_bodyUnits;        // explode_body_units
  int m_depth = 1;
  std::string m_root;  // explode_root of the laid-out spec
  double m_t = 0, m_target = -1;  // m_target: where to play once laid out (-1: stay)
  bool m_on = false, m_rootFollows = true, m_offAfter = false, m_lines = true, m_relayout = false;
  bool m_fitAfter = false;  // the play that opened the explode (or a view) frames the parts if they left the view
  std::string m_viewId;  // the view the spec came from or was saved as
  QPointer<Job> m_job;
  QPointer<Job> m_measure;
  bool m_measureRefused = false, m_exact = false;
  std::set<std::string> m_unmeasured;  // shape keys a measuring pass could not fill (not cached): the view's boxes, not measured again
  std::shared_ptr<opad::FastenerAxes> m_axes = std::make_shared<opad::FastenerAxes>();  // by shape key, this document's
  int m_serial = 0;
  QTimer m_tick;
  QElapsedTimer m_clock;
  double m_from = 0, m_to = 0, m_seconds = 0;
  double m_resume = -1;  // the distance a design edit collapsed
  Handle(AIS_InteractiveObject) m_trails;
  bool m_trailsShown = false;
  size_t m_trailCount = 0;
  DimensionHandle* m_handle = nullptr;
  int m_dragUnit = -1;
  Viewport* m_view = nullptr;
  Handle(AIS_InteractiveObject) m_triad;
  bool m_triadShown = false, m_bodyArmed = false, m_replaying = false;
  int m_triadHover = -1;
  opad::Vec3 m_triadAt{0, 0, 0};
  std::array<opad::Vec3, 2> m_triadAxes{};
  QPointF m_triadCentre, m_pressAt, m_pressGlobal;
  std::array<std::pair<QPointF, QPointF>, 2> m_triadArrows{};
  struct Drag {
    int part = -1;  // -1 none, 0 in the view's plane, 1-2 along an arrow
    std::string unit;
    QPointF start, screenAxis;
    opad::Vec3 manual{0, 0, 0}, axis{0, 0, 0};
    double scale = 1;  // the part's progress at t: a drag of d moves its offset by d / scale
    opad::Frame plane;
  } m_drag;
  QLabel* m_chip = nullptr;
  PromptBar* m_hint = nullptr;
  QMenu* m_chipMenu = nullptr;
  ToolPanel* m_panel = nullptr;
  ExplodePanel* m_form = nullptr;
  QAction* m_explode = nullptr;
  QAction* m_play = nullptr;
  QAction* m_off = nullptr;
  QAction* m_save = nullptr;
  QAction* m_keep = nullptr;
  QAction* m_split = nullptr;
  QAction* m_group = nullptr;
  QAction* m_ungroup = nullptr;
  int m_chipPercent = -1;
};
