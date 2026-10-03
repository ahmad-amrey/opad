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
                {"reattach", R"(<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1"/><path d="M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1"/>)"});

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
  add("drawings.reattach", tr("Re-attach"), "reattach", [this] { reattachSelected(); },
      [self, sheetShown](const CommandContext& c) { return sheetShown(c) && self->m_page->canvas()->selectedItems().size() == 1; },
      {"dangling", "lost reference", "repair"});
}

void DocsArea::annotateRibbon(RibbonLayout& layout) {
  layout.addTab("drawings", "drawings.annotate", tr("Annotate"));
  const auto group = [&](const char* name, const QString& title) {
    const QString id = QString("drawings.annotate.") + name;
    layout.addGroup("drawings.annotate", id, title);
    return id;
  };
  const QString dims = group("dimensions", tr("Dimensions"));
  layout.addAction(dims, services().action("drawings.dimension"));
  layout.addAction(dims, services().action("drawings.ordinate"), RibbonLayout::Size::Large,
                   {services().action("drawings.baseline"), services().action("drawings.chain")});
  layout.addAction(dims, services().action("drawings.fromDatums"), RibbonLayout::Size::Large,
                   {services().action("drawings.fromDatums.baseline"), services().action("drawings.fromDatums.chain")});
  const QString holes = group("holes", tr("Holes"));
  layout.addAction(holes, services().action("drawings.holeCallout"));
  layout.addAction(holes, services().action("drawings.holeTable"));
  const QString centres = group("centres", tr("Centre lines"));
  for (const char* id : {"drawings.centerMark", "drawings.centerLine", "drawings.centerMarks"}) layout.addAction(centres, services().action(id), RibbonLayout::Size::Small);
  const QString symbols = group("symbols", tr("Notes and symbols"));
  for (const char* id : {"drawings.note", "drawings.datum", "drawings.fcf", "drawings.surface"}) layout.addAction(symbols, services().action(id));
  layout.addAction(group("check", tr("Check")), services().action("drawings.reattach"));
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

void DocsArea::itemMenu(const std::vector<std::string>& items, QMenu& menu) {
  const opad::Scene& s = services().document()->scene;
  if (items.size() == 1) {
    const opad::SheetItem* t = s.sheet_item(items[0]);
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
