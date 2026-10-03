#include <BRepAdaptor_Curve.hxx>
#include "opad/design/drawing_sketch.hpp"
#include <chrono>
#include <cstdlib>
#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"
#include "opad/inspect.hpp"
#include "opad/commands.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include <TopoDS_Shape.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <filesystem>

using namespace opad;
TEST(dxf_native_rational_spline_and_invalid_data) {
  const auto file=std::filesystem::temp_directory_path()/(new_uuid()+".dxf");
  const std::string header="0\nSECTION\n2\nENTITIES\n0\nSPLINE\n70\n12\n71\n2\n72\n6\n73\n3\n40\n0\n40\n0\n40\n0\n40\n1\n40\n1\n40\n1\n";
  const std::string poles="10\n1\n20\n0\n30\n0\n10\n1\n20\n1\n30\n0\n10\n0\n20\n1\n30\n0\n";
  const std::string footer="0\nENDSEC\n0\nEOF\n";
  write_text_file(file,header+"41\n1\n41\n0.7071067811865476\n41\n1\n"+poles+footer);
  auto doc=Document::create();import_file(doc,file);const auto scene=resolve(doc);
  const auto shape=node_world_shape(doc,scene,scene.all_bodies().front());TopExp_Explorer edges(shape,TopAbs_EDGE);
  CHECK(edges.More());BRepAdaptor_Curve curve(TopoDS::Edge(edges.Current()));CHECK(curve.GetType()==GeomAbs_BSplineCurve);
  const auto middle=curve.Value((curve.FirstParameter()+curve.LastParameter())*.5);
  CHECK_NEAR(middle.X(),std::sqrt(.5),1e-10);CHECK_NEAR(middle.Y(),std::sqrt(.5),1e-10);
  auto invalid=[&](const std::string& data){write_text_file(file,data);auto bad=Document::create();CHECK_THROWS(import_file(bad,file));};
  invalid(header+"41\n1\n"+poles+footer);
  {write_text_file(file,header+poles+"30\n4\n"+footer);auto lifted=Document::create();CHECK(import_file(lifted,file).bodies==1);}  // off the XY plane: projected onto it
  invalid(header+"40\n-1\n"+poles+footer);
  std::filesystem::remove(file);
}
struct Files {
  std::filesystem::path dir=std::filesystem::temp_directory_path()/new_uuid();
  Files(){std::filesystem::create_directory(dir);}
  ~Files(){std::error_code e;std::filesystem::remove_all(dir,e);}
};
TEST(dxf_layers_circles_and_roundtrip) {
  Files f; auto file=f.dir/"drawing.dxf";
  write_text_file(file,"0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nOutline\n10\n0\n20\n0\n11\n20\n21\n10\n0\nCIRCLE\n8\nHoles\n10\n5\n20\n5\n40\n2\n0\nENDSEC\n0\nEOF\n");
  auto d=Document::create();import_file(d,file);auto s=resolve(d);
  CHECK_EQ(s.all_bodies().size(),2u);CHECK_EQ(s.node(s.roots[0])->children.size(),2u);
  for(auto id:s.all_bodies())CHECK_EQ(s.node(id)->representation,"drawing2d");
  d.save_as(f.dir/"drawing.opad");auto loaded=Document::load(f.dir/"drawing.opad");
  ExportOptions options;options.format="dxf";export_drawing(loaded,resolve(loaded),f.dir/"out.dxf",options);
  CHECK(read_text_file(f.dir/"out.dxf").find("CIRCLE")!=std::string::npos);
  auto round=Document::create();import_file(round,f.dir/"out.dxf");CHECK_EQ(resolve(round).all_bodies().size(),2u);
  options.format="svg";export_drawing(loaded,resolve(loaded),f.dir/"out.svg",options);
  round=Document::create();import_file(round,f.dir/"out.svg");CHECK_EQ(resolve(round).all_bodies().size(),2u);
}
TEST(dxf_single_vertex_polyline_survives_sketch_conversion) {
  Files f;const auto file=f.dir/"dots.dxf";
  write_text_file(file,"0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n90\n1\n70\n1\n10\n12\n20\n34\n0\nENDSEC\n0\nEOF\n");
  auto d=Document::create();import_file(d,file);const auto s=resolve(d);
  auto sk=design::drawing_sketch(d,s,{{s.all_bodies().front(),false}},Frame{},.01);
  CHECK_EQ(sk.entities.size(),1u);CHECK(sk.entities[0].type==design::SkEntity::Type::Point);
  CHECK_NEAR(sk.point(sk.entities[0].p[0])->x,12,1e-9);CHECK_NEAR(sk.point(sk.entities[0].p[0])->y,34,1e-9);
}
TEST(svg_beziers_and_physical_units) {
  Files f;write_text_file(f.dir/"test.svg","<svg width=\"25.4mm\" viewBox=\"0 0 96 96\"><g id=\"Curve\"><path d=\"M0,0 L96,0 C96,20 50,40 0,0 Z\"/></g></svg>");
  auto d=Document::create();import_file(d,f.dir/"test.svg");auto s=resolve(d);
  auto box=node_world_bbox(d,s,s.all_bodies()[0]);double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
  CHECK_NEAR(x1-x0,25.4,1e-4);
}
TEST(mesh_is_persistent_and_view_only) {
  Files f;write_text_file(f.dir/"mesh.obj","v 0 0 0\nv 10 0 0\nv 0 10 0\nf 1 2 3\n");
  auto d=Document::create();import_file(d,f.dir/"mesh.obj");auto saved=Document::parse(d.serialize());auto s=resolve(saved);
  CHECK_EQ(s.node(s.all_bodies()[0])->representation,"mesh");
  CHECK_EQ(tessellate(node_world_shape(saved,s,s.all_bodies()[0]),0.1).triangle_count(),1u);
  ExportOptions options; options.format="step";
  CHECK_THROWS(export_selection(saved,s,f.dir/"mesh.step",options));
  options.format="dxf";
  CHECK_THROWS(export_drawing(saved,s,f.dir/"mesh.dxf",options));
}
TEST(mesh_facets_edges_vertices_are_measurable_after_roundtrip) {
  Files f; write_text_file(f.dir/"mesh.obj","v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\nf 1 2 3\nf 1 2 4\n");
  auto d=Document::create(); import_file(d,f.dir/"mesh.obj"); d=Document::parse(d.serialize()); const auto scene=resolve(d);
  const auto id=scene.all_bodies()[0]; const auto shape=node_world_shape(d,scene,id);
  CHECK_EQ(subshape_count(shape,Ref::Kind::Face),2);
  CHECK_NEAR(node_properties(d,scene,id)["area"].get<double>(),100,1e-7);
  CHECK_EQ(subshape_count(shape,Ref::Kind::Edge),6);
  CHECK_EQ(subshape_count(shape,Ref::Kind::Vertex),4);
  Ref a; a.body=id; a.kind=Ref::Kind::Vertex; a.index=0;
  Ref b=a; b.index=1;
  CHECK_NEAR(measure_distance(d,scene,a,b)["value"].get<double>(),10,1e-7);
  a.kind=Ref::Kind::Face; b.kind=Ref::Kind::Face;
  CHECK_NEAR(inspect_ref(d,scene,a)["area"].get<double>(),50,1e-7);
  CHECK_NEAR(measure_angle(d,scene,a,b)["value"].get<double>(),90,1e-7);
  a.kind=Ref::Kind::Edge;
  CHECK_NEAR(inspect_ref(d,scene,a)["length"].get<double>(),10,1e-7);
  CHECK_THROWS(subshape(shape,Ref::Kind::Face,2));
}

