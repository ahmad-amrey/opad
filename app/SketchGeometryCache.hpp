#pragma once
#include "opad/design/sketch.hpp"
#include <unordered_map>
#include <unordered_set>

// Immutable between edits. Curves and the picking tree share the same sampled geometry.
// A replacement is prepared on a worker; the UI keeps the previous presentation meanwhile.
class SketchGeometryCache {
 public:
  using Polyline=std::vector<std::pair<double,double>>;
  struct Query {std::vector<size_t> points,entities;};
  void update(const opad::design::Sketch&,double deflection);
  bool matches(const opad::design::Sketch&,double deflection) const;
  const Polyline* samples(const opad::design::Sketch&,const opad::design::SkEntity&) const;
  const opad::design::SkPoint* point(const opad::design::Sketch&,int id) const;
  const opad::design::SkEntity* entity(const opad::design::Sketch&,int id) const;
  // What a mouse move asks (UI-27: they scanned the whole sketch per move): whether a point is the centre of a circle, an
  // arc or an ellipse, and the curves through a point (indices into the sketch's entities).
  bool centre(int point) const {return m_centres.count(point)>0;}
  const std::vector<size_t>& curvesAt(int point) const;
  Query query(double x0,double y0,double x1,double y1) const;
  size_t builds=0;
 private:
  struct Box {double x0=1e300,y0=1e300,x1=-1e300,y1=-1e300;void add(double,double);void add(const Box&);bool intersects(const Box&)const;};
  struct Curve {std::vector<double> signature;Polyline poly;Box box;};
  struct Entry {Box box;size_t index;bool point;};
  struct Node {Box box;int left=-1,right=-1;size_t begin=0,end=0;};
  std::vector<double> signature(const opad::design::Sketch&,const opad::design::SkEntity&) const;
  int partition(size_t begin,size_t end);
  std::vector<opad::design::SkPoint> m_points;
  std::vector<opad::design::SkEntity> m_entities;
  std::unordered_map<int,size_t> m_pointIndex,m_entityIndex;
  std::unordered_set<int> m_centres;
  std::unordered_map<int,std::vector<size_t>> m_pointCurves;
  std::unordered_map<int,Curve> m_curves;
  std::vector<Entry> m_entries;
  std::vector<Node> m_tree;
  double m_deflection=0;
};
