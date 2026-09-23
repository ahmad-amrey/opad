#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"
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
TEST(svg_arc_and_nested_transform) {
  Files f;write_text_file(f.dir/"arc.svg","<svg width=\"100mm\" viewBox=\"0 0 100 100\"><g transform=\"translate(10,20)\"><path d=\"M0 0 A10 10 0 0 1 20 0\"/></g></svg>");
  auto d=Document::create();import_file(d,f.dir/"arc.svg");auto s=resolve(d);
  double x0,y0,z0,x1,y1,z1;node_world_bbox(d,s,s.all_bodies()[0]).Get(x0,y0,z0,x1,y1,z1);
  CHECK_NEAR(x0,10,1e-4);CHECK_NEAR(x1,30,1e-4);CHECK_NEAR(y1-y0,10,1e-4);
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

