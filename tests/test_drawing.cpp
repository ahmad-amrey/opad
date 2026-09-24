#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"
#include "opad/inspect.hpp"
#include <TopoDS_Shape.hxx>
#include <filesystem>

using namespace opad;
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
}
