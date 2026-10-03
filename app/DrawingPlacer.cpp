#include "DrawingPlacer.hpp"
#include "BodyShape.hpp"
#include "SketchBackdrop.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "opad/design/expr.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/step_io.hpp"
#include "opad/drawing_io.hpp"
#include <AIS_TexturedShape.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <gp_Pln.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
constexpr size_t kMaxVertices = 200000;  // snap candidates kept from a drawing
}  // namespace

DrawingPlacer::DrawingPlacer(AppDocument* doc, Viewport* view, JobRunner* jobs, QWidget* window) : QObject(window), m_doc(doc), m_view(view), m_jobs(jobs) {
  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  m_hint = new QLabel(body);
  m_hint->setWordWrap(true);
  layout->addWidget(m_hint);
  auto* form = m_form = new QFormLayout;
  m_u = new QLineEdit(body);
  m_v = new QLineEdit(body);
  m_widthEdit = new QLineEdit(body);
  m_u->setObjectName("placeOffsetX");
  m_v->setObjectName("placeOffsetY");
  m_widthEdit->setObjectName("placeWidth");
  form->addRow(tr("Offset X"), m_u);  // in the shown unit ("12.7 mm", "0.5 in"); a bare number is in it too
  form->addRow(tr("Offset Y"), m_v);
  form->addRow(tr("Width"), m_widthEdit);  // pictures only
  layout->addLayout(form);
  auto* row = new QHBoxLayout;
  m_snap = new QPushButton(tr("Snap a vertex"), body);
  m_snap->setObjectName("placeSnap");
  m_snap->setCheckable(true);
  m_snap->setToolTip(tr("Click a vertex of the drawing, then the vertex it should land on (a body's, a drawing's or a sketch's)"));
  auto* reset = new QPushButton(tr("Reset"), body);
  reset->setToolTip(tr("Back to the drawing's origin on the plane's"));
  row->addWidget(m_snap);
  row->addWidget(reset);
  row->addStretch();
  layout->addLayout(row);
  m_status = new QLabel(body);
  m_status->setWordWrap(true);
  m_status->setObjectName("secondary");
  layout->addWidget(m_status);
  layout->addStretch();
  auto* footer = new QHBoxLayout;
  auto* backButton = new QPushButton(tr("Back"), body);
  backButton->setToolTip(tr("Choose another plane"));
  m_place = new QPushButton(tr("Place"), body);
  m_place->setObjectName("primary");
  m_place->setToolTip(tr("Import the drawing here"));
  auto* cancelButton = new QPushButton(tr("Cancel"), body);
  footer->addWidget(backButton);
  footer->addStretch();
  footer->addWidget(m_place);
  footer->addWidget(cancelButton);
  layout->addLayout(footer);
  m_panel = new ToolPanel("drawing-place", "drawing", &Tokens::sel, tr("Place drawing"), body, 420, window);
  m_panel->setEscapeHandler([this] {
    if (m_snapStage) { m_snapStage = 0; m_view->setSelectionFilter(m_oldFilter); refresh(); }
    else cancel();
  });
  connect(m_panel, &ToolPanel::visibilityChanged, this, [this](bool on) { if (!on && m_active) cancel(); });
  auto typed = [this] {  // a box left as shown keeps its value: "0.275591 in" would move a snapped drawing by 1e-5 mm
    if (!m_u->isModified() && !m_v->isModified()) return;
    try {
      std::vector<opad::design::ParamDef> defs;
      for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
      const opad::design::ParamTable params(defs, units::current().length);
      const double u = m_u->isModified() ? params.length(m_u->text().toStdString()) : m_du, v = m_v->isModified() ? params.length(m_v->text().toStdString()) : m_dv;
      m_u->setModified(false);
      m_v->setModified(false);
      setOffset(u, v);
    } catch (const std::exception& e) {
      m_status->setText(QString::fromUtf8(e.what()));
    }
  };
  connect(m_u, &QLineEdit::editingFinished, this, typed);
  connect(m_v, &QLineEdit::editingFinished, this, typed);
  connect(m_widthEdit, &QLineEdit::editingFinished, this, [this] {
    if (!m_widthEdit->isModified()) return;
    m_widthEdit->setModified(false);
    try {
      std::vector<opad::design::ParamDef> defs;
      for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
      setImageWidth(opad::design::ParamTable(defs, units::current().length).length(m_widthEdit->text().toStdString()));
    } catch (const std::exception& e) {
      m_status->setText(QString::fromUtf8(e.what()));
    }
  });
  connect(m_snap, &QPushButton::toggled, this, [this](bool on) {
    m_snapStage = on ? 1 : 0;
    if (!on) m_view->setSelectionFilter(m_oldFilter);
    refresh();
  });
  connect(reset, &QPushButton::clicked, this, [this] { setOffset(0, 0); });
  connect(backButton, &QPushButton::clicked, this, [this] { stop(); if (back) back(); });
  connect(m_place, &QPushButton::clicked, this, [this] {
    if (!m_loaded) return;
    const opad::Mat4 m = placement();
    stop();
    if (placed) placed(m);
  });
  connect(cancelButton, &QPushButton::clicked, this, &DrawingPlacer::cancel);
  connect(units::notifier(), &units::Notifier::changed, this, [this] { if (m_active) refresh(); });
  view->installEventFilter(this);
}

