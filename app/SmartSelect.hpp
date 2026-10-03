#pragma once
// Smart selection (TODO 11 UI-95, design notes F §A6), a feature area (AreaController.hpp). After faces or edges are
// picked a chip floats beside them naming what they belong to: the feature that made them, from the design history
// ("Boss · 5 faces"), else a detail the geometry shows (a hole, a fillet chain, a boss, a wall), a tangent chain or a
// loop. Hovering it shows those faces in the candidate amber and pulses the feature's timeline marker; a click (or
// Ctrl+Up) selects them, and on a selection that is such a set the chip carries its actions: Delete, Edit (Enter),
// Suppress, Find in timeline, Isolate for a feature; Remove faces, Select similar, Measure, Isolate for a detail (its
// sizes are the chip's tooltip). ▾ (Shift+Space) lists every candidate with those actions, and for a feature Select what
// depends on it. Ctrl+Up climbs faces -> feature or detail -> body -> component -> parent and Ctrl+Down climbs back.
// Double-click: a face -> its feature or detail (again on a face of it: edit the feature), an edge -> the loop on the
// face on the pointer's side of it, Alt -> its tangent chain. Del on picked faces or edges never tombstones the body's
// source (UI-04): a feature's whole face set deletes the feature (asking first when later features use it, with the
// result previewed), anything else opens the menu. Edit > Suggest related selections and Suggestion delay (settings
// selection/suggest, selection/suggestDelay). Everything comes from the related command (provenance and rules: nothing
// is suggested by resemblance, D4), run on a worker over a document snapshot, newest selection only.
#include <QFrame>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>

#include "AreaController.hpp"
#include "SmartRules.hpp"
#include "Viewport.hpp"

class Job;
class QLabel;
class QMenu;
class QToolButton;
namespace opad {
class Document;
}

// The chip: a native child of the viewport (rounded by a mask: no translucency over the GL surface), 28 px high, never
// focused (Del and the keys stay with the view). An icon, what it offers, a key hint, action buttons, ▾.
class SmartChip : public QFrame {
  Q_OBJECT
 public:
  explicit SmartChip(QWidget* viewport);
  void setContent(const QString& icon, const QString& text, const QString& hint, const QList<QAction*>& actions);
  QString text() const;
  QString hint() const;
  QList<QToolButton*> actionButtons() const { return m_buttons; }
  QToolButton* moreButton() const { return m_more; }
 signals:
  void hovered(bool on);
  void clicked();
  void menuRequested();
 protected:
  void enterEvent(QEnterEvent* e) override;
  void leaveEvent(QEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
 private:
  void restyle();
  QString m_iconName;
  QLabel *m_icon, *m_text, *m_hint;
  QList<QToolButton*> m_buttons;
  QToolButton* m_more;
};

class SmartSelect : public AreaController {
  Q_OBJECT
 public:
  using AreaController::AreaController;
  // The related command's answer for one selection of faces or edges.
  struct Found {
    std::vector<opad::Ref> picks;
    std::vector<smart::Candidate> candidates;
    int best = -1;    // what the chip offers (smart::headline)
    int active = -1;  // the candidate the picks are exactly (smart::matching): the chip shows its actions
    std::vector<opad::Vec3> corners;  // the picks' world box, where the chip goes
    size_t faces = 0;                 // of the picked bodies
    unsigned long long revision = 0, generation = 0;
    bool ready = false;
  };
  const Found& found() const { return m_found; }
  SmartChip* chip() const { return m_chip; }
  QMenu* openMenu() const { return m_menu; }  // the candidates menu or the delete question while it shows
  bool busy() const {
    return m_job || m_chainJob || m_madeRunning || m_capturing || m_retrying || m_wait.isActive() || m_markerWait.isActive() || m_pending != Pending::None;
  }
  size_t ladder() const { return m_stack.size(); }  // Ctrl+Down steps left
  int suggestDelay() const { return m_wait.interval(); }  // ms the picks stand before the chip asks (Edit > Suggestion delay)

  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void selectionChanged(const SelectionContext& selection) override;
  void positionOverlays(const QRect& viewport) override;
  void documentChanged(bool replaced) override;
  bool command(const QString& id, const SelectionContext& selection) override;

