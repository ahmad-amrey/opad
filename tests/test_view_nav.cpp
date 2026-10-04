// Navigation staples that need no view (UI-47, app/ViewNav.hpp): the previous / next view history, the zoom window's
// rectangle and the view twist. The view side is the gui_benches case navigate.
#include "ViewNav.hpp"
#include "check.hpp"

namespace {
ViewState at(double x, double scale = 100) {
  ViewState s;
  s.eye = {x, 0, 100};
  s.target = {x, 0, 0};
  s.scale = scale;
  return s;
}
}  // namespace

TEST(history_walks_back_and_forward) {
  ViewHistory h;
  CHECK(!h.canBack() && !h.back() && !h.forward());
  CHECK(h.record(at(0)) && h.record(at(10)) && h.record(at(20)));
  CHECK(!h.record(at(20)));                // resting where it is records nothing
  CHECK(!h.record(at(20 + 1e-6)));         // nor a hair from it (an animation's end)
  CHECK(h.record(at(20, 50)));             // a zoom is a view of its own
  CHECK(h.size() == 4 && h.position() == 3);
  const ViewState* s = h.back();
  CHECK(s && s->same(at(20)) && h.back()->same(at(10)) && h.back()->same(at(0)) && !h.back());
  CHECK(h.forward()->same(at(10)) && h.canForward());
  CHECK(!h.record(at(10)));  // arriving at the entry gone back to keeps what is ahead
  CHECK(h.canForward() && h.size() == 4);
  CHECK(h.record(at(99)));  // a new view there drops it
  CHECK(!h.canForward() && h.size() == 3 && h.back()->same(at(10)));
  h.clear();
  CHECK(h.size() == 0 && !h.canBack() && !h.canForward());
}

TEST(history_keeps_the_last_ones) {
  ViewHistory h(5);
  for (int i = 0; i < 12; ++i) h.record(at(i));
  CHECK(h.size() == 5 && h.position() == 4);
  int back = 0;
  while (h.back()) ++back;
  CHECK(back == 4 && !h.canBack());
  CHECK(at(0).same(at(0)) && !at(0).same(at(0.01)));  // 1e-5 of the view's height: 0.001 at a 100 mm view
}

TEST(zoom_window_rectangle) {
  const auto r = viewnav::zoomRect(300, 200, 100, 50, 800, 600);
  CHECK(r[0] == 100 && r[1] == 50 && r[2] == 300 && r[3] == 200);  // any drag direction, ordered
  const auto click = viewnav::zoomRect(400, 300, 402, 301, 800, 600);  // a click: twice the zoom about it
  CHECK(click[0] == 200 && click[1] == 150 && click[2] == 600 && click[3] == 450);
  const auto thin = viewnav::zoomRect(10, 10, 300, 12, 800, 600);  // a drag too thin to frame anything is a click
  CHECK(thin[2] - thin[0] == 400 && thin[3] - thin[1] == 300);
}

TEST(view_twist) {
  const gp_Dir down(0, 0, -1), front(0, 1, 0);
  CHECK(viewnav::naturalUp(down).IsEqual(gp_Dir(0, 1, 0), 1e-12));   // a plan: north up
  CHECK(viewnav::naturalUp(front).IsEqual(gp_Dir(0, 0, 1), 1e-12));  // an elevation: Z up
  const gp_Dir iso(-1, 1, -1), isoUp = viewnav::naturalUp(iso);
  CHECK(std::abs(gp_Vec(isoUp).Dot(gp_Vec(iso))) < 1e-12 && isoUp.Z() > 0);  // square to the view, still upwards
  const gp_Dir natural = viewnav::naturalUp(down);
  CHECK(std::abs(viewnav::twist(down, natural, natural)) < 1e-12);
  for (const double degrees : {30.0, 90.0, -45.0, 180.0, 135.0}) {
    const gp_Dir up = viewnav::twisted(down, natural, degrees);
    CHECK(std::abs(gp_Vec(up).Dot(gp_Vec(down))) < 1e-12);  // still square to the view
    CHECK_NEAR(viewnav::twist(down, up, natural), degrees, 1e-9);
  }
  // Turning about the axis looked along: +90 turns a plan's up (Y) about -Z to +X, the drawing seen turned counter-clockwise.
  CHECK(viewnav::twisted(down, natural, 90).IsEqual(gp_Dir(1, 0, 0), 1e-12));
  gp_Dir rolled = natural;
  rolled.Rotate(gp_Ax1(gp_Pnt(0, 0, 0), down), 90 * M_PI / 180.0);  // as Viewport::rollView turns it
  CHECK_NEAR(viewnav::twist(down, rolled, natural), 90, 1e-9);
}

CHECK_MAIN()