TEST(svg_arc_and_nested_transform) {
  Files f;write_text_file(f.dir/"arc.svg","<svg width=\"100mm\" viewBox=\"0 0 100 100\"><g transform=\"translate(10,20)\"><path d=\"M0 0 A10 10 0 0 1 20 0\"/></g></svg>");
  auto d=Document::create();import_file(d,f.dir/"arc.svg");auto s=resolve(d);
  double x0,y0,z0,x1,y1,z1;node_world_bbox(d,s,s.all_bodies()[0]).Get(x0,y0,z0,x1,y1,z1);
  CHECK_NEAR(x0,10,1e-4);CHECK_NEAR(x1,30,1e-4);CHECK_NEAR(y1-y0,10,1e-4);
}
TEST(svg_editor_metadata_references_shapes_and_text) {
  Files f;
  write_text_file(f.dir/"editor.svg",R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:sodipodi="editor" width="100mm" viewBox="0 0 100 100">
    <sodipodi:namedview><sodipodi:guide/></sodipodi:namedview>
    <defs><g id="part"><ellipse cx="5" cy="5" rx="4" ry="2"/><rect x="0" y="0" width="10" height="10" rx="2"/></g></defs>
    <g id="Layer"><use href="#part" x="20" y="10"/><text x="5" y="40" font-size="8">CAD</text></g>
  </svg>)svg");
  auto d=Document::create(); const auto result=import_file(d,f.dir/"editor.svg"); const auto scene=resolve(d);
  CHECK(result.warnings.empty()); CHECK_EQ(scene.all_bodies().size(),2u);
  int edges=0; for(const auto& id:scene.all_bodies()) edges+=subshape_count(node_world_shape(d,scene,id),Ref::Kind::Edge);
  CHECK(edges>9);
}
TEST(svg_embedded_image_survives_opad_and_svg_roundtrip) {
  Files f;
  const std::string data="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jU1sAAAAASUVORK5CYII=";
  write_text_file(f.dir/"image.svg","<svg width=\"100mm\" viewBox=\"0 0 100 100\"><image x=\"10\" y=\"20\" width=\"30\" height=\"40\" href=\""+data+"\"/></svg>");
  auto d=Document::create(); import_file(d,f.dir/"image.svg"); d=Document::parse(d.serialize());
  auto scene=resolve(d); auto id=scene.all_bodies()[0]; CHECK_EQ(scene.node(id)->raster["href"],data);
  auto box=node_world_bbox(d,scene,id); CHECK_NEAR(box.CornerMin().X(),10,1e-5); CHECK_NEAR(box.CornerMin().Y(),-60,1e-5);
  ExportOptions options; options.format="svg"; export_drawing(d,scene,f.dir/"out.svg",options);
  auto round=Document::create(); import_file(round,f.dir/"out.svg"); scene=resolve(round); id=scene.all_bodies()[0];
  CHECK_EQ(scene.node(id)->raster["href"],data);
  box=node_world_bbox(round,scene,id); CHECK_NEAR(box.CornerMin().X(),10,1e-5); CHECK_NEAR(box.CornerMin().Y(),-60,1e-5);
}
TEST(svg_unsupported_effects_are_reported_and_source_preserved) {
  Files f; const std::string source="<svg><filter id=\"effect\"/><rect width=\"10\" height=\"10\"/></svg>";
  write_text_file(f.dir/"effect.svg",source); auto d=Document::create(); const auto result=import_file(d,f.dir/"effect.svg");
  CHECK(!result.warnings.empty()); CHECK(d.serialize().find("svg_source")!=std::string::npos);
  CHECK_EQ(resolve(d).all_bodies().size(),1u);
  const auto before=d.serialize();
  write_text_file(f.dir/"cycle.svg","<svg><defs><g id=\"a\"><use href=\"#a\"/></g></defs><use href=\"#a\"/></svg>");
  CHECK_THROWS(import_file(d,f.dir/"cycle.svg")); CHECK_EQ(d.serialize(),before);
}