  // A double-click on the current pick (the viewport's lands here once its second click is processed), at `at` in the view
  // (-1, -1: unknown): an edge's loop is the one on the face on that side of the edge.
  void doubleClicked(bool alt, const QPoint& at = QPoint(-1, -1));
  void grow();    // Ctrl+Up on picked faces or edges
  void shrink();  // Ctrl+Down
  void showMenu(const QPoint& global = {});  // Shift+Space: at the chip, else at the pointer
  void deleteCandidate(int index);           // Delete on a feature: its dependents asked for first
  // A feature (or a sketch: kind "sketch") deleted with its dependents asked for first and the result previewed, one undo
  // step; without refs (from its timeline marker) the faces it made are found for "Remove its faces instead".
  void deleteFeature(const smart::Candidate& c);
  bool deleteMarker(const std::string& op);  // command timeline.delete: a feature's or a sketch's marker, as deleteFeature
  // Select what depends on a feature (the menu's Select dependents): the faces of the later features that would fail
  // without it, their markers pulsed; named in the status bar (sketches and features without faces of their own too).
  void selectUsers(const smart::Candidate& c);
  // The timeline (UI-99, SmartTimeline.cpp): a hovered marker shows what its op made in the candidate amber (a feature's
  // faces, an import's or a move's bodies; while rolled back the bodies as shown); a click on a feature that changed
  // bodies (a boss, a fillet) selects the faces it made, with its actions on the chip. What each op made is found once per
  // document state on a worker (smart::madeBy).
  void markerHovered(const std::string& op);
  const std::map<std::string, smart::Made>* made() const;  // for the document as it is now; null until found

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  enum class Pending { None, Grow, Menu, Delete, Tangent };
  bool idle() const;  // nothing else owns the picks: no load, sketch, feature input, guided tool or note
  static bool subPicks(const std::vector<opad::Ref>& refs);  // faces and edges only, a few thousand at most
  void request(bool now);
  void run();
  void withSnapshot(std::function<void(std::shared_ptr<const opad::Document>)> fn);
  void finished();  // a result for the current picks arrived: the chip, then whatever waited for it
  void refreshChip();
  void hideChip();
  void place();
  void hover(int index);  // -1: nothing
  void choose(int index);  // selects it: one rung up
  void select(std::vector<opad::Ref> refs, std::function<void()> then = {});
  void chain(const opad::Ref& edge, bool tangent, const QPoint& at);
  int focus() const;  // what the actions are for: the active candidate, else the offered one
  QList<QAction*> actionsFor(int index, QObject* parent);  // new actions for that candidate (the chip's buttons, the menus)
  void askDependents(const smart::Candidate& c, const std::vector<std::pair<std::string, std::string>>& deps, const std::vector<Viewport::PreviewPart>& parts,
                     const std::vector<std::string>& hidden);
  void commitDelete(const smart::Candidate& c, std::vector<std::string> ops);
  void deletePicks();
  void tangentFaces();  // Alt+double-click on a face: the faces joined to it by smooth edges
  QString label(const smart::Candidate& c) const;
  QString iconOf(const smart::Candidate& c) const;
  // A detail's sizes as the recogniser read them, in the shown units ("Ø 6 mm", "Depth 10 mm (through)", "Radius 2 mm",
  // "Thickness 2 mm", "Height 10 mm"): the chip's tooltip and Measure. Empty for features, chains and loops.
  QStringList measures(const smart::Candidate& c) const;
  void measure(const smart::Candidate& c);  // the readout as a toast, with Copy

  SmartChip* m_chip = nullptr;
  QPointer<QMenu> m_menu;
  Found m_found;
  std::vector<opad::Ref> m_current, m_previous;  // the selection and the one before it
  std::vector<std::vector<opad::Ref>> m_stack;   // Ctrl+Up's rungs below the current one
  std::vector<opad::Ref> m_expect;               // a selection this area made (the ladder goes on)
  bool m_handedUp = false;     // a body's Ctrl+Up went to the window (component, parent): its result is a rung
  bool m_switching = false;    // the view's filter is changing for a selection of ours: its clearing is no pick
  smart::Candidate m_chosen;   // what the chip, the menu or Ctrl+Up selected last (double-click on it edits it)
  Pending m_pending = Pending::None;
  QTimer m_wait, m_settle, m_dropSnapshot;
  unsigned m_token = 0, m_switchToken = 0, m_deleteToken = 0, m_chainToken = 0, m_usersToken = 0;
  Job* m_job = nullptr;
  Job* m_chainJob = nullptr;
  bool m_capturing = false;
  std::vector<std::function<void(std::shared_ptr<const opad::Document>)>> m_afterCapture;  // waiting for the copy
  bool m_retrying = false;
  void capture();  // a copy of the document for what waits (again shortly while the document is busy)
  struct Snapshot {
    std::shared_ptr<const opad::Document> doc;
    unsigned long long revision = 0, generation = 0;
  } m_snap;
  // A double-click: its second click is processed (a selectionChanged) before it is acted on.
  bool m_doubleArmed = false, m_double = false, m_doubleAlt = false;
  QPoint m_doubleAt{-1, -1};
  std::vector<opad::Ref> m_doubleBefore, m_doubleFirst;
  QTimer m_doubleTimer;
  QAction *m_shrink = nullptr, *m_related = nullptr, *m_suggest = nullptr;
  bool suggesting() const;  // setting selection/suggest: the chip comes by itself (else on Ctrl+Up, Shift+Space, Del)
  // The timeline's markers (SmartTimeline.cpp).
  bool markerClicked(const std::string& op);  // command timeline.select: true when it selects that feature's faces
  void showMarker();
  std::vector<opad::Ref> markerBodies(const std::string& op) const;  // what an op made or changed, as bodies of the scene shown
  void withMade(std::function<void(const std::map<std::string, smart::Made>&)> then);
  std::shared_ptr<const std::map<std::string, smart::Made>> m_made;
  unsigned long long m_madeRevision = 0, m_madeGeneration = 0;
  std::vector<std::function<void(const std::map<std::string, smart::Made>&)>> m_afterMade;
  bool m_madeRunning = false;
  unsigned m_madeToken = 0;
  std::string m_marker;  // hovered
  bool m_markerShown = false;
  unsigned m_markerToken = 0;
  QTimer m_markerWait;
};
