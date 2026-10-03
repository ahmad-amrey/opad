#include "MainWindow.hpp"
#include "AppDocument.hpp"
#include "ExportJob.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "Viewport.hpp"
#include "Units.hpp"
#include "opad/commands.hpp"
#include <QComboBox>
#include <QDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QButtonGroup>
#include <QRadioButton>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QStatusBar>
#include <QTimer>
#include <set>

void MainWindow::exportDialog(std::vector<std::string> ids) {
  if (!m_doc->hasDocument) throw opad::Error("Nothing to export.");
  if(ids.empty()) ids = currentNodeIds();
  int selBodies = 0;
  for (const auto& id : ids) selBodies += m_doc->scene.sketch(id)?1:static_cast<int>(m_doc->scene.bodies_under(id).size());
  int allBodies = static_cast<int>(m_doc->scene.all_bodies().size());
  for(const auto& sk:m_doc->scene.sketches) if(sk.visible) ++allBodies;

  QDialog dlg(this);
  dlg.setObjectName("exportDialog");
  dlg.setWindowTitle(tr("Export"));
  dlg.setFixedWidth(560);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(12);
  auto header = [&](const QString& t) { auto* l = new QLabel(t, &dlg); l->setObjectName("sectionHeader"); v->addWidget(l); };
  header(tr("FORMAT"));
  auto* grid = new QGridLayout();
  auto* group = new QButtonGroup(&dlg);
  struct Fmt { QString label, format, schema; };
  QList<Fmt> fmts = {{"STEP AP214", "step", "AP214"}, {"STEP AP242", "step", "AP242"}, {"OBJ (+MTL)", "obj", ""}, {"STL", "stl", ""}, {"GLB", "glb", ""}, {"DXF (2D)", "dxf", ""}, {"SVG (2D)", "svg", ""}, {"DWG (converter)", "dwg", ""}, {"PDF (2D)", "pdf", ""}, {"PNG (2D)", "png", ""}};
  for (const auto& f : opad::commands::exporter_formats())
    if (f != "step" && f != "obj" && f != "stl" && f != "glb" && f != "dxf" && f != "svg" && f != "dwg" && f != "pdf" && f != "png") fmts << Fmt{QString::fromStdString(f).toUpper() + tr(" (plugin)"), QString::fromStdString(f), ""};
  int i = 0;
  for (const auto& f : fmts) {
    auto* r = new QRadioButton(f.label, &dlg);
    r->setStyleSheet(QString("QRadioButton:disabled { color: %1; }").arg(theme::current().fg3.name()));
    r->setProperty("format", f.format);
    r->setProperty("schema", f.schema);
    group->addButton(r, i);
    grid->addWidget(r, i / 2, i % 2);
    if (i == 1) r->setChecked(true);
    ++i;
  }
  v->addLayout(grid);
  header(tr("OBJECTS"));
  auto* scopeRow = new QHBoxLayout();
  auto* scopeSel = new QRadioButton(QString::fromUtf8("Selection · %1 %2").arg(selBodies).arg(selBodies == 1 ? tr("object") : tr("objects")), &dlg);
  auto* scopeAll = new QRadioButton(tr("Whole document · %1 objects").arg(allBodies), &dlg);
  auto* scopeGroup=new QButtonGroup(&dlg);scopeGroup->addButton(scopeSel);scopeGroup->addButton(scopeAll);
  scopeSel->setEnabled(selBodies > 0);
  (selBodies > 0 ? scopeSel : scopeAll)->setChecked(true);
  scopeRow->addWidget(scopeSel);
  scopeRow->addWidget(scopeAll);
  scopeRow->addStretch();
  v->addLayout(scopeRow);
  auto* form = new QFormLayout();
  auto* tol = new QDoubleSpinBox(&dlg);
  tol->setRange(units::toDisplay(units::Kind::Length, 0.001), units::toDisplay(units::Kind::Length, 10));  // in the shown unit (UI-123)
  tol->setDecimals(units::decimalsFor(0.001));
  tol->setValue(units::toDisplay(units::Kind::Length, 0.010));
  tol->setSuffix(' ' + units::symbol(units::Kind::Length));
  tol->setFont(theme::mono(12));
  form->addRow(tr("Tolerance"), tol);
  v->addLayout(form);
  auto* optionsLabel=new QLabel(tr("OPTIONS"),&dlg); optionsLabel->setObjectName("sectionHeader");v->addWidget(optionsLabel);
  auto* fileGroup=new QButtonGroup(&dlg);
  auto* singleFile=new QRadioButton(tr("Single STL file"),&dlg);singleFile->setChecked(true);
  auto* perBody = new QRadioButton(tr("One STL file per body"), &dlg);
  // Each choice's two answers together (they were interleaved: single, binary, per body, ASCII).
  fileGroup->addButton(singleFile);fileGroup->addButton(perBody);v->addWidget(singleFile);v->addWidget(perBody);
  perBody->setChecked(false);
  auto* encodingGroup=new QButtonGroup(&dlg);
  auto* binary=new QRadioButton(tr("Binary STL"),&dlg);binary->setChecked(true);
  auto* ascii = new QRadioButton(tr("ASCII STL"), &dlg);
  encodingGroup->addButton(binary);encodingGroup->addButton(ascii);v->addSpacing(6);v->addWidget(binary);v->addWidget(ascii);
  auto* mtl = new QCheckBox(tr("Write material library (OBJ)"), &dlg);
  mtl->setChecked(true);
  v->addWidget(mtl);
  // 2D formats of solids and meshes: a hidden-line view of them (UI-87), choices remembered.
  auto* viewLabel = new QLabel(tr("2D VIEW"), &dlg);
  viewLabel->setObjectName("sectionHeader");
  v->addWidget(viewLabel);
  auto* viewRow = new QHBoxLayout();
  auto* viewBox = new QComboBox(&dlg);
  viewBox->setObjectName("export.view");
  for (const auto& [id, label] : std::initializer_list<std::pair<const char*, QString>>{
           {"front", tr("Front view")}, {"back", tr("Back view")}, {"top", tr("Top view")}, {"bottom", tr("Bottom view")}, {"right", tr("Right view")},
           {"left", tr("Left view")}, {"iso", tr("Isometric view")}, {"camera", tr("Current view")}})
    viewBox->addItem(label, QString(id));
  viewBox->setCurrentIndex(std::max(0, viewBox->findData(m_settings.value("export/view", "front").toString())));
  auto* hiddenLines = new QCheckBox(tr("Hidden lines"), &dlg);
  hiddenLines->setObjectName("export.hidden");
  hiddenLines->setChecked(m_settings.value("export/hidden", false).toBool());
  auto* tangentEdges = new QCheckBox(tr("Tangent edges"), &dlg);
  tangentEdges->setObjectName("export.tangent");
  tangentEdges->setChecked(m_settings.value("export/tangent", true).toBool());
  viewRow->addWidget(viewBox);
  viewRow->addSpacing(12);
  viewRow->addWidget(hiddenLines);
  viewRow->addWidget(tangentEdges);
  viewRow->addStretch();
  v->addLayout(viewRow);
  auto* viewNote = new QLabel(&dlg);
  viewNote->setObjectName("secondary");
  viewNote->setWordWrap(true);
  v->addWidget(viewNote);
  QString stem = m_doc->doc.path.empty() ? "export" : QString::fromStdString(m_doc->doc.path.stem().string());
  if(ids.size()==1) { const auto* sk=m_doc->scene.sketch(ids[0]); stem=sk?QString::fromStdString(sk->name):m_doc->nodeName(ids[0]); for(const QChar c:QString("<>:\"/\\|?*")) stem.replace(c,'_'); }
  auto* footer = new QHBoxLayout();
  auto* summary = new QLabel(&dlg);
  summary->setObjectName("secondary");
  footer->addWidget(summary, 1);
  auto* cancel = new QPushButton(tr("Cancel   Esc"), &dlg);
  auto* ok = new QPushButton(tr("Export   Enter"), &dlg);
  ok->setObjectName("primary");
  ok->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(ok);
  v->addLayout(footer);
  bool asView = false;  // a 2D format of solids or meshes: a view of them
  auto refresh = [&] {
    std::set<std::string> categories;
    auto addCategory=[&](const std::string& id) {
      if(m_doc->scene.sketch(id)) { categories.insert("drawing2d"); return; }
      for(const auto& body:m_doc->scene.bodies_under(id)) categories.insert(m_doc->scene.node(body)->representation);
    };
    if(scopeSel->isChecked()) for(const auto& id:ids) addCategory(id);
    else { for(const auto& id:m_doc->scene.all_bodies()) addCategory(id); for(const auto& sk:m_doc->scene.sketches) if(sk.visible) addCategory(sk.id); }
    for(auto* format:group->buttons()) {
      const auto fmt=format->property("format").toString(); bool compatible=!categories.empty();
      for(const auto& cat:categories) {
        if(fmt=="step") compatible &= cat=="solid";
        else if(fmt=="stl" || fmt=="obj" || fmt=="glb") compatible &= cat=="solid" || cat=="mesh";
        else if(fmt=="svg" || fmt=="dxf" || fmt=="dwg" || fmt=="pdf" || fmt=="png") compatible &= cat=="drawing2d" || cat=="solid" || cat=="mesh";  // solids: a view of them
      }
      format->setEnabled(compatible);
    }
    if(!group->checkedButton() || !group->checkedButton()->isEnabled()) for(auto* format:group->buttons()) if(format->isEnabled()) { format->setChecked(true); break; }
    const bool available=group->checkedButton() && group->checkedButton()->isEnabled(); ok->setEnabled(available);

    auto* b = group->checkedButton();
    QString fmt = b ? b->property("format").toString() : "step";
    int n = scopeSel->isChecked() ? selBodies : allBodies;
    summary->setText(tr("%1 · %2 objects · %3").arg(b ? b->text() : fmt).arg(n).arg(units::format(units::Kind::Length, units::fromDisplay(units::Kind::Length, tol->value()), tol->decimals())));
    const bool twoD = fmt=="svg" || fmt=="dxf" || fmt=="dwg" || fmt=="pdf" || fmt=="png";
    asView = twoD && (categories.count("solid") || categories.count("mesh"));
    if(asView) summary->setText(tr("%1 · a 2D view of %2 objects · %3").arg(b->text()).arg(n).arg(viewBox->currentText()));
    if(!available) summary->setText(tr("Select objects of a compatible export category."));
    optionsLabel->setVisible(fmt=="stl" || fmt=="obj");
    perBody->setVisible(fmt=="stl");singleFile->setVisible(fmt=="stl");
    ascii->setVisible(fmt=="stl");binary->setVisible(fmt=="stl");
    mtl->setVisible(fmt=="obj");
    form->setRowVisible(tol,fmt!="step" && !twoD);
    for(QWidget* w:{static_cast<QWidget*>(viewLabel),static_cast<QWidget*>(viewBox),static_cast<QWidget*>(hiddenLines),static_cast<QWidget*>(tangentEdges),static_cast<QWidget*>(viewNote)}) w->setVisible(asView);
    viewNote->setText(categories.count("drawing2d") ? tr("Drawings and sketches are left out of a view of the model.")
                      : fmt == "pdf" ? tr("A vector page on the smallest ISO sheet that holds the view; hidden lines dashed.")
                      : fmt == "png" ? tr("A picture of the view at 300 dpi on white; hidden lines dashed.")
                                     : tr("Exact lines and arcs at full size in mm; hidden lines go on their own layer."));
    dlg.adjustSize();  // the rows shown and the note's lines change its height
  };
  connect(group, &QButtonGroup::idClicked, &dlg, [&](int) { refresh(); });
  connect(viewBox, &QComboBox::currentIndexChanged, &dlg, [&](int) { refresh(); });
  connect(scopeSel, &QRadioButton::toggled, &dlg, [&](bool) { refresh(); });
  connect(tol, &QDoubleSpinBox::valueChanged, &dlg, [&](double) { refresh(); });
  connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
  connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
  refresh();
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_EXPORT_DIALOG");!shot.isEmpty()) QTimer::singleShot(150,&dlg,[&dlg,shot] { dlg.grab().save(shot); dlg.reject(); });
  if (dlg.exec() != QDialog::Accepted) return;
  auto* b = group->checkedButton();
  QString fmt = b->property("format").toString();
  const QString suggested=QDir(m_settings.value("ui/lastDir",QDir::homePath()).toString()).filePath(stem+"."+fmt);
  QString out = qEnvironmentVariable("OPAD_BENCH_EXPORT_OUT");  // benches: no file dialog
  if(out.isEmpty()) out = QFileDialog::getSaveFileName(this,tr("Export to"),suggested,tr("%1 files (*.%2)").arg(fmt.toUpper(),fmt));
  if(out.isEmpty()) return;
  if(QFileInfo(out).suffix().isEmpty()) out+="."+fmt;
  m_settings.setValue("ui/lastDir", QFileInfo(out).absolutePath());
  opad::json args{{"format", fmt.toStdString()}, {"out", out.toStdString()}, {"tolerance", units::fromDisplay(units::Kind::Length, tol->value())}, {"ascii", ascii->isChecked()}, {"per_body", perBody->isChecked()}, {"mtl", mtl->isChecked()}};
  if (!b->property("schema").toString().isEmpty()) args["schema"] = b->property("schema").toString().toStdString();
  if (scopeSel->isChecked()) args["select"] = ids;
  if (asView) {
    const QString view = viewBox->currentData().toString();
    const opad::json camera = view == "camera" ? m_viewport->cameraJson() : opad::json();
    if (camera.contains("eye") && camera.contains("target")) {  // towards the eye, as the viewport looks
      const auto eye = camera["eye"].get<opad::Vec3>(), target = camera["target"].get<opad::Vec3>();
      args["dir"] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
      args["up"] = camera["up"];
    } else args["view"] = view == "camera" ? "iso" : view.toStdString();
    args["hidden"] = hiddenLines->isChecked();
    args["tangent"] = tangentEdges->isChecked();
    m_settings.setValue("export/view", view);
    m_settings.setValue("export/hidden", hiddenLines->isChecked());
    m_settings.setValue("export/tangent", tangentEdges->isChecked());
  }
  runExport(args, out);
}

