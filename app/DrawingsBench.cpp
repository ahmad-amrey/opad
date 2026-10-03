#include "MainWindow.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QTreeWidget>

#include <cmath>
#include <functional>

#include "DrawingsFolder.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

// OPAD_BENCH_DRAWINGS=<prefix>: a 60 x 40 x 10 plate with a sheet (a front view, the side and iso views projected from
// it, a dimension, a note and an item of a newer OPAD) and part properties, made through the command layer as agents
// do. The timeline gets no marker for any of them; the browser's Drawings folder lists them nested and worded (the
// newer item marked); F2 renames a drawing and a sheet in place; Del on the front view deletes it with what hangs on it
// in one step and Ctrl+Z brings it all back; the context menu offers what fits a row and deletes the sheet.
// <prefix>.browser.png is the browser with the folder open.
bool MainWindow::benchDrawings() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_DRAWINGS");
  if (prefix.isEmpty()) return false;
  bool ok = true;
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: drawings: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto* tree = m_browser->findChild<QTreeWidget*>();
  // The folder as text: name[children], '!' after a row that is not drawn.
  const auto folder = [&]() -> QTreeWidgetItem* {
    QTreeWidgetItem* root = tree->topLevelItemCount() ? tree->topLevelItem(0) : nullptr;
    for (int i = 0; root && i < root->childCount(); ++i)
      if (root->child(i)->text(0) == "Drawings") return root->child(i);
    return nullptr;
  };
  std::function<QString(QTreeWidgetItem*)> text = [&](QTreeWidgetItem* it) {
    QStringList children;
    for (int i = 0; i < it->childCount(); ++i) children << text(it->child(i));
    return it->text(0) + (it->toolTip(0).contains("needs a newer OPAD") ? "!" : "") + (children.isEmpty() ? QString() : "[" + children.join(",") + "]");
  };
  const auto listed = [&] { QTreeWidgetItem* f = folder(); return f ? text(f) : QString(); };
  const auto row = [&](const std::string& id) -> QTreeWidgetItem* {
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* it) -> QTreeWidgetItem* {
      if (it->data(0, Qt::UserRole + 1).toString().toStdString() == id) return it;
      for (int i = 0; i < it->childCount(); ++i)
        if (QTreeWidgetItem* f = find(it->child(i))) return f;
      return nullptr;
    };
    return tree->topLevelItemCount() ? find(tree->topLevelItem(0)) : nullptr;
  };
  const auto renameInPlace = [&](const std::string& id, const QString& name) {
    m_browser->selectIds({id});
    action("edit.rename")->trigger();
    auto* editor = tree->viewport()->findChild<QLineEdit*>();
    if (!editor) return false;
    editor->setText(name);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();  // the delegate commits queued, then lets the editor go
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return !tree->viewport()->findChild<QLineEdit*>();
  };
  try {
    m_doc->newDocument();
    const opad::json made = m_doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}});
    const std::string body = made["body_ids"][0];
    const auto design = m_timeline->shownOps();
    std::string edge;  // along x at the top front: the plate's width in the front view
    const TopoDS_Shape shape = opad::node_world_shape(m_doc->doc, m_doc->scene, body);
    for (int i = 0; edge.empty() && i < opad::subshape_count(shape, opad::Ref::Kind::Edge); ++i) {
      const opad::Ref r{body, opad::Ref::Kind::Edge, i};
      const opad::json e = opad::inspect_ref(m_doc->doc, m_doc->scene, r);
      if (e.contains("direction") && std::fabs(std::fabs(e["direction"][0].get<double>()) - 1) < 1e-9 && std::fabs(e["bbox"]["center"][1].get<double>() + 20) < 1e-6 &&
          std::fabs(e["bbox"]["center"][2].get<double>() - 10) < 1e-6)
        edge = r.str();
    }
    const std::string sheet = m_doc->run("sheet", {{"size", "A4"}})["id"];
    const std::string front = m_doc->run("sheet_view", {{"sheet", sheet}, {"orient", "front"}, {"at", {80, 170}}})["id"];
    const std::string side = m_doc->run("sheet_view", {{"sheet", sheet}, {"parent", front}, {"side", "right"}})["id"];
    m_doc->run("sheet_view", {{"sheet", sheet}, {"parent", front}, {"side", "top-right"}});
    const std::string width = m_doc->run("sheet_item", {{"sheet", sheet}, {"view", front}, {"type", "horizontal"}, {"refs", {edge}}})["id"];
    m_doc->run("sheet_item", {{"sheet", sheet}, {"text", "BREAK SHARP EDGES"}});
    m_doc->run("append", {{"op", {{"op", "sheet_item"}, {"sheet", sheet}, {"view", side}, {"kind", "balloon"}}}});
    m_doc->run("part_properties", {{"target", body}, {"set", {{"part_number", "OP-1002"}, {"material", "aluminium-6061"}}}});
    check(m_timeline->shownOps() == design, QString("timeline keeps its %1 design markers, none for 8 drawing and properties ops").arg(design.size()));
    // UI-140: Properties names the material and its density at once, and the mass once the worker has the volume.
    showProperties({opad::Ref{body}});
    const auto shownProps = [&] {
      QStringList rows;
      if (auto* table = m_props->findChild<QTreeWidget*>())
        for (int i = 0; i < table->topLevelItemCount(); ++i) rows << table->topLevelItem(i)->text(0) + "=" + table->topLevelItem(i)->text(1);
      return rows.join("; ").remove(QChar(0x202A)).remove(QChar(0x202C));
    };
    const QString first = shownProps();
    QElapsedTimer waited;
    waited.start();
    while (m_propsJob && waited.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const QString measured = shownProps();
    check(first.contains("Material=Aluminium 6061") && first.contains(QString::fromUtf8("Density=2.7 g/cm³")) && !first.contains("Mass="),
          "Properties shows the material in force and its density before measuring: " + first);
    check(measured.contains("Mass=" + units::format(units::Kind::Mass, 64.8)) && measured.contains("Volume=24000"), "and the plate's mass (24000 mm3 of aluminium 6061) after: " + measured);

    const QString full = "Drawings[Drawing 1[Sheet 1[Front view[60],Left view[balloon!],Isometric view,BREAK SHARP EDGES]]]";
    check(listed() == full, "folder lists drawing > sheet > views with their items > the sheet's note: " + listed());
    QTreeWidgetItem *sheetRow = row(sheet), *widthRow = row(width);
    check(sheetRow && widthRow && sheetRow->toolTip(0).contains("A4") && sheetRow->toolTip(0).contains("first angle") && widthRow->toolTip(0).startsWith("Horizontal dimension"),
          "sheet and dimension rows say what they are: " + (sheetRow ? sheetRow->toolTip(0).replace('\n', " | ") : QString()));
    m_browser->selectIds({front});
    bool crumb = false;
    for (auto* label : m_browser->findChildren<QLabel*>()) crumb = crumb || (label->text().contains("Sheet 1") && label->text().contains("Front view"));
    check(m_viewport->selection().empty() && crumb, "selecting a view picks nothing in the model and the breadcrumb shows its sheet");

    const size_t ops = m_doc->doc.ops.size();
    const bool edited = renameInPlace("drawing:Drawing 1", "Plate") && renameInPlace(sheet, "Plate sheet");
    check(edited && m_doc->scene.sheet(sheet)->name == "Plate sheet" && m_doc->scene.sheet(sheet)->drawing == "Plate" && m_doc->doc.ops.size() == ops + 2 &&
              m_doc->undoLabel() == "rename" && listed().startsWith("Drawings[Plate[Plate sheet[") && m_timeline->shownOps() == design,
          "F2 renames the drawing and the sheet in place, one step each, no timeline marker");

    m_browser->selectIds({front});
    action("edit.delete")->trigger();
    check(m_doc->doc.ops.size() == ops + 3 && m_doc->undoLabel() == "delete" && m_doc->scene.sheet_views.empty() && m_doc->scene.sheet_items.size() == 1 &&
              listed() == "Drawings[Plate[Plate sheet[BREAK SHARP EDGES]]]",
          "Del on the front view takes its projected views and their items in one tombstone: " + listed());
    action("edit.undo")->trigger();
    check(listed() == QString(full).replace("Drawing 1", "Plate").replace("Sheet 1", "Plate sheet") && m_doc->doc.ops.size() == ops + 2, "Ctrl+Z brings them back");

    QMenu sheetMenu, itemMenu;
    drawings::contextMenu(m_doc, sheet, sheetMenu, [] {});
    drawings::contextMenu(m_doc, width, itemMenu, [] {});
    const auto has = [](QMenu& m, const char* name) { return m.findChild<QAction*>(name) != nullptr; };
    check(has(sheetMenu, "drawings.rename") && has(sheetMenu, "drawings.copyId") && has(itemMenu, "drawings.delete") && !has(itemMenu, "drawings.rename"),
          "context menu: rename, copy id and delete for a sheet; a dimension has no name to rename");
    if (QTreeWidgetItem* it = row(sheet)) {
      tree->scrollToItem(it);
      m_browser->resize(320, 420);
      const QPixmap shot = m_browser->grab();  // transparent over the viewport: put it on the theme's background
      QImage image(shot.size(), QImage::Format_ARGB32);
      image.setDevicePixelRatio(shot.devicePixelRatio());
      image.fill(theme::current().bg);
      QPainter(&image).drawPixmap(0, 0, shot);
      image.save(prefix + ".browser.png");
    }
    sheetMenu.findChild<QAction*>("drawings.delete")->trigger();
    check(!folder() && m_doc->scene.sheets.empty() && m_doc->undoLabel() == "delete", "deleting the only sheet from its menu leaves no Drawings folder");
    action("edit.undo")->trigger();
    check(folder() && m_doc->scene.sheets.size() == 1 && m_timeline->shownOps() == design, "Ctrl+Z restores the sheet; the timeline never changed");
  } catch (const std::exception& e) {
    check(false, QString("unexpected error: %1").arg(QString::fromUtf8(e.what())));
  }
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