TEST(bad_drawings_leave_document_unchanged) {
  Files f;auto d=Document::create();auto before=d.serialize();
  write_text_file(f.dir/"bad.dxf","0\nSECTION\n2\nENTITIES\n0\nLINE\n10\nnan\n0\nENDSEC\n0\nEOF\n");
  CHECK_THROWS(import_file(d,f.dir/"bad.dxf"));CHECK_EQ(d.serialize(),before);
  write_text_file(f.dir/"bad.svg","<svg><path d=\"M0 0 L\"/></svg>");CHECK_THROWS(import_file(d,f.dir/"bad.svg"));
  write_text_file(f.dir/"bad.obj","v 0 0 0\nf 1 2 3\n");CHECK_THROWS(import_file(d,f.dir/"bad.obj"));
  CHECK_EQ(d.serialize(),before);
}
CHECK_MAIN()


TEST(mesh_circle_centers_and_segments_survive_roundtrip) {
  Files f; std::ostringstream obj;
  for(int z=0;z<2;++z) for(int i=0;i<24;++i) obj << "v " << 10*cos(2*M_PI*i/24) << " " << 10*sin(2*M_PI*i/24) << " " << z*20 << "\n";
  for(int i=0;i<24;++i) { const int a=i+1,b=(i+1)%24+1; obj<<"f "<<a<<" "<<b<<" "<<b+24<<"\nf "<<a<<" "<<b+24<<" "<<a+24<<"\n"; }
  write_text_file(f.dir/"tube.obj",obj.str()); auto d=Document::create(); import_file(d,f.dir/"tube.obj");
  d=Document::parse(d.serialize()); const auto scene=resolve(d); const auto id=scene.all_bodies()[0];
  const auto rings=mesh_circles(node_world_shape(d,scene,id)); CHECK_EQ(rings.size(),2u);
  Ref a; a.body=id; a.kind=Ref::Kind::Center; a.index=rings[0].index; Ref b=a; b.index=rings[1].index;
  CHECK_NEAR(measure_distance(d,scene,a,b)["value"].get<double>(),20,1e-4);
  CHECK_NEAR(inspect_ref(d,scene,a)["diameter"].get<double>(),20,1e-4);
  CHECK_EQ(inspect_ref(d,scene,a)["segments"].get<int>(),24);
  a.kind=Ref::Kind::Edge;
  CHECK_NEAR(measure_radius(d,scene,a)["value"].get<double>(),10,1e-4);
  ExportOptions mesh;mesh.format="stl";mesh.select={id};mesh.per_body=false;export_selection(d,scene,f.dir/"tube.stl",mesh);
  auto stl=Document::create();import_file(stl,f.dir/"tube.stl");const auto ss=resolve(stl);CHECK_EQ(mesh_circles(node_world_shape(stl,ss,ss.all_bodies()[0])).size(),2u);
  std::ostringstream ellipse;
  for(int z=0;z<2;++z) for(int i=0;i<24;++i) ellipse<<"v "<<12*cos(2*M_PI*i/24)<<" "<<10*sin(2*M_PI*i/24)<<" "<<z*20<<"\n";
  for(int i=0;i<24;++i) {int a=i+1,b=(i+1)%24+1;ellipse<<"f "<<a<<" "<<b<<" "<<b+24<<"\nf "<<a<<" "<<b+24<<" "<<a+24<<"\n";}
  write_text_file(f.dir/"ellipse.obj",ellipse.str());auto e=Document::create();import_file(e,f.dir/"ellipse.obj");auto es=resolve(e);CHECK(mesh_circles(node_world_shape(e,es,es.all_bodies()[0])).empty());

}

