#include "CurveSamples.hpp"
#include "opad/design/sketch_edit.hpp"
#include "opad/design/sketch_modify.hpp"
#include "Highlight.hpp"
#include "SketchEditor.hpp"
#include "SketchGeometryCache.hpp"
#include "SketchSnap.hpp"
#include "DimensionHandle.hpp"
#include "CommandHelp.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <TColStd_HArray1OfByte.hxx>
#include <Graphic3d_AspectText3d.hxx>
#include <Graphic3d_Text.hxx>
#include <Poly_Triangulation.hxx>
#include <Prs3d_Presentation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <QKeyEvent>
#include <QApplication>
#include <QSettings>
#include <QPointer>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QCursor>
#include <cmath>

#include "I18n.hpp"
#include "Preferences.hpp"
#include "Units.hpp"
#include "Jobs.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_text.hpp"

using namespace opad::design;

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
}  // namespace

// ---------------------------------------------------------------- the overlay object
// Everything the editor shows, as plain arrays it fills before each redisplay: never pickable (the editor
// hit-tests in sketch coordinates itself), drawn in the Topmost layer.
struct SketchDrawing {
  struct Seg { opad::Vec3 a, b; QColor c; bool operator==(const Seg&) const = default; };
  struct Pt { opad::Vec3 p; QColor c; bool operator==(const Pt&) const = default; };
  struct Txt { opad::Vec3 p; QString s; QColor c; bool left = false; bool operator==(const Txt&) const = default; };  // left: starts at p (labels beside the cursor)
  std::vector<Seg> solid, dashed, thin, marks;  // marks: snap markers and constraint pictograms (SnapMarkers.hpp), 1.5 px
  std::vector<Seg> locked;                      // the line a Shift lock holds the pointer to: thick dashed, 3 px
  std::vector<Seg> cursor, cursorHalo;          // the drawing cursor (grid snapping): 1 px on a 3 px halo, over everything
  std::vector<Pt> points, bigPoints, rings;  // rings: points that can still move (a shape besides the colour, UI-124)
  std::vector<Pt> dots;  // coincidences (UI-24): a filled dot on the point, over its ring
  // A role's halo under the lines and points it marks (UI-38): the selection's (dimmed selected3d) in the sketch's own
  // presentation, the hover's white glow in the transient one. One colour each.
  std::vector<Seg> glow;
  std::vector<Pt> glowPoints;
  QColor glowColor;
  float glowAlpha = 1;  // opaque: drawn first, under the lines
  QColor rimColor;      // valid: a darker rim under the glow (the hover's on a light background, highlight::hoverRim)
  std::vector<Txt> texts;
  std::vector<opad::Vec3> fill;
  std::vector<opad::Vec3> badges;  // the constraint badges' backs (UI-24): opaque triangles, over the curves, under the marks
  QColor fillColor, textBack, badgeColor;
  float fillAlpha = 0.18f;
  // Line widths, marker sizes and text heights are device pixels: times the display scale they read the same at
  // 100 % and 150 % (at 1.0 they were tiny on a 4K screen).
  double scale = 1.0;
  std::string font;
  bool operator==(const SketchDrawing&) const = default;  // the same picture: no redisplay (a move within a grid cell)
};

class SketchPrs : public AIS_InteractiveObject, public SketchDrawing {
  DEFINE_STANDARD_RTTI_INLINE(SketchPrs, AIS_InteractiveObject)
 public:
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    if (!fill.empty()) {
      Handle(Graphic3d_ArrayOfTriangles) tri = new Graphic3d_ArrayOfTriangles(static_cast<int>(fill.size()));
      for (const auto& p : fill) tri->AddVertex(gp_Pnt(p[0], p[1], p[2]));
      Handle(Graphic3d_AspectFillArea3d) a = new Graphic3d_AspectFillArea3d();
      a->SetInteriorStyle(Aspect_IS_SOLID);
      a->SetInteriorColor(Quantity_ColorRGBA(occ(fillColor), fillAlpha));
      a->SetAlphaMode(Graphic3d_AlphaMode_Blend);
      a->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
      a->SetSuppressBackFaces(false);
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(a);
      g->AddPrimitiveArray(tri);
    }
    auto lines = [&](const std::vector<Seg>& segs, Aspect_TypeOfLine type, double width) {
      if (segs.empty()) return;
      Handle(Graphic3d_ArrayOfSegments) arr = new Graphic3d_ArrayOfSegments(static_cast<int>(segs.size()) * 2, 0, Standard_True);
      for (const auto& s : segs) {
        arr->AddVertex(gp_Pnt(s.a[0], s.a[1], s.a[2]), occ(s.c));
        arr->AddVertex(gp_Pnt(s.b[0], s.b[1], s.b[2]), occ(s.c));
      }
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(occ(segs.front().c), type, width));
      g->AddPrimitiveArray(arr);
    };
    auto halo = [&](const Handle(Graphic3d_Aspects)& aspect, const Handle(Graphic3d_ArrayOfPrimitives)& array) {
      aspect->SetInteriorColor(Quantity_ColorRGBA(occ(glowColor), glowAlpha));
      if (glowAlpha < 1) aspect->SetAlphaMode(Graphic3d_AlphaMode_Blend);
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(aspect);
      g->AddPrimitiveArray(array);
    };
    // With a rim the glow stays inside it: lines are drawn no wider than about 7 device pixels (kHoverRimWidth).
    const bool rim = rimColor.isValid();
    if (!glow.empty()) {
      Handle(Graphic3d_ArrayOfSegments) arr = new Graphic3d_ArrayOfSegments(static_cast<int>(glow.size()) * 2);
      for (const auto& s : glow) {
        arr->AddVertex(gp_Pnt(s.a[0], s.a[1], s.a[2]));
        arr->AddVertex(gp_Pnt(s.b[0], s.b[1], s.b[2]));
      }
      if (rim) {
        Handle(Graphic3d_Group) g = prs->NewGroup();
        g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(occ(rimColor), Aspect_TOL_SOLID, highlight::kHoverRimWidth));
        g->AddPrimitiveArray(arr);
      }
      halo(new Graphic3d_AspectLine3d(occ(glowColor), Aspect_TOL_SOLID, rim ? highlight::kHoverRimWidth - 2 : 8.0 * scale), arr);
    }
    if (!glowPoints.empty()) {
      Handle(Graphic3d_ArrayOfPoints) arr = new Graphic3d_ArrayOfPoints(static_cast<int>(glowPoints.size()));
      for (const auto& p : glowPoints) arr->AddVertex(gp_Pnt(p.p[0], p.p[1], p.p[2]));
      if (rim) {
        Handle(Graphic3d_Group) g = prs->NewGroup();
        g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, occ(rimColor), 7.5 * scale));
        g->AddPrimitiveArray(arr);
      }
      halo(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, occ(glowColor), 6.0 * scale), arr);
    }
    lines(thin, Aspect_TOL_SOLID, 1.0 * scale);
    lines(dashed, Aspect_TOL_DASH, 1.5 * scale);
    lines(solid, Aspect_TOL_SOLID, 2.0 * scale);
    if (!badges.empty()) {
      Handle(Graphic3d_ArrayOfTriangles) tri = new Graphic3d_ArrayOfTriangles(static_cast<int>(badges.size()));
      for (const auto& p : badges) tri->AddVertex(gp_Pnt(p[0], p[1], p[2]));
      Handle(Graphic3d_AspectFillArea3d) a = new Graphic3d_AspectFillArea3d();
      a->SetInteriorStyle(Aspect_IS_SOLID);
      a->SetInteriorColor(occ(badgeColor));
      a->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
      a->SetSuppressBackFaces(false);
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(a);
      g->AddPrimitiveArray(tri);
    }
    lines(marks, Aspect_TOL_SOLID, 1.5 * scale);
    lines(locked, Aspect_TOL_DASH, 3.0 * scale);
    auto markers = [&](const std::vector<Pt>& pts, const Handle(Graphic3d_AspectMarker3d)& aspect) {
      if (pts.empty()) return;
      Handle(Graphic3d_ArrayOfPoints) arr = new Graphic3d_ArrayOfPoints(static_cast<int>(pts.size()), Standard_True);
      for (const auto& p : pts) arr->AddVertex(gp_Pnt(p.p[0], p.p[1], p.p[2]), occ(p.c));
      Handle(Graphic3d_Group) g = prs->NewGroup();
      aspect->SetColor(occ(pts.front().c));
      g->SetGroupPrimitivesAspect(aspect);
      g->AddPrimitiveArray(arr);
    };
    markers(points, new Graphic3d_AspectMarker3d(Aspect_TOM_O_POINT, Quantity_NOC_WHITE, 2.0 * scale));
    markers(rings, new Graphic3d_AspectMarker3d(Aspect_TOM_O, Quantity_NOC_WHITE, 2.0 * scale));
    markers(bigPoints, new Graphic3d_AspectMarker3d(Aspect_TOM_O_POINT, Quantity_NOC_WHITE, 3.0 * scale));
    if (!dots.empty()) {  // a filled disc inside the point's ring, in each vertex's colour (the stock POINT marker is a pixel)
      const int d = std::max(3, int(std::lround(5 * scale))), row = (d + 7) / 8;
      Handle(TColStd_HArray1OfByte) bits = new TColStd_HArray1OfByte(0, row * d - 1, 0);
      for (int y = 0; y < d; ++y)
        for (int x = 0; x < d; ++x)
          if (std::hypot(x + 0.5 - d / 2.0, y + 0.5 - d / 2.0) <= d / 2.0) bits->ChangeValue(y * row + x / 8) |= Standard_Byte(0x80 >> (x % 8));
      markers(dots, new Graphic3d_AspectMarker3d(Quantity_NOC_WHITE, d, d, bits));
    }
    // One group per colour: a text aspect has a single colour.
    std::map<QRgb, Handle(Graphic3d_Group)> groups;
    for (const auto& t : texts) {
      Handle(Graphic3d_Group)& g = groups[t.c.rgb()];
      if (g.IsNull()) {
        g = prs->NewGroup();
        Handle(Graphic3d_AspectText3d) a = new Graphic3d_AspectText3d(occ(t.c), font.empty() ? "" : font.c_str(), 1.0, 0.0);
        a->SetDisplayType(Aspect_TODT_SUBTITLE);
        a->SetColorSubTitle(Quantity_ColorRGBA(occ(textBack)));
        g->SetGroupPrimitivesAspect(a);
      }
      Handle(Graphic3d_Text) text = new Graphic3d_Text(static_cast<float>(13.0 * scale));
      text->SetText(TCollection_ExtendedString(t.s.toUtf8().constData(), Standard_True));
      text->SetPosition(gp_Pnt(t.p[0], t.p[1], t.p[2]));
      text->SetHorizontalAlignment(t.left ? Graphic3d_HTA_LEFT : Graphic3d_HTA_CENTER);
      text->SetVerticalAlignment(Graphic3d_VTA_CENTER);
      g->AddText(text);
    }
    // Whole device pixels, so the cursor stays crisp at 150 %.
    const double hair = std::max(1.0, std::round(scale));
    lines(cursorHalo, Aspect_TOL_SOLID, hair + 2);
    lines(cursor, Aspect_TOL_SOLID, hair);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
};

QMap<QString, QStringList> SketchEditor::drawn() const {
  const SketchPrs& d = *static_cast<const SketchPrs*>(m_prs.get());
  auto names = [](const std::vector<SketchPrs::Pt>& pts) {
    QStringList out;
    for (const auto& p : pts) out << p.c.name();
    return out;
  };
  QStringList texts;
  for (const auto& t : d.texts) texts << t.s;
  return {{"points", names(d.points)}, {"rings", names(d.rings)}, {"bigPoints", names(d.bigPoints)}, {"texts", texts}};
}

// ---------------------------------------------------------------- life cycle
SketchEditor::SketchEditor(AppDocument* doc, Viewport* viewport, JobRunner* jobs, QObject* parent) : QObject(parent), m_doc(doc), m_viewport(viewport), m_jobs(jobs) {
  m_dimensionHandle=new DimensionHandle(viewport,jobs);qApp->installEventFilter(this);
  m_toolPreviewTimer.setSingleShot(true);m_toolPreviewTimer.setInterval(120);
  connect(&m_toolPreviewTimer,&QTimer::timeout,this,[this]{if(!m_active)return;if(m_editJob){m_toolPreviewTimer.start();return;}previewTool();});
  connect(m_dimensionHandle,&DimensionHandle::valueChanged,this,[this](const QString& text){m_options["distance"]=text;scheduleToolPreview();emit workflowChanged();});
  m_dimensionHandle->setLabel(tr("Offset"));m_dimensionHandle->setCapturesKeys(false);  // the keys come through sketchKey
  connect(m_dimensionHandle,&DimensionHandle::accepted,this,[this]{if(m_active && m_tool=="offset" && !m_sel.empty())applyTool();});
  m_input=new DynamicInput(viewport);
  connect(m_input,&DynamicInput::optionEdited,this,[this](const QString& key,const QString& value){m_options[key]=value;scheduleToolPreview();updateTransient();emit workflowChanged();});
  connect(m_input,&DynamicInput::typedChanged,this,[this]{if(m_active)emit changed();});  // the prompt says what Enter and Esc do now
  connect(m_input,&DynamicInput::committed,this,[this]{if(!done())m_viewport->setFocus();});
  connect(m_input,&DynamicInput::escaped,this,[this]{escape();});
  connect(m_input,&DynamicInput::undoPoint,this,[this]{undoPoint();});
  connect(m_input,&DynamicInput::valueTyped,this,[this]{if(m_active)retype();});  // the rubber band follows what is typed
  connect(m_input,&DynamicInput::dropped,this,[this]{if(!m_active)return;m_entry.reset();retype();updateInput();});
  connect(m_input,&DynamicInput::chipClicked,this,[this](const QString& key) {
    if(!m_active)return;
    if(key=="angle"){m_angleRelative=!m_angleRelative;QSettings().setValue("sketch/input/angleRelative",m_angleRelative);}
    else if(key=="second" || key=="chamferAngle" || key=="dy" || key=="moveAngle")return setAngled(key=="second" || key=="dy");
    else if(key=="diameter" && (m_tool=="polygon" || m_tool=="polygon_outer")) {  // inscribed or circumscribed: the same centre, what is typed stays
      const auto clicks=m_clicks;
      setTool(m_tool=="polygon"?"polygon_outer":"polygon");
      m_clicks=clicks;toolPrompt();
    } else if(key=="diameter" || key=="radius") {  // a circle's box: the number typed stays, now the other size
      const QString carried=m_input->text(key);
      m_circleRadius=!m_circleRadius;QSettings().setValue("sketch/input/circleRadius",m_circleRadius);
      updateInput();if(!carried.isEmpty())m_input->setText(0,carried);
    } else return;
    retype();updateInput();updateTransient();
  });
  m_input->setKeyHook([this](int box,QChar c){return m_active && entryKey(box,c);});
  connect(units::notifier(),&units::Notifier::changed,this,[this]{if(m_active)rebuild();});  // dimension labels in the shown unit
  // A snap or solver setting changed from any of its faces (Preferences, the panel, the status bar's toggles, UI-110): read
  // again, as the editor reads them once (UI-27).
  connect(preferences::notifier(),&preferences::Notifier::changed,this,[this](const QString& key){if(key.isEmpty()||key.startsWith("sketch/")||key.startsWith("view/"))refreshSnap();});
  // Grid snapping on or off (F9, the panel's checkbox): the pointer's snap again, the drawing cursor or the pointer.
  connect(m_viewport,&Viewport::gridSnapChanged,this,[this]{if(m_active)resnap();});
  // The system pointer blank (the cursor drawn) or back: drawn or gone at once. Blank again after a camera gesture or a
  // menu, the pointer may have moved meanwhile: the snap where it is now, so a click without a move goes where it is drawn.
  connect(m_viewport,&Viewport::ownCursorChanged,this,[this](bool shown){if(m_active && !m_inTransient && !(shown && followPointer()))updateTransient();});
  connect(m_viewport,&Viewport::notesMoved,this,[this] {
    if(!m_active) return;
    // The camera moved under a still pointer (the wheel zooms about it, the step may change): the drawing cursor's node again.
    if(m_inView && m_viewport->ownCursor())followPointer();
    const double pixels=m_viewport->pixelSize();
    if(pixels<m_samplePixelSize*.75 || pixels>m_samplePixelSize*1.5) rebuild();
    if(m_dimEdit && m_dimEdit->isVisible())if(const auto* c=m_sk.constraint(m_dimEditing)) {  // the value box stays on its label
      double lu,lv;labelPosition(*c,lu,lv);const QPoint at=m_viewport->widgetPoint(m_frame.to_world(lu,lv));
      m_dimEdit->move(at.x()-m_dimEdit->width()/2,at.y()-m_dimEdit->height()/2);
    }
  });
  // Resting on a point acquires it as a tracking point (or lets it go); passing over it does not.
  m_dwellTimer.setSingleShot(true);
  m_dwellTimer.setInterval(350);
  connect(&m_dwellTimer, &QTimer::timeout, this, [this] {
    if (!m_active || !m_dwellPoint || m_hover.kind != Hit::Point || m_hover.id != m_dwellPoint) return;
    if (!m_settings.tracking && !m_settings.extensions) return;  // F12 off: nothing uses them
    sketchsnap::track(m_tracked, m_dwellPoint);
    resnap();
  });
  m_fillTimer.setSingleShot(true);
  m_fillTimer.setInterval(150);
  connect(&m_fillTimer, &QTimer::timeout, this, [this] {
    if (!m_active) return;
    if (m_fillJob) m_fillJob->cancel();
    // Finding the closed regions is kernel work: on a worker, from a copy of the sketch.
    const int session=m_session,revision=m_fillRevision;
    auto sk = std::make_shared<Sketch>(m_sk);
    auto out = std::make_shared<std::vector<opad::Vec3>>();
    const opad::Frame frame = m_frame;
    m_jobs->backgroundNext();  // after every change, while drawing: never the busy cursor over the crosshair
    m_fillJob = m_jobs->async(tr("Finding profiles"), [sk, out, frame](Progress progress) {
      for (const auto& region : sketch_regions(*sk, frame)) {
        if(progress.cancelled())return;
        BRepMesh_IncrementalMesh(region.face, 0.05, Standard_False, 0.2, Standard_False);
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(region.face, loc);
        if (tri.IsNull()) continue;
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
          int a, b, c;
          tri->Triangle(i).Get(a, b, c);
          for (int n : {a, b, c}) {
            const gp_Pnt p = tri->Node(n).Transformed(loc.Transformation());
            out->push_back({p.X(), p.Y(), p.Z()});
          }
        }
      }
    }, [this, out,session,revision](bool ok, const QString&) {
      if(session!=m_session||revision!=m_fillRevision)return;
      m_fillJob = nullptr;
      if (!ok || !m_active) return;
      m_fill = std::move(*out);
      rebuild();
    });
  });
}

