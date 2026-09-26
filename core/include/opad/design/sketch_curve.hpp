#pragma once
// Equation curves (gap log #2): a fit spline whose `equation` gives x(t) and y(t) as expressions over the document's
// parameters, a range t0..t1 and a tolerance. Computing the sketch samples it adaptively until the spline through
// the samples is within the tolerance of the curve everywhere, so the point count follows from the tolerance, not
// from the author; the samples are ordinary fixed points, stored with the sketch, so replay only reads them.
#include <string>

#include "expr.hpp"
#include "sketch.hpp"

namespace opad::design {

// Whether the sketch has equation curves, and what their samples depend on (their equations and the values of the
// parameters those name): cheap, for the sketch's fingerprint, so an unchanged curve is not sampled again.
bool has_equation_curves(const Sketch& sk);
std::string equation_inputs(const Sketch& sk, const ParamTable& params);

// Samples every equation curve and puts the samples in its points: the first and last point ids stay (a closed curve
// repeats its first id at the end), interior ids are reused in order, new ones added and spare ones removed.
void evaluate_curves(Sketch& sk, const ParamTable& params);

}  // namespace opad::design