TEST(drawing_layers_convert_to_editable_extrudable_sketch) {
  Files f;write_text_file(f.dir/"layers.svg",R"(<svg width="40mm" viewBox="0 0 40 40"><g id="Outline"><rect width="20" height="10"/></g><g id="Guide"><circle cx="5" cy="5" r="2"/></g><g id="Excluded"><circle cx="30" cy="30" r="1"/></g></svg>)");
  auto d=Document::create();import_file(d,f.dir/"layers.svg");auto scene=resolve(d);json layers=json::array();
  for(auto id:scene.all_bodies()) { const auto name=scene.node(id)->name; if(name!="Excluded") layers.push_back({{"id",id},{"construction",name=="Guide"}}); }
  commands::run("drawing_to_sketch",{{"layers",layers},{"plane","xy"},{"name","Editable"}},&d);
  scene=resolve(d); CHECK_EQ(scene.sketches.size(),1u);const auto id=scene.sketches[0].id;
  const auto sk=design::Sketch::from_json(scene.sketches[0].geometry); CHECK_EQ(sk.entities.size(),5u);
  int guides=0; for(const auto& e:sk.entities) guides+=e.construction; CHECK_EQ(guides,1);
  const auto regions=design::sketch_regions(sk,scene.sketches[0].frame); CHECK_EQ(regions.size(),1u);CHECK_NEAR(regions[0].area,200,1e-6);
  Ref edge; edge.body=id;edge.kind=Ref::Kind::Edge;edge.index=0;CHECK(inspect_ref(d,scene,edge).contains("curve"));
  Ref center=edge; for(int i=0;i<5;++i) { edge.index=i; if(inspect_ref(d,scene,edge).contains("diameter")) center.index=i; } center.kind=Ref::Kind::Center; CHECK_NEAR(inspect_ref(d,scene,center)["diameter"].get<double>(),4,1e-6);
  commands::run("feature",{{"kind","extrude"},{"inputs",{{"profiles",json::array({json{{"sketch",id},{"at",{10,-5}}}})},{"distance",5}}}},&d);
  scene=resolve(d);CHECK_EQ(scene.features.size(),1u);CHECK(scene.features[0].error.empty());
  auto saved=Document::parse(d.serialize());CHECK_EQ(resolve(saved).sketches[0].geometry["entities"].size(),5u);
  CHECK_THROWS(commands::run("drawing_to_sketch",{{"layers",json::array()}},&d));
}