SketchEditor::~SketchEditor() {delete m_dimensionHandle;delete m_input;}
void SketchEditor::setVisible(bool visible) {
  if(!m_active || m_visible==visible)return;
  m_visible=visible;
  updateDimensionHandle();updateInput();m_viewport->setOwnCursor(drawsCursor());
  for(const auto& prs:{m_prs,m_transientPrs}) {
    if(visible)m_viewport->showOverlay(prs);else m_viewport->removeOverlay(prs);
  }
  if(visible)showToolPreview();else m_viewport->removeOverlay(m_toolPreviewOverlay);
  showSources();  // the picks' highlight goes and comes back with the preview
  for(const auto& prs:m_imagePrs) {
    if(visible)m_viewport->showBackdrop(prs);else m_viewport->removeOverlay(prs);
  }
  emit changed();
}

void SketchEditor::begin(const std::string& sketchId, const QString& name, const opad::json& plane, const opad::Frame& frame, const opad::json& geometry) {
  ++m_geometryRevision;if(m_geometryJob)m_geometryJob->cancel();m_geometryJob=nullptr;m_geometry.reset();
  ++m_session;m_toolPreview.reset();m_previewRequested=false;
  ++m_modelRevision;  // what previews cached by curve id (trim cuts, an extend's run) was the last sketch's: ids start again
  m_showConstraints=QSettings().value("sketch/showConstraints",true).toBool();
  m_tracked.clear();m_dwellPoint=0;m_lock.reset();m_shiftDown=m_shiftSpent=m_inView=false;m_typedValues.clear();m_entry.reset();m_pointer=m_cursor={};m_angleRelative=QSettings().value("sketch/input/angleRelative",false).toBool();m_circleRadius=QSettings().value("sketch/input/circleRadius",false).toBool();m_dragging=false;m_dragMoved=false;m_dragPending=false;m_dragReleased=false;m_inChange=false;m_options.clear();m_conflicts.clear();
  readSettings();
  m_id = sketchId;
  m_name = name;
  m_plane = plane;
  m_frame = frame;
  m_sk = Sketch::from_json(geometry);
  m_initialGeometry = m_sk.to_json();
  if (m_sk.points.empty()) m_sk.add_point(0, 0, true);  // the origin: something to constrain the first curve to
  m_solved={}; // analysed after setup; opening never changes stored coordinates
  m_undo.clear();
  m_redo.clear();
  m_sel.clear();
  m_clicks.clear();
  m_chain.clear();
  m_picked.clear();
  m_sources.clear();
  m_fill.clear();
  m_placingDim = false;
  m_modified = false;
  m_active = true;
  m_visible = true;
  m_tool = "select";
  m_prs = new SketchPrs();m_prs->SetInfiniteState(true); // axes must not inflate camera fitting
  m_transientPrs=new SketchPrs();m_transientPrs->SetInfiniteState(true);
  m_cameraBefore = m_viewport->cameraJson();m_sectionBefore=m_viewport->sectionState();m_imagesStamp.clear();
  m_viewport->beginSketchInput(this, frame, sketchId);
  m_viewport->showOverlay(m_prs);
  m_viewport->showOverlay(m_transientPrs);
  fitSketch();analyseSketch();
  rebuild();
  scheduleFill();
  emit toolChanged(m_tool);
  toolPrompt();
  emit changed();
}

void SketchEditor::end() {
  if (!m_active) return;
  m_toolPreviewTimer.stop();m_dwellTimer.stop();m_dimensionHandle->hide();forgetTyped();m_input->setFields({});m_input->hide();
  m_viewport->removeOverlay(m_toolPreviewOverlay);m_toolPreviewOverlay.Nullify();
  ++m_geometryRevision;++m_fillRevision;if(m_geometryJob)m_geometryJob->cancel();m_geometryJob=nullptr;m_geometry.reset();
  m_viewport->setEdgeHover(false);
  ++m_imageRevision;if(m_imageJob)m_imageJob->cancel();m_imageJob=nullptr;
  for(const auto& prs:m_imagePrs)m_viewport->removeOverlay(prs);m_imagePrs.clear();
  m_viewport->restoreSection(m_sectionBefore);
  ++m_session;if(m_editJob)m_editJob->cancel();m_editJob=nullptr;
  m_active = false;m_toolPreview.reset();showSources();  // the picks the tool had
  m_fillTimer.stop();
  if (m_fillJob) m_fillJob->cancel();m_fillJob=nullptr;
  if (m_dimEdit) m_dimEdit->hide();
  m_viewport->removeOverlay(m_prs);
  m_viewport->removeOverlay(m_transientPrs);m_transientPrs.Nullify();
  m_prs.Nullify();
  m_viewport->endSketchInput();
  emit changed();
}

bool SketchEditor::empty() const { return m_sk.entities.empty() && m_sk.images.empty(); }

bool SketchEditor::isFixedPoint(int id) const {
  const SkPoint* p = m_sk.point(id);
  return p && p->fixed;
}

double SketchEditor::tol() const { return 8.0 * m_viewport->pixelSize(); }

void SketchEditor::scheduleFill() { ++m_fillRevision;if(m_fillJob)m_fillJob->cancel();m_fillJob=nullptr;m_fillTimer.start(); }

// ---------------------------------------------------------------- undo
void SketchEditor::begin_change() {
  ++m_modelRevision;
  invalidatePreview();
  m_dangling.clear();
  m_sk.id_watermark=m_sk.next_id()-1;
  m_before = m_sk;
  m_beforePlane=m_plane;m_beforeFrame=m_frame;m_conflicts.clear();
  m_inChange = true;
}

void SketchEditor::cancel_change() {
  if (!m_inChange) return;
  m_sk = m_before;
  m_inChange = false;
}

bool SketchEditor::end_change(const QString& what) {
  m_inChange = false;
  if(m_sk.points.size()>300) {
    const auto after=std::make_shared<Sketch>(m_sk),before=std::make_shared<Sketch>(m_before);const auto result=std::make_shared<SolveResult>();
    const auto oldPlane=m_beforePlane;const auto oldFrame=m_beforeFrame;const auto options=solveOptions();const int session=m_session;
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});QPointer<SketchEditor> guard(this);
    m_editJob=m_jobs->async(what,[after,result,options,defs](Progress p){if(p.cancelled())return;evaluate_dimensions(*after,ParamTable(defs));*result=solve(*after,options);if(!result->converged)throw opad::Error("the edit conflicts with existing constraints");},[this,guard,after,before,result,session,oldPlane,oldFrame](bool ok,const QString& error){
      if(!guard||!m_active||session!=m_session)return;m_editJob=nullptr;++m_modelRevision;  // what previews cached is stale either way
      if(ok){m_sk=*after;m_solved=*result;m_undo.push_back({*before,oldPlane,oldFrame});m_redo.clear();m_modified=true;}
      else {const bool planeChanged=m_plane!=oldPlane;m_sk=*before;m_plane=oldPlane;m_frame=oldFrame;if(planeChanged){m_viewport->endSketchInput();m_viewport->beginSketchInput(this,m_frame,m_id);fitSketch();}m_chain.clear();m_clicks.clear();m_picked.clear();m_placingDim=false;m_dimEditing=0;m_conflicts={result->failed.begin(),result->failed.end()};emit status(error);}
      m_panelFieldsDirty=true;rebuild();scheduleFill();emit changed();
    });
    rebuild();return true;
  }
  SolveResult r;
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    evaluate_dimensions(m_sk,ParamTable(defs));
    r = solve(m_sk,solveOptions());
  } catch (const std::exception& e) {
    m_sk = m_before;
    m_plane=m_beforePlane;m_frame=m_beforeFrame;
    emit status(i18n::t(QString::fromUtf8(e.what())));
    rebuild();
    return false;
  }
  if (!r.converged) {
    m_conflicts={r.failed.begin(),r.failed.end()};
    m_sk = m_before;  // never leave the sketch over-constrained: the change is refused
    m_plane=m_beforePlane;m_frame=m_beforeFrame;
    QStringList ids;for(int id:r.failed)ids<<QString::number(id);
    emit status(tr("%1 conflicts with constraints %2. Edit or remove them, or switch a dimension to reference.").arg(what,ids.join(", ")));
    rebuild();
    return false;
  }
  m_solved = r;
  m_undo.push_back({m_before,m_beforePlane,m_beforeFrame});
  if (m_undo.size() > 200) {m_undo.erase(m_undo.begin());if(m_chainUndoStart)--m_chainUndoStart;}
  m_redo.clear();
  m_modified = true;
  rebuild();
  scheduleFill();
  emit changed();
  return true;
}

void SketchEditor::undo() {
  if(m_previewComputing && m_editJob){invalidatePreview();m_editJob->cancel();m_editJob=nullptr;m_previewComputing=false;}
  if(m_editJob){
    if(!m_undoPending){m_undoPending=true;connect(m_editJob,&Job::finished,this,[this]{m_undoPending=false;if(m_active)undo();},Qt::QueuedConnection);}
    return;
  }
  m_toolPreviewTimer.stop();m_clicks.clear();m_chain.clear();m_picked.clear();m_sources.clear();cancel_change();
  m_tool="select";m_placingDim=false;m_dragging=false;m_boxSelecting=false;m_dimensionHandle->hide();emit toolChanged(m_tool);
  if(m_undo.empty()){rebuild();emit changed();return;}
  ++m_modelRevision;
  invalidatePreview();
  const bool planeChanged=m_plane!=m_undo.back().plane;
  m_redo.push_back({std::move(m_sk),m_plane,m_frame});  // moved, not copied: a converted drawing's 30,000 curves
  m_sk = std::move(m_undo.back().geometry);
  m_plane=m_undo.back().plane;m_frame=m_undo.back().frame;
  if(planeChanged){m_viewport->endSketchInput();m_viewport->beginSketchInput(this,m_frame,m_id);fitSketch();}
  m_undo.pop_back();
  m_clicks.clear();
  m_chain.clear();
  m_picked.clear();
  m_sel.clear();
  analyseSketch();m_conflicts.clear();m_panelFieldsDirty=true;
  m_modified = true;
  rebuild();
  scheduleFill();
  emit changed();
}

void SketchEditor::redo() {
  if (m_editJob || m_redo.empty()) return;
  ++m_modelRevision;
  invalidatePreview();
  const bool planeChanged=m_plane!=m_redo.back().plane;
  m_undo.push_back({std::move(m_sk),m_plane,m_frame});
  m_sk = std::move(m_redo.back().geometry);
  m_plane=m_redo.back().plane;m_frame=m_redo.back().frame;
  if(planeChanged){m_viewport->endSketchInput();m_viewport->beginSketchInput(this,m_frame,m_id);fitSketch();}
  m_redo.pop_back();
  m_sel.clear();
  analyseSketch();m_conflicts.clear();m_panelFieldsDirty=true;
  m_modified = true;
  rebuild();
  scheduleFill();
  emit changed();
}

// ---------------------------------------------------------------- geometry queries
std::vector<std::pair<double, double>> SketchEditor::sampled(const SkEntity& e) const {
  if(m_geometry)if(const auto* cached=m_geometry->samples(m_sk,e))return *cached;
  std::vector<std::pair<double, double>> out;
  if(e.type==SkEntity::Type::Point) return out;
  const auto edge=entity_edge(m_sk,e,opad::Frame());
  if(!edge.IsNull()) for(const auto& point:curveSamples(edge,std::max(1e-7,m_viewport->pixelSize()*0.25))) out.push_back({point.X(),point.Y()});
  return out;
}

const SkPoint* SketchEditor::pointOf(int id) const { return m_geometry ? m_geometry->point(m_sk, id) : m_sk.point(id); }

double SketchEditor::distanceTo(const SkEntity& e, double u, double v) const {
  if (e.type == SkEntity::Type::Point) {
    const SkPoint* p = pointOf(e.p.empty() ? 0 : e.p[0]);
    return p ? std::hypot(p->x - u, p->y - v) : 1e300;
  }
  if (e.type == SkEntity::Type::Circle) {
    const SkPoint* c = pointOf(e.p.empty() ? 0 : e.p[0]);
    return c ? std::fabs(std::hypot(c->x - u, c->y - v) - e.r) : 1e300;
  }
  const auto pts = sampled(e);
  double best = 1e300;
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const double ax = pts[i].first, ay = pts[i].second, bx = pts[i + 1].first, by = pts[i + 1].second;
    const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    const double t = len2 < 1e-18 ? 0 : std::clamp(((u - ax) * dx + (v - ay) * dy) / len2, 0.0, 1.0);
    best = std::min(best, std::hypot(ax + t * dx - u, ay + t * dy - v));
  }
  return best;
}

SketchEditor::Hit SketchEditor::hitTest(double u, double v) const {
  if(!m_geometry || m_geometryJob)return {};
  const double t = tol(), grip = kHandlePixels * m_viewport->pixelSize();
  const auto localCandidates=m_geometry->query(u-grip*1.1,v-grip*1.1,u+grip*1.1,v+grip*1.1);
  Hit hit;
  double best = grip;
  // Trim takes a piece of a curve: its points are no target (within the 12 px a point takes, UI-124, a click near a line's
  // end hit the end and trimmed the wrong piece).
  const std::vector<size_t> none;
  for (size_t index : m_tool == "trim" ? none : localCandidates.points) {
    const auto& p=m_sk.points[index];
    if (!selectable(p.id)) continue;
    const double d = std::hypot(p.x - u, p.y - v);
    if (d < best) { best = d; hit = {Hit::Point, p.id}; }
  }
  if (hit.kind != Hit::None) return hit;
  for (const auto& c : m_sk.constraints) {
    if (!c.is_dimension() || !selectable(c.id)) continue;
    double lu, lv;
    labelPosition(c, lu, lv);
    if (std::fabs(lu - u) < 3.5 * t && std::fabs(lv - v) < 1.4 * t) return {Hit::Dimension, c.id};
  }
  best = t;
  for(const auto& [id,x,y]:m_glyphHits)if(selectable(id) && std::fabs(x-u)<grip && std::fabs(y-v)<grip)return {Hit::Dimension,id};
  for (size_t index : localCandidates.entities) {
    const auto& e=m_sk.entities[index];
    if (!selectable(e.id)) continue;
    const double d = distanceTo(e, u, v);
    if (d < best) { best = d; hit = {Hit::Entity, e.id}; }
  }
  return hit;
}

