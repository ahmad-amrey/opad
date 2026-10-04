// The Drawings workspace's annotations (TODO 11 UI-79, UI-80, UI-81): the Annotate tab's commands that start the sheet's
// tools (SheetAnnotate.hpp), centre marks drawn by the views themselves, dimensions from datums planned on a worker, the
// annotations' context menu (re-attach, delete) and their selection kept with the browser's Drawings folder.
#include <QAction>
#include <QMenu>
#include <QPointer>

#include <algorithm>

#include "AppDocument.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DocsArea.hpp"
#include "DrawingsFolder.hpp"
#include "Icons.hpp"
#include "Ribbon.hpp"
#include "SheetAnnotate.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/tables.hpp"

OPAD_ICON_TABLE(annotate,
                {"dimOrdinate", R"(<path d="M4 19h16M5 5v14M11 9v10M17 13v6"/><path d="M3 5h4M9 9h4M15 13h4" opacity=".55"/>)"},
                {"dimBaseline", R"(<path d="M5 4v16"/><path d="M5 8h8M5 13h12M5 18h15"/><path d="M13 6v4M17 11v4M20 16v4" opacity=".55"/>)"},
                {"dimChain", R"(<path d="M3 8v8M10 8v8M16 8v8M21 8v8"/><path d="M3 12h18"/>)"},
                {"dimDatums", R"(<rect x="3" y="14" width="7" height="7"/><path d="M6.5 14v-3M4.5 11h4L6.5 8z"/><path d="M14 20V5M19 20v-9M12 20h10" opacity=".55"/>)"},
                {"holeCallout", R"(<circle cx="8" cy="15" r="4"/><path d="M11 12l5-6h5"/><path d="M14 3h7" opacity=".55"/>)"},
                {"holeTable", R"(<rect x="3" y="4" width="18" height="16" rx="1"/><path d="M3 9h18M3 14.5h18M9 4v16"/><circle cx="15" cy="17.2" r="1.3" opacity=".55"/>)"},
                {"centerMark", R"(<circle cx="12" cy="12" r="6"/><path d="M12 3v4M12 10.5v3M12 17v4M3 12h4M10.5 12h3M17 12h4"/>)"},
                {"centerLine", R"(<path d="M3 12h5M10 12h1.5M13.5 12h1.5M17 12h4"/><path d="M5 6h14M5 18h14" opacity=".55"/>)"},
                {"centerMarksAuto", R"(<rect x="3" y="4" width="18" height="16" rx="1" opacity=".55"/><circle cx="12" cy="12" r="4"/><path d="M12 6v12M6 12h12" stroke-dasharray="2 1.5"/>)"},
                {"noteLeader", R"(<path d="M4 20l7-7"/><path d="M4 20l1-4 3 3z"/><path d="M11 13h3M15 9h6M15 13h6"/>)"},
                {"datumSymbol", R"(<rect x="8" y="3" width="8" height="8"/><path d="M12 11v5M8.5 21h7L12 16z"/>)"},
                {"featureFrame", R"(<rect x="2" y="7" width="20" height="10"/><path d="M8 7v10M15 7v10"/><circle cx="5" cy="12" r="1.6"/>)"},
                {"surfaceTexture", R"(<path d="M3 20h18"/><path d="M6 13l3 7 7-14h5"/><path d="M7.5 15.5h4"/>)"},
                {"reattach", R"(<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1"/><path d="M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1"/>)"},
                {"partsList", R"(<rect x="3" y="4" width="18" height="16" rx="1"/><path d="M3 15.5h18M3 11h18M8 4v16"/><path d="M11 7.5h7" opacity=".55"/>)"},
                {"balloon", R"(<circle cx="15" cy="9" r="6"/><path d="M10.8 13.2L4 20"/><path d="M14 7.5l1.5-1v5"/>)"},
                {"autoBalloon", R"(<circle cx="6.5" cy="7" r="3.5"/><circle cx="17.5" cy="7" r="3.5"/><path d="M8.2 10l3.3 8M15.8 10l-3.3 8"/><rect x="8" y="18" width="8" height="3" opacity=".55"/>)"},
                {"revisionTable", R"(<rect x="3" y="4" width="18" height="16" rx="1"/><path d="M3 9h18M8 4v16"/><path d="M11 13h7M11 16.5h5" opacity=".55"/>)"},
                {"issueRevision", R"(<path d="M6 21V3.5h11.5l-2.5 4 2.5 4H6"/><path d="M9 7.5h3" opacity=".55"/>)"});

