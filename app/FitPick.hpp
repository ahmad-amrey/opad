#pragma once
// Fitted references picked in the view (design/fit_refs.cpp, opad/scan_fit.hpp): a click on a mesh body (a scan, an STL)
// where a plane or an axis is taken gives a plane or axis fitted to the region grown from that triangle, a box over it
// the plane through the triangles it took. These only build the request (UI thread, no geometry); the fit itself runs
// on a worker (opad::design::fit_reference) and fills in its result, which the feature keeps.
#include <QString>

#include <string>
#include <vector>

#include "opad/json.hpp"
#include "opad/util.hpp"

namespace opad {
struct Scene;
}

namespace fitpick {

// Whether the reference is on a mesh body (its faces are triangles, its edges their sides).
bool onMesh(const opad::Scene& scene, const opad::Ref& ref);
// The fit a pick on a mesh gives: {"fit": {body, seed, tolerance, of}} (`axis`: of "auto", a round region's axis or a
// flat's normal; else a plane). Null when the pick is not a mesh's face or edge.
opad::json fromPick(const opad::Scene& scene, const opad::Ref& ref, bool axis);
// The triangles of one mesh body a box or a brush took, as a plane fit ({"fit": {body, triangles: ranges}}); null when
// they are not all faces of one mesh body.
opad::json fromTriangles(const opad::Scene& scene, const std::vector<opad::Ref>& refs);
// A plane or line fitted to picked points (vertices, a mesh's nodes too): {"fit": {points, body?, of}}.
opad::json fromPoints(const std::vector<opad::Ref>& refs, bool line);
// Whether a plane or axis input's value is a fitted reference, and whether its result is there.
bool isFit(const opad::json& value);
bool fitted(const opad::json& value);
// What the input shows: "Fitted plane", "Hole axis", "Face normal", "Wall direction", "Scan axes"...
QString title(const opad::json& value);
// Its quality, as the pick box's note: "rms 0.03 mm · 3,799 of 3,912 · good"; empty before the fit has run.
QString quality(const opad::json& value);
// "good" / "fair" / "poor" (empty when not fitted).
std::string grade(const opad::json& value);

}  // namespace fitpick