TEST(individual_sketch_exports_in_its_own_plane_even_when_hidden) {
  Files f; auto d=Document::create(); design::Sketch sk; sk.add_circle(sk.add_point(12,8),3);
  commands::run("sketch",{{"name","Vertical circle"},{"plane",{{"base","yz"}}},{"geometry",sk.to_json()}},&d);
  auto scene=resolve(d);const auto id=scene.sketches[0].id; commands::run("appearance",{{"target",id},{"visible",false}},&d);scene=resolve(d);
  for(const auto* format:{"svg","dxf"}) {
    ExportOptions o;o.format=format;o.select={id};const auto path=f.dir/(std::string("sketch.")+format);
    CHECK_EQ(export_drawing(d,scene,path,o).bodies,1);
    auto round=Document::create();import_file(round,path);const auto rs=resolve(round);Ref edge;edge.body=rs.all_bodies()[0];edge.kind=Ref::Kind::Edge;edge.index=0;
    const auto info=inspect_ref(round,rs,edge);CHECK_NEAR(info["diameter"].get<double>(),6,1e-6);CHECK_NEAR(info["center"][0].get<double>(),12,1e-6);CHECK_NEAR(info["center"][1].get<double>(),8,1e-6);
  }
  ExportOptions wrong;wrong.select={id};CHECK_THROWS(export_selection(d,scene,f.dir/"bad.step",wrong));
}
TEST(individual_solids_and_meshes_export_without_their_neighbors) {
  Files f;auto d=Document::create();Scene scene;
  cache_shape(d,"box",BRepPrimAPI_MakeBox(10,20,30).Shape());
  Node n;n.id="one";n.name="Box";n.kind=Node::Kind::Body;n.body_key="box";scene.nodes[n.id]=n;scene.roots.push_back(n.id);
  n.id="neighbor";n.local=Mat4::translation(100,0,0);scene.nodes[n.id]=n;scene.roots.push_back(n.id);
  for(const auto* format:{"step","obj","stl"}) {
    ExportOptions o;o.format=format;o.select={"one"};o.per_body=false;
    const auto path=f.dir/(std::string("one.")+format);CHECK_EQ(export_selection(d,scene,path,o).bodies,1);
    auto round=Document::create();import_file(round,path);const auto rs=resolve(round);CHECK_EQ(rs.all_bodies().size(),1u);
    auto box=node_world_bbox(round,rs,rs.all_bodies()[0]);CHECK(box.CornerMax().X()<11);
    if(std::string(format)!="step") { ExportOptions mesh;mesh.format=format;mesh.select={rs.all_bodies()[0]};mesh.per_body=false;CHECK_EQ(export_selection(round,rs,f.dir/(std::string("mesh.")+format),mesh).bodies,1); }
  }
}

