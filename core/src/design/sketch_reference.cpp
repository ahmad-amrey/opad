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
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <set>

namespace opad::design {
Sketch derive_sketch(const Document& doc,const Scene& scene,const Frame& frame,const json& source,const std::string& mode,const std::map<std::string,TopoDS_Shape>& fresh) {
  ParamTable params;Ctx ctx{doc,params,scene,fresh,{}};TopoDS_Shape shape;
  if(source.contains("sketch")) {
    Frame plane;const auto sk=ctx.sketch(source.at("sketch").get<std::string>(),&plane);
    if(source.contains("entity")){const auto* e=sk.entity(source.at("entity").get<int>());if(!e)throw Error("source sketch curve no longer exists");shape=entity_edge(sk,*e,plane);}
    else {TopoDS_Compound compound;BRep_Builder b;b.MakeCompound(compound);for(const auto& edge:sketch_edges(sk,plane,true))b.Add(compound,edge);shape=compound;}
  } else if(source.contains("base") || source.contains("feature")) {
    const auto axis=ctx.axis(source);shape=BRepBuilderAPI_MakeEdge(axis.Location(),axis.Location().Translated(gp_Vec(axis.Direction())*100)).Edge();
  } else shape=ctx.resolve(source).sub;
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
  std::map<std::string,Sketch> cache;
  for(auto& e:sketch.entities)if(!e.source.is_null()) {
    const auto& source=e.source;const auto ref=source.at("ref");const auto mode=source.value("mode","project");const auto key=ref.dump()+"|"+mode;
    if(!cache.count(key))cache.emplace(key,derive(ref,mode));const auto& geometry=cache.at(key);const size_t slot=source.at("slot").get<size_t>();
    if(geometry.entities.size()!=source.at("count").get<size_t>()||slot>=geometry.entities.size())throw Error("projected source topology changed; break the link or project it again");
    const auto fresh=geometry.entities[slot];if(fresh.type!=e.type||fresh.p.size()!=e.p.size())throw Error("projected curve topology changed; break the link or project it again");
    for(size_t i=0;i<e.p.size();++i){auto* p=sketch.point(e.p[i]);const auto q=*geometry.point(fresh.p[i]);p->x=q.x;p->y=q.y;}
    e.r=fresh.r;e.degree=fresh.degree;e.periodic=fresh.periodic;e.knots=fresh.knots;e.multiplicities=fresh.multiplicities;e.weights=fresh.weights;
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
