// Sketch fillet on a profile like a drawing turned into a sketch (core fillet_geometry / fillet_corner / fillet_problem):
// a short upright line with its length dimension meeting a long arc, a construction line ending at the same corner, and
// corners where no fillet fits, each with the reason the tool says (the click did nothing visible before).
#include <cmath>
#include <set>

#include "check.hpp"
#include "opad/design/sketch_modify.hpp"

using namespace opad::design;
using CT = SkConstraint::Type;
using P = FilletProblem;

namespace {
struct Profile {
  Sketch sk;
  int corner = 0;           // the upright line meeting the long arc, a construction line ending there too
  int upright = 0, length = 0;
  int apart = 0;            // a line and an arc whose ends are two points held together (a drawing's coincident ends)
  int three = 0;            // three lines end here
  int tangent = 0;          // a line running on into an arc
  int tight = 0;            // a line half a millimetre long against a short arc
};

Profile make_profile() {
  Profile p;
  Sketch& sk = p.sk;
  // The corner of the report: an upright line 4 mm long (its dimension), the long arc leaving it, a construction line.
  const int base = sk.add_point(10, 1);
  p.corner = sk.add_point(10, 5);
  p.upright = sk.add_line(base, p.corner);
  sk.add_constraint(CT::Vertical, {p.upright});
  p.length = sk.add_constraint(CT::Distance, {p.upright}, 4);
  // The arc leaves the corner down and to the right (centre up and right of it, counter-clockwise from the corner).
  const double s1 = -60 * M_PI / 180;
  const int centre = sk.add_point(10 + 4.2, 5 + 5.6), arcEnd = sk.add_point(14.2 + 7 * std::cos(s1), 10.6 + 7 * std::sin(s1));
  sk.add_arc(centre, p.corner, arcEnd);
  const int guideEnd = sk.add_point(12, 5);
  const int guide = sk.add_line(p.corner, guideEnd, true);
  sk.add_constraint(CT::Horizontal, {guide});
  // Two ends apart, held by a coincident constraint.
  const int a0 = sk.add_point(20, 0), a1 = sk.add_point(20, 4), b0 = sk.add_point(20, 4), bc = sk.add_point(13, 4), b1 = sk.add_point(13, 11);
  sk.add_line(a0, a1);
  sk.add_arc(bc, b0, b1);
  sk.add_constraint(CT::Coincident, {a1, b0});
  p.apart = a1;
  // Three lines from one point.
  p.three = sk.add_point(40, 20);
  sk.add_line(p.three, sk.add_point(45, 20));
  sk.add_line(p.three, sk.add_point(40, 25));
  sk.add_line(p.three, sk.add_point(36, 24));
  // A line running on into an arc: the arc leaves upward where the line leaves downward.
  p.tangent = sk.add_point(30, 4);
  sk.add_line(sk.add_point(30, 0), p.tangent);
  sk.add_arc(sk.add_point(23, 4), p.tangent, sk.add_point(23, 11));
  // Half a millimetre of line against an arc of a few degrees.
  p.tight = sk.add_point(60, 4);
  sk.add_line(sk.add_point(60, 3.5), p.tight);
  const double a = std::atan2(-0.8, -0.6) + 5 * M_PI / 180;  // leaving down and to the right, as at the report's corner
  sk.add_arc(sk.add_point(60 + 4.2, 4 + 5.6), p.tight, sk.add_point(64.2 + 7 * std::cos(a), 9.6 + 7 * std::sin(a)));
  CHECK(solve(sk).converged);
  return p;
}
}  // namespace

// The report's corner: the fillet fits beside the construction line and the sketch solves, its length dimension kept.
TEST(fillet_at_a_line_arc_corner_with_a_construction_line) {
  Profile p = make_profile();
  CHECK(fillet_problem(p.sk, p.corner, 2).kind == P::None);
  const int arc = fillet_corner(p.sk, p.corner, 2);
  evaluate_dimensions(p.sk);
  CHECK(solve(p.sk).converged);
  CHECK(p.sk.entity(arc) && p.sk.entity(arc)->type == SkEntity::Type::Arc);
  CHECK(p.sk.constraint(p.length) && std::fabs(p.sk.constraint(p.length)->value - 4) < 1e-9);  // still 4 mm, on the old side
}

// Where none fits, why (the tool says it on the status line and in a toast).
TEST(no_fillet_says_why) {
  const Profile p = make_profile();
  CHECK(fillet_problem(p.sk, p.apart, 2).kind == P::TooFew);  // the arc ends at a point of its own
  const P many = fillet_problem(p.sk, p.three, 1);
  CHECK(many.kind == P::TooMany && many.curves == 3);
  CHECK(fillet_problem(p.sk, p.tangent, 1).kind == P::Tangent);
  const P big = fillet_problem(p.sk, p.tight, 2);
  CHECK(big.kind == P::TooBig && big.largest > 0 && big.largest < 2);
  FilletCorner f;
  CHECK(fillet_geometry(p.sk, p.tight, big.largest * 0.99, f) && !fillet_geometry(p.sk, p.tight, big.largest * 1.05, f));  // about the largest
}

CHECK_MAIN()