TEST(optional_drawing_conversion_benchmark) {
  const char* file=std::getenv("OPAD_DRAWING_BENCH"); if (!file) return;
  auto d=Document::create(); import_file(d,file); const auto scene=resolve(d);
  std::vector<design::DrawingLayer> layers;
  for (const auto& id:scene.all_bodies()) if(scene.node(id)->raster.is_null()) layers.push_back({id,false});
  auto start=std::chrono::steady_clock::now();
  auto sk=design::drawing_sketch(d,scene,layers,Frame{},0.01);
  auto ms=[&] {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();};
  std::cerr<<"conversion "<<ms()<<" ms; "<<sk.entities.size()<<" entities, "<<sk.points.size()<<" points"<<std::endl;
  start=std::chrono::steady_clock::now(); auto regions=design::sketch_regions(sk,Frame{});
  std::cerr<<"profiles "<<ms()<<" ms; "<<regions.size()<<" regions"<<std::endl;
  CHECK(!regions.empty());
}

TEST(native_bezier_conversion_is_exact_after_transform_and_roundtrip) {
  Files f;write_text_file(f.dir/"bezier.svg",R"SVG(<svg width="100mm" viewBox="0 0 100 100"><path transform="translate(7,9)" d="M0 0 C0 20 30 20 30 0 L0 0 Z"/></svg>)SVG");
  auto d=Document::create();import_file(d,f.dir/"bezier.svg");const auto scene=resolve(d);
  auto sk=design::drawing_sketch(d,scene,{{scene.all_bodies()[0],false}},Frame{},0.01);
  CHECK_EQ(sk.entities.size(),2u);sk=design::Sketch::from_json(sk.to_json());
  int curves=0;for(const auto& e:sk.entities)if(e.type==design::SkEntity::Type::Spline) {
    ++curves;CHECK_EQ(e.degree,3);CHECK_EQ(e.p.size(),4u);
    auto edge=design::entity_edge(sk,e,Frame{});CHECK(!edge.IsNull());
    BRepAdaptor_Curve c(edge);const auto mid=c.Value((c.FirstParameter()+c.LastParameter())/2);
    CHECK_NEAR(mid.X(),22,1e-6);CHECK_NEAR(mid.Y(),-24,1e-6);
  }
  CHECK_EQ(curves,1);CHECK_EQ(design::sketch_regions(sk,Frame{}).size(),1u);
}
TEST(segmented_circle_reconstruction_preserves_sharp_rectangle) {
  Files f;std::ostringstream svg;svg<<"<svg width='50mm' viewBox='0 0 50 50'><path d='M ";
  for(int i=0;i<=128;++i) {if(i)svg<<" L ";svg<<20+10*cos(i*2*M_PI/128)<<" "<<20+10*sin(i*2*M_PI/128);}
  svg<<" Z M35 35 L45 35 L45 45 L35 45 Z'/></svg>";write_text_file(f.dir/"segments.svg",svg.str());
  auto d=Document::create();import_file(d,f.dir/"segments.svg");const auto scene=resolve(d);
  auto sk=design::drawing_sketch(d,scene,{{scene.all_bodies()[0],false}},Frame{},0.01);
  int circles=0,lines=0;for(const auto& e:sk.entities) {circles+=e.type==design::SkEntity::Type::Circle;lines+=e.type==design::SkEntity::Type::Line;}
  CHECK_EQ(circles,1);CHECK_EQ(lines,4);CHECK_EQ(design::sketch_regions(sk,Frame{}).size(),2u);
}