SketchEditor::Snap SketchEditor::snap(double u, double v, bool infer, bool grid) const {
  Snap s;
  s.u = u;
  s.v = v;
  if(!infer)return s; // Alt suppresses inference, automatic coincidence and the grid
  // Grid snapping takes the points a tool places; a pick (trim, a dimension's curve, a constraint) is where the pointer is.
  const double step = grid && gridPoints() && m_viewport->gridSnap() ? m_viewport->gridStep() : 0;
  if(!m_geometry || m_geometryJob) {  // a large sketch's curves being prepared: the grid alone meanwhile (the cursor moves on)
    if(step>0){s.u=sketchsnap::onGrid(u,step);s.v=sketchsnap::onGrid(v,step);s.grid=true;s.kind=Snap::Kind::Grid;}
    return s;
  }
  const double t = tol();
  // What is near the pointer, never the whole sketch, and the settings as read (UI-27).
  const Settings& on = m_settings;
  const bool automatic = on.inference, extensions = on.extensions, tracking = on.tracking;
  auto P = [&](int id) { return m_geometry->point(m_sk, id); };
  using CT=SkConstraint::Type;
  // The step's last point (UI-23: in every tool whose next point has a direction from it): its horizontal and vertical,
  // the angle ray, the perpendicular and tangent snaps and Ortho go from there.
  double fx=0,fy=0;
  int fid=0;
  const bool from=fromPoint(fx,fy,fid);
  auto curveOf=[&](const SkEntity& e,sketchsnap::Curve& c) {  // a line, circle or arc as sketchsnap sees it
    if(e.type!=SkEntity::Type::Line && e.type!=SkEntity::Type::Circle && e.type!=SkEntity::Type::Arc)return false;
    const SkPoint *a=P(e.p[0]),*b=e.p.size()>1?P(e.p[1]):nullptr,*end=e.p.size()>2?P(e.p[2]):nullptr;
    if(!a || (e.type==SkEntity::Type::Line && !b) || (e.type==SkEntity::Type::Arc && !end))return false;
    c=sketchsnap::Curve{};c.x=a->x;c.y=a->y;
    if(e.type==SkEntity::Type::Line){c.ex=b->x;c.ey=b->y;}
    else if(e.type==SkEntity::Type::Circle){c.r=e.r;c.sweep=2*M_PI;}
    else{c.r=std::hypot(b->x-a->x,b->y-a->y);c.start=std::atan2(b->y-a->y,b->x-a->x);c.sweep=std::atan2(end->y-a->y,end->x-a->x)-c.start;if(c.sweep<=0)c.sweep+=2*M_PI;}
    return true;
  };

  if (!m_lock) {
    // The object snaps in reach: existing points first, then the others, each nearest first (counted from where Shift taps
    // began going through them: a tap shows the next, UI-23). What else holds the point there (UI-21): a line's midpoint,
    // both curves of an intersection, a quadrant on its circle and level with (or above) its centre; from the last point
    // the segment square to a line or a circle, or touching a circle or an arc (UI-23).
    const auto local=m_geometry->query(u-t*1.1,v-t*1.1,u+t*1.1,v+t*1.1);
    std::vector<std::pair<double,Snap>> points,others;
    const double ou=m_snapChoice?m_choiceU:u,ov=m_snapChoice?m_choiceV:v;
    for (size_t index : local.points) {
      const auto& p=m_sk.points[index];
      if(!(m_geometry->centre(p.id)?on.center:on.endpoint))continue;
      if (!m_chain.empty() && p.id == m_chain.back()) continue;  // not onto the point the segment starts at
      if (!(std::hypot(p.x - u, p.y - v) < t)) continue;
      Snap c=s;c.point=p.id;c.u=p.x;c.v=p.y;c.kind=Snap::Kind::Point;c.target=p.id;
      points.push_back({std::hypot(p.x-ou,p.y-ov),c});
    }
    auto candidate=[&](double x,double y,Snap::Kind kind,int a,int b,std::vector<Snap::Hold> holds,std::vector<Snap::Hold> segment={}) {
      if(!(std::hypot(x-u,y-v)<t))return;
      for(const auto& o:others)if(o.second.kind==kind && std::hypot(o.second.u-x,o.second.v-y)<1e-9*(1+std::fabs(x)+std::fabs(y)))return;  // one per place and kind
      Snap c=s;c.u=x;c.v=y;c.kind=kind;c.target=a;c.other=b;
      if(automatic){c.holds=std::move(holds);c.segment=std::move(segment);}
      others.push_back({std::hypot(x-ou,y-ov),c});
    };
    std::vector<const SkEntity*> nearby;
    for(size_t index:local.entities) {
      const auto& e=m_sk.entities[index];
      if(e.type==SkEntity::Type::Line) {
        const auto *a=P(e.p[0]),*b=P(e.p[1]);
        if(on.midpoint)candidate((a->x+b->x)/2,(a->y+b->y)/2,Snap::Kind::Midpoint,e.id,0,{{CT::Midpoint,e.id}});
        if(on.intersection && distanceTo(e,u,v)<t)nearby.push_back(&e);
      } else if(e.type==SkEntity::Type::Circle || e.type==SkEntity::Type::Arc) {
        const auto* c=P(e.p[0]);
        const double r=e.type==SkEntity::Type::Circle?e.r:std::hypot(P(e.p[1])->x-c->x,P(e.p[1])->y-c->y);
        if(on.quadrant)for(int q=0;q<4;++q) {
          const double x=c->x+r*std::cos(q*M_PI/2),y=c->y+r*std::sin(q*M_PI/2);
          if(distanceTo(e,x,y)<t)candidate(x,y,Snap::Kind::Quadrant,e.id,0,{{CT::Coincident,e.id},{q%2?CT::Vertical:CT::Horizontal,c->id}});
        }
        if(on.intersection && distanceTo(e,u,v)<t)nearby.push_back(&e);
      }
      // From the last point: square to the curve or touching it (not its own line, not about its own centre).
      sketchsnap::Curve c;
      const bool line=e.type==SkEntity::Type::Line;
      if(from && curveOf(e,c) && !(line?std::count(e.p.begin(),e.p.end(),fid)>0:e.p[0]==fid)) {
        double x[2],y[2];
        if(on.perpendicular)for(int k=0,n=sketchsnap::normals(c,fx,fy,x,y);k<n;++k)
          candidate(x[k],y[k],Snap::Kind::Perpendicular,e.id,0,{{CT::Coincident,e.id}},{line?Snap::Hold{CT::Perpendicular,e.id}:Snap::Hold{CT::Coincident,e.p[0]}});
        if(on.tangent && !line)for(int k=0,n=sketchsnap::tangents(c,fx,fy,x,y);k<n;++k)
          candidate(x[k],y[k],Snap::Kind::Tangent,e.id,0,{{CT::Coincident,e.id}},{{CT::Tangent,e.id}});
      }
    }
    for(size_t i=0;i<nearby.size();++i)for(size_t j=i+1;j<nearby.size();++j) {
      // Local display polylines use the same quarter-pixel tolerance as picking; intersections snap to it.
      const auto a=sampled(*nearby[i]),b=sampled(*nearby[j]);
      for(size_t k=1;k<a.size();++k)for(size_t l=1;l<b.size();++l) {
        const double ax=a[k-1].first,ay=a[k-1].second,dx=a[k].first-ax,dy=a[k].second-ay;
        const double bx=b[l-1].first,by=b[l-1].second,ex=b[l].first-bx,ey=b[l].second-by,den=dx*ey-dy*ex;
        if(std::fabs(den)<1e-15)continue;
        const double ta=((bx-ax)*ey-(by-ay)*ex)/den,tb=((bx-ax)*dy-(by-ay)*dx)/den;
        if(ta>=0 && ta<=1 && tb>=0 && tb<=1)candidate(ax+ta*dx,ay+ta*dy,Snap::Kind::Intersection,nearby[i]->id,nearby[j]->id,{{CT::Coincident,nearby[i]->id},{CT::Coincident,nearby[j]->id}});
      }
    }
    if(on.apparent) {  // apparent intersections: the lines of segments crossing past their ends, a segment's line meeting a circle or an arc
      const double around=250*m_viewport->pixelSize();
      std::vector<std::pair<int,sketchsnap::Curve>> lines,rounds;
      for(size_t index:m_geometry->query(u-around,v-around,u+around,v+around).entities) {
        const auto& e=m_sk.entities[index];
        sketchsnap::Curve c;
        double fu,fv;
        if(!curveOf(e,c))continue;
        const double d=sketchsnap::foot(c,u,v,fu,fv);
        if(c.r>0){if(d<t)rounds.push_back({e.id,c});}
        else if(d<around && std::hypot(fu-u,fv-v)<t)lines.push_back({e.id,c});  // its line passes the pointer, the segment not far off
      }
      for(size_t i=0;i<lines.size();++i) {
        for(size_t j=i+1;j<lines.size();++j)
          if(double x,y;sketchsnap::apparent(lines[i].second,lines[j].second,x,y))candidate(x,y,Snap::Kind::Apparent,lines[i].first,lines[j].first,{{CT::Coincident,lines[i].first},{CT::Coincident,lines[j].first}});
        for(const auto& [id,round]:rounds) {
          double x[2],y[2];
          for(int k=0,n=sketchsnap::apparent(lines[i].second,round,x,y);k<n;++k)candidate(x[k],y[k],Snap::Kind::Apparent,lines[i].first,id,{{CT::Coincident,lines[i].first},{CT::Coincident,id}});
        }
      }
    }
    auto nearest=[](const std::pair<double,Snap>& a,const std::pair<double,Snap>& b){return a.first<b.first;};
    std::stable_sort(points.begin(),points.end(),nearest);
    std::stable_sort(others.begin(),others.end(),nearest);
    points.insert(points.end(),others.begin(),others.end());
    if(!points.empty()) {
      const int n=int(points.size()),shown=m_snapChoice%n;
      Snap c=points[size_t(shown)].second;
      c.choices=n;c.choice=shown;
      return c;
    }
  }

  // Ortho (F8, UI-23): with nothing locked the pointer keeps to the horizontal or the vertical through the step's last point,
  // whichever is nearer, as a lock onto it would (the object snaps above still come first).
  std::optional<Lock> ortho;
  if(!m_lock && from && on.ortho) {
    const bool across=std::fabs(u-fx)>=std::fabs(v-fy);
    ortho=Lock{{fx,fy,across?1.0:0.0,across?0.0:1.0,fid},across && automatic,!across && automatic};
  }
  const Lock* lock=m_lock?&*m_lock:ortho?&*ortho:nullptr;
  // Locked (Shift, UI-19; Ortho): the pointer's foot on the locked line stands in for it.
  double pu = u, pv = v;
  if (lock) sketchsnap::project(lock->line, u, v, 0, pu, pv);
  const auto localCandidates=m_geometry->query(pu-t*1.1,pv-t*1.1,pu+t*1.1,pv+t*1.1);

  // Below the object snaps (sketchsnap::resolve): a crossing (of guides, the angle ray and curves), a grid node, one
  // guide, the angle ray, a curve, the grid. Guides: horizontal and vertical from the step's last point (constraints
  // when automatic), the same from each tracked point (two of them cross at (A.x, B.y)), and the lines through a
  // tracked point extended past their ends. Locked, they are what stops the pointer along the locked line.
  struct Meaning { Snap::Kind kind; int target; bool horizontal, vertical; Snap::Hold hold{SkConstraint::Type::Coincident,0}; };  // hold: what the point keeps of it (UI-21)
  std::vector<sketchsnap::Guide> guides;
  std::vector<Meaning> meaning;
  auto guide=[&](const sketchsnap::Guide& g,const Meaning& m){guides.push_back(g);meaning.push_back(m);};
  if(from && tracking) {
    const auto kind=automatic?Snap::Kind::None:Snap::Kind::Aligned;
    if(std::fabs(pu-fx)>3*t)guide({fx,fy,1,0,fid},{kind,fid,automatic,false});
    if(std::fabs(pv-fy)>3*t)guide({fx,fy,0,1,fid},{kind,fid,false,automatic});
  }
  if(tracking)for(int id:m_tracked)if(const SkPoint* p=P(id);p && p->id!=fid) {
    guide({p->x,p->y,1,0,p->id},{Snap::Kind::Aligned,p->id,false,false,{SkConstraint::Type::Horizontal,p->id}});
    guide({p->x,p->y,0,1,p->id},{Snap::Kind::Aligned,p->id,false,false,{SkConstraint::Type::Vertical,p->id}});
  }
  // A tracked line's extension can be outside its finite bounding box: the lines through the tracked points, in sketch order.
  auto isTracked=[&](int id){return std::find(m_tracked.begin(),m_tracked.end(),id)!=m_tracked.end();};
  std::set<size_t> trackedLines;
  if(extensions)for(int id:m_tracked)for(size_t index:m_geometry->curvesAt(id))trackedLines.insert(index);
  for(size_t index:trackedLines) {
    if(index>=m_sk.entities.size())continue;
    const auto& e=m_sk.entities[index];
    if(e.type!=SkEntity::Type::Line || e.p.size()!=2)continue;
    const int end=isTracked(e.p[0])?e.p[0]:isTracked(e.p[1])?e.p[1]:0;
    const auto *a=P(e.p[0]),*b=P(e.p[1]),*p=end?P(end):nullptr;
    if(!a || !b || !p)continue;
    const double dx=b->x-a->x,dy=b->y-a->y,len=std::hypot(dx,dy);
    if(len<1e-9)continue;
    const double k=((pu-a->x)*dx+(pv-a->y)*dy)/(len*len);
    if(k<0 || k>1)guide({p->x,p->y,dx/len,dy/len,p->id},{Snap::Kind::Extension,e.id,false,false,{SkConstraint::Type::Coincident,e.id}});
  }
  sketchsnap::Guide ray{0,0,1,0,fid};
  // With a grid (and nothing locked) the angle only names a node exactly on one of its rays: the point stays on the node.
  const double gu=sketchsnap::onGrid(pu,step>0?step:1),gv=sketchsnap::onGrid(pv,step>0?step:1);
  const bool nodes=step>0 && !lock;
  const bool angled=from && on.angle && (nodes?sketchsnap::angleRay(fx,fy,gu,gv,on.angleStep*M_PI/180,step*1e-6,ray)
                                             :sketchsnap::angleRay(fx,fy,pu,pv,on.angleStep*M_PI/180,t,ray));
  // The lines, circles and arcs under the pointer: it lands on the nearest, or where a guide crosses one (with a grid,
  // also those through the node, which then lies on them). Locked: those along the locked line within reach (the view's
  // diagonal) of where the stops are counted from, the pointer (or where it was when a Shift tap showed one).
  const double reach=std::hypot(m_viewport->width(),m_viewport->height())*m_viewport->pixelSize();
  double su=u,sv=v;
  if(lock && lock->stop>=0){su=lock->su;sv=lock->sv;}
  SketchGeometryCache::Query reached;
  if(lock) {
    const auto& g=lock->line;
    double fu,fv;
    sketchsnap::project(g,su,sv,0,fu,fv);
    reached=m_geometry->query(fu-reach*std::fabs(g.dx)-t,fv-reach*std::fabs(g.dy)-t,fu+reach*std::fabs(g.dx)+t,fv+reach*std::fabs(g.dy)+t);
  }
  std::vector<sketchsnap::Curve> curves;
  std::vector<int> curveIds;
  if(on.nearest)for(size_t index:(lock?reached:localCandidates).entities) {
    const auto& e=m_sk.entities[index];
    sketchsnap::Curve c;
    if(!curveOf(e,c) || (!lock && distanceTo(e,pu,pv)>=t))continue;
    curves.push_back(c);curveIds.push_back(e.id);
  }
  if(on.nearest && nodes) {
    const double onNode=step*1e-6;
    for(size_t index:m_geometry->query(gu-onNode,gv-onNode,gu+onNode,gv+onNode).entities) {
      const auto& e=m_sk.entities[index];
      sketchsnap::Curve c;
      if(std::find(curveIds.begin(),curveIds.end(),e.id)!=curveIds.end() || !curveOf(e,c))continue;
      if(double fu,fv;sketchsnap::foot(c,gu,gv,fu,fv)<onNode){curves.push_back(c);curveIds.push_back(e.id);}
    }
  }
  using By=sketchsnap::Pick::By;
  if(lock) {  // along the locked line: where another guide, the angle ray or a curve crosses it, else a grid line
    if(angled)guide(ray,{Snap::Kind::Angle,fid,false,false});
    // Its stops in the view: the nearest holds the pointer within the capture; Shift taps on a lock that stays show the
    // others (the view: its corners on the sketch plane, unless one misses it).
    auto all=sketchsnap::stops(lock->line,su,sv,reach,guides,curves);
    std::array<std::pair<double,double>,4> view;
    const double w=m_viewport->width(),h=m_viewport->height();
    bool framed=true;
    for(int i=0;i<4;++i)framed=framed && m_viewport->planePoint(QPointF(i==1 || i==2?w:0,i>=2?h:0),m_frame,view[size_t(i)].first,view[size_t(i)].second);
    auto inView=[&](const sketchsnap::Pick& p){
      int sides=0;
      for(size_t i=0;i<4;++i) {
        const auto &a=view[i],&b=view[(i+1)%4];
        const double c=(b.first-a.first)*(p.v-a.second)-(b.second-a.second)*(p.u-a.first);
        sides|=c>0?1:c<0?2:0;
      }
      return sides!=3;
    };
    if(framed)all.erase(std::remove_if(all.begin(),all.end(),[&](const sketchsnap::Pick& p){return !inView(p);}),all.end());
    const int shown=lock->stop>=0 && !all.empty()?lock->stop%int(all.size()):-1;
    const auto pick=shown>=0?all[size_t(shown)]:sketchsnap::along(lock->line,u,v,t,step,guides,curves);
    int at=shown;
    for(size_t i=0;at<0 && pick.by==By::Cross && i<all.size();++i)if(all[i].u==pick.u && all[i].v==pick.v)at=int(i);
    s.u=pick.u;s.v=pick.v;s.kind=Snap::Kind::Locked;s.target=lock->line.anchor;s.line=lock->line;s.onLine=true;s.ortho=!m_lock;
    s.horizontal=lock->horizontal;s.vertical=lock->vertical;s.holds=automatic?lock->holds:std::vector<Snap::Hold>{};s.grid=step>0 && pick.by==By::Guide;
    s.stops=int(all.size());s.stop=at;
    if(pick.other>=0){const auto& m=meaning[size_t(pick.other)];s.horizontal|=m.horizontal;s.vertical|=m.vertical;s.other=guides[size_t(pick.other)].anchor;if(automatic && m.hold.ref)s.holds.push_back(m.hold);}
    if(pick.curve>=0){s.curve=curveIds[size_t(pick.curve)];s.entity=automatic?s.curve:0;}
    s.point=pointAt(s.u,s.v);  // a point already there (one the locked line runs through): that one
  } else {
    const auto pick=sketchsnap::resolve(u,v,t,step,guides,angled?&ray:nullptr,curves);
    s.u=pick.u;s.v=pick.v;
    s.grid=pick.by==By::Node;
    auto follow=[&](int i){  // what a guide adds: its constraint, or the guide to draw; and the line Shift locks onto
      const auto& m=meaning[size_t(i)];
      s.horizontal|=m.horizontal;s.vertical|=m.vertical;
      if(m.kind!=Snap::Kind::None){s.kind=m.kind;s.target=m.target;}
      s.line=guides[size_t(i)];s.onLine=true;
      if(automatic && m.hold.ref)s.holds.push_back(m.hold);
    };
    switch(pick.by) {
      case By::Cross:  // two of the guides and the ray, or one of them and a curve (which the point then lies on)
        if(pick.guide>=0)follow(pick.guide);
        if(pick.other>=0)follow(pick.other);
        s.kind=Snap::Kind::Cross;s.onLine=false;
        s.target=pick.guide>=0?guides[size_t(pick.guide)].anchor:fid;
        s.other=pick.other>=0?guides[size_t(pick.other)].anchor:pick.ray && pick.guide>=0?fid:0;
        if(pick.curve>=0){s.curve=curveIds[size_t(pick.curve)];s.entity=automatic?s.curve:0;}
        break;
      case By::Node:  // a guide or the angle ray through the node names it; a curve through it holds it
        s.kind=Snap::Kind::Grid;
        if(pick.guide>=0)follow(pick.guide);
        else if(pick.ray){s.kind=Snap::Kind::Angle;s.target=fid;s.line=ray;s.onLine=true;}
        if(pick.curve>=0){s.curve=curveIds[size_t(pick.curve)];s.entity=automatic?s.curve:0;}
        {  // a point already on the node, or a hair off it (under the drawn cursor): that one, as its snap, never a second
           // point beside it (the object snaps above are measured from the hidden pointer, up to half a step away)
          const double hair=2*m_viewport->pixelSize();
          int there=0;
          double best=hair;
          for(size_t index:m_geometry->query(s.u-hair,s.v-hair,s.u+hair,s.v+hair).points) {
            if(index>=m_sk.points.size())continue;
            const auto& p=m_sk.points[index];
            if(from && p.id==fid)continue;
            if(!m_chain.empty() && p.id==m_chain.back())continue;
            if(const double d=std::hypot(p.x-s.u,p.y-s.v);d<best){best=d;there=p.id;}
          }
          if(there) {
            const auto& p=*m_sk.point(there);
            s.u=p.x;s.v=p.y;s.grid=best<1e-9*std::max(1.0,step);
            if(!s.grid)s.horizontal=s.vertical=false;  // off the node: no longer level with what the node was
            s.point=there;s.kind=Snap::Kind::Point;s.target=there;s.entity=s.curve=0;s.holds.clear();
          }
        }
        if(!s.onLine) {  // the pointer on a guide or the angle ray that misses the node: Shift locks onto it (lockOn; the prompt says so)
          double nearest=t;
          for(const auto& g:guides)if(const double d=sketchsnap::distance(g,u,v);d<nearest){nearest=d;s.line=g;s.onLine=true;}
          sketchsnap::Guide held{0,0,1,0,fid};
          if(!s.onLine && from && on.angle && sketchsnap::angleRay(fx,fy,u,v,on.angleStep*M_PI/180,t,held)){s.line=held;s.onLine=true;}
        }
        break;
      case By::Guide: follow(pick.guide);break;
      case By::Ray: s.kind=Snap::Kind::Angle;s.target=fid;s.line=ray;s.onLine=true;break;
      case By::Curve: s.target=curveIds[size_t(pick.curve)];s.entity=automatic?s.target:0;s.kind=Snap::Kind::Curve;break;
      case By::Pointer: break;
    }
  }
  if((s.horizontal || s.vertical) && s.kind==Snap::Kind::Grid)s.kind=Snap::Kind::None;  // the inference names it; the grid ring stays
  // Quantised back onto the step's last point: that point again, which the click ignores (no zero-length line).
  if(from && fid>0 && std::hypot(s.u-fx,s.v-fy)<1e-9*std::max(1.0,step)){s.point=fid;s.horizontal=s.vertical=false;s.holds.clear();}
  return s;
}

