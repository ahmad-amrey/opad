#pragma once
// A drag triad (TODO 11 P2): flat arrows facing the camera along fixed directions and a square in the middle, drawn over
// everything (TopOSD) at a point, the part under the pointer (or being dragged) lit; optionally a ring round an axis to turn
// about it (Move with Rotate on: the ring the bodies travel along as the angle changes). Exploded views use it with two arrows
// square to the part's own (its DimensionHandle is the third, ExplodeDrag.cpp), Move / copy with X, Y and Z in the axis
// colours (DesignTriad.cpp). It only draws and measures: the owner routes the mouse and says what a pull means.
#include <AIS_InteractiveObject.hxx>
#include <QColor>
#include <QPointF>

#include <vector>

#include "opad/scene.hpp"

class Viewport;

class TranslateTriad {
 public:
  explicit TranslateTriad(Viewport* view);
  ~TranslateTriad();
  TranslateTriad(const TranslateTriad&) = delete;
  TranslateTriad& operator=(const TranslateTriad&) = delete;
  static constexpr double kLength = 40, kHead = 14, kShaft = 2.5, kWing = 7.5, kSquare = 6;  // widget pixels
  static constexpr int kRing = 100;  // the ring's part number
  struct Arrow {
    opad::Vec3 dir{1, 0, 0};  // a unit direction
    QColor colour;            // invalid: the theme's second text colour
  };
  // Shown at `at` with these arrows (an arrow seen end on is left out), or moved there; again after every camera move.
  void place(const opad::Vec3& at, const std::vector<Arrow>& arrows);
  void hide();
  bool shown() const { return m_shown; }
  // The ring: round `axis` (a unit direction) through `centre`, `radius` world units; off with on = false. Seen nearly edge
  // on it is not drawn (a pull round it would mean nothing). Takes effect with the next place().
  void setRing(bool on, const opad::Vec3& centre = {0, 0, 0}, const opad::Vec3& axis = {0, 0, 1}, double radius = 0);
  bool ringShown() const { return m_shown && !m_ringScreen.empty(); }
  QPointF ringPoint(double angle) const;  // a widget point on the ring, `angle` radians from its first sample (benches)
  const opad::Vec3& at() const { return m_at; }
  const std::vector<Arrow>& arrows() const { return m_arrows; }
  void setHover(int part);  // the part lit under the pointer: -1 none, 0 the square, k arrow k (from 1), kRing the ring
  int hover() const { return m_hover; }
  int partAt(const QPointF& widget) const;  // what a press there takes: -1 nothing, 0 the square, k arrow k, kRing the ring
  QPointF partPoint(int part) const;        // the square's middle, the middle of an arrow's shaft (benches)
  QPointF arrowTip(int part) const;         // the end of arrow k on screen (its value box goes there)

  // A pull from `from` (widget points): `along` is how far the pointer went along arrow `part`'s direction in world
  // units, `inPlane` the world move in the view's plane through the triad (part 0). Measured from the triad as it was when
  // the pull began, so the triad may follow the pointer meanwhile.
  void begin(int part, const QPointF& from);
  int dragging() const { return m_drag; }  // the part being pulled, -1 none
  double along(const QPointF& to) const;
  opad::Vec3 inPlane(const QPointF& to) const;
  double turn(const QPointF& to);  // the ring: radians turned round it since the press (right-handed about its axis), unwrapped
  void end();

 private:
  void draw();
  Viewport* m_view;
  Handle(AIS_InteractiveObject) m_object;
  bool m_shown = false;
  int m_hover = -1, m_drag = -1;
  opad::Vec3 m_at{0, 0, 0};
  std::vector<Arrow> m_arrows;
  QPointF m_centre;
  std::vector<std::pair<QPointF, QPointF>> m_screen;  // each arrow's shaft start and tip on screen (equal: end on)
  QPointF m_from, m_screenAxis;
  opad::Vec3 m_axis{0, 0, 0};
  opad::Frame m_plane;
  struct Ring {
    bool on = false;
    opad::Vec3 centre{0, 0, 0}, axis{0, 0, 1};
    double radius = 0;
  } m_ring;
  opad::Frame m_ringFrame;            // in the ring's plane, x from the centre to its first sample
  std::vector<QPointF> m_ringScreen;  // its samples on screen (closed), empty when not drawn
  double m_lastPhi = 0, m_turned = 0;
};
