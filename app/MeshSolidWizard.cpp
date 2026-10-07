// Mesh to solid (design.mesh_solid): a mesh body rebuilt as a solid, previewed with how far it is from the mesh, then
// committed as one undo step. The conversion and its measuring run on a worker (opad::design::convert_mesh); its plan is
// what Create commits, so the body committed is the body measured.
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "BodyShape.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Panels.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/design/mesh_solid.hpp"

namespace {

QString signedPercent(double v) { return (v >= 0 ? QStringLiteral("+") : QString()) + QLocale().toString(v, 'f', 2) + QStringLiteral("%"); }

// What the result is made of, what it was rebuilt from, and how close it follows the mesh.
QString reportText(const opad::design::MeshConversion& c) {
  const auto& r = c.report;
  const auto& d = c.deviation;
  QStringList lines;
  const int curves = r.value("curves", 0);
  if (c.method == "extrude") lines << MainWindow::tr("A sketch of %L1 curves, extruded").arg(curves);
  else if (c.method == "revolve") lines << MainWindow::tr("A sketch of %L1 curves, revolved").arg(curves);
  else lines << MainWindow::tr("A solid of fitted surfaces (the mesh is neither an extrusion nor a revolution)");
  QStringList kinds;
  const std::pair<const char*, QString> names[] = {{"plane", MainWindow::tr("planes")},      {"cylinder", MainWindow::tr("cylinders")},
                                                   {"cone", MainWindow::tr("cones")},        {"sphere", MainWindow::tr("spheres")},
                                                   {"torus", MainWindow::tr("tori")},         {"spline", MainWindow::tr("spline surfaces")},
                                                   {"revolution", MainWindow::tr("revolved surfaces")}, {"extrusion", MainWindow::tr("extruded surfaces")},
                                                   {"other", MainWindow::tr("other surfaces")}};
  for (const auto& [key, name] : names)
    if (const int n = r.at("faces").value(key, 0); n > 0) kinds << QStringLiteral("%L1 %2").arg(n).arg(name);
  lines << MainWindow::tr("%L1 faces (%2) from %L3 triangles").arg(d.shape_faces).arg(kinds.join(QStringLiteral(", "))).arg(d.mesh_triangles);
  lines << MainWindow::tr("Deviation from the mesh: largest %1, mean %2").arg(units::format(units::Kind::Length, d.max), units::format(units::Kind::Length, d.mean));
  lines << MainWindow::tr("Within the tolerance (%1): %2 of the mesh's vertices")
               .arg(units::format(units::Kind::Length, d.tolerance), QLocale().toString(d.within * 100, 'f', 1) + QStringLiteral("%"));
  lines << MainWindow::tr("The solid's surface from the mesh: largest %1 (the facets' own chord error: %2)")
               .arg(units::format(units::Kind::Length, d.back_max), units::format(units::Kind::Length, d.sagitta));
  const double dv = d.mesh_volume > 0 ? (d.shape_volume - d.mesh_volume) / d.mesh_volume * 100 : 0;
  const double da = d.mesh_area > 0 ? (d.shape_area - d.mesh_area) / d.mesh_area * 100 : 0;
  lines << MainWindow::tr("Volume %1, area %2 against the mesh").arg(signedPercent(dv), signedPercent(da));
  const double score = d.score();
  const auto& t = theme::current();
  const QColor tone = score >= 99 ? t.green : score >= 90 ? t.amber : t.red;
  QString html = QStringLiteral("<p style='margin:0 0 4px 0'><b><span style='color:%1'>%2</span></b></p>")
                     .arg(theme::css(tone), MainWindow::tr("Match: %1").arg(QLocale().toString(score, 'f', 1) + QStringLiteral("%")).toHtmlEscaped());
  for (const auto& l : lines) html += QStringLiteral("<p style='margin:0'>%1</p>").arg(l.toHtmlEscaped());
  return html;
}

}  // namespace

