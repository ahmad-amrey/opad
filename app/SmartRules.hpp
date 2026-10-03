#pragma once
// The decisions behind smart selection and Del (TODO 11 UI-95, UI-04), free of widgets so tests/test_smart_select can
// check them: the related command's candidates as the chip reads them, which one the chip offers, what Del does to a
// selection of objects and which later features deleting a feature would break.
#include <string>
#include <utility>
#include <vector>

#include "opad/core.hpp"

namespace smart {

struct Candidate {
  std::string kind;  // feature, import, hole, fillet, chamfer, boss, pocket, wall, tangent, loop, similar, body
  std::string op;    // feature and import: the op that made the faces
  std::string name;  // a feature's or import's name; a recognised group's label (English data)
  std::string category, featureKind, icon, rule;
  std::vector<opad::Ref> refs;  // what selecting it selects (faces; edges for chains and loops; the body)
  size_t count = 0;             // refs may be fewer (truncated)
  bool containsSelection = false, truncated = false;
  opad::json params = opad::json::object();
  bool feature() const { return kind == "feature"; }
  bool group() const;  // a recognised detail: hole, fillet, chamfer, boss, pocket, wall
  std::vector<std::string> bodies() const;  // the bodies its refs are on, in order
};

// The related command's answer (opad::design::related), in its order.
std::vector<Candidate> candidates(const opad::json& related);
bool sameRefs(std::vector<opad::Ref> a, std::vector<opad::Ref> b);
// The candidate that is exactly the selection (the picks are a feature's whole face set, a hole ...), -1 if none.
int matching(const std::vector<Candidate>& c, const std::vector<opad::Ref>& picks);
// What the chip offers for picked faces or edges (design notes F §A6): the first candidate holding every pick that adds
// to them (related ranks the feature that made part of the body first, then the smallest recognised group, the feature
// or import that made the body, chains and loops). Never a similar set (that rule is asked for: Select similar) nor the
// body (the next rung of Ctrl+Up). -1: nothing to offer.
int headline(const std::vector<Candidate>& c, const std::vector<opad::Ref>& picks);

// Del on objects (UI-04): what to tombstone and what to take out with a Remove feature, never more than was selected.
// Sketches are tombstoned. Bodies (a component counts as its bodies) of a document with a design history go to one
// Remove feature, so the history and everything built on them stays. Without a history an import is tombstoned only
// when every body it made is selected; the bodies of an import picked in part are removed instead.
struct Deletion {
  std::vector<std::string> tombstone;  // op ids (imports, sketches)
  std::vector<std::string> remove;     // body ids for one Remove feature
  bool empty() const { return tombstone.empty() && remove.empty(); }
};
Deletion routeDelete(const opad::Scene& scene, const std::vector<std::string>& ids);
std::vector<opad::json> deletionOps(const Deletion& d, const opad::Scene& scene);  // for applyOps: tombstones, then the Remove

// The later features a plan of deleting `deleted` leaves failing that work now: {op, name}, in history order.
std::vector<std::pair<std::string, std::string>> dependents(const opad::json& report, const opad::Scene& scene, const std::vector<std::string>& deleted);

}  // namespace smart
