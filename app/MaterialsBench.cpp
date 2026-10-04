#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QMenu>
#include <QSettings>
#include <QTreeWidget>

#include <cmath>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "PropertiesPanel.hpp"
#include "SheetAnnotate.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "Toast.hpp"
#include "opad/materials.hpp"

// OPAD_BENCH_MATERIALS=<prefix> (UI-140): a 60 x 40 x 10 plate and a pin. Material is offered for the selection (Inspect
// menu, the ribbons, after Part properties in the context menu) as a menu of the library's materials with their
// densities; Aluminium 6061 on the plate is one step that colours it as the material (a toast with Undo), Properties then
// names it and gives the plate's mass (64.8 g); the menu checks the plate's material and offers None; with Colour as
// the material switched off (remembered), Steel on the pin leaves its colour; Undo; None clears the plate's. A parts list
// placed with Mass ticked in its bar has the column (MASS (g), the plate's 64.8); selected, unticking Mass removes it in
// one edit. <prefix>.properties.png.
OPAD_BENCH(OPAD_BENCH_MATERIALS, materials) {
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs != nullptr;
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: materials: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: materials: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  auto* table = w.m_props->findChild<QTreeWidget*>();
  const auto shown = [&] {  // the Properties rows as "label=value"
    QStringList rows;
    for (int i = 0; i < table->topLevelItemCount(); ++i) rows << table->topLevelItem(i)->text(0) + "=" + table->topLevelItem(i)->text(1);
    return rows.join("; ").remove(QChar(0x202A)).remove(QChar(0x202C));
  };
  AppDocument* doc = w.m_doc;
  QSettings().remove("parts/materialColour");
  try {
    doc->newDocument();
    const std::string plate = doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}})["body_ids"][0];
    const std::string pin = doc->run("feature", {{"kind", "cylinder"}, {"inputs", {{"x", "0 mm"}, {"y", "100 mm"}, {"diameter", "6 mm"}, {"height", "20 mm"}}}})["body_ids"][0];
    waitFor([&] { return !w.m_jobs->busy(); }, 10000);
    const auto select = [&](const std::string& id) {
      w.m_selRefs = {opad::Ref{id}};
      w.updateCommands();
    };
    QAction* material = w.action("inspect.material");
    select(plate);
    QMenu* menu = material ? material->menu() : nullptr;
    check(material && material->isEnabled() && menu, "Material is a command with a menu, enabled for a selected body");
    if (!menu) throw opad::Error("no menu");
    int menus = 0;
    for (QObject* o : material->associatedObjects()) menus += qobject_cast<QMenu*>(o) != nullptr;
    QMenu context;
    context.addAction(w.action("inspect.properties"));
    docs->contextMenu(w.selectionContext(), context);
    check(menus >= 2 && context.actions().value(2) == material, "in the Inspect menu and the ribbons' menus, and after Part properties in the context menu");
    const auto open = [&] {
      emit menu->aboutToShow();
      return menu->findChildren<QAction*>();
    };
    const auto item = [&](const char* name) { return menu->findChild<QAction*>(name); };
    open();
    int library = 0;
    for (QAction* a : menu->actions()) library += a->objectName().startsWith("material.") && a->isCheckable() && a->objectName() != "material.colour";
    check(library == static_cast<int>(opad::materials().size()) && item("material.aluminium-6061") && item("material.aluminium-6061")->text().contains("2.7") &&
              item("material.colour")->isChecked() && !item("material.none")->isEnabled(),
          QString("the library's %1 materials with their densities, Colour as the material on, None off (nothing set)").arg(library));
    const size_t ops = doc->doc.ops.size();
    item("material.aluminium-6061")->trigger();
    const opad::Material* al = opad::material("aluminium-6061");
    check(waitFor([&] { return doc->node(plate) && doc->node(plate)->properties.value("material", "") == "aluminium-6061"; }, 10000) && doc->doc.ops.size() > ops &&
              doc->node(plate)->has_color && std::fabs(doc->node(plate)->color[0] - al->color[0]) < 1e-6,
          "Aluminium 6061 on the plate: set, coloured as the material");
    bool toasted = false;
    for (Toast* t : w.m_toasts->toasts()) toasted = toasted || (t->text().contains("Aluminium 6061") && t->actionButton());
    check(toasted, "a toast names it, with Undo");
    w.action("inspect.properties")->trigger();
    const bool massShown = waitFor([&] { return !w.m_propsJob && shown().contains("Mass=64.8"); }, 20000);
    check(massShown && shown().contains("Material=Aluminium 6061"), "Properties names the material and gives the plate's mass: " + shown());
    w.m_propsPanel->grab().save(prefix + ".properties.png");
    open();
    check(item("material.aluminium-6061")->isChecked() && item("material.none")->isEnabled(), "the menu checks the plate's material and offers None");
    // Colour as the material off (remembered): Steel on the pin keeps its colour.
    item("material.colour")->setChecked(false);
    check(!QSettings().value("parts/materialColour", true).toBool(), "Colour as the material switched off and remembered");
    select(pin);
    const auto was = doc->node(pin)->color;
    const bool hadColour = doc->node(pin)->has_color;
    open();
    item("material.steel")->trigger();
    check(waitFor([&] { return doc->node(pin)->properties.value("material", "") == "steel"; }, 10000) && doc->node(pin)->has_color == hadColour && doc->node(pin)->color == was,
          "Steel on the pin: set, its colour left as it was");
    w.action("edit.undo")->trigger();
    check(waitFor([&] { return !doc->node(pin)->properties.contains("material"); }, 10000), "Undo takes it back");
    select(plate);
    open();
    item("material.none")->trigger();
    check(waitFor([&] { return !doc->node(plate)->properties.contains("material"); }, 10000), "None clears the plate's");
    QSettings().remove("parts/materialColour");
    open();
    item("material.aluminium-6061")->trigger();
    waitFor([&] { return doc->node(plate)->properties.contains("material"); }, 10000);

    // A parts list with its Mass column.
    w.action("workspace.drawings")->trigger();
    std::string sheet;
    docs->createDrawing({{"size", "A3"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "1:1"}, {"views", {"front", "iso"}}},
                        [&](const std::string& id) { sheet = id; });
    SheetPage* page = docs->sheetPage();
    SheetCanvas* canvas = page->canvas();
    SheetAnnotator* tools = page->annotator();
    const auto settled = [&] {
      const auto states = canvas->viewStates();
      return !canvas->busy() && !tools->busy() && !doc->designBusy && !states.empty() &&
             std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return v.final || !v.error.isEmpty(); });
    };
    check(waitFor([&] { return !sheet.empty(); }, 20000) && waitFor(settled, 30000), "a drawing of the plate and the pin");
    w.action("drawings.partsList")->trigger();
    check(tools->massBox() && tools->massBox()->isVisible() && !tools->massBox()->isChecked(), "the parts list's bar offers Mass, off");
    tools->massBox()->setChecked(true);
    const auto previewText = [&](const QString& text) {
      const auto& p = canvas->preview();
      return p && std::any_of(p->prims.begin(), p->prims.end(), [&](const opad::drawing::Prim& x) {
               return x.kind == opad::drawing::Prim::Kind::Text && QString::fromStdString(x.text).contains(text);
             });
    };
    const opad::Sheet* sh = doc->scene.sheet(sheet);
    const opad::drawing::Vec2 corner{sh->width - 10, 10 + 36};
    tools->moveTo(canvas->toScene(corner));
    check(waitFor([&] { tools->moveTo(canvas->toScene(corner)); return !tools->busy() && previewText("MASS (g)") && previewText("64.8"); }, 15000),
          "ticked: the preview has a MASS (g) column with the plate's 64.8");
    tools->clickAt(canvas->toScene(corner));
    std::string list;
    check(waitFor([&] {
            if (const opad::Sheet* s = doc->scene.sheet(sheet))
              for (const auto& id : s->items)
                if (const opad::SheetItem* t = doc->scene.sheet_item(id); t && t->kind == "parts_list") list = id;
            return !list.empty();
          }, 15000) && waitFor(settled, 30000),
          "placed");
    const auto columns = [&] { return doc->scene.sheet_item(list) ? doc->scene.sheet_item(list)->def.value("columns", opad::json()) : opad::json(); };
    const auto massColumn = [&] {
      const opad::json c = columns();
      return c.is_array() && std::find(c.begin(), c.end(), "mass") != c.end();
    };
    check(massColumn(), "its columns end in mass: " + QString::fromStdString(columns().dump()));
    tools->cancel();  // Esc: the tool ends, the bar edits the selected list
    canvas->selectItems({list});
    tools->itemsSelected({list});
    check(tools->massBox()->isVisible() && tools->massBox()->isChecked(), "selected, its bar shows Mass ticked");
    tools->massBox()->setChecked(false);
    check(waitFor([&] { return !massColumn() && columns().is_array() && !columns().empty(); }, 10000),
          "unticked: one edit, the column gone, the others kept: " + QString::fromStdString(columns().dump()));
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  QSettings().remove("parts/materialColour");
  trace::log(QString("bench: materials: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