// The point the step's next click is measured from (UI-23).
bool SketchEditor::fromPoint(double& x, double& y, int& id) const {
  if (m_tool == "line" || m_tool == "spline") {
    const SkPoint* p = m_chain.empty() ? nullptr : pointOf(m_chain.back());
    if (p) x = p->x, y = p->y, id = p->id;
    return p;
  }
  static const QStringList directed = {"rect3", "circle2", "circle3", "arc3", "arcc", "polygon", "polygon_outer", "slot", "cslot", "arcslot", "ellipse", "conic", "control_spline"};
  const size_t n = m_clicks.size();
  if (!n || !directed.contains(m_tool)) return false;
  if (n == 2 && (m_tool == "arc3" || m_tool == "slot" || m_tool == "cslot" || m_tool == "ellipse" || m_tool == "rect3")) return false;  // a size or a side: no direction
  const Snap& c = n == 2 && (m_tool == "arcc" || m_tool == "arcslot") ? m_clicks[0] : m_clicks.back();  // an arc's end: about its centre
  x = c.u, y = c.v, id = c.point ? c.point : -1;
  return true;
}

int SketchEditor::pointFor(const Snap& s) {
  if (s.point) return s.point;
  const int id = m_sk.add_point(s.u, s.v);
  if (s.entity) m_sk.add_constraint(SkConstraint::Type::Coincident, {id, s.entity});
  for (const auto& h : s.holds)
    if ((m_sk.point(h.ref) || m_sk.entity(h.ref)) && !(h.type == SkConstraint::Type::Coincident && h.ref == s.entity)) m_sk.add_constraint(h.type, {id, h.ref});
  return id;
}

bool SketchEditor::pointHere() const {
  const size_t n = m_clicks.size();
  const QString& k = m_tool;
  if (k == "line" || k == "spline" || k == "point" || k == "control_spline" || k == "conic" || k == "rect" || k == "crect" || k == "polygon" || k == "polygon_outer") return true;  // a circumscribed one's side middle (P6)
  if (k == "circle") return n == 0;
  if (k == "arc3" || k == "arcc" || k == "slot" || k == "ellipse" || k == "rect3" || k == "arcslot") return n < 2;
  return (k == "cslot" || k == "tangent_arc") && n == 1;
}

bool SketchEditor::curveHere() const {
  const size_t n = m_clicks.size();
  const QString& k = m_tool;
  return k == "circle3" || k == "circle2" || (n == 1 && k == "circle") || (n == 2 && (k == "arc3" || k == "arcc" || k == "rect3"));
}

std::vector<snapmarkers::Glyph> SketchEditor::snapGlyphs(const Snap& s) const {
  using G = snapmarkers::Glyph;
  using CT = SkConstraint::Type;
  std::vector<G> out;
  auto add = [&](G g) {
    if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
  };
  const bool segment = m_tool == "line" && !m_chain.empty();  // the click ends a segment
  if (s.point) {
    if ((pointHere() || curveHere()) && !(segment && s.point == m_chain.back())) add(G::Coincident);
  } else if (pointHere()) {
    if (s.entity) add(G::OnCurve);
    for (const auto& h : s.holds) add(h.type == CT::Midpoint ? G::Midpoint : h.type == CT::Horizontal ? G::Horizontal : h.type == CT::Vertical ? G::Vertical : G::OnCurve);
  }
  if (segment)
    for (const auto& h : s.segment) add(h.type == CT::Tangent ? G::Tangent : G::Perpendicular);  // square to a line, through a circle's centre
  if (alignsHere() && s.horizontal) add(G::Horizontal);
  if (alignsHere() && s.vertical) add(G::Vertical);
  return out;
}

bool SketchEditor::alignsHere() const {
  const size_t n = m_clicks.size();
  const QString& k = m_tool;
  if (k == "line") return !m_chain.empty();
  if (k == "arcc" || k == "arcslot") return n == 1 || n == 2;
  return n == 1 && (k == "arc3" || k == "rect3" || k == "polygon" || k == "polygon_outer" || k == "slot" || k == "cslot" || k == "ellipse");
}

// ---------------------------------------------------------------- input
void SketchEditor::sketchPress(double u, double v, Qt::KeyboardModifiers mods) {
  invalidatePreview();
  dropPreviewJob();
  if (!m_active || m_editJob || m_geometryJob) return;
  // A press on nothing starts a selection window with the tools that act on selected curves (break link too: TODO 11
  // wave 3, P4). Not while mirror waits for its line: a miss there kept the window and dropped the curves to mirror.
  const bool mirrorLine=m_tool=="mirror" && option("mirrorAxis","picked")=="picked" && option("mirrorStage","seed")=="axis";
  const bool edgeSelection=!mirrorLine && QStringList{"offset","move","rotate","scale","copy","rect_pattern","polar_pattern","break","explode","mirror","break_link"}.contains(m_tool);
  if(edgeSelection && hitTest(u,v).kind==Hit::None){
    if(!(mods & (Qt::ShiftModifier|Qt::ControlModifier)))m_sel.clear();
    m_boxSelecting=true;m_dragU=m_boxU=u;m_dragV=m_boxV=v;rebuild();emit changed();return;
  }
  if ((m_tool=="select" || (m_tool=="spline" && m_chain.empty())) && mods.testFlag(Qt::AltModifier)) return insertSplineNode(u,v);
  if (m_dimEdit && m_dimEdit->isVisible()) commitDimensionEdit();
  if (m_tool == "trim") {  // a click trims at its release; a drag first is a fence (UI-28)
    m_fencing = true;
    m_fenceMoved = false;
    m_fenceU = m_fenceToU = u;
    m_fenceV = m_fenceToV = v;
    m_fenceMods = mods;
    return;
  }
  if (m_tool == "select") {
    const Hit h = hitTest(u, v);
    const bool add = mods & (Qt::ShiftModifier | Qt::ControlModifier);
    if (h.kind == Hit::None) {
      if (!add) m_sel.clear();
    } else {
      auto it = std::find(m_sel.begin(), m_sel.end(), h.id);
      if (it != m_sel.end() && add) m_sel.erase(it);
      else if (it == m_sel.end()) {
        if (!add) m_sel.clear();
        m_sel.push_back(h.id);
      }
    }
    // A press on geometry may become a drag.
    m_dragging = h.kind != Hit::None;
    m_dragMoved = false;m_dragPending=false;m_dragReleased=false;m_dragGrid=false;
    m_dragHit = h;
    m_dragU = u;
    m_dragV = v;
    m_dragStart.clear();
    m_boxSelecting=h.kind==Hit::None;
    m_boxU=u;m_boxV=v;
    if (h.kind == Hit::Point) {
      if (const SkPoint* p = m_sk.point(h.id)) m_dragStart.push_back({p->id, {p->x, p->y}});
    } else if (h.kind == Hit::Entity) {
      if (const SkEntity* e = m_sk.entity(h.id))
        for (int pid : e->p)
          if (const SkPoint* p = m_sk.point(pid)) m_dragStart.push_back({p->id, {p->x, p->y}});
    }
    rebuild();
    emit changed();
    return;
  }
  if (m_tool == "image_edit") {  // a press on a picture takes it to drag (P5); elsewhere nothing
    imagePress(u, v);
    return;
  }
  const Snap s = snap(u, v, !mods.testFlag(Qt::AltModifier));
  m_shiftUsed = true;  // Shift with a click is no tap
  if (m_lock) m_shiftSpent = mods.testFlag(Qt::ShiftModifier) || m_shiftDown;  // the lock was this click's: Shift again for the next
  if (pointTyped()) { useTyped(&s); return; }  // the typed values win
  const bool gridded = gridPoints();
  click(s, mods);
  if (m_active && gridPoints() != gridded && m_viewport->gridSnap()) resnap();  // a tangent arc's end: on the grid from now
}

bool SketchEditor::dragSnap(double u, double v, Qt::KeyboardModifiers mods, double& su, double& sv, double& r) {
  const double du = u - m_dragU, dv = v - m_dragV;
  su = du;
  sv = dv;
  r = 0;
  m_dropPoint = m_dropCurve = 0;
  // Grid snapping: the grabbed point (a curve's first one) lands on a grid node and the rest moves with it; a rim drag takes
  // whole steps of radius. Alt drags freely.
  const double step = m_viewport->gridSnap() && !mods.testFlag(Qt::AltModifier) ? m_viewport->gridStep() : 0;
  m_dragGrid = step > 0;
  m_dragCursorU = u;
  m_dragCursorV = v;
  const SkEntity* e = m_dragHit.kind == Hit::Entity ? m_sk.entity(m_dragHit.id) : nullptr;
  if (e && e->type == SkEntity::Type::Circle && !e->fixed) {  // the rim: the radius, the cursor on the rim where the hand is
    if (const SkPoint* c = m_sk.point(e->p[0])) {
      const double d = std::hypot(u - c->x, v - c->y);
      r = step > 0 ? std::max(step, sketchsnap::onGrid(d, step)) : std::max(1e-3, d);
      if (d > 1e-12) {
        m_dragGridU = m_dragCursorU = c->x + (u - c->x) * r / d;
        m_dragGridV = m_dragCursorV = c->y + (v - c->y) * r / d;
      }
    }
    return m_dragGrid;
  }
  // A dragged point is held to the point or curve it comes near (UI-28), before the grid: merged or put on it on drop.
  if (m_dragHit.kind == Hit::Point && !m_dragStart.empty() && !mods.testFlag(Qt::AltModifier) &&
      dropTarget(m_dragHit.id, u, v, m_dropU, m_dropV)) {
    const auto& at = m_dragStart.front().second;
    su = m_dropU - at.first;
    sv = m_dropV - at.second;
    m_dragGrid = false;
    return true;
  }
  if (step > 0 && !m_dragStart.empty()) {
    const auto& at = m_dragStart.front().second;
    m_dragGridU = sketchsnap::onGrid(at.first + du, step);
    m_dragGridV = sketchsnap::onGrid(at.second + dv, step);
    su = m_dragGridU - at.first;
    sv = m_dragGridV - at.second;
    // The cursor: on that node when a point is dragged; a curve's first point can be far from the hand (a line's start,
    // an arc's centre), so there it is where the hand took the curve, moved by the same whole steps.
    if (m_dragHit.kind == Hit::Point) m_dragCursorU = m_dragGridU, m_dragCursorV = m_dragGridV;
    else m_dragCursorU = m_dragU + su, m_dragCursorV = m_dragV + sv;
  }
  return m_dragGrid;
}

void SketchEditor::sketchMove(double u, double v, Qt::KeyboardModifiers mods, bool dragging) {
  if (!m_active) return;
  if(m_boxSelecting && dragging) {m_boxU=u;m_boxV=v;updateTransient();return;}
  if(m_imageDrag && dragging) {imageDragTo(u,v);return;}
  if(m_fencing && dragging) {
    m_fenceToU=u;m_fenceToV=v;
    m_fenceMoved=m_fenceMoved || std::hypot(u-m_fenceU,v-m_fenceV)>tol();
    updateTransient();return;
  }
  if (m_tool == "select" && dragging && m_dragging) {
    if(m_editJob) {  // the last solve still runs (a large sketch): the drawing cursor follows the hand meanwhile
      m_dragPending=true;m_dragNextU=u;m_dragNextV=v;m_dragNextMods=mods;
      double su,sv,radius;
      if(m_dragMoved && m_dragHit.kind!=Hit::Dimension && dragSnap(u,v,mods,su,sv,radius))updateTransient();
      return;
    }
    if (!m_dragMoved && std::hypot(u - m_dragU, v - m_dragV) < 0.5 * tol()) return;
    if (!m_dragMoved) {
      m_dragMoved = true;
      begin_change();
    }
    const double du = u - m_dragU, dv = v - m_dragV;
    if (m_dragHit.kind == Hit::Dimension) {
      if (SkConstraint* c = m_sk.constraint(m_dragHit.id)) {
        double lu, lv;
        labelPosition(m_before.constraints[static_cast<size_t>(c - &m_sk.constraints[0])], lu, lv);
        c->pos[0] = lu + du;
        c->pos[1] = lv + dv;
      }
      rebuild();
      return;
    }
    Sketch attempt = m_sk; // carry forward the last constrained solution during a drag
    SolveOptions opt=solveOptions();
    double su=du,sv=dv,radius=0;
    dragSnap(u,v,mods,su,sv,radius);
    if(radius>0)attempt.entity(m_dragHit.id)->r = radius;  // the rim (unless a dimension holds it: the solver pulls it back)
    else for (const auto& [pid, at] : m_dragStart) opt.drags.push_back({pid, at.first + su, at.second + sv});
    if(attempt.points.size()>300) {
      const auto result=std::make_shared<Sketch>(std::move(attempt));const auto solved=std::make_shared<SolveResult>();const int session=m_session;
      QPointer<SketchEditor> guard(this);
      m_editJob=m_jobs->async(tr("Solving sketch"),[result,solved,opt](Progress progress){if(!progress.cancelled())*solved=solve(*result,opt);},
        [this,guard,result,solved,session](bool ok,const QString& error){
          if(!guard||!m_active||session!=m_session)return;m_editJob=nullptr;
          if(ok&&solved->converged){m_sk=*result;m_solved=*solved;rebuild();}
          else if(!error.isEmpty())emit status(error);
          const bool released=m_dragReleased;
          if(m_dragPending){m_dragPending=false;sketchMove(m_dragNextU,m_dragNextV,m_dragNextMods,true);}
          if(released)sketchRelease(m_dragNextU,m_dragNextV,m_dragNextMods);
        });
      return;
    }
    try {
      const SolveResult r = solve(attempt, opt);
      if (r.converged) {
        m_sk = attempt;
        m_solved = r;
      }
    } catch (const std::exception&) {
    }
    rebuild();
    return;
  }
  const Hit h = hitTest(u, v);
  const bool shift=mods.testFlag(Qt::ShiftModifier);
  m_lastU=u;m_lastV=v;m_lastMods=mods;m_inView=true;
  if(m_lock && m_lock->stop>=0 && std::hypot(u-m_lock->su,v-m_lock->sv)>tol())m_lock->stop=-1;  // moved on: the pointer leads again
  if(m_snapChoice && std::hypot(u-m_choiceU,v-m_choiceV)>tol())m_snapChoice=0;  // moved on: the nearest object snap again
  if(!shift){m_shiftDown=m_shiftSpent=m_cyclePending=false;if(m_lock && !m_lock->sticky)unlock();}  // Shift let go where its key release was not seen
  else if(!m_lock && !m_shiftSpent && !(m_cyclePending && m_shiftClock.isValid() && m_shiftClock.elapsed()<300))lockOn();  // over several snaps: locks once it is no tap
  // Resting on a point acquires it (UI-19), also while the lock keeps the pointer's snap on its line.
  if(placing() && h.kind==Hit::Point){if(h.id!=m_dwellPoint){m_dwellPoint=h.id;m_dwellTimer.start();}}
  else{m_dwellPoint=0;m_dwellTimer.stop();}
  const Snap s = m_tool=="select"?Snap{u,v}:snap(u, v, !mods.testFlag(Qt::AltModifier));
  const bool redraw = h.kind != m_hover.kind || h.id != m_hover.id || m_tool != "select" || m_placingDim;
  const bool dimensionHover=h.kind==Hit::Dimension||m_hover.kind==Hit::Dimension;
  m_hover = h;
  m_pointer = s;
  m_cursor = typedPoint(s);  // typed values hold it
  m_haveCursor = true;
  trackSlotSweep();  // an arc slot's end: which way round the pointer went
  if(m_tool=="offset" && !dragging && !m_geometryJob)updateDimensionHandle();
  if (redraw) {if(m_placingDim||dimensionHover)rebuild();else updateTransient();}
  updateInput();  // the pointer's values, beside it
  noteHints();
}

// Off the view: no point or curve is under the pointer any more (a tool's rubber band keeps its last place).
void SketchEditor::sketchLeave() {
  m_inView = false;
  m_dwellPoint = 0;
  m_dwellTimer.stop();
  noteHints();
  if (!m_active) return;
  if (m_hover.kind == Hit::None) {
    if (m_drawnCursor && !m_dragging) updateTransient();  // the drawing cursor goes with the pointer
    return;
  }
  const bool dimension = m_hover.kind == Hit::Dimension;
  m_hover = Hit{};
  if (dimension) rebuild();
  else updateTransient();
}

bool SketchEditor::placing() const {
  // Snapping only means something to tools that place points; trim, offset, constraints and the like pick curves.
  static const QStringList tools = {"line", "rect", "crect", "circle", "circle2", "circle3", "arc3", "arcc", "polygon", "polygon_outer", "slot", "cslot", "arcslot",
                                    "ellipse", "spline", "control_spline", "point", "text", "conic", "rect3", "image_insert", "image_calibrate", "paste", "copybase"};
  return tools.contains(m_tool);
}

bool SketchEditor::gridPoints() const { return placing() || (m_tool == "tangent_arc" && m_clicks.size() == 1); }

void SketchEditor::altKey(bool pressed) {
  if (!m_active || m_lastMods.testFlag(Qt::AltModifier) == pressed) return;
  m_lastMods.setFlag(Qt::AltModifier, pressed);
  resnap();
}

bool SketchEditor::followPointer() {
  if (!m_active || m_dragging || m_boxSelecting || m_tool == "select" || !m_viewport->isVisible()) return false;
  const QPoint global = QCursor::pos(), at = m_viewport->mapFromGlobal(global);
  if (QApplication::widgetAt(global) != m_viewport) return false;  // off the view, over an overlay on it or a window over it
  double u, v;
  if (!m_viewport->planePoint(QPointF(at), m_frame, u, v)) return false;
  sketchMove(u, v, m_lastMods, false);
  return true;
}

// Shift locks the pointer onto the guide or angle ray it is on; off them, onto the way to it from the last point (of the
// polyline, or the shape's last click), else from the newest tracked point.
bool SketchEditor::lockOn() {
  if (m_lock || !m_haveCursor || !placing()) return false;
  // With grid snapping the pointer's point is a node: the guide or the angle ray the pointer itself is on, and what it
  // means, are as without the grid (along the lock the stops are then on grid lines).
  const Snap on = m_viewport->gridSnap() && m_inView ? snap(m_lastU, m_lastV, !m_lastMods.testFlag(Qt::AltModifier), false) : m_pointer;
  Lock lock;
  if (on.onLine) {
    lock.line = on.line;
    lock.horizontal = on.horizontal;
    lock.vertical = on.vertical;
    lock.holds = on.holds;  // level with a tracked point, on a line's extension: still so along it
  } else {
    const SkPoint* p = m_sk.point(!m_chain.empty() ? m_chain.back() : !m_clicks.empty() ? m_clicks.back().point : m_tracked.empty() ? 0 : m_tracked.back());
    if (!p && m_clicks.empty()) return false;
    const double x = p ? p->x : m_clicks.back().u, y = p ? p->y : m_clicks.back().v, dx = m_pointer.u - x, dy = m_pointer.v - y, len = std::hypot(dx, dy);  // to the cursor
    if (!(len > 1e-9)) return false;
    lock.line = {x, y, dx / len, dy / len, p ? p->id : 0};
  }
  m_lock = lock;
  return true;
}

