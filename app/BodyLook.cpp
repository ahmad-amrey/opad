#include "BodyLook.hpp"

#include <algorithm>

namespace looks {
std::array<double, 3> mix(const std::array<double, 3>& a, const std::array<double, 3>& b, double t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

BodyLook compose(BodyLook look, const std::array<const LookDelta*, kLookSources>& layers, bool ghostsPickable, const GhostStyle& ghost) {
  for (const LookDelta* d : layers) {
    if (!d) continue;
    if (d->visible) look.visible = *d->visible;
    if (d->color) look.color = *d->color;
    if (d->opacity) look.opacity = *d->opacity;
    look.opacity *= d->fade;
    if (d->ghost) {
      look.ghost = true;
      look.color = mix(look.color, ghost.color, 0.5);
      look.opacity = std::min(look.opacity, ghost.opacity);
      look.pickable = ghostsPickable;
    }
    if (d->pickable) look.pickable = *d->pickable;
    if (d->layer) look.layer = *d->layer;
    for (int i = 0; i < 3; ++i) look.offset[i] += d->offset[i];
  }
  look.opacity = std::clamp(look.opacity, 0.0, 1.0);
  return look;
}
}  // namespace looks
