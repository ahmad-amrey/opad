#include "check.hpp"
#include "SketchGeometryCache.hpp"
using namespace opad::design;
TEST(spatial_queries_cache_edits_and_undo) {
  Sketch sk;
  for(int i=0;i<1000;++i)sk.add_line(sk.add_point(i*10,0),sk.add_point(i*10+5,0));
  SketchGeometryCache cache;cache.update(sk,.01);const auto builds=cache.builds;
  CHECK(cache.matches(sk,.01));
  auto near=cache.query(101,-1,104,1);CHECK_EQ(near.entities.size(),1u);CHECK(near.points.empty());
  const auto original=sk;
  sk.points[0].y=20;CHECK(!cache.matches(sk,.01));CHECK(cache.samples(sk,sk.entities.front())==nullptr);
  cache.update(sk,.01);CHECK_EQ(cache.builds,builds+1);CHECK(cache.matches(sk,.01));
  CHECK(!cache.query(-1,19,6,21).points.empty());
  sk=original;cache.update(sk,.01);CHECK_EQ(cache.builds,builds+2);CHECK(cache.query(-1,19,6,21).points.empty());
  sk.entities.erase(sk.entities.begin());cache.update(sk,.01);CHECK_EQ(cache.query(-1,-1,6,1).entities.size(),0u);
}
TEST(curves_share_pick_samples_and_refine_only_on_zoom_in) {
  Sketch sk;const int id=sk.add_circle(sk.add_point(7,9),10);
  SketchGeometryCache cache;cache.update(sk,.1);const auto* poly=cache.samples(sk,*sk.entity(id));CHECK(poly&&poly->size()>8);
  for(auto [x,y]:*poly)CHECK_NEAR(std::hypot(x-7,y-9),10,1e-9);
  CHECK_EQ(cache.query(16.9,8.9,17.1,9.1).entities.size(),1u);
  CHECK(cache.matches(sk,1));CHECK(!cache.matches(sk,.01));cache.update(sk,.01);CHECK_EQ(cache.builds,2u);
  CHECK(cache.matches(sk,.01));
}
// UI-27: what a mouse move asks comes from the cache, never a scan of the sketch: centres, the curves through a point, ids.
TEST(centres_curves_at_a_point_and_ids) {
  Sketch sk;
  const int c=sk.add_point(0,0),a=sk.add_point(5,0),b=sk.add_point(0,5),e=sk.add_point(9,9);
  const int circle=sk.add_circle(c,5),arc=sk.add_arc(a,c,b),line=sk.add_line(a,e),other=sk.add_line(e,b);
  SketchGeometryCache cache;cache.update(sk,.01);
  CHECK(cache.centre(c) && cache.centre(a) && !cache.centre(b) && !cache.centre(e));
  CHECK_EQ(cache.curvesAt(a).size(),2u);  // the arc about it and the line from it
  CHECK(sk.entities[cache.curvesAt(e)[0]].id==line && sk.entities[cache.curvesAt(e)[1]].id==other);
  CHECK(cache.curvesAt(12345).empty());
  CHECK(cache.entity(sk,arc)==sk.entity(arc) && cache.entity(sk,circle)->r==5 && cache.point(sk,b)==sk.point(b) && cache.entity(sk,b)==nullptr);
  sk.remove(line);  // stale until updated: ids are still found (by the sketch), indices are checked
  CHECK(cache.entity(sk,other)==sk.entity(other) && cache.entity(sk,line)==nullptr);
  cache.update(sk,.01);
  CHECK_EQ(cache.curvesAt(a).size(),1u);
}
CHECK_MAIN()