void DrawingPlacer::start(const QString& file, const opad::Frame& plane, std::function<void(ToolPanel*)> open) {
  stop();
  m_active = true;
  m_file = file;
  m_plane = plane;
  m_du = m_dv = 0;
  m_loaded = false;
  const QString suffix = QFileInfo(file).suffix().toLower();
  m_image = QStringList{"png", "jpg", "jpeg", "bmp", "gif", "webp"}.contains(suffix);
  m_imageW = m_imageH = m_width = 0;
  m_form->setRowVisible(m_widthEdit, m_image);
  m_panel->setHeader(m_image ? "canvas" : "drawing", m_image ? tr("Place canvas") : tr("Place drawing"));
  m_hint->setText(m_image ? tr("The picture sits on the chosen plane, centred on its origin. Drag it, type an offset or a width, or snap a corner onto a vertex.")
                          : tr("The drawing sits on the chosen plane with its own origin at the plane's. Drag it, type an offset, or snap one of its vertices onto another vertex."));
  m_place->setToolTip(m_image ? tr("Import the picture here") : tr("Import the drawing here"));
  m_oldFilter = m_view->selectionFilter();
  refresh();
  open(m_panel);
  m_view->lookAt(plane, true, false);
  // The drawing is read on a worker, as the import itself will be; only its curves and vertices come back.
  const int serial = ++m_serial;
  auto shape = std::make_shared<TopoDS_Compound>();
  auto prs = std::make_shared<std::shared_ptr<BodyPrs>>();
  auto vertices = std::make_shared<std::vector<gp_Pnt>>();
  auto picture = std::make_shared<Handle(Image_PixMap)>();
  auto size = std::make_shared<std::pair<double, double>>(0, 0);
  const std::filesystem::path path(file.toStdU16String());
  m_status->setText(tr("Reading %1").arg(QFileInfo(file).fileName()));
  m_job = m_jobs->async(m_image ? tr("Reading picture") : tr("Reading drawing"), [shape, prs, vertices, path, picture, size](Progress p) {
    opad::Document scratch = opad::Document::create();
    opad::ImportOptions options;
    options.progress = [p](double, const std::string&) { return !p.cancelled(); };
    opad::import_file(scratch, path, options);
    const opad::Scene scene = opad::resolve(scratch);
    for (const auto& id : scene.all_bodies())  // a picture: its rectangle, as it will look, its corners and centre to snap
      if (const auto* n = scene.node(id); n && n->representation == "image" && n->raster.is_object()) {
        const auto& c = n->raster.at("corners");
        const double w = c[1][0].get<double>(), h = c[0][1].get<double>();
        *size = {w, h};
        TopoDS_Face face = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 0, w, 0, h).Face();
        BRepMesh_IncrementalMesh(face, std::max(w, h));
        BRep_Builder builder;
        builder.MakeCompound(*shape);
        builder.Add(*shape, face);
        *vertices = {gp_Pnt(0, 0, 0), gp_Pnt(w, 0, 0), gp_Pnt(w, h, 0), gp_Pnt(0, h, 0), gp_Pnt(w / 2, h / 2, 0)};
        const std::string& href = n->raster.at("href").get_ref<const std::string&>();
        const QImage image = decodePicture(QByteArray::fromBase64(QByteArray::fromStdString(href.substr(href.find(',') + 1))), 1024);
        if (!image.isNull()) *picture = texturePixels(image);
        return;
      }
    BRep_Builder builder;
    builder.MakeCompound(*shape);
    for (const auto& id : scene.all_bodies()) {
      const auto* n = scene.node(id);
      if (n && n->representation == "drawing2d") builder.Add(*shape, opad::body_shape(scratch, n->body_key));
    }
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(*shape, TopAbs_VERTEX, map);
    for (int i = 1; i <= map.Extent() && vertices->size() < kMaxVertices; ++i) vertices->push_back(BRep_Tool::Pnt(TopoDS::Vertex(map(i))));
    if (p.cancelled()) return;
    Bnd_Box box;
    BRepBndLib::Add(*shape, box);
    *prs = BodyPrs::build(*shape, box);
  }, [this, serial, shape, prs, vertices, picture, size](bool ok, const QString& error) {
    if (serial != m_serial || !m_active) return;
    m_job = nullptr;
    if (!ok) { m_status->setText(error); return; }
    m_vertices = std::move(*vertices);
    if (m_image) {
      m_imageW = size->first, m_imageH = size->second;
      m_width = m_imageW;
      Handle(AIS_TexturedShape) textured = new AIS_TexturedShape(*shape);
      if (!picture->IsNull()) {
        textured->SetTexturePixMap(*picture);
        textured->SetTextureMapOn();
        textured->DisableTextureModulate();
        textured->SetTextureRepeat(false);
      }
      textured->Attributes()->SetAutoTriangulation(false);  // meshed on the worker
      textured->SetColor(occ(theme::current().sel));
      textured->SetTransparency(0.2);
      m_preview = textured;
      m_loaded = true;
      move();
      m_view->showBackdrop(m_preview);
    } else {
      Handle(AIS_Shape) ais = new BodyShape(*shape, *prs);
      ais->SetColor(occ(theme::current().sel));
      ais->SetWidth(1.5);
      m_preview = ais;
      m_loaded = true;
      move();
      m_view->showOverlay(m_preview);
    }
    refresh();
    emit ready();
  });
}

