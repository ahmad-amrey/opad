#pragma once
// Point-pair registration: the rigid move (a turn and a shift, no scaling) that best puts points A1, A2, ... onto B1, B2,
// ... in the least-squares sense (Kabsch / Umeyama through an SVD). Align's "point_pairs" mode (design/features.cpp): a
// scan's points picked against the matching points of its CAD body, another body or a sketch.
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <vector>

namespace opad::pairs {

struct PairFit {
  gp_Trsf motion;                 // A_i -> about B_i
  std::vector<double> residuals;  // |motion(A_i) - B_i| for every pair, the one left out too (mm)
  int dropped = -1;               // the pair left out as stray (0-based); -1: every pair was used
  double rms = 0, max = 0;        // of the residuals of the pairs used
};

// Throws opad::Error with the reason when the pairs cannot fix the move: the two lists differ in length, fewer than three
// pairs, or either side's points all at one place or on one line (the move could still turn about it). With
// `drop_outlier` and four pairs or more, the one pair whose error is far above what the others' scatter explains (judged on
// a fit without it, the way the scan fits leave out stray points) is left out and reported; never one whose leaving would
// put the rest on a line.
PairFit fit_pairs(const std::vector<gp_Pnt>& from, const std::vector<gp_Pnt>& to, bool drop_outlier);

}  // namespace opad::pairs
