#include "check.hpp"
#include "opad/design/sketch_reference.hpp"
#include "opad/design/sketch_trace.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch_modify.hpp"
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAdaptor_Curve.hxx>
#include "opad/geometry.hpp"
#include "opad/step_io.hpp"
#include <cmath>
using namespace opad;
using namespace opad::design;

TEST(bitmap_trace_preserves_holes_and_removes_noise) {
  const int width=32;std::vector<unsigned char> grey(width*width,255);
  for(int y=4;y<28;++y)for(int x=4;x<28;++x)if(x<10||x>=22||y<10||y>=22)grey[y*width+x]=0;
  grey[0]=0;TraceOptions options;options.smoothing=0;options.noise=2;options.corner_angle=180;
  const auto traced=trace_bitmap(grey,width,width,options);CHECK_EQ(traced.entities.size(),size_t(8));
  const auto regions=sketch_regions(traced,{});CHECK_EQ(regions.size(),size_t(2));
  double area=0;for(const auto& r:regions)area+=r.area;CHECK_NEAR(area,24*24,1e-6);
  CHECK_THROWS(trace_bitmap(grey,width,width,options,[]{return true;}));
}

TEST(bitmap_trace_diagonal_contacts_stay_separate) {
  std::vector<unsigned char> grey={0,255,255,0};TraceOptions options;options.smoothing=0;options.noise=0;options.tolerance=0;options.corner_angle=180;
  auto traced=trace_bitmap(grey,2,2,options);CHECK_EQ(traced.entities.size(),size_t(8));
}

TEST(projected_curve_keeps_exact_basis_and_can_break_link) {
  Document doc=Document::create();Sketch seed;const int circle=seed.add_circle(seed.add_point(0,0),10);
  auto op=make_sketch_op("Source",{{"base","xy"}},seed.to_json());const auto id=new_uuid();op["id"]=id;apply_ops(doc,{op});
  Frame frame;frame.y={0,std::sqrt(.5),std::sqrt(.5)};
  const json source={{"sketch",id},{"entity",circle}};const auto generated=derive_sketch(doc,resolve(doc),frame,source);
  CHECK_EQ(generated.entities.size(),size_t(1));CHECK(generated.entities[0].type==SkEntity::Type::Spline);
  BRepAdaptor_Curve curve(entity_edge(generated,generated.entities[0],{}));CHECK(!curve.BSpline().IsNull());
  Sketch linked;append_reference(linked,generated,source,"project",true);CHECK(linked.entities[0].fixed);CHECK(!linked.entities[0].source.is_null());
  const int stable=linked.entities[0].id;break_reference(linked,{stable});CHECK(!linked.entities[0].fixed);CHECK(linked.entities[0].source.is_null());
}

TEST(linked_projection_regenerates_with_stable_ids) {
  Document doc=Document::create();Sketch seed;int circle=seed.add_circle(seed.add_point(0,0),10);
  const auto id=new_uuid();auto op=make_sketch_op("Source",{{"base","xy"}},seed.to_json());op["id"]=id;apply_ops(doc,{op});
  const json source={{"sketch",id},{"entity",circle}};Sketch projected;append_reference(projected,derive_sketch(doc,resolve(doc),{},source),source,"project",true);
  const auto target=new_uuid();op=make_sketch_op("Projection",{{"base","xy"}},projected.to_json());op["id"]=target;apply_ops(doc,{op});
  seed.entity(circle)->r=20;apply_ops(doc,{make_edit_op(id,{{"geometry",seed.to_json()}})});
  const auto scene=resolve(doc);CHECK(scene.unresolved.empty());const auto updated=Sketch::from_json(scene.sketch(target)->geometry);
  CHECK_EQ(updated.entities[0].id,projected.entities[0].id);CHECK_NEAR(updated.entities[0].r,20,1e-8);
}
TEST(body_section_silhouette_and_vertex_projection) {
  Document doc=Document::create();import_brep(doc,brep_from_shape(BRepPrimAPI_MakeCylinder(10,20).Shape()),"Cylinder");const auto scene=resolve(doc);const auto body=scene.all_bodies().front();
  Frame frame;frame.origin={0,0,5};auto section=derive_sketch(doc,scene,frame,{{"body",body},{"kind","body"}},"intersect");
  CHECK_EQ(section.entities.size(),size_t(1));CHECK(section.entities[0].type==SkEntity::Type::Circle);CHECK_NEAR(section.entities[0].r,10,1e-7);
  auto outline=derive_sketch(doc,scene,{},{{"body",body},{"kind","body"}},"silhouette");CHECK(!outline.entities.empty());const auto regions=sketch_regions(outline,{});CHECK(!regions.empty());CHECK_NEAR(regions[0].area,100*M_PI,1e-6);
  auto vertex=derive_sketch(doc,scene,{},{{"body",body},{"kind","vertex"},{"index",1}});CHECK_EQ(vertex.entities.size(),size_t(1));CHECK(vertex.entities[0].type==SkEntity::Type::Point);
}
TEST(sketch_backdrop_round_trip_and_id_delta) {
  Sketch sk;sk.images.push_back({{"id",1},{"data","test"},{"position",{2,3}},{"width",10},{"height",5},{"opacity",.5}});CHECK(sk.next_id()>1);
  const auto before=sk.to_json();sk.images[0]["width"]=20;const auto after=sk.to_json();const auto delta=sketch_delta(before,after);CHECK_EQ(delta["images"].size(),size_t(1));CHECK(apply_sketch_delta(before,delta)==after);
  CHECK(Sketch::from_json(after).to_json()==after);sk.images[0]["width"]=-1;CHECK_THROWS(sk.validate());
}
CHECK_MAIN()
