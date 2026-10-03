#include "opad/design/sketch_reference.hpp"
#include "opad/design/sketch_modify.hpp"
#include "engine.hpp"
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <HLRAlgo_Projector.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include "opad/geometry.hpp"
#include <algorithm>
#include <cmath>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <set>

namespace opad::design {
namespace {
// A linked KiCad board's geometry by what it is (UI-134): {"asset": <import>, "kicad": "outline" | "holes" | "hole" | "part",
// "node": <the Outline layer, the Mounting holes component, one hole, a footprint's component>, "ref"?}. Those node ids are
// kept by a sync (by place, a footprint by its uuid), so the projection follows the board. A part is the outline of its
// box in its own frame (a connector's cutout), seen along the sketch's normal.
TopoDS_Shape asset_source(const Ctx& ctx,const Frame& frame,const json& source) {
  const std::string what=source.value("kicad",""),id=source.value("node","");
  const Node* n=ctx.scene.node(id);
  if(!n)throw Error(what=="part"?"a projected KiCad part is no longer on the board":what=="hole"?"a projected mounting hole is no longer on the board":"the projected KiCad board geometry is no longer there; project it again");
  const std::vector<std::string> bodies=n->kind==Node::Kind::Body?std::vector<std::string>{id}:ctx.scene.bodies_under(id);
  for(const auto& b:bodies)if(const Node* m=ctx.scene.node(b);m && m->body_missing)throw Error("the linked KiCad board is not loaded");
  TopoDS_Compound out;BRep_Builder builder;builder.MakeCompound(out);
  if(what!="part") {
    for(const auto& b:bodies){TopoDS_Shape proto=ctx.key_shape(ctx.scene.node(b)->body_key);const gp_Trsf t=ctx.node_trsf(b);builder.Add(out,t.Form()==gp_Identity?proto:proto.Moved(TopLoc_Location(t)));}
    return out;
  }
  const gp_Trsf at=ctx.node_trsf(id),back=at.Inverted();Bnd_Box box;
  for(const auto& b:bodies) {
    const Bnd_Box own=key_tight_bbox(ctx.doc,ctx.scene.node(b)->body_key);if(own.IsVoid())continue;
    double x0,y0,z0,x1,y1,z1;own.Get(x0,y0,z0,x1,y1,z1);gp_Trsf t=back;t.Multiply(ctx.node_trsf(b));
    for(int i=0;i<8;++i)box.Add(gp_Pnt(i&1?x1:x0,i&2?y1:y0,i&4?z1:z0).Transformed(t));
  }
  if(box.IsVoid())throw Error("a projected KiCad part has no geometry");
  double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
  std::vector<std::array<double,2>> pts;
  for(int i=0;i<8;++i){const gp_Pnt p=gp_Pnt(i&1?x1:x0,i&2?y1:y0,i&4?z1:z0).Transformed(at);double u,v;frame.to_local({p.X(),p.Y(),p.Z()},u,v);pts.push_back({std::round(u*1e9)/1e9,std::round(v*1e9)/1e9});}
  std::sort(pts.begin(),pts.end());pts.erase(std::unique(pts.begin(),pts.end()),pts.end());
  auto cross=[](const std::array<double,2>& o,const std::array<double,2>& a,const std::array<double,2>& b){return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0]);};
  std::vector<std::array<double,2>> hull(2*pts.size());size_t k=0;  // the outline of the box as the sketch sees it (convex hull)
  for(size_t i=0;i<pts.size();++i){while(k>=2&&cross(hull[k-2],hull[k-1],pts[i])<=1e-12)--k;hull[k++]=pts[i];}
  for(size_t i=pts.size()-1,t=k+1;i-->0;){while(k>=t&&cross(hull[k-2],hull[k-1],pts[i])<=1e-12)--k;hull[k++]=pts[i];}
  hull.resize(k>1?k-1:k);
  if(hull.size()<3)throw Error("a projected KiCad part is seen edge-on from this sketch plane");
  auto world=[&](const std::array<double,2>& q){const auto p=frame.to_world(q[0],q[1]);return gp_Pnt(p[0],p[1],p[2]);};
  for(size_t i=0;i<hull.size();++i)builder.Add(out,BRepBuilderAPI_MakeEdge(world(hull[i]),world(hull[(i+1)%hull.size()])).Edge());
  return out;
}

bool asset_ref(const json& ref){return ref.is_object()&&ref.contains("asset");}
}  // namespace

