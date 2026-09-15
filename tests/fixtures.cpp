// Generates the STEP fixtures used by the test-suite (and handy for manual testing):
//   box.step       a 40x30x20 mm block with a 8 mm through-hole (single body, named, coloured)
//   assembly.step  a base plate, a sub-assembly of four identical bolts (instances) and a lid
// usage: opad-fixtures <out-dir>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <Interface_Static.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_StepModelType.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

#include <cstdio>
#include <filesystem>
#include <string>

static bool write_step(const Handle(TDocStd_Document)& doc, const std::filesystem::path& out) {
  STEPCAFControl_Writer w;
  w.SetColorMode(Standard_True);
  w.SetNameMode(Standard_True);
  Interface_Static::SetCVal("write.step.schema", "AP214");
  if (!w.Transfer(doc, STEPControl_AsIs)) return false;
  return w.Write(out.string().c_str()) == IFSelect_RetDone;
}

static TopoDS_Shape make_block_with_hole() {
  TopoDS_Shape box = BRepPrimAPI_MakeBox(40, 30, 20).Shape();
  TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1)), 4, 22).Shape();
  return BRepAlgoAPI_Cut(box, hole).Shape();
}

static TopoDS_Shape make_bolt() {
  TopoDS_Shape head = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 4, 3).Shape();
  TopoDS_Shape shank = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -12), gp_Dir(0, 0, 1)), 2.5, 12).Shape();
  TopoDS_Compound c;
  BRep_Builder b;
  b.MakeCompound(c);
  b.Add(c, head);
  b.Add(c, shank);
  return c;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: opad-fixtures <out-dir>\n");
    return 2;
  }
  std::filesystem::path out = argv[1];
  std::filesystem::create_directories(out);

  {  // box.step
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    TDF_Label l = st->AddShape(make_block_with_hole(), Standard_False);
    TDataStd_Name::Set(l, "Block");
    ct->SetColor(l, Quantity_Color(0.2, 0.5, 0.8, Quantity_TOC_RGB), XCAFDoc_ColorSurf);
    if (!write_step(doc, out / "box.step")) return 1;
  }
  {  // assembly.step
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());

    TopoDS_Shape plate = BRepPrimAPI_MakeBox(100, 60, 5).Shape();
    TopoDS_Shape lid = BRepPrimAPI_MakeBox(100, 60, 3).Shape();
    TopoDS_Shape bolt = make_bolt();

    TDF_Label plate_l = st->NewShape();
    st->SetShape(plate_l, plate);
    TDataStd_Name::Set(plate_l, "Plate");
    ct->SetColor(plate_l, Quantity_Color(0.6, 0.6, 0.65, Quantity_TOC_RGB), XCAFDoc_ColorSurf);

    TDF_Label lid_l = st->NewShape();
    st->SetShape(lid_l, lid);
    TDataStd_Name::Set(lid_l, "Lid");
    ct->SetColor(lid_l, Quantity_Color(0.9, 0.3, 0.2, Quantity_TOC_RGB), XCAFDoc_ColorSurf);

    TDF_Label bolt_l = st->NewShape();
    st->SetShape(bolt_l, bolt);
    TDataStd_Name::Set(bolt_l, "Bolt M5");
    ct->SetColor(bolt_l, Quantity_Color(0.85, 0.75, 0.2, Quantity_TOC_RGB), XCAFDoc_ColorSurf);

    // Sub-assembly "Fasteners" with four bolt instances.
    TDF_Label fast_l = st->NewShape();
    TDataStd_Name::Set(fast_l, "Fasteners");
    const double xs[4] = {10, 90, 10, 90}, ys[4] = {10, 10, 50, 50};
    for (int i = 0; i < 4; ++i) {
      gp_Trsf t;
      t.SetTranslation(gp_Vec(xs[i], ys[i], 5 + 3));
      TDF_Label c = st->AddComponent(fast_l, bolt_l, TopLoc_Location(t));
      TDataStd_Name::Set(c, ("Bolt " + std::to_string(i + 1)).c_str());
    }

    // Top-level assembly.
    TDF_Label asm_l = st->NewShape();
    TDataStd_Name::Set(asm_l, "Fixture");
    TDataStd_Name::Set(st->AddComponent(asm_l, plate_l, TopLoc_Location()), "Plate");
    {
      gp_Trsf t;
      t.SetTranslation(gp_Vec(0, 0, 5));
      TDF_Label c = st->AddComponent(asm_l, lid_l, TopLoc_Location(t));
      TDataStd_Name::Set(c, "Lid");
    }
    TDataStd_Name::Set(st->AddComponent(asm_l, fast_l, TopLoc_Location()), "Fasteners");
    st->UpdateAssemblies();
    if (!write_step(doc, out / "assembly.step")) return 1;
  }
  std::printf("%s\n", out.string().c_str());
  return 0;
}
