#include "MainWindow.hpp"
#include "AppDocument.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "opad/commands.hpp"
#include <QDialog>
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
  QList<Fmt> fmts = {{"STEP AP214", "step", "AP214"}, {"STEP AP242", "step", "AP242"}, {"OBJ (+MTL)", "obj", ""}, {"STL", "stl", ""}, {"GLB", "glb", ""}, {"DXF (2D)", "dxf", ""}, {"SVG (2D)", "svg", ""}, {"DWG (converter)", "dwg", ""}};
  for (const auto& f : opad::commands::exporter_formats())
    if (f != "step" && f != "obj" && f != "stl" && f != "glb" && f != "dxf" && f != "svg" && f != "dwg") fmts << Fmt{QString::fromStdString(f).toUpper() + tr(" (plugin)"), QString::fromStdString(f), ""};
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
  auto* scopeSel = new QRadioButton(selBodies == 1 ? tr("Selection · 1 object") : tr("Selection · %1 objects").arg(selBodies), &dlg);
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
        else if(fmt=="svg" || fmt=="dxf" || fmt=="dwg") compatible &= cat=="drawing2d";
      }
      format->setEnabled(compatible);
    }
    if(!group->checkedButton() || !group->checkedButton()->isEnabled()) for(auto* format:group->buttons()) if(format->isEnabled()) { format->setChecked(true); break; }
    const bool available=group->checkedButton() && group->checkedButton()->isEnabled(); ok->setEnabled(available);

    auto* b = group->checkedButton();
    QString fmt = b ? b->property("format").toString() : "step";
    int n = scopeSel->isChecked() ? selBodies : allBodies;
    summary->setText(tr("%1 · %2 objects · %3").arg(b ? b->text() : fmt).arg(n).arg(units::format(units::Kind::Length, units::fromDisplay(units::Kind::Length, tol->value()), tol->decimals())));
    if(!available) summary->setText(tr("Select objects of a compatible export category."));
    optionsLabel->setVisible(fmt=="stl" || fmt=="obj");
    perBody->setVisible(fmt=="stl");singleFile->setVisible(fmt=="stl");
    ascii->setVisible(fmt=="stl");binary->setVisible(fmt=="stl");
    mtl->setVisible(fmt=="obj");
    form->setRowVisible(tol,fmt!="step");
  };
  connect(group, &QButtonGroup::idClicked, &dlg, [&](int) { refresh(); });
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
  QString out = QFileDialog::getSaveFileName(this,tr("Export to"),suggested,tr("%1 files (*.%2)").arg(fmt.toUpper(),fmt));
  if(out.isEmpty()) return;
  if(QFileInfo(out).suffix().isEmpty()) out+="."+fmt;
  m_settings.setValue("ui/lastDir", QFileInfo(out).absolutePath());
  opad::json args{{"format", fmt.toStdString()}, {"out", out.toStdString()}, {"tolerance", units::fromDisplay(units::Kind::Length, tol->value())}, {"ascii", ascii->isChecked()}, {"per_body", perBody->isChecked()}, {"mtl", mtl->isChecked()}};
  if (!b->property("schema").toString().isEmpty()) args["schema"] = b->property("schema").toString().toStdString();
  if (scopeSel->isChecked()) args["select"] = ids;
  opad::json r = opad::commands::run("export", args, &m_doc->doc);
  resultToast(tr("Exported %1 objects to %2").arg(r.value("bodies", 0)).arg(QFileInfo(out).fileName()), QFileInfo(out).absolutePath());
}