// TODO 10 A12: a drawing keeps its own plane and origin. Where the import puts it is one placement in the import op
// (centred when opened on its own, on a plane or face when imported), and a sketch converted from it uses exactly
// that frame: its coordinates are the drawing's.
TEST(drawing_placement_is_kept_by_its_sketch) {
  Files f;
  const auto file = f.dir / "plate.dxf";
  write_text_file(file, "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nOutline\n10\n100\n20\n50\n11\n140\n21\n50\n0\nLINE\n8\nOutline\n10\n140\n20\n50\n11\n140\n21\n80\n0\nLINE\n8\nOutline\n10\n140\n20\n80\n11\n100\n21\n80\n0\nLINE\n8\nOutline\n10\n100\n20\n80\n11\n100\n21\n50\n0\nENDSEC\n0\nEOF\n");
  auto root_transform = [](const Document& d) {
    for (auto it = d.ops.rbegin(); it != d.ops.rend(); ++it)
      if (it->type == "import") return Mat4::from_json(it->data["nodes"][0].value("transform", Mat4().to_json()));
    return Mat4();
  };
  // Opened on its own: centred on the grid (its box centre at the world origin).
  auto opened = Document::create();
  commands::run("import", {{"file", file.string()}, {"center", true}}, &opened);
  const Mat4 centred = root_transform(opened);
  CHECK_NEAR(centred.apply({120, 65, 0})[0], 0, 1e-12);
  CHECK_NEAR(centred.apply({120, 65, 0})[1], 0, 1e-12);
  // Imported onto the XZ plane with an offset: drawing XY -> world XZ.
  auto placed = Document::create();
  Mat4 offset = Mat4::translation(7, 3, 0);
  commands::run("import", {{"file", file.string()}, {"plane", {{"base", "xz"}}}, {"placement", offset.to_json()}}, &placed);
  const Scene scene = resolve(placed);
  std::vector<design::DrawingLayer> layers;
  for (const auto& id : scene.all_bodies()) layers.push_back({id, false});
  const Frame frame = design::drawing_frame(scene, layers);
  const Frame xz = design::base_frame("xz");
  for (int i = 0; i < 3; ++i) {
    CHECK_NEAR(frame.x[i], xz.x[i], 1e-12);
    CHECK_NEAR(frame.y[i], xz.y[i], 1e-12);
  }
  const Vec3 origin = xz.to_world(7, 3);
  for (int i = 0; i < 3; ++i) CHECK_NEAR(frame.origin[i], origin[i], 1e-12);
  // Converted without naming a plane: the sketch sits in that frame, with the drawing's own coordinates.
  json layer_args = json::array();
  for (const auto& l : layers) layer_args.push_back({{"id", l.id}});
  commands::run("drawing_to_sketch", {{"layers", layer_args}}, &placed);
  const Scene after = resolve(placed);
  const auto& sketch = after.sketches.back();
  CHECK(sketch.frame.to_json() == frame.to_json());
  bool corner = false;
  for (const auto& p : sketch.geometry.at("points")) corner |= std::abs(p.at("x").get<double>() - 140) < 1e-9 && std::abs(p.at("y").get<double>() - 80) < 1e-9;
  CHECK(corner);
  // Layers of two drawings placed apart do not share a sketch.
  commands::run("import", {{"file", file.string()}, {"center", true}}, &placed);
  std::vector<design::DrawingLayer> both;
  for (const auto& id : resolve(placed).all_bodies()) both.push_back({id, false});
  CHECK_THROWS(design::drawing_frame(resolve(placed), both));
}
