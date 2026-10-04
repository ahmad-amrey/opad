#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QTreeWidget>

#include <cmath>
#include <functional>

#include "BenchRegistry.hpp"
#include "BomExport.hpp"
#include "Commands.hpp"
#include "DocsArea.hpp"
#include "PartProperties.hpp"
#include "Units.hpp"
#include "opad/materials.hpp"

// OPAD_BENCH_BOM=<prefix>: a 60 x 40 x 10 plate and two pins. The Properties panel shows the plate's PART section with
// its link and no raw "part" row; a click on the link opens the Part properties dialog, which takes a part number, a
// library material (its density shown, colour as the material offered) and a vendor and applies them as one step, after
// which the panel shows them and the plate's mass. Both pins get a part number and "purchased" in one step; the plate and
// a pin together show their differing fields as several values and change only what is typed; a density that is not a
// number is refused; a cleared field is removed. Then the pins go into a component and File > Export bill of materials
// previews the parts (the pins one row of two by their part number, the plate's mass, the pins' missing one marked) while
// the document is held, indented (the component with its row below it) and of the selected component, and writes the
// CSV with the separator chosen. <prefix>.part.png is the dialog, <prefix>.properties.png the panel, <prefix>.bom.png and
// <prefix>.bom.csv the export.
// OPAD_BENCH_BOM_OPEN=<png>: the loaded file's BoM in parts, indented and top mode, timed (the Engine: no stall, the
// worker's time); OPAD_BENCH_BOM_MATERIAL=<material> sets one on the roots first (masses, never saved);
// OPAD_BENCH_BOM_VIEWER=1 requires the file to be open in viewer mode.
OPAD_BENCH(OPAD_BENCH_BOM_OPEN, bomOpen) {  // the loaded file's (the Engine: timings, stalls)
  const QString& shot = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  QElapsedTimer clock;
  bool ok = true;
  const auto listed = [&](BomDialog* bom, const char* mode) {
    if (auto* box = bom->findChild<QComboBox*>("bom.mode")) box->setCurrentIndex(box->findData(mode));
    clock.start();
    while (!bom->ready() && clock.elapsed() < 600000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const opad::json& totals = bom->bom().value("totals", opad::json::object());
    trace::log(QString("bench: bom-open: %1: %2 rows, %3 parts in %4 ms %5").arg(mode).arg(totals.value("rows", 0)).arg(totals.value("parts", 0))
                   .arg(clock.elapsed()).arg(bom->ready() ? "PASS" : "FAIL"));
    ok = ok && bom->ready();
  };
  if (const QString material = qEnvironmentVariable("OPAD_BENCH_BOM_MATERIAL"); !material.isEmpty())  // masses of every body (never saved)
    w.m_doc->run("part_properties", {{"targets", w.m_doc->scene.roots}, {"set", {{"material", material.toStdString()}}}});
  if (qEnvironmentVariableIsSet("OPAD_BENCH_BOM_VIEWER")) {  // a file other than .opad: listed as it is viewed, nothing saved first
    trace::log(QString("bench: bom-open: in viewer mode %1").arg(w.m_doc->browse ? "PASS" : "FAIL"));
    ok = w.m_doc->browse;
  }
  docs->exportBom({});
  if (auto* bom = w.findChild<BomDialog*>()) {
    listed(bom, "parts");
    bom->grab().save(shot);
    listed(bom, "indented");
    listed(bom, "top");
    bom->reject();
  }
  if (w.m_doc->browse) {  // part properties are an edit: Save first to edit (the bench answers Cancel), no dialog
    docs->editPartProperties(w.m_doc->scene.roots);
    const bool asked = !w.findChild<PartPropertiesDialog*>() && !w.m_doc->isDirty();
    trace::log(QString("bench: bom-open: Part properties in viewer mode asks to save first %1").arg(asked ? "PASS" : "FAIL"));
    ok = ok && asked;
  }
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}

OPAD_BENCH(OPAD_BENCH_BOM, bom) {
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = true;
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: bom: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto* table = w.m_props->findChild<QTreeWidget*>();
  const auto shown = [&] {  // the Properties rows as "label=value"
    QStringList rows;
    for (int i = 0; i < table->topLevelItemCount(); ++i) rows << table->topLevelItem(i)->text(0) + "=" + table->topLevelItem(i)->text(1);
    return rows.join("; ").remove(QChar(0x202A)).remove(QChar(0x202C));
  };
  const auto measured = [&] {
    QElapsedTimer waited;
    waited.start();
    while (w.m_propsJob && waited.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  };
  const auto dialog = [&]() -> PartPropertiesDialog* {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // the one closed before
    for (auto* d : w.findChildren<PartPropertiesDialog*>())
      if (d->isVisible()) return d;
    return nullptr;
  };
  const auto field = [](PartPropertiesDialog* d, const char* key) { return d->findChild<QLineEdit*>(QString("part.") + key); };
  const auto apply = [](PartPropertiesDialog* d) {
    for (auto* b : d->findChildren<QPushButton*>())
      if (b->objectName() == "primary") return b;
    return static_cast<QPushButton*>(nullptr);
  };
  try {
    // The documentation area's commands (DocsArea): records of their own, each right after its neighbour in every menu
    // that shows it (File, Inspect, the ribbon groups' menus) and in the selection's context menu, on both ribbons.
    const CommandInfo *bomCommand = w.m_commands.find("file.exportBom"), *partCommand = w.m_commands.find("inspect.partProperties");
    const auto follows = [&](const char* id, const char* anchor) {
      int menus = 0;
      for (QObject* o : w.action(id)->associatedObjects())
        if (auto* menu = qobject_cast<QMenu*>(o)) {
          const QList<QAction*> all = menu->actions();
          const qsizetype at = all.indexOf(w.action(id));
          if (at < 1 || all[at - 1] != w.action(anchor)) return false;
          ++menus;
        }
      return menus >= 2;
    };
    QMenu context;
    context.addAction(w.action("inspect.properties"));
    context.addAction(w.action("edit.delete"));
    docs->contextMenu({}, context);
    check(bomCommand && partCommand && !bomCommand->editsDocument && partCommand->editsDocument && bomCommand->menuPath == "file" &&
              partCommand->menuPath == "inspect" && bomCommand->workspaces.contains("review") && bomCommand->workspaces.contains("design") &&
              partCommand->workspaces.contains("review") && follows("file.exportBom", "file.export") && follows("inspect.partProperties", "inspect.properties") &&
              context.actions().value(1) == w.action("inspect.partProperties"),
          "Export bill of materials and Part properties are the area's commands, placed after Export… and Properties");
    w.m_doc->newDocument();
    const std::string plate = w.m_doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}})["body_ids"][0];
    const auto pin = [&](const char* x) {
      return w.m_doc->run("feature", {{"kind", "cylinder"}, {"inputs", {{"x", x}, {"y", "100 mm"}, {"diameter", "6 mm"}, {"height", "20 mm"}}}})["body_ids"][0].get<std::string>();
    };
    const std::string pin1 = pin("0 mm"), pin2 = pin("20 mm");
    const auto props = [&](const std::string& id) { return w.m_doc->node(id)->properties; };
    QElapsedTimer settle;  // the new bodies' display and selection jobs: a selection that lands later closes Properties
    settle.start();
    while (w.m_jobs->busy() && settle.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    w.m_selRefs = {opad::Ref{plate}};
    w.action("inspect.properties")->trigger();
    QCoreApplication::processEvents();
    const QString first = shown();
    check(w.m_propsPanel->isVisible() && first.contains("PART=") && first.contains("=Edit part properties…") && !first.contains("Part={"),
          "Properties ends in a PART section with its link, no raw part row: " + first);
    // A click on the link, as the mouse gives it.
    QTreeWidgetItem* link = table->findItems(QString::fromUtf8("Edit part properties…"), Qt::MatchExactly, 1).value(0);
    if (link) {
      table->scrollToItem(link);
      const QPoint at(table->header()->sectionViewportPosition(1) + 20, table->visualItemRect(link).center().y());
      QMouseEvent press(QEvent::MouseButtonPress, at, table->viewport()->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QMouseEvent release(QEvent::MouseButtonRelease, at, table->viewport()->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(table->viewport(), &press);
      QApplication::sendEvent(table->viewport(), &release);
    }
    PartPropertiesDialog* d = dialog();
    check(d && apply(d) && !apply(d)->isEnabled() && field(d, "part_number")->text().isEmpty(), "clicking it opens Part properties, nothing to apply yet");
    if (!d) throw opad::Error("no dialog");
    field(d, "part_number")->setText("OP-2001");
    field(d, "description")->setText("Base plate");
    auto* material = d->findChild<QComboBox*>("part.material");
    material->setCurrentIndex(material->findText("Aluminium 6061"));
    field(d, "vendor")->setText("Acme");
    QString hint;
    for (auto* l : d->findChildren<QLabel*>())
      if (l->objectName() == "secondary" && l->isVisibleTo(d)) hint = l->text();
    auto* colour = d->findChild<QCheckBox*>("part.appearance");
    check(hint.contains("Aluminium 6061") && hint.contains("2.7") && field(d, "density")->placeholderText().startsWith("2.7") && colour->isEnabled() && apply(d)->isEnabled(),
          "a library material shows its density and offers its colour: " + hint);
    colour->setChecked(true);
    d->adjustSize();
    d->grab().save(prefix + ".part.png");
    const size_t ops = w.m_doc->doc.ops.size();
    apply(d)->click();
    const opad::Material* alu = opad::material("aluminium-6061");
    const opad::Node* n = w.m_doc->node(plate);
    check(!dialog() && w.m_doc->doc.ops.size() == ops + 2 && w.m_doc->undoLabel() == "part properties" && props(plate).value("part_number", "") == "OP-2001" &&
              props(plate).value("material", "") == "aluminium-6061" && props(plate).value("vendor", "") == "Acme" && n->has_color && std::fabs(n->color[0] - alu->color[0]) < 1e-9,
          "Apply sets them and the material's colour as one step: " + QString::fromStdString(props(plate).dump()));
    measured();
    const QString after = shown();
    check(after.contains("PART=") && after.contains("Part number=OP-2001") && after.contains("Description=Base plate") && after.contains("Vendor=Acme") &&
              after.contains("Material=Aluminium 6061") && after.contains("Mass=" + units::format(units::Kind::Mass, 64.8)),
          "the panel shows them at once, and the plate's mass: " + after);
    table->scrollToBottom();
    w.m_propsPanel->grab().save(prefix + ".properties.png");

    docs->editPartProperties({pin1, pin2});
    d = dialog();
    field(d, "part_number")->setText("ISO 8734 6x20");
    auto* listed = d->findChild<QComboBox*>("part.bom");
    listed->setCurrentIndex(listed->findData("purchased"));
    auto* never = d->findChild<QCheckBox*>("part.section");
    check(never && !never->isChecked() && !never->isTristate(), "Part properties offers Never cut in section views, unticked");
    if (never) never->setChecked(true);
    apply(d)->click();
    check(w.m_doc->doc.ops.size() == ops + 4 && w.m_doc->undoLabel() == "part properties" && props(pin1).value("part_number", "") == "ISO 8734 6x20" &&
              props(pin2).value("bom", "") == "purchased" && props(pin1).value("section", opad::json()) == false && props(pin2).value("section", opad::json()) == false,
          "two pins get a part number, purchased and never cut in sections (section false) in one step");

    docs->editPartProperties({plate, pin1});
    d = dialog();
    check(d && field(d, "part_number")->text().isEmpty() && field(d, "part_number")->placeholderText() == "Several values" && field(d, "vendor")->text().isEmpty() &&
              !apply(d)->isEnabled() && d->findChild<QCheckBox*>("part.section")->checkState() == Qt::PartiallyChecked,
          "the plate and a pin: what they differ in shows as several values (Never cut half ticked), nothing to apply");
    field(d, "notes")->setText("Deburr");
    check(d->command()["set"] == opad::json({{"notes", "Deburr"}}) && apply(d)->isEnabled(), "typing one field changes only that field");
    apply(d)->click();
    check(props(plate).value("part_number", "") == "OP-2001" && props(pin1).value("part_number", "") == "ISO 8734 6x20" && props(plate).value("notes", "") == "Deburr" &&
              props(pin1).value("notes", "") == "Deburr",
          "each keeps its own part number, both get the notes");

    docs->editPartProperties({plate});
    d = dialog();
    field(d, "density")->setText("heavy");
    auto* error = d->findChild<QLabel*>("part.error");
    const bool refused = error->isVisibleTo(d) && !apply(d)->isEnabled();
    field(d, "density")->setText("2,8");
    check(refused && !error->isVisibleTo(d) && d->command()["set"] == opad::json({{"density", 2.8}}), "a density that is no number is refused; 2,8 reads as 2.8");
    d->reject();
    const size_t kept = w.m_doc->doc.ops.size();
    docs->editPartProperties({plate});
    d = dialog();
    field(d, "part_number")->clear();
    apply(d)->click();
    check(w.m_doc->doc.ops.size() == kept + 1 && !props(plate).contains("part_number") && !shown().contains("Part number="), "Cancel changes nothing; a cleared field is removed");
    w.action("edit.undo")->trigger();
    check(props(plate).value("part_number", "") == "OP-2001", "Ctrl+Z brings it back");

    w.m_doc->run("component", {{"name", "Pins"}});
    std::string pins;
    for (const auto& [id, node] : w.m_doc->scene.nodes)
      if (node.name == "Pins") pins = id;
    w.m_doc->run("reparent", {{"targets", {pin1, pin2}}, {"parent", pins}});
    w.action("file.exportBom")->trigger();
    auto* bom = w.findChild<BomDialog*>();
    if (!bom) throw opad::Error("no BoM dialog");
    const bool held = w.m_doc->designBusy;
    QElapsedTimer waited;
    const auto ready = [&] {
      waited.start();
      while (!bom->ready() && waited.elapsed() < 30000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      return bom->ready();
    };
    auto* preview = bom->findChild<QTreeWidget*>("bom.preview");
    const auto rows = [&] {  // "item qty part number name material mass" per row, children in brackets
      std::function<QString(QTreeWidgetItem*)> row = [&](QTreeWidgetItem* it) {
        QStringList cells;
        for (int c = 0; c < 6; ++c) cells << it->text(c);
        QStringList kids;
        for (int i = 0; i < it->childCount(); ++i) kids << row(it->child(i));
        return cells.join('|') + (kids.isEmpty() ? QString() : "[" + kids.join("; ") + "]");
      };
      QStringList out;
      for (int i = 0; i < preview->topLevelItemCount(); ++i) out << row(preview->topLevelItem(i));
      return out.join("; ");
    };
    const auto statusText = [&] {
      for (auto* l : bom->findChildren<QLabel*>())
        if (l->objectName() == "secondary") return l->text();
      return QString();
    };
    const auto previews = [&](const QString& want) { return ready() && rows() == want; };  // waits first: the message reads after
    bool pass = held && previews(QString::fromUtf8("1|1|OP-2001|Box1|Aluminium 6061|0.065; 2|2|ISO 8734 6x20|Cylinder||—")) && !w.m_doc->designBusy &&
                statusText().contains(QString::fromUtf8("2 rows · 3 parts")) && statusText().contains("1 without a mass");
    check(pass, "Export bill of materials previews the parts while the document is held, then lets it go: " + rows() + " / " + statusText());
    auto* mode = bom->findChild<QComboBox*>("bom.mode");
    mode->setCurrentIndex(mode->findData("indented"));
    pass = previews(QString::fromUtf8("1|1|OP-2001|Box1|Aluminium 6061|0.065; 2|1||Pins||—[2.1|2|ISO 8734 6x20|Cylinder||—]"));
    check(pass, "indented: the component with its pins below it: " + rows());
    const bool whole = !bom->findChild<QRadioButton*>("bom.selected")->isEnabled();
    bom->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    docs->exportBom({pins});  // with the component selected
    bom = w.findChild<BomDialog*>();
    preview = bom->findChild<QTreeWidget*>("bom.preview");
    mode = bom->findChild<QComboBox*>("bom.mode");
    pass = whole && bom->findChild<QRadioButton*>("bom.selected")->isChecked() && mode->currentData() == "indented" &&
           previews(QString::fromUtf8("1|2|ISO 8734 6x20|Cylinder||—"));
    check(pass, "nothing selected, the document's; a component selected, its own (the list kept as it was set): " + rows());
    bom->findChild<QRadioButton*>("bom.whole")->setChecked(true);
    mode->setCurrentIndex(mode->findData("parts"));
    auto* unit = bom->findChild<QComboBox*>("bom.unit");
    unit->setCurrentIndex(unit->findData("g"));
    auto* separator = bom->findChild<QComboBox*>("bom.separator");
    separator->setCurrentIndex(separator->findData(";"));
    ready();
    bom->grab().save(prefix + ".bom.png");
    const QString out = prefix + ".bom.csv";
    QFile::remove(out);
    bool written = false;
    QObject::connect(bom, &BomDialog::exported, &w, [&](const QString&, const QString& error) { written = error.isEmpty(); });
    w.m_doc->run("part_properties", {{"target", plate}, {"set", {{"description", "Base plate (anodised)"}}}});  // as an agent might, meanwhile
    bom->exportTo(out);
    waited.start();
    while (!written && waited.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QFile file(out);
    const QByteArray csv = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    pass = written && csv.startsWith("\xEF\xBB\xBF" "Item;Qty;Part number;Name;Description;Material;Mass (g);Total mass (g);Vendor;Purchased;Source;Notes\r\n") &&
           csv.contains("\r\n1;1;OP-2001;Box1;Base plate (anodised);Aluminium 6061;64.80;64.80;Acme;;design;Deburr\r\n") &&
           csv.contains("\r\n2;2;ISO 8734 6x20;Cylinder;;;;;;yes;design;Deburr\r\n") && w.statusBar()->currentMessage().contains("Bill of materials written to");
    check(pass, "Export CSV lists again what changed since and writes it in grams with semicolons, UTF-8 with a byte order mark: " + QString::fromUtf8(csv).replace("\r\n", " | "));
  } catch (const std::exception& e) {
    check(false, QString("unexpected error: %1").arg(QString::fromUtf8(e.what())));
  }
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