using Tool = SheetAnnotator::Tool;

void DocsArea::buildAnnotateCommands() {
  const QPointer<DocsArea> self(this);
  const auto sheetShown = [self](const CommandContext& c) { return c.document && self && self->m_page && !self->m_page->sheet().empty(); };
  const auto add = [&](const char* id, const QString& label, const char* icon, std::function<void()> fn, std::function<bool(const CommandContext&)> when,
                       QStringList keywords = {}, bool checkable = false) {
    CommandInfo info;
    info.id = id;
    info.label = label;
    info.icon = icon;
    info.group = tr("Drawings");
    info.keywords = std::move(keywords);
    info.workspaces = {"drawings"};
    info.editsDocument = true;
    info.checkable = checkable;
    info.enabledWhen = std::move(when);
    return services().addCommand(info, [self, fn] {
      if (self) self->services().guarded(fn);
    });
  };
  const auto tool = [this](Tool t) { return [this, t] { startTool(t); }; };
  add("drawings.dimension", tr("Dimension"), "dimension", tool(Tool::Dimension), sheetShown,
      {"smart dimension", "length", "distance", "angle", "radius", "diameter", "tolerance", "precision"});
  add("drawings.ordinate", tr("Ordinate dimensions"), "dimOrdinate", tool(Tool::Ordinate), sheetShown, {"datum", "running dimensions", "coordinates"});
  add("drawings.baseline", tr("Baseline dimensions"), "dimBaseline", tool(Tool::Baseline), sheetShown, {"datum", "parallel dimensions"});
  add("drawings.chain", tr("Chain dimensions"), "dimChain", tool(Tool::Chain), sheetShown, {"continuous dimensions"});
  add("drawings.fromDatums", tr("Dimension from datums"), "dimDatums", [this] { dimensionFromDatums("ordinate"); }, sheetShown,
      {"datum A B C", "hole positions", "ordinate", "distances from edges"});
  add("drawings.fromDatums.baseline", tr("Baseline dimensions from datums"), "dimBaseline", [this] { dimensionFromDatums("baseline"); }, sheetShown);
  add("drawings.fromDatums.chain", tr("Chain dimensions from datums"), "dimChain", [this] { dimensionFromDatums("chain"); }, sheetShown);
  add("drawings.holeCallout", tr("Hole callout"), "holeCallout", tool(Tool::HoleCallout), sheetShown, {"thread", "counterbore", "countersink", "depth"});
  add("drawings.holeTable", tr("Hole table"), "holeTable", tool(Tool::HoleTable), sheetShown, {"hole chart", "tags", "positions"});
  add("drawings.centerMark", tr("Centre mark"), "centerMark", tool(Tool::CentreMark), sheetShown, {"center mark", "cross"});
  add("drawings.centerLine", tr("Centre line"), "centerLine", tool(Tool::CentreLine), sheetShown, {"center line", "axis", "symmetry"});
  QAction* marks = add("drawings.centerMarks", tr("Centre marks on views"), "centerMarksAuto", [] {}, sheetShown, {"center marks", "automatic", "axes"}, true);
  connect(marks, &QAction::triggered, this, [this](bool on) {
    services().guarded([&] {
      std::vector<std::string> v = m_page->canvas()->selectedViews();
      if (v.empty())
        if (const opad::Sheet* s = services().document()->scene.sheet(m_page->sheet())) v = s->views;
      setViewStyle(v, {{"centermarks", on}});
    });
  });
  add("drawings.note", tr("Note"), "noteLeader", tool(Tool::Note), sheetShown, {"text", "leader", "label"});
  add("drawings.datum", tr("Datum"), "datumSymbol", tool(Tool::Datum), sheetShown, {"datum feature", "GD&T", "A B C"});
  add("drawings.fcf", tr("Feature control frame"), "featureFrame", tool(Tool::Frame), sheetShown,
      {"GD&T", "geometric tolerance", "position", "flatness", "perpendicularity"});
  add("drawings.surface", tr("Surface texture"), "surfaceTexture", tool(Tool::Surface), sheetShown, {"roughness", "Ra", "finish"});
  add("drawings.partsList", tr("Parts list"), "partsList", tool(Tool::PartsList), sheetShown, {"bill of materials", "BoM table", "item list"});
  add("drawings.balloon", tr("Balloon"), "balloon", tool(Tool::Balloon), sheetShown, {"item number", "callout", "part number"});
  add("drawings.autoBalloon", tr("Auto-balloon"), "autoBalloon", [this] { autoBalloon(); }, sheetShown, {"balloons", "item numbers", "parts list"});
  add("drawings.revisionTable", tr("Revision table"), "revisionTable", tool(Tool::RevisionTable), sheetShown, {"revisions", "change history", "issue"});
  add("drawings.reattach", tr("Re-attach"), "reattach", [this] { reattachSelected(); },
      [self, sheetShown](const CommandContext& c) { return sheetShown(c) && self->m_page->canvas()->selectedItems().size() == 1; },
      {"dangling", "lost reference", "repair"});
}

