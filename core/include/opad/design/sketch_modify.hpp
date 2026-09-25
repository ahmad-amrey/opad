#pragma once
#include "sketch_geom.hpp"
namespace opad::design {
std::vector<int> connected_entities(const Sketch& sketch,const std::vector<int>& seeds);
struct SketchTransform {
  double x=0,y=0,angle=0,scale=1,cx=0,cy=0;
  bool mirror=false; // reflect about the local x axis through (cx,cy), then rotate/scale/translate
};
std::vector<int> transform_entities(Sketch& sk,const std::vector<int>& ids,const SketchTransform& transform,bool copy);
std::vector<int> append_sketch_shape(Sketch& sk,const TopoDS_Shape& shape,const Frame& frame={},bool construction=false,bool fixed=false);
void offset_entities(Sketch& sk,const std::vector<int>& ids,double distance,bool round);
int heal_endpoints(Sketch& sk,double tolerance);
int heal_to_curves(Sketch& sk,double tolerance);
void split_entity(Sketch& sk,int entity,double x,double y);
void break_intersections(Sketch& sk,const std::vector<int>& entities);
void extend_entity(Sketch& sk,int entity,int boundary,double x,double y);
void chamfer_corner(Sketch& sk,int point,double first,double second);
void delete_curve_node(Sketch& sk,int point);
void boolean_regions(Sketch& sk,double ax,double ay,double bx,double by,const std::string& operation);
void identify_regions(const Sketch& sk,std::vector<Region>& regions,const Frame& frame={});
// Signed source entity IDs of a region's oriented boundary, stable when its geometry moves.
std::vector<int> region_sources(const Sketch& sk,const Region& region,const Frame& frame={});
}
