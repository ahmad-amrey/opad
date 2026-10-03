#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTreeWidget>

#include <cmath>

#include "PartProperties.hpp"
#include "opad/materials.hpp"

// OPAD_BENCH_BOM=<prefix>: a 60 x 40 x 10 plate and two pins. The Properties panel shows the plate's PART section with
// its link and no raw "part" row; a click on the link opens the Part properties dialog, which takes a part number, a
// library material (its density shown, colour as the material offered) and a vendor and applies them as one step, after
// which the panel shows them and the plate's mass. Both pins get a part number and "purchased" in one step; the plate and
// a pin together show their differing fields as several values and change only what is typed; a density that is not a
// number is refused; a cleared field is removed. <prefix>.part.png is the dialog, <prefix>.properties.png the panel.
bool MainWindow::benchBom() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_BOM");
  if (prefix.isEmpty()) return false;
  bool ok = true;
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: bom: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto* table = m_props->findChild<QTreeWidget*>();
  const auto shown = [&] {  // the Properties rows as "label=value"
    QStringList rows;
    for (int i = 0; i < table->topLevelItemCount(); ++i) rows << table->topLevelItem(i)->text(0) + "=" + table->topLevelItem(i)->text(1);
    return rows.join("; ").remove(QChar(0x202A)).remove(QChar(0x202C));
  };
  const auto measured = [&] {
    QElapsedTimer waited;
    waited.start();
    while (m_propsJob && waited.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  };
  const auto dialog = [&]() -> PartPropertiesDialog* {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // the one closed before
    for (auto* d : findChildren<PartPropertiesDialog*>())
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
    m_doc->newDocument();
    const std::string plate = m_doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}})["body_ids"][0];
    const auto pin = [&](const char* x) {
      return m_doc->run("feature", {{"kind", "cylinder"}, {"inputs", {{"x", x}, {"y", "100 mm"}, {"diameter", "6 mm"}, {"height", "20 mm"}}}})["body_ids"][0].get<std::string>();
    };
    const std::string pin1 = pin("0 mm"), pin2 = pin("20 mm");
    const auto props = [&](const std::string& id) { return m_doc->node(id)->properties; };
    QElapsedTimer settle;  // the new bodies' display and selection jobs: a selection that lands later closes Properties
    settle.start();
    while (m_jobs->busy() && settle.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    m_selRefs = {opad::Ref{plate}};
    action("inspect.properties")->trigger();
    QCoreApplication::processEvents();
    const QString first = shown();
    check(m_propsPanel->isVisible() && first.contains("PART=") && first.contains("=Edit part properties…") && !first.contains("Part={"),
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
    const size_t ops = m_doc->doc.ops.size();
    apply(d)->click();
    const opad::Material* alu = opad::material("aluminium-6061");
    const opad::Node* n = m_doc->node(plate);
    check(!dialog() && m_doc->doc.ops.size() == ops + 2 && m_doc->undoLabel() == "part properties" && props(plate).value("part_number", "") == "OP-2001" &&
              props(plate).value("material", "") == "aluminium-6061" && props(plate).value("vendor", "") == "Acme" && n->has_color && std::fabs(n->color[0] - alu->color[0]) < 1e-9,
          "Apply sets them and the material's colour as one step: " + QString::fromStdString(props(plate).dump()));
    measured();
    const QString after = shown();
    check(after.contains("PART=") && after.contains("Part number=OP-2001") && after.contains("Description=Base plate") && after.contains("Vendor=Acme") &&
              after.contains("Material=Aluminium 6061") && after.contains("Mass=64.8 g"),
          "the panel shows them at once, and the plate's mass: " + after);
    table->scrollToBottom();
    m_propsPanel->grab().save(prefix + ".properties.png");

    editPartProperties({pin1, pin2});
    d = dialog();
    field(d, "part_number")->setText("ISO 8734 6x20");
    auto* bom = d->findChild<QComboBox*>("part.bom");
    bom->setCurrentIndex(bom->findData("purchased"));
    apply(d)->click();
    check(m_doc->doc.ops.size() == ops + 4 && m_doc->undoLabel() == "part properties" && props(pin1).value("part_number", "") == "ISO 8734 6x20" &&
              props(pin2).value("bom", "") == "purchased",
          "two pins get a part number and purchased in one step");

    editPartProperties({plate, pin1});
    d = dialog();
    check(d && field(d, "part_number")->text().isEmpty() && field(d, "part_number")->placeholderText() == "Several values" && field(d, "vendor")->text().isEmpty() &&
              !apply(d)->isEnabled(),
          "the plate and a pin: what they differ in shows as several values, nothing to apply");
    field(d, "notes")->setText("Deburr");
    check(d->command()["set"] == opad::json({{"notes", "Deburr"}}) && apply(d)->isEnabled(), "typing one field changes only that field");
    apply(d)->click();
    check(props(plate).value("part_number", "") == "OP-2001" && props(pin1).value("part_number", "") == "ISO 8734 6x20" && props(plate).value("notes", "") == "Deburr" &&
              props(pin1).value("notes", "") == "Deburr",
          "each keeps its own part number, both get the notes");

    editPartProperties({plate});
    d = dialog();
    field(d, "density")->setText("heavy");
    auto* error = d->findChild<QLabel*>("part.error");
    const bool refused = error->isVisibleTo(d) && !apply(d)->isEnabled();
    field(d, "density")->setText("2,8");
    check(refused && !error->isVisibleTo(d) && d->command()["set"] == opad::json({{"density", 2.8}}), "a density that is no number is refused; 2,8 reads as 2.8");
    d->reject();
    const size_t kept = m_doc->doc.ops.size();
    editPartProperties({plate});
    d = dialog();
    field(d, "part_number")->clear();
    apply(d)->click();
    check(m_doc->doc.ops.size() == kept + 1 && !props(plate).contains("part_number") && !shown().contains("Part number="), "Cancel changes nothing; a cleared field is removed");
    action("edit.undo")->trigger();
    check(props(plate).value("part_number", "") == "OP-2001", "Ctrl+Z brings it back");
  } catch (const std::exception& e) {
    check(false, QString("unexpected error: %1").arg(QString::fromUtf8(e.what())));
  }
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