void SketchEditor::unlock() {
  if (!m_lock) return;
  const bool sticky = m_lock->sticky;
  m_lock.reset();
  if (sticky) emit changed();  // the prompt no longer offers Esc for it
}

// Shift (UI-19): pressed, it locks; let go at once (a tap) the lock stays until a click or Esc, held it lasts while Shift
// is down. A tap on a lock that stays shows its next stop (sketchkeys::shift), with none it lets go. Shift with another
// key or a click is no tap.
void SketchEditor::shiftKey(bool pressed) {
  if (!m_active || pressed == m_shiftDown) return;
  m_shiftDown = pressed;
  m_lastMods.setFlag(Qt::ShiftModifier, pressed);
  if (pressed) {
    m_shiftClock.start();
    m_shiftUsed = false;
    m_unstick = m_lock && m_lock->sticky;
    m_cyclePending = !m_lock && m_inView && m_pointer.choices > 1;  // several object snaps in reach: a tap shows the next (UI-23)
    if (!m_lock && m_inView && !m_shiftSpent && !m_cyclePending) lockOn();
  } else {
    const bool tap = !m_shiftUsed && m_shiftClock.isValid() && m_shiftClock.elapsed() < 300;
    m_shiftSpent = false;
    if (m_cyclePending && tap && !m_lock) {  // the next object snap in reach, counted from here while the pointer stays
      if (!m_snapChoice) m_choiceU = m_lastU, m_choiceV = m_lastV;
      m_snapChoice = (m_pointer.choice + 1) % m_pointer.choices;
    } else if (m_lock && m_lock->sticky && m_unstick && tap) {
      if (sketchkeys::shift(keyState()) == sketchkeys::Shift::NextStop) {
        if (m_lock->stop < 0) m_lock->su = m_lastU, m_lock->sv = m_lastV;  // counted from here while the pointer stays
        m_lock->stop = (m_pointer.stop + 1) % m_pointer.stops;
      } else unlock();
    } else if (m_lock && !m_lock->sticky && !tap) unlock();
    else if (m_lock && tap && !m_lock->sticky) {
      m_lock->sticky = true;
      emit changed();  // the prompt offers Esc to let go
    }
    m_cyclePending = false;
  }
  resnap();
}

void SketchEditor::refreshSnap() {
  readSettings();
  if (m_active) resnap();
}

void SketchEditor::readSettings() {
  const QSettings s;
  auto kind = [&](const char* name) { return s.value(QString("sketch/snap/") + name, true).toBool(); };
  Settings& o = m_settings;
  o.endpoint = kind("endpoint"), o.midpoint = kind("midpoint"), o.center = kind("center"), o.quadrant = kind("quadrant"), o.intersection = kind("intersection");
  o.apparent = kind("apparent"), o.perpendicular = kind("perpendicular"), o.tangent = kind("tangent"), o.nearest = kind("nearest"), o.angle = kind("angle");
  o.inference = kind("inference");
  o.extensions = s.value("view/extensions", true).toBool(), o.tracking = s.value("view/tracking", true).toBool(), o.ortho = s.value("view/orthoSnap", false).toBool();
  o.angleStep = s.value("sketch/angleStep", 15).toDouble();
  o.tolerance = s.value("sketch/tolerance", 1e-8).toDouble(), o.iterations = s.value("sketch/iterations", 100).toInt();
  ++m_settingsReads;
}

void SketchEditor::resnap() {
  if (m_active && m_inView && m_haveCursor && !m_dragging && !m_boxSelecting && m_tool != "select") sketchMove(m_lastU, m_lastV, m_lastMods, false);
  else updateTransient();
}

void SketchEditor::sketchRelease(double u, double v, Qt::KeyboardModifiers) {
  if(m_active && m_imageDrag) {imageDragTo(u,v);imageRelease();return;}
  if(m_active && m_fencing) {
    m_fencing=false;
    if(m_fenceMoved)fenceTrim(m_fenceU,m_fenceV,u,v);
    else {const Snap s=snap(m_fenceU,m_fenceV,!m_fenceMods.testFlag(Qt::AltModifier));click(s,m_fenceMods);}
    updateTransient();emit changed();return;
  }
  if(m_active && m_boxSelecting) {
    m_boxSelecting=false;
    if(m_geometry && !m_geometryJob && std::hypot(u-m_dragU,v-m_dragV)>tol()) {
      const double x0=std::min(u,m_dragU),x1=std::max(u,m_dragU),y0=std::min(v,m_dragV),y1=std::max(v,m_dragV);
      const bool crossing=u<m_dragU;
      auto inside=[&](double x,double y){return x>=x0 && x<=x1 && y>=y0 && y<=y1;};
      auto crosses=[&](double ax,double ay,double bx,double by) {
        double low=0,high=1;
        auto clip=[&](double p,double q){if(std::fabs(p)<1e-15)return q>=0;double r=q/p;if(p<0)low=std::max(low,r);else high=std::min(high,r);return low<=high;};
        return clip(ax-bx,ax-x0)&&clip(bx-ax,x1-ax)&&clip(ay-by,ay-y0)&&clip(by-ay,y1-ay);
      };
      const auto candidates=m_geometry->query(x0-tol()*.1,y0-tol()*.1,x1+tol()*.1,y1+tol()*.1);
      std::set<int> selected(m_sel.begin(),m_sel.end());
      for(size_t index:candidates.entities){if(index>=m_sk.entities.size())continue;const auto& e=m_sk.entities[index];if(!selectable(e.id))continue;
        const auto pts=sampled(e);bool all=!pts.empty(),any=false;
        for(size_t i=0;i<pts.size();++i) {bool in=inside(pts[i].first,pts[i].second);all&=in;any|=in;if(i)any|=crosses(pts[i-1].first,pts[i-1].second,pts[i].first,pts[i].second);}
        if(crossing?any:all)if(selected.insert(e.id).second)m_sel.push_back(e.id);
      }
      if(m_tool=="select")for(size_t index:candidates.points){if(index>=m_sk.points.size())continue;const auto& p=m_sk.points[index];if(selectable(p.id)&&inside(p.x,p.y)&&selected.insert(p.id).second)m_sel.push_back(p.id);}
      if(m_tool=="select")for(const auto& c:m_sk.constraints)if(selectable(c.id)) {double x,y;labelPosition(c,x,y);if(inside(x,y))m_sel.push_back(c.id);}
    }
    if(m_tool=="offset" && chainOnClick())selectConnected();
    if(m_tool=="break_link")m_sel.erase(std::remove_if(m_sel.begin(),m_sel.end(),[this](int id){const auto* e=m_sk.entity(id);return !e || e->source.is_null();}),m_sel.end());  // linked curves only
    rebuild();emit changed();if(m_tool!="select"){toolPrompt();scheduleToolPreview();}return;
  }
  if (!m_active || !m_dragging) return;
  if(m_editJob){m_dragReleased=true;return;}
  m_dragReleased=false;
  m_dragging = false;
  if (!m_dragMoved) return;
  m_dragMoved = false;
  // Dropped on a point: merged into it; on a curve: kept on it (UI-28), when the sketch still solves so (else it stays
  // where the drag left it). A large sketch is solved so on a worker (as end_change does), in the drag's undo step.
  if ((m_dropPoint || m_dropCurve) && m_sk.points.size() > 300) {
    const auto attempt = std::make_shared<Sketch>(m_sk);
    const auto result = std::make_shared<SolveResult>();
    const int dragged = m_dragHit.id, point = m_dropPoint, curve = m_dropCurve, session = m_session;
    const auto options = solveOptions();
    std::vector<ParamDef> defs;
    for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    QPointer<SketchEditor> guard(this);
    m_editJob = m_jobs->async(point ? tr("Merging points") : tr("Putting the point on the curve"), [attempt, result, dragged, point, curve, options, defs](Progress progress) {
      if (progress.cancelled()) return;
      if (point) merge_points(*attempt, dragged, point);
      else attempt->add_constraint(SkConstraint::Type::Coincident, {dragged, curve});
      evaluate_dimensions(*attempt, ParamTable(defs));
      *result = solve(*attempt, options);
      if (!result->converged) throw opad::Error("the sketch would not solve so");
    }, [this, guard, attempt, result, point, curve, session](bool ok, const QString& error) {
      if (!guard || !m_active || session != m_session) return;
      m_editJob = nullptr;
      ++m_modelRevision;
      if (ok) {
        m_sk = std::move(*attempt);
        m_solved = *result;
        m_sel.clear();
        emit status(point ? tr("Merged with point %1").arg(point) : tr("Put on curve %1").arg(curve));
      } else if (!error.isEmpty()) {
        emit status(tr("Not joined: %1").arg(i18n::t(error)));
      }
      scheduleFill();
      rebuild();
      emit changed();
    });
    m_dropPoint = m_dropCurve = 0;
  } else if (m_dropPoint || m_dropCurve) {
    Sketch attempt = m_sk;
    try {
      if (m_dropPoint) merge_points(attempt, m_dragHit.id, m_dropPoint);
      else attempt.add_constraint(SkConstraint::Type::Coincident, {m_dragHit.id, m_dropCurve});
      std::vector<ParamDef> defs;
      for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
      evaluate_dimensions(attempt, ParamTable(defs));
      const SolveResult r = solve(attempt, solveOptions());
      if (!r.converged) throw opad::Error("the sketch would not solve so");
      m_sk = std::move(attempt);
      m_solved = r;
      m_sel.clear();
      emit status(m_dropPoint ? tr("Merged with point %1").arg(m_dropPoint) : tr("Put on curve %1").arg(m_dropCurve));
    } catch (const std::exception& e) {
      emit status(tr("Not joined: %1").arg(i18n::t(QString::fromUtf8(e.what()))));
    }
    m_dropPoint = m_dropCurve = 0;
  }
  // The drag already solved every step; record it as one undo step.
  m_inChange = false;
  m_undo.push_back({m_before,m_beforePlane,m_beforeFrame});
  m_redo.clear();
  m_modified = true;
  scheduleFill();
  rebuild();
  emit changed();
}

void SketchEditor::sketchDoubleClick(double u, double v) {
  if (!m_active) return;
  if (m_tool == "line" || (m_tool == "spline" && !m_chain.empty())) return finishChain();
  const Hit h = hitTest(u, v);
  if((m_tool=="select" || m_tool=="spline") && h.kind==Hit::Point) {m_sel={h.id};editSplineNode();return;}
  if (h.kind == Hit::Dimension) editDimension(h.id, false);
}

// At the shortcut override (from the view, a tool panel or the main window): which keys are the sketch's, so no window
// shortcut sees them, and the editing keys act. Keys that type values act when they are pressed (sketchType): the box
// they start takes the keyboard for the keys that follow, not for this one.
bool SketchEditor::sketchKey(QKeyEvent* e) {
  if (!m_active) return false;
  if (typingKey(e)) return true;  // also while a job runs: a digit never falls through to a shortcut
  if((e->modifiers()&~Qt::KeypadModifier)!=Qt::NoModifier)return false;  // the keypad's Enter is Enter
  const int key = e->key();
  if (key != Qt::Key_Escape && key != Qt::Key_Return && key != Qt::Key_Enter && key != Qt::Key_Backspace && key != Qt::Key_Delete) return false;
  dropPreviewJob();  // the editing keys act on the sketch as it is (Enter right after a value: not lost to its preview)
  if(m_editJob)return false;
  switch (key) {
    case Qt::Key_Escape:
      escape();
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      return done();
    case Qt::Key_Backspace:
      if (m_input->backspace()) return true;  // a value typed over the view (its box did not get the keyboard)
      undoPoint();  // never deletes curves while a tool runs, never leaves the tool
      return true;
    case Qt::Key_Delete:
      deleteSelection();
      return true;
    default:
      return false;
  }
}

void SketchEditor::deleteSelection() {
  if(m_editJob)return;
  if (m_sel.empty()) return;
  begin_change();
  for (int id : m_sel) {
    if (isFixedPoint(id)) continue;
    // A point that curves hang on goes with its curves only when it was picked alone and on purpose: keep it.
    m_sk.remove(id);
  }
  m_sel.clear();
  end_change(tr("Delete"));
}

void SketchEditor::toggleConstruction() {
  if(m_editJob)return;
  bool any = false;
  begin_change();
  for (int id : m_sel)
    if (SkEntity* e = m_sk.entity(id)) {
      e->construction = !e->construction;
      any = true;
    }
  if (!any) {
    cancel_change();
    emit status(help::expand(tr("Select curves first, then Construction ({key:sketch.construction}) turns them into construction geometry (and back).")));
    return;
  }
  end_change(tr("Construction"));
}

// ---------------------------------------------------------------- drawing
bool SketchEditor::prepareGeometry() {
  const double deflection=std::max(1e-7,m_viewport->pixelSize()*.25);
  if(m_geometry && m_geometry->matches(m_sk,deflection)) {
    if(m_geometryJob){++m_geometryRevision;m_geometryJob->cancel();m_geometryJob=nullptr;}
    return true;
  }
  if(m_geometryJob)return false; // Coalesce redraws; completion rechecks the current geometry and zoom.
  const int revision=++m_geometryRevision;
  if(m_geometryJob)m_geometryJob->cancel();m_geometryJob=nullptr;
  m_samplePixelSize=m_viewport->pixelSize();
  if(m_sk.points.size()<=300) {
    m_geometry=m_geometry?std::make_shared<SketchGeometryCache>(*m_geometry):std::make_shared<SketchGeometryCache>();
    m_geometry->update(m_sk,deflection);return true;
  }
  auto snapshot=std::make_shared<Sketch>(m_sk);const auto previous=m_geometry;
  auto result=std::make_shared<std::shared_ptr<SketchGeometryCache>>();QPointer<SketchEditor> guard(this);
  m_jobs->backgroundNext();
  m_geometryJob=m_jobs->async(tr("Preparing sketch curves"),[snapshot,previous,result,deflection](Progress p){
    if(p.cancelled())return;
    *result=previous?std::make_shared<SketchGeometryCache>(*previous):std::make_shared<SketchGeometryCache>();
    (*result)->update(*snapshot,deflection);
  },[this,guard,result,revision](bool ok,const QString& error){
    if(!guard||!m_active||revision!=m_geometryRevision)return;m_geometryJob=nullptr;
    if(!ok){emit status(error);return;}
    m_geometry=*result;rebuild();
    resnap();  // meanwhile the pointer had the grid alone: its object snaps again, so the cursor drawn and a click agree
  });return false;
}