void DrawingPlacer::stop() {
  m_active = m_dragging = false;
  ++m_serial;
  if (m_job) m_job->cancel();
  m_job = nullptr;
  if (!m_preview.IsNull()) m_view->removeOverlay(m_preview);
  if (!m_marker.IsNull()) m_view->removeOverlay(m_marker);
  m_preview.Nullify();
  m_marker.Nullify();
  if (m_snapStage) m_view->setSelectionFilter(m_oldFilter);
  m_snapStage = 0;
  { QSignalBlocker block(m_snap); m_snap->setChecked(false); }
  m_panel->hide();
}

void DrawingPlacer::cancel() {
  if (!m_active) return;
  stop();
  emit cancelled();
}

opad::Mat4 DrawingPlacer::placement() const {
  const opad::Vec3 n = m_plane.normal(), o = m_plane.to_world(m_du, m_dv);
  opad::Mat4 m;
  for (int r = 0; r < 3; ++r) { m.at(r, 0) = m_plane.x[r]; m.at(r, 1) = m_plane.y[r]; m.at(r, 2) = n[r]; m.at(r, 3) = o[r]; }
  return m;
}

void DrawingPlacer::setOffset(double u, double v) {
  if (!m_active || !std::isfinite(u) || !std::isfinite(v)) return;
  m_du = u;
  m_dv = v;
  move();
  refresh();
}

bool DrawingPlacer::snap(const opad::Vec3& from, const opad::Vec3& to) {
  double fu, fv, tu, tv;  // both projected on the plane: an off-plane 3D vertex lands where it projects
  m_plane.to_local(from, fu, fv);
  m_plane.to_local(to, tu, tv);
  setOffset(m_du + tu - fu, m_dv + tv - fv);
  return true;
}

void DrawingPlacer::setImageWidth(double mm) {
  if (!m_image || !(mm > 0) || !std::isfinite(mm)) return;
  m_width = mm;
  move();
  refresh();
}

opad::Mat4 DrawingPlacer::shown() const {
  if (!m_image || !(m_imageW > 0)) return placement();
  opad::Mat4 scale;
  scale.at(0, 0) = scale.at(1, 1) = scale.at(2, 2) = m_width / m_imageW;
  return placement() * scale * opad::Mat4::translation(-m_imageW / 2, -m_imageH / 2, 0);
}

void DrawingPlacer::move() {
  if (m_preview.IsNull()) return;
  if (m_image) return m_view->moveOverlay(m_preview, opad::trsf_from_mat(shown()));  // no recompute: the texture stays
  m_preview->SetLocalTransformation(opad::trsf_from_mat(placement()));
  m_view->updateOverlay(m_preview);
}