Sketch derive_sketch(const Document& doc,const Scene& scene,const Frame& frame,const json& source,const std::string& mode,const std::map<std::string,TopoDS_Shape>& fresh) {
  ParamTable params;Ctx ctx{doc,params,scene,fresh,{}};TopoDS_Shape shape;
  if(source.contains("sketch")) {
    Frame plane;const auto sk=ctx.sketch(source.at("sketch").get<std::string>(),&plane);
    if(source.contains("entity")){const auto* e=sk.entity(source.at("entity").get<int>());if(!e)throw Error("source sketch curve no longer exists");shape=entity_edge(sk,*e,plane);}
    else {TopoDS_Compound compound;BRep_Builder b;b.MakeCompound(compound);for(const auto& edge:sketch_edges(sk,plane,true))b.Add(compound,edge);shape=compound;}
  } else if(source.contains("base") || source.contains("feature")) {
    const auto axis=ctx.axis(source);shape=BRepBuilderAPI_MakeEdge(axis.Location(),axis.Location().Translated(gp_Vec(axis.Direction())*100)).Edge();
  } else if(asset_ref(source)) shape=asset_source(ctx,frame,source);
  else shape=ctx.resolve(source).sub;
  if(shape.IsNull())throw Error("no source geometry to project");
  Sketch out;const bool construction=mode=="include";
  if(mode=="intersect") {
    BRepAlgoAPI_Section section(shape,frame_plane(frame),Standard_False);section.Approximation(Standard_True);section.Build();
    if(!section.IsDone())throw Error("body-plane intersection failed");shape=section.Shape();
  } else if(mode=="silhouette") {
    Handle(HLRBRep_Algo) algorithm=new HLRBRep_Algo();algorithm->Add(shape);algorithm->Projector(HLRAlgo_Projector(frame_ax3(frame).Ax2()));algorithm->Update();algorithm->Hide();
    HLRBRep_HLRToShape result(algorithm);const auto outline=result.OutLineVCompound();const auto sharp=result.VCompound();
    if(!outline.IsNull())append_sketch_shape(out,outline,{},construction,true);
    if(!sharp.IsNull())append_sketch_shape(out,sharp,{},construction,true);
    if(out.entities.empty())throw Error("no silhouette in this direction");return out;
  }
  if(shape.ShapeType()==TopAbs_VERTEX) {
    const auto p=BRep_Tool::Pnt(TopoDS::Vertex(shape));double x,y;frame.to_local({p.X(),p.Y(),p.Z()},x,y);
    SkEntity e;e.type=SkEntity::Type::Point;e.p={out.add_point(x,y,true)};e.fixed=true;e.construction=true;e.id=out.next_id();out.entities.push_back(e);
  } else append_sketch_shape(out,shape,frame,construction,true);
  if(out.entities.empty())throw Error("source has no curves in this projection");return out;
}

void append_reference(Sketch& destination,const Sketch& geometry,const json& source,const std::string& mode,bool linked) {
  std::map<int,int> points;
  for(const auto& p:geometry.points)points[p.id]=destination.add_point(p.x,p.y,linked);
  for(size_t i=0;i<geometry.entities.size();++i){auto e=geometry.entities[i];e.id=destination.next_id();e.fixed=linked;for(int& p:e.p)p=points.at(p);
    if(linked)e.source={{"ref",source},{"mode",mode},{"slot",i},{"count",geometry.entities.size()}};
    destination.entities.push_back(std::move(e));
  }
  destination.validate();
}

void refresh_references(Sketch& sketch,const std::function<Sketch(const json&,const std::string&)>& derive) {
  std::map<std::string,Sketch> cache;std::vector<std::pair<json,std::string>> again;std::set<std::string> redo;
  for(auto& e:sketch.entities)if(!e.source.is_null()) {
    const auto& source=e.source;const auto ref=source.at("ref");const auto mode=source.value("mode","project");const auto key=ref.dump()+"|"+mode;
    if(!cache.count(key))cache.emplace(key,derive(ref,mode));const auto& geometry=cache.at(key);const size_t slot=source.at("slot").get<size_t>();
    if(redo.count(key))continue;
    const bool same=geometry.entities.size()==source.at("count").get<size_t>()&&slot<geometry.entities.size()&&geometry.entities[slot].type==e.type&&geometry.entities[slot].p.size()==e.p.size();
    // A board's outline or a part changed shape in a sync (UI-134): its curves are projected again, the rest of the sketch kept.
    if(!same&&asset_ref(ref)){redo.insert(key);again.push_back({ref,mode});continue;}
    if(geometry.entities.size()!=source.at("count").get<size_t>()||slot>=geometry.entities.size())throw Error("projected source topology changed; break the link or project it again");
    const auto fresh=geometry.entities[slot];if(fresh.type!=e.type||fresh.p.size()!=e.p.size())throw Error("projected curve topology changed; break the link or project it again");
    for(size_t i=0;i<e.p.size();++i){auto* p=sketch.point(e.p[i]);const auto q=*geometry.point(fresh.p[i]);p->x=q.x;p->y=q.y;}
    e.r=fresh.r;e.degree=fresh.degree;e.periodic=fresh.periodic;e.knots=fresh.knots;e.multiplicities=fresh.multiplicities;e.weights=fresh.weights;
  }
  for(const auto& [ref,mode]:again) {
    std::vector<int> old;
    for(const auto& e:sketch.entities)if(!e.source.is_null()&&e.source.at("ref")==ref&&e.source.value("mode","project")==mode)old.push_back(e.id);
    break_reference(sketch,old);  // what the user drew onto those points keeps them, no longer fixed
    for(int id:old)sketch.remove(id);
    append_reference(sketch,cache.at(ref.dump()+"|"+mode),ref,mode,true);
  }
  sketch.validate();
}

void break_reference(Sketch& sketch,const std::vector<int>& entities) {
  std::set<int> released;
  for(int id:entities)if(auto* e=sketch.entity(id);e && !e->source.is_null()){e->source=nullptr;e->fixed=false;released.insert(e->p.begin(),e->p.end());}
  for(const auto& e:sketch.entities)if(e.fixed)for(int id:e.p)released.erase(id);
  for(int id:released)sketch.point(id)->fixed=false;
}
}
