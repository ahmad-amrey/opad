// The fixed order in which per-body looks compose (UI-121): base appearance, then asset, lock, activation, compare,
// explode and candidate layers; selection is the viewport's (Topmost) and comes after all of them.
#include "BodyLook.hpp"
#include "check.hpp"

namespace {
using Layers = std::array<const LookDelta*, kLookSources>;
Layers with(std::initializer_list<std::pair<LookSource, const LookDelta*>> deltas) {
  Layers layers{};
  for (const auto& [source, delta] : deltas) layers[static_cast<std::size_t>(source)] = delta;
  return layers;
}
BodyLook base() {
  BodyLook b;
  b.color = {0.2, 0.4, 0.6};
  b.opacity = 0.8;
  return b;
}
}  // namespace

TEST(no_layer_is_the_document_appearance) {
  CHECK(looks::compose(base(), Layers{}, false) == base());
  LookDelta empty;
  CHECK(looks::compose(base(), with({{LookSource::Compare, &empty}}), false) == base());
}

TEST(activation_ghost_under_a_compare_tint) {
  LookDelta ghost, tint;
  ghost.ghost = true;
  tint.color = std::array<double, 3>{0.9, 0.3, 0.1};
  const BodyLook look = looks::compose(base(), with({{LookSource::Activation, &ghost}, {LookSource::Compare, &tint}}), false);
  CHECK(look.color == tint.color);  // the tint, drawn at the ghost's opacity
  CHECK_NEAR(look.opacity, looks::kGhostOpacity, 1e-12);
  CHECK(look.ghost && !look.pickable && look.visible && look.layer == Graphic3d_ZLayerId_Default);
  // The ghost alone greys the document colour; ghosts become pickable while references are asked for.
  const BodyLook alone = looks::compose(base(), with({{LookSource::Activation, &ghost}}), true);
  CHECK(alone.color == looks::mix(base().color, looks::kGhostGrey, 0.5) && alone.pickable);
  // The viewport's ghost is the theme's role (Tokens::ghost): its colour and alpha.
  const looks::GhostStyle themed{{0.36, 0.38, 0.42}, 0.3};
  const BodyLook light = looks::compose(base(), with({{LookSource::Activation, &ghost}}), false, themed);
  CHECK(light.color == looks::mix(base().color, themed.color, 0.5));
  CHECK_NEAR(light.opacity, 0.3, 1e-12);
}

TEST(lock_fade_explode_and_candidate) {
  LookDelta lock, explode, again, candidate;
  lock.fade = 0.5;
  lock.pickable = false;
  explode.offset = {10, 0, -5};
  again.offset = {1, 2, 3};
  candidate.color = std::array<double, 3>{1, 0.7, 0.1};
  candidate.pickable = true;
  candidate.layer = Graphic3d_ZLayerId_Top;
  const BodyLook locked = looks::compose(base(), with({{LookSource::Lock, &lock}}), false);
  CHECK_NEAR(locked.opacity, 0.4, 1e-12);
  CHECK(!locked.pickable && locked.color == base().color);
  const BodyLook all = looks::compose(base(), with({{LookSource::Lock, &lock}, {LookSource::Explode, &explode}, {LookSource::Candidate, &candidate}}), false);
  CHECK(all.pickable && all.color == *candidate.color && all.layer == Graphic3d_ZLayerId_Top);  // the later layer wins
  CHECK(all.offset == (std::array<double, 3>{10, 0, -5}));
  CHECK_NEAR(all.opacity, 0.4, 1e-12);
  const BodyLook moved = looks::compose(base(), with({{LookSource::Asset, &again}, {LookSource::Explode, &explode}}), false);
  CHECK(moved.offset == (std::array<double, 3>{11, 2, -2}));  // offsets add up
}

TEST(lock_fade_then_ghost_and_hidden) {
  LookDelta lock, ghost, hide;
  lock.fade = 0.5;
  ghost.ghost = true;
  hide.visible = false;
  BodyLook opaque = base();
  opaque.opacity = 1;
  CHECK_NEAR(looks::compose(opaque, with({{LookSource::Lock, &lock}, {LookSource::Activation, &ghost}}), false).opacity, looks::kGhostOpacity, 1e-12);
  const BodyLook hidden = looks::compose(base(), with({{LookSource::Compare, &hide}}), true);
  CHECK(!hidden.visible && !hidden.shownPickable());
  LookDelta show;
  show.visible = true;
  CHECK(looks::compose(base(), with({{LookSource::Compare, &hide}, {LookSource::Candidate, &show}}), false).visible);
  // A small part hidden while the view moves stays hidden whatever the layers under it show.
  CHECK(!looks::compose(base(), with({{LookSource::Candidate, &show}, {LookSource::Navigation, &hide}}), false).visible);
  LookDelta loud;
  loud.opacity = 3;
  CHECK_NEAR(looks::compose(base(), with({{LookSource::Candidate, &loud}}), false).opacity, 1, 1e-12);
}

CHECK_MAIN()