void SketchEditor::rebuild() {
  showSources();
  if(m_prs.IsNull() || !prepareGeometry())return;
  refreshImages();
  if (m_prs.IsNull()) return;
  const Tokens& t = m_viewport->tokens();
  SketchPrs& d = *static_cast<SketchPrs*>(m_prs.get());
  d.solid.clear();
  d.dashed.clear();
  d.thin.clear();
  d.marks.clear();
  d.badges.clear();
  d.points.clear();
  d.bigPoints.clear();
  d.dots.clear();
  d.rings.clear();
  d.texts.clear();
  d.glow.clear();
  d.glowPoints.clear();
  const highlight::Selection role = highlight::selection(t, QColor(), false);  // the sketch's lines lie on the background
  d.glowColor = role.halo;
  d.glowAlpha = float(role.haloAlpha);
  d.fill = m_fill;
  d.fillColor = t.sel;
  m_glyphHits.clear();
  d.textBack = t.bg2;
  d.scale = m_viewport->displayScale();
  d.font = theme::ui().family().toStdString();
  auto W = [&](double u, double v) { return m_frame.to_world(u, v); };
  const double px = m_viewport->pixelSize();
  m_samplePixelSize=px;
  const std::set<int> freePts(m_solved.free_points.begin(), m_solved.free_points.end());
  const std::set<int> selected(m_sel.begin(), m_sel.end());
  const std::set<int> picked(m_picked.begin(), m_picked.end());
  // What a selected constraint or dimension holds lights up with it (TODO 11 wave 3, P6: a row picked in the Constraints
  // list, a badge clicked), as on hover.
  std::set<int> held;
  if (!selected.empty())
    for (const auto& c : m_sk.constraints)
      if (selected.count(c.id)) held.insert(c.refs.begin(), c.refs.end());
  auto entityColor = [&](const SkEntity& e) {
    if (selected.count(e.id) || picked.count(e.id) || held.count(e.id)) return t.selected3d;
    if (!e.source.is_null())return t.amber;
    if (e.fixed) return t.green;
    bool free = false;
    for (int pid : e.p) free = free || freePts.count(pid) > 0;
    if (e.type == SkEntity::Type::Circle || e.type == SkEntity::Type::Ellipse) {
      // A radius nothing holds is a freedom too; the solver reports only points, so look for what pins it.
      bool held = false;
      for (const auto& c : m_sk.constraints)
        if (!c.reference && std::find(c.refs.begin(), c.refs.end(), e.id) != c.refs.end() &&
            (c.type == SkConstraint::Type::Radius || c.type == SkConstraint::Type::Diameter || c.type == SkConstraint::Type::Equal || c.type == SkConstraint::Type::Tangent || c.type == SkConstraint::Type::Coincident || c.type == SkConstraint::Type::Fix))
          held = true;
      free = free || !held;
    }
    return free ? t.sel : t.fg;  // blue while it can still move, neutral once fully defined
  };

  // The plane's axes through the origin, faint.
  const double reach = 4000 * px;
  d.thin.push_back({W(-reach, 0), W(reach, 0), QColor(200, 70, 70, 255)});
  d.thin.push_back({W(0, -reach), W(0, reach), QColor(70, 170, 70, 255)});

  for (const auto& e : m_sk.entities) {
    const QColor c = entityColor(e);
    if (e.type == SkEntity::Type::Point) continue;
    const auto pts = sampled(e);
    auto& into = e.construction ? d.dashed : d.solid;
    for (size_t i = 0; i + 1 < pts.size(); ++i) into.push_back({W(pts[i].first, pts[i].second), W(pts[i + 1].first, pts[i + 1].second), c});
    if (selected.count(e.id) || picked.count(e.id) || held.count(e.id))
      for (size_t i = 0; i + 1 < pts.size(); ++i) d.glow.push_back({W(pts[i].first, pts[i].second), W(pts[i + 1].first, pts[i + 1].second), c});
  }
  for (const auto& p : m_sk.points) {
    const bool hot = selected.count(p.id) || picked.count(p.id) || held.count(p.id);
    const QColor c = hot ? t.selected3d : p.fixed ? t.green : freePts.count(p.id) ? t.sel : t.fg;
    (hot ? d.bigPoints : !p.fixed && freePts.count(p.id) ? d.rings : d.points).push_back({W(p.x, p.y), c});  // free: a ring without its dot
    if (hot) d.glowPoints.push_back({W(p.x, p.y), c});
    if(m_dangling.count(p.id)) d.bigPoints.push_back({W(p.x,p.y),t.red});
  }

  // Constraint badges next to what they hold (UI-24): a pictogram in a badge, green (red in conflict, the hover colour when
  // selected or hovered), laid out so that none covers another; a coincidence is a dot on its point, the explicit ones (a
  // point on a curve) and where curves end on one point (they share it: no constraint). With Show constraints off only the
  // conflicting and selected ones show.
  // One badge of a kind per curve: a hexagon's first side holds five "equal"s and a slot's caps two tangents each, which drew
  // rows of identical badges. The others stay reachable through the badge on the other curve.
  using G = snapmarkers::Glyph;
  double rx = px, ry = 0, ux = 0, uy = px;  // one pixel right and up on the screen, in sketch coordinates
  pixelAxes(rx, ry, ux, uy);
  const double det = rx * uy - ux * ry;
  const opad::Vec3 normal = m_frame.normal(), look = m_viewport->viewDirection();
  const double toward = normal[0] * look[0] + normal[1] * look[1] + normal[2] * look[2] > 0 ? -1 : 1;
  auto lifted = [&](double u, double v, double pixels) {  // off the plane towards the viewer: badges cover the curves under them
    opad::Vec3 w = W(u, v);
    for (int i = 0; i < 3; ++i) w[i] += normal[i] * toward * pixels * px;
    return w;
  };
  auto glyphOf = [](SkConstraint::Type type) -> std::optional<G> {
    switch (type) {
      case SkConstraint::Type::Horizontal: return G::Horizontal;
      case SkConstraint::Type::Vertical: return G::Vertical;
      case SkConstraint::Type::Parallel: return G::Parallel;
      case SkConstraint::Type::Perpendicular: return G::Perpendicular;
      case SkConstraint::Type::Tangent: return G::Tangent;
      case SkConstraint::Type::Smooth: return G::Smooth;
      case SkConstraint::Type::Curvature: return G::Curvature;
      case SkConstraint::Type::Equal: return G::Equal;
      case SkConstraint::Type::Concentric: return G::Concentric;
      case SkConstraint::Type::Midpoint: return G::Midpoint;
      case SkConstraint::Type::Symmetric: return G::Symmetric;
      case SkConstraint::Type::Collinear: return G::Collinear;
      case SkConstraint::Type::Fix: return G::Fix;
      default: return std::nullopt;  // coincident: a dot on its point; dimensions draw themselves
    }
  };
  struct Badge { int id; G glyph; snapmarkers::Place at; QColor color; bool conflict; };  // conflict: red and marked "!" (not by colour alone, UI-124)
  std::vector<Badge> badges;
  std::set<std::pair<int, int>> shownGlyphs;
  m_coincidentDots.clear();
  // Selected: the selection's role (UI-38); under the pointer: the hover's colour (UI-24).
  auto colourOf = [&](const SkConstraint& c) {
    return m_conflicts.count(c.id) ? t.red : selected.count(c.id) ? t.selected3d : m_hover.kind == Hit::Dimension && m_hover.id == c.id ? t.hov : t.green;
  };
  for (const auto& c : m_sk.constraints) {
    if (c.is_dimension() || c.refs.empty()) continue;
    if (!m_showConstraints && !m_conflicts.count(c.id) && !selected.count(c.id)) continue;
    if (!i18n::t(QString::fromLatin1(SkConstraint::type_name(c.type))).contains(m_constraintFilter, Qt::CaseInsensitive)) continue;
    if (c.type == SkConstraint::Type::Coincident) {  // a dot on the point it holds
      if (const SkPoint* p = m_geometry->point(m_sk, c.refs[0])) {
        d.dots.push_back({lifted(p->x, p->y, 2), colourOf(c)});
        m_coincidentDots.push_back({c.id, p->x, p->y});
      }
      continue;
    }
    const auto glyph = glyphOf(c.type);
    if (!glyph || !(std::fabs(det) > 0)) continue;
    for (int ref : c.refs) {
      double gu = 0, gv = 0;
      if (const SkEntity* e = m_sk.entity(ref)) {
        const auto pts = sampled(*e);
        if (pts.empty()) continue;
        gu = pts[pts.size() / 2].first;
        gv = pts[pts.size() / 2].second;
        if (e->type == SkEntity::Type::Line) { gu = (pts[0].first + pts[1].first) / 2; gv = (pts[0].second + pts[1].second) / 2; }
      } else if (const SkPoint* p = m_geometry->point(m_sk, ref)) {
        gu = p->x;
        gv = p->y;
      } else {
        continue;
      }
      if (!shownGlyphs.insert({ref, int(*glyph)}).second && !selected.count(c.id) && !m_conflicts.count(c.id)) continue;
      badges.push_back({c.id, *glyph, {(gu * uy - ux * gv) / det, (rx * gv - gu * ry) / det}, colourOf(c), m_conflicts.count(c.id) > 0});  // in pixels
      if (c.type == SkConstraint::Type::Midpoint || c.type == SkConstraint::Type::Symmetric || c.type == SkConstraint::Type::Fix) break;  // one badge is enough
    }
  }
  m_joinDots.clear();
  if (i18n::t(QString::fromLatin1(SkConstraint::type_name(SkConstraint::Type::Coincident))).contains(m_constraintFilter, Qt::CaseInsensitive)) {
    std::unordered_map<int, int> ends;  // curves ending on each point; a closed curve's ends count once
    for (const auto& e : m_sk.entities) {
      if (e.p.empty() || e.periodic) continue;
      const bool line = e.type == SkEntity::Type::Line || e.type == SkEntity::Type::Spline, arc = e.type == SkEntity::Type::Arc && e.p.size() == 3;
      const int a = line ? e.p.front() : arc ? e.p[1] : e.type == SkEntity::Type::Point ? e.p[0] : 0, b = line ? e.p.back() : arc ? e.p[2] : a;
      if (!a) continue;
      ++ends[a];
      if (b != a) ++ends[b];
    }
    for (const auto& [id, n] : ends)
      if (n > 1 && (m_showConstraints || selected.count(id)))
        if (const SkPoint* p = m_geometry->point(m_sk, id)) { m_joinDots.push_back(id); d.dots.push_back({lifted(p->x, p->y, 2), selected.count(id) || picked.count(id) ? t.hov : t.green}); }
    std::sort(m_joinDots.begin(), m_joinDots.end());
  }
  std::vector<snapmarkers::Place> anchors;
  for (const auto& b : badges) anchors.push_back(b.at);
  const auto placed = snapmarkers::layoutBadges(anchors, 16, 2);
  for (size_t i = 0; i < badges.size(); ++i) {
    const double cu = placed[i].x * rx + placed[i].y * ux, cv = placed[i].x * ry + placed[i].y * uy;  // back in sketch coordinates
    auto at = [&](double x, double y, double lift) { return lifted(cu + x * rx + y * ux, cv + x * ry + y * uy, lift); };
    for (const auto& corner : {std::array<double, 6>{-8, -8, 8, -8, 8, 8}, std::array<double, 6>{-8, -8, 8, 8, -8, 8}})
      for (int k = 0; k < 3; ++k) d.badges.push_back(at(corner[size_t(2 * k)], corner[size_t(2 * k + 1)], 1));
    for (const auto& s : snapmarkers::badge(16, 16, 4)) d.marks.push_back({at(s.x0, s.y0, 2), at(s.x1, s.y1, 2), badges[i].color == t.green ? t.line : badges[i].color});
    for (const auto& s : snapmarkers::glyph(badges[i].glyph, 11)) d.marks.push_back({at(s.x0, s.y0, 2), at(s.x1, s.y1, 2), badges[i].color});
    if (badges[i].conflict) d.texts.push_back({at(10, 10, 2), QStringLiteral("!"), badges[i].color, true});
    m_glyphHits.push_back({badges[i].id, cu, cv});
  }
  d.badgeColor = t.bg2;

  // Dimensions.
  auto dimension = [&](const SkConstraint& c, bool pending) {
    // Selected: the selection colour; under the pointer: full-strength ink (white text would vanish on the light theme's
    // label boxes); being placed: the tool's rubber-band colour.
    const QColor col = selected.count(c.id) ? t.selected3d : m_hover.kind == Hit::Dimension && m_hover.id == c.id ? t.fg : pending ? t.hov : t.fg2;
    double lu, lv;
    labelPosition(c, lu, lv);
    auto P = [&](int id, double& x, double& y) {
      if (const SkPoint* p = m_geometry->point(m_sk,id)) { x = p->x; y = p->y; return true; }
      return false;
    };
    auto ends = [&](double& ax, double& ay, double& bx, double& by) {
      if (c.refs.size() == 1 && P(c.refs[0], bx, by)) {  // a coordinate: from the sketch origin
        ax = ay = 0;
        return true;
      }
      if (c.refs.size() == 1) {
        const SkEntity* e = m_sk.entity(c.refs[0]);
        return e && e->p.size() >= 2 && P(e->p[0], ax, ay) && P(e->p[1], bx, by);
      }
      return c.refs.size() == 2 && P(c.refs[0], ax, ay) && P(c.refs[1], bx, by);
    };
    double ax, ay, bx, by;
    using T = SkConstraint::Type;
    if ((c.type == T::Distance || c.type == T::HDistance || c.type == T::VDistance) && ends(ax, ay, bx, by)) {
      // Extension lines to a dimension line through the label.
      double dx = bx - ax, dy = by - ay;
      if (c.type == T::HDistance) dy = 0;
      if (c.type == T::VDistance) dx = 0;
      const double len = std::hypot(dx, dy);
      if (len > 1e-9) {
        const double tx = dx / len, ty = dy / len, nx = -ty, ny = tx;
        const double off = (lu - ax) * nx + (lv - ay) * ny;
        const double a2x = ax + nx * off, a2y = ay + ny * off;
        const double offb = (lu - bx) * nx + (lv - by) * ny;
        const double b2x = bx + nx * offb, b2y = by + ny * offb;
        d.thin.push_back({W(ax, ay), W(a2x, a2y), col});
        d.thin.push_back({W(bx, by), W(b2x, b2y), col});
        d.thin.push_back({W(a2x, a2y), W(b2x, b2y), col});
        lu += nx * 9 * px * (off < 0 ? -1 : 1);  // the value sits beside its dimension line, not on it
        lv += ny * 9 * px * (off < 0 ? -1 : 1);
        for (const auto& [ex, ey, s] : {std::tuple{a2x, a2y, 1.0}, std::tuple{b2x, b2y, -1.0}}) {  // arrow heads
          d.thin.push_back({W(ex, ey), W(ex + s * (tx * 9 - nx * 3) * px, ey + s * (ty * 9 - ny * 3) * px), col});
          d.thin.push_back({W(ex, ey), W(ex + s * (tx * 9 + nx * 3) * px, ey + s * (ty * 9 + ny * 3) * px), col});
        }
      }
    } else if (c.type == T::Distance && c.refs.size() == 2) {  // point-line or line-line: from the point to its foot
      const SkEntity* line = m_sk.entity(c.refs[1]);
      const SkEntity* first = m_sk.entity(c.refs[0]);
      double qx = 0, qy = 0;
      const bool havePoint = first ? (first->p.size() >= 1 && P(first->p[0], qx, qy)) : P(c.refs[0], qx, qy);
      if (line && line->p.size() >= 2 && havePoint && P(line->p[0], ax, ay) && P(line->p[1], bx, by)) {
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        const double k = len2 < 1e-18 ? 0 : ((qx - ax) * dx + (qy - ay) * dy) / len2;
        d.thin.push_back({W(qx, qy), W(ax + k * dx, ay + k * dy), col});
      }
    } else if ((c.type == T::Radius || c.type == T::Diameter) && !c.refs.empty()) {
      const SkEntity* e = m_sk.entity(c.refs[0]);
      if (e && !e->p.empty() && P(e->p[0], ax, ay)) {
        const double r = e->type == SkEntity::Type::Arc && e->p.size() > 1 && P(e->p[1], bx, by) ? std::hypot(bx - ax, by - ay) : e->r;
        const double dl = std::hypot(lu - ax, lv - ay);
        const double ux = dl > 1e-9 ? (lu - ax) / dl : 1, uy = dl > 1e-9 ? (lv - ay) / dl : 0;
        const double from = c.type == T::Diameter ? -r : 0;
        d.thin.push_back({W(ax + ux * from, ay + uy * from), W(ax + ux * std::max(r, dl), ay + uy * std::max(r, dl)), col});
      }
    } else if (c.type == T::Angle && c.refs.size() == 2) {
      const SkEntity *l1 = m_sk.entity(c.refs[0]), *l2 = m_sk.entity(c.refs[1]);
      double cx, cy, ex, ey;
      if (l1 && l2 && l1->p.size() >= 2 && l2->p.size() >= 2 && P(l1->p[0], ax, ay) && P(l1->p[1], bx, by) && P(l2->p[0], cx, cy) && P(l2->p[1], ex, ey)) {
        // An arc about the lines' intersection, through the label.
        const double d1x = bx - ax, d1y = by - ay, d2x = ex - cx, d2y = ey - cy, den = d1x * d2y - d1y * d2x;
        double ox = ax, oy = ay;
        if (std::fabs(den) > 1e-12) {
          const double k = ((cx - ax) * d2y - (cy - ay) * d2x) / den;
          ox = ax + k * d1x;
          oy = ay + k * d1y;
        }
        const double r = std::max(12 * px, std::hypot(lu - ox, lv - oy));
        double a0 = std::atan2(d1y, d1x), a1 = std::atan2(d2y, d2x);
        double sweep = a1 - a0;
        while (sweep > M_PI) sweep -= 2 * M_PI;
        while (sweep < -M_PI) sweep += 2 * M_PI;
        const int n = 24;
        for (int i = 0; i < n; ++i)
          d.thin.push_back({W(ox + r * std::cos(a0 + sweep * i / n), oy + r * std::sin(a0 + sweep * i / n)), W(ox + r * std::cos(a0 + sweep * (i + 1) / n), oy + r * std::sin(a0 + sweep * (i + 1) / n)), col});
      }
    }
    if (!(m_dimEdit && m_dimEdit->isVisible() && m_dimEditing == c.id)) d.texts.push_back({W(lu, lv), dimensionText(c), col});  // the value box covers it while typing
  };
  for (const auto& c : m_sk.constraints)
    if (c.is_dimension()) dimension(c, false);
  if (m_placingDim && m_haveCursor) {
    SkConstraint c = m_pendingDim;
    c.pos[0] = m_cursor.u;
    c.pos[1] = m_cursor.v;
    try { c.value = dimension_value(m_sk, c); } catch (const std::exception&) {}  // what it measures now (it read "0 mm")
    dimension(c, true);
  }

  for(const auto& e:m_sk.entities) if(e.type==SkEntity::Type::Spline && e.degree) {
    for(size_t i=1;i<e.p.size();++i) {
      const auto* a=m_geometry->point(m_sk,e.p[i-1]);const auto* b=m_geometry->point(m_sk,e.p[i]);
      if(a && b) d.thin.push_back({W(a->x,a->y),W(b->x,b->y),t.fg3});
    }
  }
  m_prs->SetToUpdate();
  if(m_visible)m_viewport->updateOverlay(m_prs);
  updateTransient();
}

QStringList SketchEditor::transientTexts() const {
  QStringList out;
  if (!m_transientPrs.IsNull())
    for (const auto& text : static_cast<const SketchPrs*>(m_transientPrs.get())->texts) out << text.s;
  return out;
}

QStringList SketchEditor::overlayTexts() const {
  QStringList out;
  if (!m_prs.IsNull())
    for (const auto& text : static_cast<const SketchPrs*>(m_prs.get())->texts) out << text.s;
  return out;
}

size_t SketchEditor::badgeTriangles() const { return m_prs.IsNull() ? 0 : static_cast<const SketchPrs*>(m_prs.get())->badges.size() / 3; }
size_t SketchEditor::coincidenceDots() const { return m_prs.IsNull() ? 0 : static_cast<const SketchPrs*>(m_prs.get())->dots.size(); }

size_t SketchEditor::transientSolid(const QColor& c) const {
  size_t n = 0;
  if (!m_transientPrs.IsNull())
    for (const auto& s : static_cast<const SketchPrs*>(m_transientPrs.get())->solid) n += s.c == c;
  return n;
}

size_t SketchEditor::transientDashed(const QColor& c) const {
  size_t n = 0;
  if (!m_transientPrs.IsNull())
    for (const auto& s : static_cast<const SketchPrs*>(m_transientPrs.get())->dashed) n += s.c == c;
  return n;
}

size_t SketchEditor::sketchSolid(const QColor& c) const {
  size_t n = 0;
  if (!m_prs.IsNull())
    for (const auto& s : static_cast<const SketchPrs*>(m_prs.get())->solid) n += s.c == c;
  return n;
}

size_t SketchEditor::transientLocked() const { return m_transientPrs.IsNull() ? 0 : static_cast<const SketchPrs*>(m_transientPrs.get())->locked.size(); }
size_t SketchEditor::transientCursor() const { return m_transientPrs.IsNull() ? 0 : static_cast<const SketchPrs*>(m_transientPrs.get())->cursor.size(); }

bool SketchEditor::cursorCrisp() const {
  if (m_transientPrs.IsNull()) return false;
  const auto& c = static_cast<const SketchPrs*>(m_transientPrs.get())->cursor;
  if (c.size() != 4) return false;
  const int hair = int(std::max(1.0, std::round(m_viewport->displayScale())));
  // crosshair(): a horizontal arm, then a vertical one (SnapMarkers.hpp): on whole pixels across them
  return std::abs(m_viewport->pixelAlign(c[0].a, hair).y()) < 0.01 && std::abs(m_viewport->pixelAlign(c[2].a, hair).x()) < 0.01;
}