void DocsArea::annotateRibbon(RibbonLayout& layout) {
  layout.addTab("drawings", "drawings.annotate", tr("Annotate"));
  const auto group = [&](const char* tab, const char* name, const QString& title) {
    const QString id = QString(tab) + "." + name;
    layout.addGroup(tab, id, title);
    return id;
  };
  const QString dims = group("drawings.annotate", "dimensions", tr("Dimensions"));
  layout.addAction(dims, services().action("drawings.dimension"));
  layout.addAction(dims, services().action("drawings.ordinate"), RibbonLayout::Size::Large,
                   {services().action("drawings.baseline"), services().action("drawings.chain")});
  layout.addAction(dims, services().action("drawings.fromDatums"), RibbonLayout::Size::Large,
                   {services().action("drawings.fromDatums.baseline"), services().action("drawings.fromDatums.chain")});
  layout.addAction(group("drawings.annotate", "holes", tr("Holes")), services().action("drawings.holeCallout"));
  const QString centres = group("drawings.annotate", "centres", tr("Centre lines"));
  for (const char* id : {"drawings.centerMark", "drawings.centerLine", "drawings.centerMarks"}) layout.addAction(centres, services().action(id), RibbonLayout::Size::Small);
  const QString symbols = group("drawings.annotate", "symbols", tr("Notes and symbols"));
  for (const char* id : {"drawings.note", "drawings.datum", "drawings.fcf", "drawings.surface"}) layout.addAction(symbols, services().action(id));
  layout.addAction(group("drawings.annotate", "check", tr("Check")), services().action("drawings.reattach"));
  // Tables: what lists the parts and the holes, and the balloons that number them on the views (UI-84).
  layout.addTab("drawings", "drawings.tables", tr("Tables"));
  const QString parts = group("drawings.tables", "parts", tr("Parts"));
  for (const char* id : {"drawings.partsList", "drawings.balloon", "drawings.autoBalloon"}) layout.addAction(parts, services().action(id));
  if (QAction* bom = services().action("file.exportBom")) layout.addAction(parts, bom, RibbonLayout::Size::Small);
  const QString other = group("drawings.tables", "other", tr("Other tables"));
  layout.addAction(other, services().action("drawings.holeTable"));
  layout.addAction(other, services().action("drawings.revisionTable"));
}