void MainWindow::runExport(const opad::json& args, const QString& out) {
  exportJob(m_doc, m_jobs, this, args, out, [this](const opad::json& result) { m_lastExport = result; });
}

void exportJob(AppDocument* doc, JobRunner* jobs, QMainWindow* window, const opad::json& args, const QString& out, std::function<void(const opad::json&)> done) {
  const bool sheet = args.contains("sheet"), view = sheet || args.contains("view") || args.contains("dir");
  const QString phase = sheet ? QObject::tr("Projecting the views") : QObject::tr("Projecting the view");
  auto result = std::make_shared<opad::json>();
  QPointer<QMainWindow> self(window);
  const QString title = !sheet ? (view ? QObject::tr("Exporting a 2D view") : QObject::tr("Exporting"))
                        : args["sheet"].get<std::string>().rfind("drawing:", 0) == 0 ? QObject::tr("Exporting a drawing") : QObject::tr("Exporting a sheet");
  Job* job = doc->readAsync(jobs, title, [args, result, view, phase](const opad::Document& doc, const opad::Scene& scene, Progress p) {
    if (view) p.setPhase(phase, -1);
    *result = opad::commands::export_document(doc, scene, args, [p, phase](double f, const std::string&) {
      p.setPhase(phase, f < 0 ? -1 : static_cast<int>(f * 100));
      return !p.cancelled();
    });
  }, [self, result, out, done](bool ok, const QString& error) {
    if (!self) return;
    if (ok) self->statusBar()->showMessage(QObject::tr("Exported %1 objects to %2").arg(result->value("bodies", 0)).arg(QDir::toNativeSeparators(out)), 8000);
    else if (error != "cancelled") QMessageBox::warning(self, QObject::tr("OPAD"), i18n::t(error));
    if (done) done(ok ? *result : opad::json{{"error", error.toStdString()}});
  });
  if (!job) throw opad::Error("The document is busy; try again in a moment.");
}