const std::vector<std::vector<std::pair<double, double>>>& SketchEditor::textPreview() {
  const QString text = option("text", "OPAD"), style = option("textStyle", "outline");
  const QString key = text + '\n' + option("height", "10 mm") + '\n' + style + '\n' + option("font", "Arial");
  if (key == m_textPreviewKey) return m_textPreview;
  m_textPreviewKey = key;
  m_textPreview.clear();
  try {
    if (text.isEmpty() || text.size() > 512) return m_textPreview;
    std::vector<ParamDef> defs;
    for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    const double height = ParamTable(defs, m_doc->scene.units).length(option("height", "10 mm").toStdString());
    if (!(height > 0)) return m_textPreview;
    if (style == "stroke" || style == "block") {  // the built-in font's strokes (cap height 1), as createText lays them out
      for (auto stroke : stroke_text(text.toStdString())) {
        for (auto& p : stroke) p = {p.first * height, p.second * height};
        m_textPreview.push_back(std::move(stroke));
      }
    } else {  // the system font's outlines, scaled as createText scales them
      QFont font(option("font", "Arial"));
      font.setPixelSize(1000);
      QPainterPath path;
      path.addText(0, 0, font, text);
      const double scale = height / std::max(1.0, QFontMetricsF(font).capHeight());
      for (const QPolygonF& polygon : path.toSubpathPolygons()) {
        std::vector<std::pair<double, double>> line;
        for (const QPointF& p : polygon) line.push_back({p.x() * scale, -p.y() * scale});
        m_textPreview.push_back(std::move(line));
      }
    }
  } catch (const std::exception&) {  // an expression that does not evaluate (yet): no preview
    m_textPreview.clear();
  }
  return m_textPreview;
}

void SketchEditor::pixelAxes(double& rx, double& ry, double& ux, double& uy) const {
  const QPointF c(m_viewport->width() / 2.0, m_viewport->height() / 2.0);
  double u0, v0, u1, v1, u2, v2;
  if (m_viewport->planePoint(c, m_frame, u0, v0) && m_viewport->planePoint(c + QPointF(100, 0), m_frame, u1, v1) && m_viewport->planePoint(c + QPointF(0, -100), m_frame, u2, v2))
    rx = (u1 - u0) / 100, ry = (v1 - v0) / 100, ux = (u2 - u0) / 100, uy = (v2 - v0) / 100;  // 100 px apart: planePoint takes whole device pixels
}

void SketchEditor::setShowConstraints(bool on) {
  if (m_showConstraints == on) return;
  m_showConstraints = on;
  QSettings().setValue("sketch/showConstraints", on);
  if (m_active) rebuild();
  emit changed();
}

// Grid snapping: the drawing cursor jumps between the nodes (AutoCAD's SNAP), so the editor draws it at the snapped point
// and the system pointer is hidden over the view, while a tool takes points (Alt frees it) and while a dragged point
// snaps to the nodes.
bool SketchEditor::drawsCursor() const {
  if (!m_active || !m_visible || m_transientPrs.IsNull() || !m_viewport->gridSnap()) return false;
  if (m_tool == "select") return m_dragging && m_dragMoved && m_dragGrid;
  return gridPoints() && !m_lastMods.testFlag(Qt::AltModifier);
}