void DrawingPlacer::refresh() {
  if (!m_u->hasFocus()) m_u->setText(units::editable(units::Kind::Length, m_du));
  if (!m_v->hasFocus()) m_v->setText(units::editable(units::Kind::Length, m_dv));
  if (!m_widthEdit->hasFocus()) m_widthEdit->setText(m_width > 0 ? units::editable(units::Kind::Length, m_width) : QString());
  m_place->setEnabled(m_loaded);
  m_snap->setEnabled(m_loaded);
  if (!m_loaded) return;
  if (m_snapStage == 1) m_status->setText(m_image ? tr("Snap: click a corner or the centre of the picture") : tr("Snap: click a vertex of the drawing"));
  else if (m_snapStage == 2) m_status->setText(tr("Snap: click the vertex it should land on (Esc cancels)"));
  else m_status->setText(m_image ? tr("Drag the picture or type the offset and width.") : tr("Drag the drawing, or type the offset. Place imports it here."));
}

bool DrawingPlacer::nearestVertex(const QPointF& at, opad::Vec3& world) const {
  const opad::Mat4 m = shown();
  double best = 14 * 14;
  bool found = false;
  for (const auto& p : m_vertices) {
    const opad::Vec3 w = m.apply({p.X(), p.Y(), p.Z()});
    const QPointF s = m_view->widgetPoint(w);
    const double d = QPointF::dotProduct(s - at, s - at);
    if (d < best) { best = d; world = w; found = true; }
  }
  return found;
}

bool DrawingPlacer::eventFilter(QObject* object, QEvent* event) {
  if (object != m_view || !m_active || !m_loaded) return false;
  if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
    if (m_snapStage) { m_snapStage = 0; m_snap->setChecked(false); }
    else cancel();
    return true;
  }
  if (event->type() == QEvent::MouseButtonPress) {
    auto* e = static_cast<QMouseEvent*>(event);
    if (e->button() != Qt::LeftButton || QRect(m_view->width() - 205, 0, 205, 185).contains(e->position().toPoint())) return false;  // navigation, the cube
    if (m_snapStage == 1) {
      if (nearestVertex(e->position(), m_snapFrom)) {
        m_snapStage = 2;
        m_view->setSelectionFilter(Viewport::SelFilter::Vertex);
        TopoDS_Compound cross;
        BRep_Builder b;
        b.MakeCompound(cross);
        const double s = m_view->pixelSize() * 7;
        for (int axis = 0; axis < 3; ++axis) {
          gp_Pnt a(m_snapFrom[0], m_snapFrom[1], m_snapFrom[2]), c = a;
          a.SetCoord(axis + 1, a.Coord(axis + 1) - s);
          c.SetCoord(axis + 1, c.Coord(axis + 1) + s);
          b.Add(cross, BRepBuilderAPI_MakeEdge(a, c).Edge());
        }
        Handle(AIS_Shape) marker = new AIS_Shape(cross);
        marker->SetColor(occ(theme::current().amber));
        marker->SetWidth(2);
        if (!m_marker.IsNull()) m_view->removeOverlay(m_marker);
        m_marker = marker;
        m_view->showOverlay(m_marker);
      } else m_status->setText(tr("Snap: click closer to a vertex of the drawing"));
      refresh();
      return true;
    }
    if (m_snapStage == 2) {
      opad::Ref ref;
      if (m_view->originReferenceAt(e->position(), ref)) {
        try {
          const opad::json info = opad::inspect_ref(m_doc->doc, m_doc->scene, ref);
          const opad::json& p = info.contains("point") ? info["point"] : info.at("center");
          snap(m_snapFrom, {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
          m_snapStage = 0;
          m_snap->setChecked(false);
          if (!m_marker.IsNull()) m_view->removeOverlay(m_marker);
          m_marker.Nullify();
        } catch (const std::exception& error) {
          m_status->setText(QString::fromUtf8(error.what()));
        }
      } else m_status->setText(tr("Snap: click a vertex of a body, a drawing or a sketch"));
      return true;
    }
    double u, v;
    if (!m_view->planePoint(e->position(), m_plane, u, v)) return false;
    m_dragging = true;
    m_pressU = u;
    m_pressV = v;
    m_startU = m_du;
    m_startV = m_dv;
    return true;
  }
  if (event->type() == QEvent::MouseMove && m_dragging) {
    double u, v;
    if (!m_view->planePoint(static_cast<QMouseEvent*>(event)->position(), m_plane, u, v)) return true;
    double du = m_startU + u - m_pressU, dv = m_startV + v - m_pressV;
    if (m_view->gridSnap()) {
      const double step = m_view->gridStep();
      du = std::round(du / step) * step;
      dv = std::round(dv / step) * step;
    }
    setOffset(du, dv);
    return true;
  }
  if (event->type() == QEvent::MouseButtonRelease && m_dragging) {
    m_dragging = false;
    return true;
  }
  return false;
}
