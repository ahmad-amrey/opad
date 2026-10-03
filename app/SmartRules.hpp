#pragma once
// The decisions behind smart selection and Del (TODO 11 UI-95, UI-04), free of widgets so tests/test_smart_select can
// check them: the related command's candidates as the chip reads them, which one the chip offers, what Del does to a
// selection of objects and which later features deleting a feature would break.
#include <functional>
#include <map>
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
// that made the body, chains and loops). Never a similar set (that rule is asked for: Select similar), the body (the next
// rung of Ctrl+Up) or the import that made it (no history: that is the body's faces), nor a group or chain holding more
// than half of `bodyFaces` (the faces of the picked bodies; 0: unknown), which is no detail of the body. -1: nothing.
int headline(const std::vector<Candidate>& c, const std::vector<opad::Ref>& picks, size_t bodyFaces = 0);

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

// What depends on a feature (Select dependents): the later features deleting it would leave failing, and the faces they
// made on the bodies they changed (provenance; a sketch, or a feature that made no face of its own, has none). Plans and
// walks geometry: workers only.
struct Users {
  std::vector<std::pair<std::string, std::string>> ops;  // {op, name}, in history order
  std::vector<opad::Ref> faces;
};
Users usersOf(const opad::Document& doc, const std::string& op, const std::function<bool()>& cancel = {});

// What the ops of the history made, as the timeline points at them (UI-99), by op: the faces a feature made on the bodies
// it produced or changed (provenance, the whole history replayed) and those bodies; an import's bodies (no faces: they
// are the import). `changes`: the feature made no body of its own (a boss, a fillet: its faces are what it is, a click on
// its marker selects them), else its bodies are. Every body a feature touched is walked once. Workers only.
struct Made {
  std::vector<opad::Ref> faces;
  std::vector<std::string> bodies;
  bool changes = false;
};
std::map<std::string, Made> madeBy(const opad::Document& doc, const std::function<bool()>& cancel = {});

}  // namespace smart