void DocsArea::readyAnnotate() {
  SheetAnnotator* annotator = m_page->annotator();
  SheetCanvas* canvas = m_page->canvas();
  annotator->setRunner([self = QPointer<DocsArea>(this)](const std::string& command, const opad::json& args, std::function<void(const opad::json&)> then) {
    if (self) self->run(command, args, std::move(then));
  });
  connect(annotator, &SheetAnnotator::message, this, [this](const QString& text) { services().toast(text); });
  connect(annotator, &SheetAnnotator::toolChanged, this, [this] { services().updateCommands(); });
  connect(annotator, &SheetAnnotator::added, this, [this] { services().updateCommands(); });
  connect(canvas, &SheetCanvas::itemSelectionChanged, this, [this, annotator](const std::vector<std::string>& items) {
    annotator->itemsSelected(items);
    if (!items.empty()) services().browser()->selectIds(items);
    services().updateCommands();
  });
  connect(m_page, &SheetPage::reattachRequested, this, [this, annotator, canvas](const std::string& item) {
    canvas->selectItems({item});
    annotator->reattach(item);
  });
}

void DocsArea::startTool(Tool tool) {
  if (!m_page || m_page->sheet().empty()) return newDrawing();
  services().setWorkspace("drawings");
  m_page->canvas()->setFocus();
  m_page->viewTool()->cancel();
  m_page->annotator()->start(tool);
}

void DocsArea::reattachSelected() {
  const auto items = m_page ? m_page->canvas()->selectedItems() : std::vector<std::string>{};
  if (items.size() != 1) throw opad::Error("Select the annotation to re-attach first.");
  m_page->annotator()->reattach(items[0]);
}

void DocsArea::dimensionFromDatums(const std::string& type) {
  if (!m_page || m_page->sheet().empty()) return newDrawing();
  const opad::Scene& s = services().document()->scene;
  const opad::Sheet* sheet = s.sheet(m_page->sheet());
  if (!sheet) return;
  std::string view;
  if (const auto selected = m_page->canvas()->selectedViews(); selected.size() == 1) view = selected[0];
  for (const auto& id : sheet->items)  // else the first view with datum symbols
    if (const opad::SheetItem* t = s.sheet_item(id); view.empty() && t && t->kind == "datum") view = t->view;
  if (view.empty()) throw opad::Error("Place datum symbols on a view first (Datum), then dimension it from them.");
  const opad::json args = {{"sheet", sheet->id}, {"view", view}, {"type", type}};
  auto ops = std::make_shared<opad::json>();
  QPointer<DocsArea> self(this);
  m_page->canvas()->read(
      tr("Dimensioning from the datums"), [args, ops](const opad::Document& doc, const opad::Scene& scene, Progress) { *ops = opad::drawing::datum_dimensions(doc, scene, args)["ops"]; },
      [self, ops](bool ok, const QString& error) {
        if (!self) return;
        if (!ok) return self->services().guarded([&] { throw opad::Error(error.toStdString()); });
        self->run("sheet_datum_dimensions", {{"ops", *ops}}, [self](const opad::json& out) {
          if (self && !out.is_null()) self->services().showMessage(tr("%n dimension sets from the datums", nullptr, static_cast<int>(out.value("ids", opad::json::array()).size())));
        });
      });
}

