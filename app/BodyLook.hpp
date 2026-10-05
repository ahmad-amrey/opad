#pragma once
// Per-body display state (UI-121). Features that change how bodies look for a while (an appearance being edited,
// activation ghosts, lock fade, compare tints, explode offsets, smart-select candidates, asset styling) each own one layer
// of LookDeltas on the viewport (Viewport::setLookLayer) instead of writing AIS state themselves; the viewport composes
// the layers over the document's appearance in one fixed order and applies the result per body. Hover and selection come
// after all of them: a selected body moves to the Topmost layer (X-ray) whatever its look, and is highlighted there.
//
// Composition, in LookSource order, each layer over what the earlier ones made:
//   visible, color, opacity, pickable, layer   replaced when the delta has them
//   fade                                       multiplies the opacity
//   ghost                                      colour halfway to the ghost colour, opacity at most the ghost's, not
//                                              pickable (pickable while ghosts are, Viewport::setGhostsPickable); the
//                                              viewport passes the theme's ghost role (Tokens::ghost, its alpha), the
//                                              delta's ghostOpacity replaces that alpha
//   reference                                  pickable only while ghosts are (a reference: measured, snapped to, a
//                                              feature's input), never selected: a locked body
//   offset                                     added (a world translation; picking follows)
// So an activation ghost under a compare tint is drawn in the tint at the ghost's opacity, a lock fade under a ghost
// ends at the ghost's opacity, and a smart-select candidate on a ghost may set its own opacity and pickable.
#include <Graphic3d_ZLayerId.hxx>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// Edit: an appearance being edited and not written yet (the opacity slider while it is dragged), over the document's.
// Navigation: parts hidden while the view moves (Viewport::setSmallPartFilter), over everything else.
enum class LookSource { Edit, Asset, Lock, Activation, Compare, Explode, Candidate, Navigation, Count };
constexpr std::size_t kLookSources = static_cast<std::size_t>(LookSource::Count);

struct LookDelta {
  std::optional<bool> visible;
  std::optional<std::array<double, 3>> color;
  std::optional<double> opacity;
  double fade = 1;
  bool ghost = false;
  std::optional<double> ghostOpacity;  // a ghost's opacity in place of the theme's (the user's inactive opacity)
  bool reference = false;              // picked only as a reference (while ghosts are), never selected: a locked body
  std::optional<bool> pickable;
  std::optional<Graphic3d_ZLayerId> layer;  // where the body is drawn while it is not selected
  std::array<double, 3> offset{0, 0, 0};
  bool operator==(const LookDelta&) const = default;
};

struct BodyLook {
  bool visible = true;
  std::array<double, 3> color{0.75, 0.75, 0.78};
  double opacity = 1;
  bool ghost = false, pickable = true;
  Graphic3d_ZLayerId layer = Graphic3d_ZLayerId_Default;
  std::array<double, 3> offset{0, 0, 0};
  // A 2D drawing's lines (Drawing2D.hpp): width in device pixels (0 = not a drawing: the drawer's own) and the stipple of
  // its linetype (16 bits, pixels per bit; 0xFFFF solid).
  double lineWidth = 0;
  uint16_t linePattern = 0xFFFF, lineFactor = 1;
  bool operator==(const BodyLook&) const = default;
  bool shownPickable() const { return visible && pickable; }
};

namespace looks {
constexpr double kGhostOpacity = 0.15;  // other components while one is active (without a theme)
constexpr std::array<double, 3> kGhostGrey{0.62, 0.64, 0.68};
struct GhostStyle {
  std::array<double, 3> color = kGhostGrey;
  double opacity = kGhostOpacity;
};
// `base` is the document's appearance; layers[s] is LookSource s's delta for the body (nullptr: none).
BodyLook compose(BodyLook base, const std::array<const LookDelta*, kLookSources>& layers, bool ghostsPickable, const GhostStyle& ghost = {});
std::array<double, 3> mix(const std::array<double, 3>& a, const std::array<double, 3>& b, double t);  // a + (b - a) t
}  // namespace looks
