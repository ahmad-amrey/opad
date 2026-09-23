#pragma once
#include <Bnd_Box.hxx>
#include <algorithm>
#include <numeric>
#include <set>
#include <vector>

// Color the overlap graph: disjoint parts reuse a small bias, intersecting parts
// get distinct integer depth slots. Input order is the stable scene-node order.
inline std::vector<int> depthSlots(const std::vector<Bnd_Box>& boxes) {
  std::vector<int> ranks(boxes.size(), 0), order(boxes.size());
  std::iota(order.begin(), order.end(), 0);
  auto xmin = [&](int i) { return boxes[i].IsVoid() ? 0.0 : boxes[i].CornerMin().X(); };
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return xmin(a) < xmin(b); });
  std::vector<int> active;
  for (int i : order) {
    if (boxes[i].IsVoid()) continue;
    active.erase(std::remove_if(active.begin(), active.end(), [&](int j) {
      return boxes[j].CornerMax().X() < xmin(i);
    }), active.end());
    std::set<int> used;
    for (int j : active) if (!boxes[i].IsOut(boxes[j])) used.insert(ranks[j]);
    while (used.count(ranks[i])) ++ranks[i];
    active.push_back(i);
  }
  return ranks;
}
