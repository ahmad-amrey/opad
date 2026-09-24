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
CHECK_MAIN()