void MainWindow::meshToSolid() {
  if (!m_doc->hasDocument || m_design->busy() || m_design->sketchActive()) return;
  if (auto* existing = findChild<ToolPanel*>("meshSolidWizard")) {
    existing->show();
    existing->raise();
    return;
  }
  std::vector<std::string> meshes;
  for (const auto& id : m_doc->scene.all_bodies())
    if (const auto* n = m_doc->scene.node(id); n && n->representation == "mesh") meshes.push_back(id);
  if (meshes.empty()) throw opad::Error("Open or import a mesh (STL, OBJ, 3MF or PLY) before converting it to a solid.");
  cancelTool();
  auto* dialog = new QWidget;
  auto* panel = new ToolPanel("meshSolid", "mesh", &Tokens::sel, tr("Mesh to solid"), dialog, 560, this);
  panel->setObjectName("meshSolidWizard");
  panel->setHelpId("design.mesh_solid");
  m_panels << panel;
  connect(panel, &QObject::destroyed, this, [this, panel] { m_panels.removeAll(panel); });
  auto* layout = new QVBoxLayout(dialog);
  auto* hint = new QLabel(tr("Rebuilds the mesh as a solid: a sketch and an extrusion or revolution when the mesh is one, otherwise fitted planes, "
                             "cylinders, cones, spheres and tori."),
                          dialog);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto* form = new QFormLayout;
  auto* source = new QComboBox(dialog);
  source->setObjectName("meshSolidSource");
  std::string chosen;
  for (const auto& id : currentNodeIds())
    for (const auto& body : m_doc->scene.bodies_under(id))
      if (chosen.empty() && std::find(meshes.begin(), meshes.end(), body) != meshes.end()) chosen = body;
  for (const auto& id : meshes) {
    source->addItem(QString::fromStdString(m_doc->scene.node(id)->name), QString::fromStdString(id));
    if (id == chosen) source->setCurrentIndex(source->count() - 1);
  }
  auto* mode = new QComboBox(dialog);
  mode->addItem(tr("Sketch and feature when possible"), "auto");
  mode->addItem(tr("Fitted solid only"), "solid");
  // In the shown unit (UI-123); the conversion takes mm. 0: 1/2000 of the mesh's size.
  auto* tolerance = new QDoubleSpinBox(dialog);
  tolerance->setDecimals(units::decimalsFor(0.0001));
  tolerance->setRange(0, units::toDisplay(units::Kind::Length, 10));
  tolerance->setSingleStep(units::toDisplay(units::Kind::Length, 0.01));
  tolerance->setSpecialValueText(tr("Automatic"));
  tolerance->setValue(0);
  tolerance->setSuffix(' ' + units::symbol(units::Kind::Length));
  auto* angle = new QDoubleSpinBox(dialog);
  angle->setRange(5, 80);
  angle->setDecimals(0);
  angle->setValue(35);
  angle->setSuffix(QStringLiteral(" °"));
  angle->setToolTip(tr("The largest angle between neighbouring facets of one curved surface; more is a sharp edge."));
  auto* name = new QLineEdit(source->currentText(), dialog);
  form->addRow(tr("Mesh"), source);
  form->addRow(tr("Result"), mode);
  form->addRow(tr("Tolerance"), tolerance);
  form->addRow(tr("Facet angle"), angle);
  form->addRow(tr("Body name"), name);
  layout->addLayout(form);
  auto* removeSource = new QCheckBox(tr("Remove the mesh after conversion"), dialog);
  layout->addWidget(removeSource);
  auto* report = new QLabel(dialog);
  report->setObjectName("meshSolidReport");
  report->setWordWrap(true);
  report->setTextFormat(Qt::RichText);
  report->setMinimumHeight(report->fontMetrics().lineSpacing() * 7);
  report->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  layout->addWidget(report, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Create solid"));
  buttons->button(QDialogButtonBox::Ok)->setObjectName("primary");
  layout->addWidget(buttons);

  struct State {
    int serial = 0;
    QPointer<Job> job;
    std::shared_ptr<opad::design::MeshConversion> result;  // the preview's conversion, for the serial it was made for
    int resultSerial = -1;
    size_t ops = 0;  // the document's op count it was planned on
    unsigned long long generation = 0;
    bool applying = false, closed = false;
  };
  auto state = std::make_shared<State>();
  QPointer<QWidget> guard(dialog);
  auto ready = [=, this] {
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!state->applying && state->result && state->resultSerial == state->serial &&
                                                     state->ops == m_doc->doc.ops.size() && state->generation == m_doc->generation);
  };
  auto compute = [=, this] {
    if (state->closed || state->applying) return;
    const int serial = ++state->serial;
    if (state->job) state->job->cancel();
    state->result.reset();
    m_viewport->clearPreviewBodies();
    ready();
    report->setText(tr("Fitting surfaces to the mesh..."));
    const std::string mesh = source->currentData().toString().toStdString();
    if (mesh.empty() || !m_doc->scene.node(mesh)) return;
    auto snapshot = std::make_shared<opad::Document>(m_doc->doc);
    auto result = std::make_shared<opad::design::MeshConversion>();
    auto prs = std::make_shared<std::shared_ptr<BodyPrs>>();
    opad::design::MeshConversionOptions o;
    o.solid.tolerance = units::fromDisplay(units::Kind::Length, tolerance->value());
    o.solid.angle = angle->value();
    o.mode = mode->currentData().toString().toStdString();
    o.name = name->text().trimmed().toStdString();
    o.remove_source = removeSource->isChecked();
    o.component = m_doc->activeComponent();  // made in the active component, as Finish sketch does (UI-33)
    const size_t ops = m_doc->doc.ops.size();
    const auto generation = m_doc->generation;
    state->job = m_jobs->async(tr("Mesh to solid"), [=](Progress p) {
      p.setPhase(tr("Fitting surfaces to the mesh"));
      *result = opad::design::convert_mesh(*snapshot, opad::resolve(*snapshot), mesh, o, [p] { return p.cancelled(); });
      if (p.cancelled()) return;
      p.setPhase(tr("Preparing the preview"));
      Bnd_Box box;
      BRepBndLib::Add(result->shape, box, Standard_False);
      const double defl = box.IsVoid() ? 0.1 : std::clamp(std::sqrt(box.SquareExtent()) * 0.002, 0.02, 2.0);
      BodyPrs::meshForDisplay(result->shape, defl);
      *prs = BodyPrs::build(result->shape, box, true);
    }, [=, this](bool ok, const QString& error) {
      if (!guard || state->closed || serial != state->serial) return;
      state->job = nullptr;
      if (!ok) {
        report->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::css(theme::current().red), i18n::t(error).toHtmlEscaped()));
        return;
      }
      state->result = result;
      state->resultSerial = serial;
      state->ops = ops;
      state->generation = generation;
      // The mesh steps aside for the result, in the new-body preview colour, so the two can be compared by toggling.
      m_viewport->setPreviewBodies({Viewport::PreviewPart{std::string(), result->shape, *prs, theme::current().previewNew}}, {mesh});
      report->setText(reportText(*result));
      ready();
    });
  };
  auto* debounce = new QTimer(dialog);
  debounce->setSingleShot(true);
  debounce->setInterval(300);
  connect(debounce, &QTimer::timeout, dialog, compute);
  auto changed = [=] {
    ++state->serial;
    ready();
    debounce->start();
  };
  connect(source, &QComboBox::currentIndexChanged, dialog, [=] {
    name->setText(source->currentText());
    changed();
  });
  connect(mode, &QComboBox::currentIndexChanged, dialog, changed);
  connect(tolerance, &QDoubleSpinBox::valueChanged, dialog, changed);
  connect(angle, &QDoubleSpinBox::valueChanged, dialog, changed);
  connect(removeSource, &QCheckBox::toggled, dialog, changed);
  connect(name, &QLineEdit::editingFinished, dialog, changed);
  connect(m_doc, &AppDocument::changed, dialog, [=] {
    if (!state->applying) changed();
  });
  connect(buttons, &QDialogButtonBox::accepted, dialog, [=, this] {
    ready();
    if (!buttons->button(QDialogButtonBox::Ok)->isEnabled()) return;
    state->applying = true;
    ready();
    auto plan = std::make_shared<opad::design::Plan>(std::move(state->result->plan));
    state->result.reset();
    m_viewport->clearPreviewBodies();
    m_design->commitPlanned(plan, tr("Mesh to solid"), [=](bool applied, const QString& failure) {
      if (!guard || state->closed) return;
      state->applying = false;
      if (applied) {
        panel->hide();
        return;
      }
      report->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::css(theme::current().red), failure.toHtmlEscaped()));
      changed();
    });
  });
  connect(buttons, &QDialogButtonBox::rejected, panel, &QWidget::hide);
  connect(panel, &ToolPanel::visibilityChanged, this, [=, this](bool on) {
    if (on || state->closed) return;
    state->closed = true;
    ++state->serial;
    debounce->stop();
    if (state->job) state->job->cancel();
    m_viewport->clearPreviewBodies();
    m_prompt->hide();
    panel->deleteLater();
  });
  connect(m_doc, &AppDocument::aboutToReplace, panel, [=] { panel->hide(); });
  m_prompt->set("mesh", tr("Mesh to solid"), {{tr("Check the result and create the solid"), {}}}, tr("Esc cancels"));
  m_prompt->show();
  positionOverlays();
  openPanel(panel);
  ready();
  compute();
}