void SketchEditor::updateTransient() {
  m_inTransient = true;
  m_viewport->setOwnCursor(drawsCursor());  // ownCursorChanged lands here again: this call draws what it means
  m_inTransient = false;
  if(m_transientPrs.IsNull())return;
  auto& d=*static_cast<SketchPrs*>(m_transientPrs.get());
  const SketchDrawing before=d;  // the same picture again (a move within one grid cell): no redisplay, no frame
  const auto& t=m_viewport->tokens();
  auto W=[&](double u,double v){return m_frame.to_world(u,v);};
  const double px=m_viewport->pixelSize();
  // Snap markers and pictograms face the viewer (SnapMarkers.hpp): one pixel right (rx, ry) and up (ux, uy) on the screen,
  // in sketch coordinates.
  double rx=px,ry=0,ux=0,uy=px;
  pixelAxes(rx,ry,ux,uy);
  // The drawing cursor where the click goes (the pointer's snap; typed values move the rubber band, not the cursor), or
  // where a drag takes the hand; while the system pointer is hidden for it, and the pointer is over the view.
  auto drawCursor=[&] {
    d.cursor.clear();d.cursorHalo.clear();
    m_drawnCursor.reset();
    if(!m_viewport->ownCursor() || !(m_inView || m_dragging))return;
    const double x=m_tool=="select"?m_dragCursorU:m_pointer.u,y=m_tool=="select"?m_dragCursorV:m_pointer.v;
    m_drawnCursor=std::make_pair(x,y);
    // On whole device pixels (SketchPrs draws it round(scale) px wide), so it is crisp wherever the node falls on the screen.
    const QPointF shift=m_viewport->pixelAlign(W(x,y),int(std::max(1.0,std::round(m_viewport->displayScale()))));
    const double cx=x+shift.x()*rx-shift.y()*ux,cy=y+shift.x()*ry-shift.y()*uy;
    auto at=[&](double sx,double sy){return W(cx+sx*rx+sy*ux,cy+sx*ry+sy*uy);};
    for(const auto& s:snapmarkers::crosshair()) {
      d.cursorHalo.push_back({at(s.x0,s.y0),at(s.x1,s.y1),t.vp});
      d.cursor.push_back({at(s.x0,s.y0),at(s.x1,s.y1),t.fg});
    }
  };
  auto show=[&] {
    drawCursor();
    if(static_cast<const SketchDrawing&>(d)==before)return;
    ++m_transientRedisplays;
    m_transientPrs->SetToUpdate();
    if(m_visible)m_viewport->updateOverlay(m_transientPrs);
  };
  if(!m_geometry || m_geometryJob)return show();  // a large sketch's curves being prepared: only the cursor moves on meanwhile
  d.solid.clear();d.thin.clear();d.dashed.clear();d.marks.clear();d.locked.clear();d.points.clear();d.bigPoints.clear();d.rings.clear();d.texts.clear();d.fill.clear();
  d.glow.clear();d.glowPoints.clear();
  // The hover's role (UI-38): a white glow under what is hovered, over a darker rim on a light background.
  d.glowColor=highlight::hoverHalo(t);d.rimColor=highlight::hoverRim(t);d.glowAlpha=1;
  m_rubberKinds.clear();
  d.textBack=t.bg2;d.scale=m_viewport->displayScale();d.font=theme::ui().family().toStdString();
  auto mark=[&](double x,double y,const std::vector<snapmarkers::Seg>& segs,double ox,double oy,const QColor& c) {  // (ox, oy): px off (x, y)
    auto at=[&](double sx,double sy){return W(x+(sx+ox)*rx+(sy+oy)*ux,y+(sx+ox)*ry+(sy+oy)*uy);};
    for(const auto& s:segs)d.marks.push_back({at(s.x0,s.y0),at(s.x1,s.y1),c});
  };
  m_marker.reset();
  m_markerTurn=0;
  m_glyphs.clear();
  if(m_hover.kind==Hit::Dimension) {  // a constraint's badge or a dimension's value: what it holds lights up (UI-24)
    const auto c=std::find_if(m_sk.constraints.begin(),m_sk.constraints.end(),[&](const SkConstraint& k){return k.id==m_hover.id;});
    if(c!=m_sk.constraints.end())for(int ref:c->refs) {
      if(const auto* e=m_sk.entity(ref)) {
        const auto pts=sampled(*e);
        for(size_t i=1;i<pts.size();++i)d.solid.push_back({W(pts[i-1].first,pts[i-1].second),W(pts[i].first,pts[i].second),t.hov});
        if(e->type==SkEntity::Type::Point)if(const auto* p=e->p.empty()?nullptr:m_geometry->point(m_sk,e->p[0]))d.bigPoints.push_back({W(p->x,p->y),t.hov});
      } else if(const auto* p=m_geometry->point(m_sk,ref))d.bigPoints.push_back({W(p->x,p->y),t.hov});
    }
  } else if(m_hover.kind==Hit::Point) {
    if(const auto* p=m_geometry->point(m_sk,m_hover.id)){d.glowPoints.push_back({W(p->x,p->y),t.hover});d.bigPoints.push_back({W(p->x,p->y),t.fg});}
    if(m_tool=="select" && std::binary_search(m_joinDots.begin(),m_joinDots.end(),m_hover.id))  // a join's dot: the curves meeting there light up
      for(size_t index:m_geometry->curvesAt(m_hover.id)) if(index<m_sk.entities.size()) {
        const auto pts=sampled(m_sk.entities[index]);
        for(size_t i=1;i<pts.size();++i)d.solid.push_back({W(pts[i-1].first,pts[i-1].second),W(pts[i].first,pts[i].second),t.hov});
      }
    if(m_tool=="fillet") {  // the arc a click there makes, at the radius set (typed before anything is picked too)
      const auto arc=filletPreview(m_hover.id);
      for(size_t i=1;i<arc.size();++i)d.solid.push_back({W(arc[i-1].first,arc[i-1].second),W(arc[i].first,arc[i].second),t.hov});
      if(arc.size()>1)++m_rubberKinds["arc"];
    }
  } else if(m_hover.kind==Hit::Entity && !(gridPoints() && m_viewport->ownCursor())) {  // on the grid only the snap's own curve lights up
    // Trim lights up the piece the click removes, in red; the whole curve read as "this curve goes".
    const auto piece=m_tool=="trim"&&m_haveCursor?trimPreview(m_hover.id,m_cursor.u,m_cursor.v):std::vector<std::pair<double,double>>{};
    if(!piece.empty()){for(size_t i=1;i<piece.size();++i)d.solid.push_back({W(piece[i-1].first,piece[i-1].second),W(piece[i].first,piece[i].second),t.red});++m_rubberKinds["trim"];}
    else if(const auto* e=m_sk.entity(m_hover.id)){const auto pts=sampled(*e);for(size_t i=1;i<pts.size();++i){d.glow.push_back({W(pts[i-1].first,pts[i-1].second),W(pts[i].first,pts[i].second),t.hover});d.solid.push_back({W(pts[i-1].first,pts[i-1].second),W(pts[i].first,pts[i].second),t.fg});}}
    if(m_tool=="extend" && m_haveCursor) {  // where a click there runs the end to (UI-28)
      const auto run=extendPreview(m_hover.id,m_pointer.u,m_pointer.v);
      for(size_t i=1;i<run.size();++i)d.dashed.push_back({W(run[i-1].first,run[i-1].second),W(run[i].first,run[i].second),t.green});
      if(run.size()>1)++m_rubberKinds["extension"];
    }
  }
  if(m_boxSelecting) {
    const QColor color=m_boxU<m_dragU?t.green:t.sel;
    auto& lines=m_boxU<m_dragU?d.dashed:d.thin;
    lines.push_back({W(m_dragU,m_dragV),W(m_boxU,m_dragV),color});
    lines.push_back({W(m_boxU,m_dragV),W(m_boxU,m_boxV),color});
    lines.push_back({W(m_boxU,m_boxV),W(m_dragU,m_boxV),color});
    lines.push_back({W(m_dragU,m_boxV),W(m_dragU,m_dragV),color});
  }
  // A grid node a dragged point snapped to: the grid marker.
  if(m_dragging && m_dragMoved && m_dragGrid)mark(m_dragGridU,m_dragGridV,snapmarkers::marker(snapmarkers::Marker::Grid),0,0,t.green);
  // The point or curve a dragged point is held to (UI-28): its marker and what the drop does.
  if(m_dragging && m_dragMoved && (m_dropPoint || m_dropCurve)) {
    mark(m_dropU,m_dropV,snapmarkers::marker(m_dropPoint?snapmarkers::Marker::Endpoint:snapmarkers::Marker::Nearest),0,0,t.green);
    d.texts.push_back({W(m_dropU+14*px,m_dropV+14*px),m_dropPoint?tr("Merge"):tr("On curve"),t.green,true});
  }
  // A trim fence (UI-28): its line, and in red what it takes.
  if(m_fencing && m_fenceMoved) {
    d.dashed.push_back({W(m_fenceU,m_fenceV),W(m_fenceToU,m_fenceToV),t.red});
    int shown=0;
    for(const auto& [id,x,y]:fenceHits(m_fenceU,m_fenceV,m_fenceToU,m_fenceToV)) {
      if(++shown>64)break;
      const auto piece=trimPreview(id,x,y);
      for(size_t i=1;i<piece.size();++i)d.solid.push_back({W(piece[i-1].first,piece[i-1].second),W(piece[i].first,piece[i].second),t.red});
    }
  }
  // Rubber band of the running tool (also from typed values alone: drawing by the keyboard, the pointer not in the view).
  if ((m_haveCursor || !m_typedValues.empty()) && m_tool != "select") {
    const QColor rb = t.hov;
    const double cu = m_cursor.u, cv = m_cursor.v;
    auto seg = [&](double x0, double y0, double x1, double y1) { d.solid.push_back({W(x0, y0), W(x1, y1), rb}); };
    auto circle = [&](double x, double y, double r) {
      for (int i = 0; i < 72; ++i) seg(x + r * std::cos(i * M_PI / 36), y + r * std::sin(i * M_PI / 36), x + r * std::cos((i + 1) * M_PI / 36), y + r * std::sin((i + 1) * M_PI / 36));
    };
    if (m_tool=="spline" && !m_chain.empty()) {
      Sketch preview=m_sk;auto nodes=m_chain;
      if(const auto* last=preview.point(nodes.back());last && std::hypot(last->x-cu,last->y-cv)>1e-7) nodes.push_back(preview.add_point(cu,cv));
      if(nodes.size()>=2) {
        const int id=add_cubic_spline(preview,nodes);auto edge=entity_edge(preview,*preview.entity(id),opad::Frame{});
        if(!edge.IsNull()) {BRepAdaptor_Curve c(edge);auto previous=c.Value(c.FirstParameter());const int samples=int(nodes.size())*24;
          for(int i=1;i<=samples;++i) {auto at=c.Value(c.FirstParameter()+(c.LastParameter()-c.FirstParameter())*i/samples);seg(previous.X(),previous.Y(),at.X(),at.Y());previous=at;}
          ++m_rubberKinds["spline"];}
      }
    } else if (m_tool == "line" && !m_chain.empty()) {
      if (const SkPoint* p = m_geometry->point(m_sk,m_chain.back())) seg(p->x, p->y, cu, cv), ++m_rubberKinds["line"];
    } else if (!m_clicks.empty()) {
      const Snap& a = m_clicks[0];
      if (m_tool == "rect") { seg(a.u, a.v, cu, a.v); seg(cu, a.v, cu, cv); seg(cu, cv, a.u, cv); seg(a.u, cv, a.u, a.v); m_rubberKinds["line"] += 4; }
      else if (m_tool == "crect") {
        const double w = std::fabs(cu - a.u), h = std::fabs(cv - a.v);
        seg(a.u - w, a.v - h, a.u + w, a.v - h); seg(a.u + w, a.v - h, a.u + w, a.v + h); seg(a.u + w, a.v + h, a.u - w, a.v + h); seg(a.u - w, a.v + h, a.u - w, a.v - h);
        m_rubberKinds["line"] += 4;
      } else if (m_tool == "circle") circle(a.u, a.v, std::hypot(cu - a.u, cv - a.v)), ++m_rubberKinds["circle"];
      else if (m_tool == "polygon") {
        // The polygon itself, its first corner at the pointer, as the click will make it, and dashed the construction circle
        // through its corners that the click keeps (as its guide draws it).
        const int sides = std::clamp(option("sides", "6").toInt(), 3, 256);
        const double r = std::hypot(cu - a.u, cv - a.v), a0 = std::atan2(cv - a.v, cu - a.u);
        for (int i = 0; i < sides; ++i) {
          const double t0 = a0 + 2 * M_PI * i / sides, t1 = a0 + 2 * M_PI * (i + 1) / sides;
          seg(a.u + r * std::cos(t0), a.v + r * std::sin(t0), a.u + r * std::cos(t1), a.v + r * std::sin(t1));
        }
        for (int i = 0; i < 72; ++i)
          d.dashed.push_back({W(a.u + r * std::cos(i * M_PI / 36), a.v + r * std::sin(i * M_PI / 36)), W(a.u + r * std::cos((i + 1) * M_PI / 36), a.v + r * std::sin((i + 1) * M_PI / 36)), rb});
        m_rubberKinds["line"] += sides;
        ++m_rubberKinds["construction circle"];
      }
      // The final shape through the pointer for the three-click tools, instead of straight rubber bands.
      else if (m_clicks.size() == 2 && (m_tool == "arc3" || m_tool == "circle3" || m_tool == "arcc" || m_tool == "slot" || m_tool == "ellipse")) {
        const Snap& b = m_clicks[1];
        auto positive = [](double t) { t = std::fmod(t, 2 * M_PI); return t < 0 ? t + 2 * M_PI : t; };
        auto arc = [&](double x, double y, double r, double from, double sweep) {
          const int n = std::max(8, int(std::ceil(std::fabs(sweep) / (2 * M_PI) * 96)));
          for (int i = 0; i < n; ++i) seg(x + r * std::cos(from + sweep * i / n), y + r * std::sin(from + sweep * i / n), x + r * std::cos(from + sweep * (i + 1) / n), y + r * std::sin(from + sweep * (i + 1) / n));
        };
        if (m_tool == "arc3" || m_tool == "circle3") {
          const double ax = a.u, ay = a.v, bx = b.u, by = b.v, cx = cu, cy = cv, dd = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
          if (std::fabs(dd) < 1e-12) { seg(ax, ay, bx, by); seg(bx, by, cx, cy); m_rubberKinds["line"] += 2; }
          else {
            const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / dd;
            const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / dd;
            const double r = std::hypot(ax - ux, ay - uy);
            if (m_tool == "circle3") circle(ux, uy, r), ++m_rubberKinds["circle"];
            else {  // from the first click to the second, round the side the pointer is on (as the click decides it)
              const double a0 = std::atan2(ay - uy, ax - ux), a1 = std::atan2(by - uy, bx - ux), am = std::atan2(cy - uy, cx - ux);
              const bool ccw = positive(am - a0) < positive(a1 - a0);
              arc(ux, uy, r, a0, ccw ? positive(a1 - a0) : -positive(a0 - a1));
              ++m_rubberKinds["arc"];
            }
          }
        } else if (m_tool == "arcc") {
          const double r = std::hypot(b.u - a.u, b.v - a.v), from = std::atan2(b.v - a.v, b.u - a.u);
          double sweep = slotSweep(cu, cv);  // the way the pointer went round, past half a turn too (P5)
          if (const auto typed = m_typedValues.find("sweep"); typed != m_typedValues.end()) sweep = typed->second;
          arc(a.u, a.v, r, from, sweep);
          d.dashed.push_back({W(a.u, a.v), W(b.u, b.v), rb});
          ++m_rubberKinds["arc"];
          ++m_rubberKinds["construction line"];  // the dashed radius to the start
        } else if (m_tool == "slot") {
          const double dx = b.u - a.u, dy = b.v - a.v, len = std::hypot(dx, dy);
          if (len > 1e-9) {
            const double nx = -dy / len, ny = dx / len, r = std::fabs((cu - a.u) * nx + (cv - a.v) * ny), along = std::atan2(dy, dx);
            seg(a.u + nx * r, a.v + ny * r, b.u + nx * r, b.v + ny * r);
            seg(a.u - nx * r, a.v - ny * r, b.u - nx * r, b.v - ny * r);
            arc(b.u, b.v, r, along - M_PI / 2, M_PI);
            arc(a.u, a.v, r, along + M_PI / 2, M_PI);
            d.dashed.push_back({W(a.u, a.v), W(b.u, b.v), rb});
            m_rubberKinds["line"] += 2;
            m_rubberKinds["arc"] += 2;
            ++m_rubberKinds["construction line"];  // the centre line, made as construction
          }
        } else {  // ellipse: centre, end of the major axis, then the minor half-axis from the pointer
          const double dx = b.u - a.u, dy = b.v - a.v, major = std::hypot(dx, dy);
          if (major > 1e-9) {
            const double minor = std::fabs((cu - a.u) * (-dy / major) + (cv - a.v) * (dx / major)), ux = dx / major, uy = dy / major;
            for (int i = 0; i < 96; ++i) {
              auto at = [&](int k) { const double t = 2 * M_PI * k / 96; return std::pair<double, double>{a.u + major * std::cos(t) * ux - minor * std::sin(t) * uy, a.v + major * std::cos(t) * uy + minor * std::sin(t) * ux}; };
              const auto p = at(i), q = at(i + 1);
              seg(p.first, p.second, q.first, q.second);
            }
            ++m_rubberKinds["ellipse"];
          }
        }
      } else if (m_tool == "slot" || m_tool == "arc3" || m_tool == "arcc" || m_tool == "circle3" || m_tool == "ellipse") seg(a.u, a.v, cu, cv), ++m_rubberKinds["line"];
      // Before their shape can show, the other tools of three clicks draw the band their guides draw from the first click: a
      // 3-point rectangle's base, an arc slot's radius, a conic's start tangent, and a centre slot's centre line both ways.
      else if (m_clicks.size() == 1 && (m_tool == "rect3" || m_tool == "arcslot" || m_tool == "conic")) seg(a.u, a.v, cu, cv), ++m_rubberKinds["line"];
      else if (m_clicks.size() == 1 && m_tool == "cslot") seg(2 * a.u - cu, 2 * a.v - cv, cu, cv), ++m_rubberKinds["line"];
      else if (m_tool == "image_calibrate") {  // the distance being measured: to the pointer, then between the two clicks
        if (m_clicks.size() == 1) d.dashed.push_back({W(a.u, a.v), W(cu, cv), t.amber});
        else d.solid.push_back({W(a.u, a.v), W(m_clicks[1].u, m_clicks[1].v), t.amber});
        ++m_rubberKinds["measure"];
      }
      for (const auto& k : m_clicks) d.points.push_back({W(k.u, k.v), m_tool == "image_calibrate" ? t.amber : rb});  // where the clicks so far went (a centre, the first end)
    }
    if (m_tool == "image_insert") {  // the picture's frame at the width set, its lower-left corner at the click, else at the pointer
      const QSizeF picture = insertPicture();
      double width = 0;
      try {
        std::vector<ParamDef> defs;
        for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
        width = ParamTable(defs, m_doc->scene.units).length(option("imageWidth", "100 mm").toStdString());
      } catch (const std::exception&) {
      }
      if (picture.isValid() && !picture.isEmpty() && width > 0) {
        const double x = m_clicks.empty() ? cu : m_clicks.front().u, y = m_clicks.empty() ? cv : m_clicks.front().v, h = width * picture.height() / picture.width();
        d.dashed.push_back({W(x, y), W(x + width, y), rb});
        d.dashed.push_back({W(x + width, y), W(x + width, y + h), rb});
        d.dashed.push_back({W(x + width, y + h), W(x, y + h), rb});
        d.dashed.push_back({W(x, y + h), W(x, y), rb});
        ++m_rubberKinds["frame"];
      }
    }
    if (m_tool == "image_edit") {  // the picture a press takes (under the pointer), or the one dragged where it goes (P5)
      std::vector<std::pair<double, double>> corners;
      if (m_imageDrag) imageFrame(m_imageDrag->id, m_imageDrag->du, m_imageDrag->dv, corners);
      else if (const int id = imageAt(m_pointer.u, m_pointer.v)) imageFrame(id, 0, 0, corners);
      for (size_t i = 0; i < corners.size(); ++i)
        d.dashed.push_back({W(corners[i].first, corners[i].second), W(corners[(i + 1) % corners.size()].first, corners[(i + 1) % corners.size()].second), rb});
      if (!corners.empty()) ++m_rubberKinds["frame"];
    }
    if (m_tool == "paste" && m_clip)  // the copied curves by their base point at the pointer, as the click places them
      for (const auto& line : m_clip->outline) {
        for (size_t i = 1; i < line.size(); ++i) seg(cu + line[i - 1].first, cv + line[i - 1].second, cu + line[i].first, cv + line[i].second);
        ++m_rubberKinds["outline"];
      }
    if (m_tool == "text")  // the letters on their baseline from the pointer, as the click places them (there was only a dot)
      for (const auto& line : textPreview()) {
        for (size_t i = 1; i < line.size(); ++i) seg(cu + line[i - 1].first, cv + line[i - 1].second, cu + line[i].first, cv + line[i].second);
        ++m_rubberKinds["outline"];
      }
    const Sketch preview=primitivePreview();
    for(const auto& e:preview.entities) {
      const auto edge=entity_edge(preview,e,opad::Frame{});
      if(edge.IsNull())continue;
      static const char* const kinds[]={"point","line","circle","arc","ellipse","spline"};
      ++m_rubberKinds[QString(e.construction?"construction ":"")+kinds[std::clamp(int(e.type),0,5)]];
      const auto pts=curveSamples(edge,px*0.25);
      for(size_t i=1;i<pts.size();++i) {  // construction curves dashed, as the sketch draws them once made
        if(e.construction)d.dashed.push_back({W(pts[i-1].X(),pts[i-1].Y()),W(pts[i].X(),pts[i].Y()),rb});
        else seg(pts[i-1].X(),pts[i-1].Y(),pts[i].X(),pts[i].Y());
      }
    }
    // The sizes and angles of the step, read out where they are measured; a typed one held, with a padlock (UI-17).
    for (const auto& r : readouts()) {
      const QColor color = r.locked ? t.sel : t.fg2;
      if (r.leader) d.thin.push_back({W(r.fu, r.fv), W(r.tu, r.tv), color});
      if (r.r > 0) {  // the angle's arc, from the direction it is measured from (drawn a little longer)
        const int n = std::max(4, int(std::ceil(std::fabs(r.sweep) / (2 * M_PI) * 64)));
        for (int i = 0; i < n; ++i)
          d.thin.push_back({W(r.cu + r.r * std::cos(r.from + r.sweep * i / n), r.cv + r.r * std::sin(r.from + r.sweep * i / n)),
                            W(r.cu + r.r * std::cos(r.from + r.sweep * (i + 1) / n), r.cv + r.r * std::sin(r.from + r.sweep * (i + 1) / n)), color});
        d.dashed.push_back({W(r.cu, r.cv), W(r.cu + (r.r + 12 * px) * std::cos(r.from), r.cv + (r.r + 12 * px) * std::sin(r.from)), color});
      }
      if (boxed(r.key)) continue;  // its box sits there and shows it
      d.texts.push_back({W(r.u, r.v), r.text, color});
      if (r.locked) {  // a padlock past the value: its body and its shackle
        const double x = r.u + r.ox * (r.ext + 10 * px), y = r.v + r.oy * (r.ext + 10 * px) - 1.5 * px, w = 4 * px, h = 3 * px;
        d.thin.push_back({W(x - w, y - h), W(x + w, y - h), color});
        d.thin.push_back({W(x + w, y - h), W(x + w, y + h), color});
        d.thin.push_back({W(x + w, y + h), W(x - w, y + h), color});
        d.thin.push_back({W(x - w, y + h), W(x - w, y - h), color});
        for (int i = 0; i < 8; ++i)
          d.thin.push_back({W(x + 2.5 * px * std::cos(i * M_PI / 8), y + h + 2.5 * px * std::sin(i * M_PI / 8)), W(x + 2.5 * px * std::cos((i + 1) * M_PI / 8), y + h + 2.5 * px * std::sin((i + 1) * M_PI / 8)), color});
      }
    }
    if (!placing()) return show();
    const bool snapped = m_cursor.kind != Snap::Kind::None || m_cursor.horizontal || m_cursor.vertical;
    // What the pointer is pulled to: that object is drawn in the inference colour and named beside the cursor, so
    // the user sees which point, curve or alignment will be used (and constrained) before clicking; an object snap by
    // its marker's shape (UI-23).
    const QColor snapColor = t.green;
    using M = snapmarkers::Marker;
    auto curve = [&](int id) {
      if (const auto* e = m_sk.entity(id)) {
        const auto pts = sampled(*e);
        for (size_t i = 1; i < pts.size(); ++i) d.solid.push_back({W(pts[i - 1].first, pts[i - 1].second), W(pts[i].first, pts[i].second), snapColor});
      }
    };
    auto isCentre = [&](int id) { return m_geometry->centre(id); };
    QString label;
    using K = Snap::Kind;
    auto anchor = [&](int id, double& x, double& y) {  // a guide's point; -1: the step's last click, where no point is
      if (id < 0) return fromPoint(x, y, id);
      const auto* p = m_geometry->point(m_sk, id);
      if (p) x = p->x, y = p->y;
      return p != nullptr;
    };
    auto towards = [&](int id) {  // dashed from that point to the pointer
      double x, y;
      if (id && anchor(id, x, y)) d.dashed.push_back({W(x, y), W(cu, cv), snapColor});
    };
    auto past = [&](int id) {  // a line extended from its nearer end to the pointer
      if (const auto* e = m_sk.entity(id); e && e->type == SkEntity::Type::Line && e->p.size() == 2)
        if (const auto *a = m_geometry->point(m_sk, e->p[0]), *b = m_geometry->point(m_sk, e->p[1]); a && b) {
          const auto* from = std::hypot(a->x - cu, a->y - cv) < std::hypot(b->x - cu, b->y - cv) ? a : b;
          d.dashed.push_back({W(from->x, from->y), W(cu, cv), snapColor});
        }
    };
    switch (m_cursor.kind) {
      case K::Point:
        m_marker = isCentre(m_cursor.target) ? M::Centre : M::Endpoint;
        label = isCentre(m_cursor.target) ? tr("Centre") : tr("Point");
        break;
      case K::Midpoint: curve(m_cursor.target); label = tr("Midpoint"); m_marker = M::Midpoint; break;
      case K::Quadrant: curve(m_cursor.target); label = tr("Quadrant"); m_marker = M::Quadrant; break;
      case K::Intersection: curve(m_cursor.target); curve(m_cursor.other); label = tr("Intersection"); m_marker = M::Intersection; break;
      case K::Apparent:  // the lines extended to where they would cross
        curve(m_cursor.target);
        curve(m_cursor.other);
        past(m_cursor.target);
        past(m_cursor.other);
        label = tr("Apparent intersection");
        m_marker = M::Apparent;
        break;
      case K::Perpendicular: curve(m_cursor.target); towards(-1); label = tr("Perpendicular"); m_marker = M::Perpendicular; break;
      case K::Tangent: curve(m_cursor.target); towards(-1); label = tr("Tangent"); m_marker = M::Tangent; break;
      case K::Curve: curve(m_cursor.target); label = m_cursor.entity ? tr("On curve") : tr("Nearest"); m_marker = M::Nearest; break;
      case K::Extension: {  // its ⊢ turned along the line, the stem out of the line's end (the line in screen pixels)
        m_marker = M::Extension;
        past(m_cursor.target);
        const auto& g = m_cursor.line;
        const double out = (cu - g.x) * g.dx + (cv - g.y) * g.dy < 0 ? -1 : 1, det = rx * uy - ry * ux;
        if (std::fabs(det) > 0) m_markerTurn = std::atan2(out * (rx * g.dy - ry * g.dx) / det, out * (g.dx * uy - g.dy * ux) / det);
        label = tr("Extension");
        break;
      }
      case K::Aligned:
        towards(m_cursor.target);
        label = tr("Tracking");
        m_marker = M::Tracking;
        break;
      case K::Angle:
        if (double x, y; anchor(m_cursor.target, x, y))
          label = units::format(units::Kind::Angle, std::round(std::atan2(cv - y, cu - x) * 180 / M_PI), units::current().radians ? 3 : 0);
        break;
      case K::Cross:  // guides crossing (both drawn), or a guide meeting a curve (highlighted)
        towards(m_cursor.target);
        towards(m_cursor.other);
        if (m_cursor.curve) curve(m_cursor.curve);
        label = m_cursor.curve ? tr("Intersection") : tr("Tracking");
        m_marker = M::Intersection;
        break;
      case K::Locked: {  // the locked line from its anchor past the pointer, and what stops the pointer on it
        const auto& g = m_cursor.line;
        const double past = ((cu - g.x) * g.dx + (cv - g.y) * g.dy < 0 ? -40 : 40) * px;
        (m_cursor.ortho ? d.dashed : d.locked).push_back({W(g.x, g.y), W(cu + g.dx * past, cv + g.dy * past), snapColor});  // a lock's thick
        towards(m_cursor.other);
        if (m_cursor.curve) curve(m_cursor.curve);
        const bool sticky = m_lock && m_lock->sticky;
        if (m_cursor.ortho) {  // Ortho (F8): horizontal or vertical from the last point, no padlock
          label = tr("Ortho");
          if (m_cursor.other || m_cursor.curve) m_marker = M::Intersection;
          break;
        }
        if (m_cursor.other || m_cursor.curve) {  // on a stop: which of them, when Shift taps go through more
          label = m_cursor.other ? tr("Locked ∩ tracking") : tr("Locked ∩ curve");
          if (sticky && m_cursor.stops > 1 && m_cursor.stop >= 0) label += QStringLiteral(" · %1/%2").arg(m_cursor.stop + 1).arg(m_cursor.stops);
          m_marker = M::Intersection;
        } else label = !sticky ? tr("Locked") : m_cursor.stops ? tr("Locked · Shift goes to the next stop") : tr("Locked · Shift or Esc lets go");
        mark(cu, cv, snapmarkers::marker(M::Locked, 9), -14, 12, snapColor);  // the padlock above left: the label is above right
        break;
      }
      case K::Grid:  // a node; a curve through it, which the point will lie on, lit up
        if (m_cursor.curve) curve(m_cursor.curve);
        label = tr("Grid");
        m_marker = M::Grid;
        break;
      case K::Typed: {  // what the typed values hold the point to, dashed: the X or Y line, the ΔX/ΔY legs, the angle's ray
        double bu = 0, bv = 0;
        const bool base = inputBase(bu, bv);
        auto typed = [&](const char* key) { return m_typedValues.count(key) > 0; };
        const double reach = 60 * px;
        if (typed("x")) d.dashed.push_back({W(cu, cv - reach), W(cu, cv + reach), snapColor});
        if (typed("y")) d.dashed.push_back({W(cu - reach, cv), W(cu + reach, cv), snapColor});
        if (base && (typed("dx") || typed("dy"))) {
          d.dashed.push_back({W(bu, bv), W(cu, bv), snapColor});
          d.dashed.push_back({W(cu, bv), W(cu, cv), snapColor});
        }
        if (base && typed("angle")) {  // the ray, and what the angle is measured from
          const double a = std::atan2(cv - bv, cu - bu), r = angleReference();
          d.dashed.push_back({W(bu, bv), W(cu + reach * std::cos(a), cv + reach * std::sin(a)), snapColor});
          d.dashed.push_back({W(bu, bv), W(bu + reach * std::cos(r), bv + reach * std::sin(r)), snapColor});
        }
        if (base && typed("length") && !typed("angle")) {  // a length alone: the point slides round this circle
          const double r = m_typedValues.at("length");
          for (int i = 0; i < 96; ++i) d.thin.push_back({W(bu + r * std::cos(i * M_PI / 48), bv + r * std::sin(i * M_PI / 48)), W(bu + r * std::cos((i + 1) * M_PI / 48), bv + r * std::sin((i + 1) * M_PI / 48)), snapColor});
        }
        break;
      }
      case K::None: break;
    }
    if (m_marker) mark(cu, cv, snapmarkers::turned(snapmarkers::marker(*m_marker), m_markerTurn), 0, 0, snapColor);
    else d.bigPoints.push_back({W(cu, cv), snapped ? t.green : rb});
    if (m_cursor.grid && m_marker != M::Grid) mark(cu, cv, snapmarkers::marker(M::Grid, 7), -12, -12, snapColor);  // quantised to the grid too
    if ((m_cursor.horizontal || m_cursor.vertical) && m_cursor.kind == K::None) {
      towards(-1);
      label = m_cursor.horizontal ? tr("Horizontal") : tr("Vertical");
    }
    if (m_cursor.choices > 1 && !label.isEmpty()) label += QStringLiteral(" · %1/%2").arg(m_cursor.choice + 1).arg(m_cursor.choices);  // Shift taps go through them
    // The acquired points that alignments are measured from: a cross each, while it is not the point under the cursor.
    for (const int id : m_tracked)
      if (const auto* reference = m_geometry->point(m_sk, id); reference && m_cursor.point != id) mark(reference->x, reference->y, snapmarkers::marker(M::Tracking, 12), 0, 0, snapColor);
    if (!label.isEmpty()) d.texts.push_back({W(cu + 14 * px, cv + 14 * px), label, snapColor, true});  // above right: the pointer covers below right
    // The constraints the click adds (UI-21): their pictograms in badges, a row below right of the pointer.
    m_glyphs = snapGlyphs(m_cursor);
    for (size_t i = 0; i < m_glyphs.size(); ++i) {
      const double ox = 24 + 19 * double(i), oy = -22, h = 8;
      auto at = [&](double x, double y) { return W(cu + (x + ox) * rx + (y + oy) * ux, cv + (x + ox) * ry + (y + oy) * uy); };
      for (const auto& corner : {std::array<double, 6>{-h, -h, h, -h, h, h}, std::array<double, 6>{-h, -h, h, h, -h, h}})
        for (int k = 0; k < 3; ++k) d.fill.push_back(at(corner[size_t(2 * k)], corner[size_t(2 * k + 1)]));
      mark(cu, cv, snapmarkers::badge(2 * h, 2 * h, 4), ox, oy, t.line);
      mark(cu, cv, snapmarkers::glyph(m_glyphs[i], 10), ox, oy, snapColor);
    }
    d.fillColor = t.bg2;
    d.fillAlpha = 0.92f;
  }
  show();
}