void DocsArea::autoBalloon() {
  if (!m_page || m_page->sheet().empty()) return newDrawing();
  const opad::Scene& s = services().document()->scene;
  const opad::Sheet* sheet = s.sheet(m_page->sheet());
  if (!sheet) return;
  std::string view;
  if (const auto selected = m_page->canvas()->selectedViews(); selected.size() == 1) view = selected[0];
  for (const char* want : {"iso", "iso-back", ""})  // a pictorial view shows every part, else the first base view
    for (const auto& id : sheet->views)
      if (const opad::SheetView* v = s.sheet_view(id); view.empty() && v && v->error.empty() && v->kind == "base" &&
                                                       (!*want || opad::drawing::view_orientation(s, *v) == want))
        view = id;
  if (view.empty()) throw opad::Error("Place a view first, then balloon its parts.");
  const opad::json args = {{"sheet", sheet->id}, {"view", view}};
  auto plan = std::make_shared<opad::json>();
  QPointer<DocsArea> self(this);
  m_page->canvas()->read(
      tr("Ballooning the view"), [args, plan](const opad::Document& doc, const opad::Scene& scene, Progress) { *plan = opad::drawing::plan_balloons(doc, scene, args); },
      [self, plan](bool ok, const QString& error) {
        if (!self) return;
        if (!ok) return self->services().guarded([&] { throw opad::Error(error.toStdString()); });
        if ((*plan)["ops"].empty()) {
          self->services().toast(tr("Every part the view shows has its balloon"));
          return;
        }
        self->run("sheet_balloons", {{"plan", *plan}}, [self](const opad::json& out) {
          if (!self || out.is_null()) return;
          const int n = static_cast<int>(out.value("ids", opad::json::array()).size());
          self->services().toast(out.value("created", false) ? tr("%n balloons and a parts list", nullptr, n) : tr("%n balloons", nullptr, n));
        });
      });
}

void DocsArea::renumberList(const std::string& list) {
  const opad::SheetItem* t = services().document()->scene.sheet_item(list);
  if (!t || t->kind != "parts_list" || !m_page) return;
  auto numbers = std::make_shared<opad::json>();
  QPointer<DocsArea> self(this);
  m_page->canvas()->read(
      tr("Numbering the parts list"),
      [list, numbers](const opad::Document& doc, const opad::Scene& scene, Progress) {
        const opad::SheetItem* item = scene.sheet_item(list);
        const opad::Sheet* sheet = item ? scene.sheet(item->sheet) : nullptr;
        if (!sheet) throw opad::Error("the parts list is gone");
        *numbers = opad::drawing::parts_rows(doc, scene, *sheet, item->def, true)["numbers"];
      },
      [self, list, numbers](bool ok, const QString& error) {
        if (!self) return;
        if (!ok) return self->services().guarded([&] { throw opad::Error(error.toStdString()); });
        self->run("sheet_edit", {{"target", list}, {"set", {{"numbers", *numbers}}}});
      });
}

void DocsArea::itemMenu(const std::vector<std::string>& items, QMenu& menu) {
  const opad::Scene& s = services().document()->scene;
  if (items.size() == 1) {
    const opad::SheetItem* t = s.sheet_item(items[0]);
    if (t && t->kind == "parts_list") {
      QAction* renumber = menu.addAction(icons::themed("partsList", 16), tr("Renumber items"), this, [this, id = items[0]] { services().guarded([&] { renumberList(id); }); });
      renumber->setObjectName("drawings.menu.renumber");
      renumber->setToolTip(tr("Numbers the rows 1, 2, 3… again in the bill of materials' order; balloons follow"));
    }
    const bool dangling = m_page->canvas()->dangling().count(items[0]) > 0;
    if (t && t->def.contains("refs")) {
      QAction* re = menu.addAction(icons::themed("reattach", 16), dangling ? tr("Re-attach (it lost what it measures)") : tr("Re-attach"), this,
                                   [this, id = items[0]] { services().guarded([&] { m_page->annotator()->reattach(id); }); });
      re->setObjectName("drawings.menu.reattach");
    }
    menu.addSeparator();
  }
  QAction* del = menu.addAction(icons::themed("delete", 16), (items.size() == 1 ? tr("Delete annotation") : tr("Delete %1 annotations").arg(items.size())) + "\tDel", this,
                                [this, items] { whenFree([this, items] { services().guarded([&] { drawings::remove(services().document(), items); }); }); });
  del->setObjectName("drawings.menu.deleteItem");
}
