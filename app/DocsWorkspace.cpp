// The Drawings workspace of the documentation area (TODO 11 UI-78): commands, the ribbon tab, the sheet page in the
// viewport's place and what its canvas asks for. Every edit is one command through run(), which waits until nothing reads
// the document (the canvas's own worker is stopped for it).
#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <filesystem>

#include "AppDocument.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DocsArea.hpp"
#include "DrawingsFolder.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "Ribbon.hpp"
#include "SheetCanvas.hpp"
#include "SheetDialogs.hpp"
#include "SheetPage.hpp"
#include "TemplateFields.hpp"
#include "opad/drawing/sheet.hpp"

OPAD_ICON_TABLE(sheets,
                {"print", R"(<path d="M7 9V3h10v6"/><rect x="3" y="9" width="18" height="8" rx="1"/><path d="M7 14h10v7H7z"/><path d="M17.5 12h1" opacity=".55"/>)"},
                {"sheetAdd", R"(<rect x="3" y="6" width="14" height="14" rx="1"/><path d="M19.5 2.5v6M16.5 5.5h6"/>)"},
                {"sheetProperties", R"(<rect x="3" y="4" width="18" height="16" rx="1"/><path d="M11 20v-5.5h10M11 17.2h10"/><path d="M6 8h5M6 11h3" opacity=".55"/>)"},
                {"templateFile", R"(<path d="M5 3h10l4 4v14H5z"/><path d="M15 3v4h4"/><path d="M10 21v-4.5h9"/>)"},
                {"viewBase", R"(<rect x="3" y="4" width="18" height="16" rx="1" opacity=".5"/><rect x="7" y="8" width="8" height="6"/>)"},
                {"viewProjected", R"(<rect x="3" y="8" width="7" height="7"/><rect x="14" y="8" width="7" height="7"/><path d="M10.5 11.5h3" stroke-dasharray="1.5 1.5"/><path d="M3 4h7M14 4h7" opacity=".55"/>)"},
                {"viewIso", R"(<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9z"/><path d="M4 7.5l8 4.5 8-4.5M12 12v9"/><path d="M2 2h4M2 2v4" opacity=".55"/>)"},
                {"hiddenLines", R"(<rect x="4" y="5" width="16" height="14"/><path d="M4 12h16" stroke-dasharray="3 2"/>)"},
                {"tangentEdges", R"(<path d="M3 19h7a7 7 0 0 0 7-7V4"/><path d="M8.5 15.5a6 6 0 0 0 5-5" opacity=".55"/>)"},
                {"viewsUpdate", R"(<rect x="3" y="5" width="11" height="9" rx="1"/><path d="M21 13a5 5 0 1 1-1.5-3.6M21 7.5v3h-3"/>)"},
                {"templateFields", R"(<rect x="3" y="4" width="18" height="16" rx="1" opacity=".55"/><rect x="6" y="12" width="12" height="5" stroke-dasharray="2 1.5"/><path d="M9 13.5v2" />)"});

namespace {
std::vector<std::string> nodesIn(AppDocument* doc, std::vector<std::string> ids) {  // bodies and components
  ids.erase(std::remove_if(ids.begin(), ids.end(), [doc](const std::string& id) { return !doc->node(id); }), ids.end());
  return ids;
}
std::vector<std::string> strings(const opad::json& v) {
  std::vector<std::string> out;
  if (v.is_array())
    for (const auto& e : v)
      if (e.is_string()) out.push_back(e.get<std::string>());
  return out;
}
std::vector<std::pair<std::string, QString>> baseViews() {  // the Base view button's other orientations
  return {{"top", DocsArea::tr("Base view from the top")}, {"right", DocsArea::tr("Base view from the right")}, {"left", DocsArea::tr("Base view from the left")},
          {"back", DocsArea::tr("Base view from the back")}, {"bottom", DocsArea::tr("Base view from below")}};
}
}  // namespace

// ---------------------------------------------------------------- commands and ribbon
void DocsArea::buildDrawingCommands() {
  // Asked by the window whenever it re-checks its commands, also while it goes (after this area): through a guard.
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
  add("drawings.new", tr("New drawing…"), "drawingSheet", [this] { newDrawing(); }, [](const CommandContext& c) { return c.document; },
      {"technical drawing", "sheet", "template", "title block", "projection"});
  add("drawings.newSheet", tr("New sheet"), "sheetAdd", [this] { newSheet(); }, sheetShown, {"page"});
  add("drawings.sheetProperties", tr("Sheet properties…"), "sheetProperties", [this] { sheetProperties(); }, sheetShown,
      {"title block", "paper", "size", "scale", "first angle", "third angle"});
  add("drawings.templateFile", tr("Template from DXF or DWG…"), "templateFile", [this] { templateFromFile(); }, sheetShown, {"frame", "title block", "company"});
  add("drawings.templateFields", tr("Title block fields…"), "templateFields", [this] { templateFields(); }, sheetShown, {"template", "attributes", "placeholders"});
  add("drawings.baseView", tr("Base view"), "viewBase", [this] { placeView("front"); }, sheetShown, {"front view", "place view"});
  for (const auto& [orient, label] : baseViews())
    add(("drawings.baseView." + orient).c_str(), label, "viewBase", [this, o = orient] { placeView(o); }, sheetShown);
  add("drawings.projectedView", tr("Projected view"), "viewProjected", [this] { placeProjected(); },
      [self, sheetShown](const CommandContext& c) { return sheetShown(c) && self->m_page->canvas()->selectedViews().size() == 1; }, {"side view", "top view", "orthographic"});
  add("drawings.isoView", tr("Isometric view"), "viewIso", [this] { placeView("iso"); }, sheetShown, {"pictorial", "3D view"});
  QAction* hidden = add("drawings.hiddenLines", tr("Hidden lines"), "hiddenLines", [] {}, sheetShown, {"dashed", "hidden edges"}, true);
  QAction* tangent = add("drawings.tangentEdges", tr("Tangent edges"), "tangentEdges", [] {}, sheetShown, {"smooth edges"}, true);
  const auto views = [this] {  // the selected views, else the sheet's
    std::vector<std::string> v = m_page->canvas()->selectedViews();
    if (v.empty())
      if (const opad::Sheet* s = services().document()->scene.sheet(m_page->sheet())) v = s->views;
    return v;
  };
  connect(hidden, &QAction::triggered, this, [this, views](bool on) { services().guarded([&] { setViewStyle(views(), {{"hidden", on}}); }); });
  connect(tangent, &QAction::triggered, this, [this, views](bool on) { services().guarded([&] { setViewStyle(views(), {{"tangent", on ? "thin" : "hide"}}); }); });
  add("drawings.update", tr("Update views"), "viewsUpdate", [this] { m_page->canvas()->refresh(); }, sheetShown, {"refresh", "regenerate", "rebuild"});
  add("drawings.fit", tr("Fit sheet"), "fit", [this] { m_page->canvas()->fitSheet(); }, sheetShown, {"zoom"});
  add("drawings.exportSheet", tr("Export sheet…"), "export", [this] { exportSheet(m_page->sheet()); }, sheetShown, {"PDF", "DXF", "DWG", "SVG", "PNG", "print"});
  add("drawings.issue", tr("Issue revision…"), "issueRevision", [this] { issueRevision(); }, sheetShown, {"release", "revision", "freeze", "git tag", "approve"});
  {
    CommandInfo print;
    print.id = "drawings.print";
    print.label = tr("Print…");
    print.icon = "print";
    print.key = QKeySequence("Ctrl+Alt+P");  // Ctrl+P is Properties
    print.group = tr("Drawings");
    print.keywords = {"printer", "plot", "paper", "preview", "PDF"};
    print.workspaces = {"drawings"};
    print.enabledWhen = sheetShown;
    services().addCommand(print, [self] {
      if (self) self->services().guarded([&] { self->printSheets(); });
    });
  }
  add("drawings.exportDrawing", tr("Export drawing as PDF…"), "export", [this] {
        const opad::Sheet* s = services().document()->scene.sheet(m_page->sheet());
        exportSheet(s && !s->drawing.empty() ? "drawing:" + s->drawing : m_page->sheet());
      }, sheetShown, {"PDF", "pages", "all sheets"});
  buildAnnotateCommands();
}

void DocsArea::drawingsRibbon(RibbonLayout& layout) {
  layout.addWorkspace("drawings", {tr("Drawings"), "drawingSheet", "Ctrl+3", tr("Technical drawings of the model: sheets with a frame and a title block, standard views, PDF, DXF and DWG."),
                                   tr("ops: sheet · sheet_view · sheet_item · properties")});
  layout.addTab("drawings", "drawings.drawing", tr("Drawing"));
  const auto group = [&](const char* name, const QString& title, std::initializer_list<const char*> ids) {
    const QString id = QString("drawings.drawing.") + name;
    layout.addGroup("drawings.drawing", id, title);
    for (const char* a : ids) layout.addAction(id, services().action(a));
  };
  group("sheet", tr("Sheet"), {"drawings.new", "drawings.newSheet", "drawings.sheetProperties", "file.documentProperties", "drawings.templateFile", "drawings.templateFields"});
  layout.addGroup("drawings.drawing", "drawings.drawing.views", tr("Views"));
  QList<QAction*> bases;
  for (const auto& [orient, label] : baseViews()) bases << services().action(QString::fromStdString("drawings.baseView." + orient));
  layout.addAction("drawings.drawing.views", services().action("drawings.baseView"), RibbonLayout::Size::Large, bases);
  layout.addAction("drawings.drawing.views", services().action("drawings.projectedView"));
  layout.addAction("drawings.drawing.views", services().action("drawings.isoView"));
  group("style", tr("Style"), {"drawings.hiddenLines", "drawings.tangentEdges", "drawings.update"});
  layout.addGroup("drawings.drawing", "drawings.drawing.output", tr("Output"));
  layout.addAction("drawings.drawing.output", services().action("drawings.print"));
  layout.addAction("drawings.drawing.output", services().action("drawings.exportSheet"), RibbonLayout::Size::Large, {services().action("drawings.exportDrawing")});
  for (const char* id : {"drawings.issue", "file.export", "file.exportBom", "drawings.fit"}) layout.addAction("drawings.drawing.output", services().action(id));
  annotateRibbon(layout);
}

// ---------------------------------------------------------------- the page
void DocsArea::readyDrawings() {
  m_page = new SheetPage(services().document(), services().jobs(), services().window());
  m_page->hide();
  SheetCanvas* canvas = m_page->canvas();
  canvas->setRunner([self = QPointer<DocsArea>(this)](const std::string& command, const opad::json& args, std::function<void(const opad::json&)> then) {
    if (self) self->run(command, args, std::move(then));
  });
  connect(m_page, &SheetPage::newDrawingRequested, this, [this] { services().guarded([&] { newDrawing(); }); });
  connect(m_page, &SheetPage::newSheetRequested, this, [this] { services().guarded([&] { newSheet(); }); });
  connect(m_page, &SheetPage::sheetShown, this, [this] { services().updateCommands(); });
  connect(m_page, &SheetPage::issueRequested, this, [this] { services().guarded([&] { issueRevision(); }); });
  connect(canvas, &SheetCanvas::selectionChanged, this, [this](const std::vector<std::string>& views) {
    services().browser()->selectIds(views);  // the window's selection follows: Properties, the status bar, the commands
    syncStyleActions();
    services().updateCommands();
  });
  connect(canvas, &SheetCanvas::contextMenuRequested, this, [this](const std::vector<std::string>& views, const QPoint& at) {
    QMenu menu;
    if (!views.empty() && services().document()->scene.sheet_item(views[0])) itemMenu(views, menu);
    else viewMenu(views, menu);
    if (!menu.isEmpty()) menu.exec(at);
  });
  connect(canvas, &SheetCanvas::deleteRequested, this, [this](const std::vector<std::string>& views) {
    whenFree([this, views] { services().guarded([&] { drawings::remove(services().document(), views); }); });
  });
  readyAnnotate();
  if (services().workspace() == "drawings") workspaceChanged("drawings");  // the one the window started in
}

void DocsArea::workspaceChanged(const QString& id) {
  if (!m_page) return;
  if (id == "drawings") {
    services().setCentralPage(m_page);
    m_page->documentChanged();
    m_page->canvas()->setFocus();
  } else if (services().centralPage() == m_page) {
    m_page->canvas()->cancelPlacement();
    m_page->annotator()->cancel();
    services().setCentralPage(nullptr);
  }
  services().updateCommands();
}

void DocsArea::documentChanged(bool) {
  if (!m_page) return;
  m_page->documentChanged();
  syncStyleActions();
}

void DocsArea::selectionChanged(const SelectionContext& selection) {
  if (!m_page || !m_page->isVisible()) return;
  std::vector<std::string> views, items;
  const opad::Scene& s = services().document()->scene;
  for (const auto& id : selection.ids) {
    if (const opad::SheetView* v = s.sheet_view(id); v && v->sheet == m_page->sheet()) views.push_back(id);
    if (const opad::SheetItem* t = s.sheet_item(id); t && t->sheet == m_page->sheet()) items.push_back(id);
  }
  m_page->canvas()->selectViews(views);
  m_page->canvas()->selectItems(items);
  m_page->annotator()->itemsSelected(items);
  syncStyleActions();
}

void DocsArea::syncStyleActions() {
  QAction *hidden = services().action("drawings.hiddenLines"), *tangent = services().action("drawings.tangentEdges"), *marks = services().action("drawings.centerMarks");
  if (!m_page || !hidden || !tangent) return;
  const opad::Scene& s = services().document()->scene;
  std::vector<std::string> views = m_page->canvas()->selectedViews();
  if (views.empty())
    if (const opad::Sheet* sheet = s.sheet(m_page->sheet())) views = sheet->views;
  bool anyHidden = false, allHidden = !views.empty(), anyTangent = false, allMarks = !views.empty();
  for (const auto& id : views) {
    const opad::SheetView* v = s.sheet_view(id);
    if (!v) continue;
    const opad::json style = v->def.value("style", opad::json::object());
    allMarks = allMarks && style.is_object() && style.value("centermarks", false);
    try {
      const auto spec = opad::drawing::view_spec(s, *v);
      anyHidden = anyHidden || spec.hidden;
      allHidden = allHidden && spec.hidden;
      anyTangent = anyTangent || spec.tangent;
    } catch (const std::exception&) {
    }
  }
  hidden->setChecked(allHidden && anyHidden);
  tangent->setChecked(anyTangent);
  if (marks) marks->setChecked(allMarks);
}

// ---------------------------------------------------------------- commands
void DocsArea::whenFree(std::function<void()> fn) {
  AppDocument* doc = services().document();
  if (doc->designBusy || doc->loading || doc->snapshotBusy() || doc->converting()) {
    SheetCanvas* canvas = m_page ? m_page->canvas() : nullptr;  // its worker stops, and none starts before fn has run
    if (canvas) canvas->pause();
    QTimer::singleShot(20, this, [this, fn, canvas] {
      whenFree(fn);
      if (canvas) canvas->resume();
    });
    return;
  }
  fn();
}

void DocsArea::run(const std::string& command, opad::json args, std::function<void(const opad::json&)> then) {
  whenFree([this, command, args, then] {
    opad::json out;
    bool ok = false;
    services().guarded([&] {
      out = services().document()->run(command, args);
      ok = true;
    });
    if (then) then(ok ? out : opad::json(nullptr));
  });
}

void DocsArea::newDrawing(std::vector<std::string> nodes) {
  AppDocument* doc = services().document();
  if (!doc->hasDocument) throw opad::Error("Open or create a document first.");
  if (!services().requireEditable([this, nodes] { services().guarded([&] { newDrawing(nodes); }); })) return;
  if (nodes.empty()) nodes = nodesIn(doc, services().selection().ids);
  auto* dialog = new NewDrawingDialog(doc, nodes, services().window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this, [this, dialog] {
    const opad::json args = dialog->args();
    services().guarded([&] { createDrawing(args); });
  });
  dialog->open();
}

void DocsArea::createDrawing(const opad::json& args, std::function<void(const std::string&)> done) {
  AppDocument* doc = services().document();
  QPointer<DocsArea> self(this);
  whenFree([this, self, doc, args, done] {
    // The views are measured on a worker first; the command then finds their boxes measured.
    Job* job = doc->readAsync(
        services().jobs(), tr("Laying out the drawing"),
        [args](const opad::Document& d, const opad::Scene& s, Progress) {
          const std::string standard = args.value("standard", "iso");
          opad::json sheet = {{"size", opad::drawing::paper_size(args.value("size", "A3"), args.value("orientation", "landscape") == "landscape")},
                              {"standard", standard}, {"projection", args.value("projection", standard == "asme" ? "third" : "first")}};
          sheet["template"] = opad::drawing::make_template(standard == "asme" ? "ansi" : "iso", sheet["size"]["w"].get<double>(), sheet["size"]["h"].get<double>());
          opad::json source = opad::json::object();
          if (args.contains("select")) source["nodes"] = args["select"];
          opad::drawing::plan_views(d, s, sheet, strings(args.value("views", opad::json::array({"front"}))), source, args.value("scale", "auto"));
        },
        [self, args, done](bool ok, const QString& error) {
          if (!self) return;
          if (!ok) {
            if (error != "cancelled") self->services().guarded([&] { throw opad::Error(error.toStdString()); });
            return;
          }
          self->run("sheet", args, [self, done](const opad::json& out) {
            if (!self || out.is_null()) return;
            const std::string id = out.value("id", "");
            self->services().setWorkspace("drawings");
            self->m_page->showSheet(id);
            self->services().showMessage(tr("Drawing created at %1").arg(QString::fromStdString(out.value("scale", ""))));
            if (done) done(id);
          });
        });
    if (!job) QTimer::singleShot(50, this, [this, args, done] { createDrawing(args, done); });
  });
}

void DocsArea::newSheet() {
  AppDocument* doc = services().document();
  const opad::Sheet* s = m_page ? doc->scene.sheet(m_page->sheet()) : nullptr;
  if (!s) return newDrawing();
  opad::json args = {{"drawing", s->drawing}, {"standard", s->standard}, {"projection", s->projection}, {"scale", opad::drawing::scale_text(s->scale)}};
  const opad::json size = s->def.value("size", opad::json::object());
  if (size.contains("preset")) {
    args["size"] = size["preset"];
    args["orientation"] = s->width >= s->height ? "landscape" : "portrait";
  } else {
    args["width"] = s->width, args["height"] = s->height;
  }
  const opad::json t = s->def.value("template", opad::json());
  if (!t.is_object()) args["template"] = "none";
  else if (t.value("id", "") == "iso" || t.value("id", "") == "ansi") args["template"] = t["id"];
  else args["template"] = t;  // a file's: the same geometry
  if (s->def.contains("values")) args["values"] = s->def["values"];
  run("sheet", args, [this](const opad::json& out) {
    if (!out.is_null()) m_page->showSheet(out.value("id", ""));
  });
}

void DocsArea::sheetProperties() {
  const std::string id = m_page ? m_page->sheet() : std::string();
  if (id.empty()) throw opad::Error("Open a sheet first.");
  auto* dialog = new SheetPropertiesDialog(services().document(), id, services().window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this, [this, dialog, id] {
    services().guarded([&] {
      const opad::json set = dialog->change();
      if (!set.is_null()) run("sheet_edit", {{"target", id}, {"set", set}});
    });
  });
  dialog->open();
}

void DocsArea::templateFromFile(const QString& given) {
  const std::string sheet = m_page ? m_page->sheet() : std::string();
  if (sheet.empty()) throw opad::Error("Open a sheet first.");
  QString file = given;
  if (file.isEmpty()) {
    QSettings settings;
    file = QFileDialog::getOpenFileName(services().window(), tr("Template from a drawing file"), settings.value("ui/lastDir", QDir::homePath()).toString(),
                                        tr("Drawings (*.dxf *.dwg)"));
    if (file.isEmpty()) return;
    settings.setValue("ui/lastDir", QFileInfo(file).absolutePath());
  }
  auto brep = std::make_shared<std::string>();
  auto t = std::make_shared<opad::json>();
  const auto path = std::filesystem::path(file.toStdU16String());
  QPointer<DocsArea> self(this);
  services().jobs()->async(
      tr("Reading the template"), [path, brep, t](Progress) { *t = opad::drawing::read_template_file(path, *brep); },
      [self, sheet, brep, t](bool ok, const QString& error) {
        if (!self) return;
        if (!ok) return self->services().guarded([&] { throw opad::Error(error.toStdString()); });
        opad::json set = {{"template", *t}, {"template_brep", *brep}};
        if (t->contains("size")) set["size"] = (*t)["size"];  // its paper
        const int fields = static_cast<int>(t->value("fields", opad::json::array()).size());
        self->run("sheet_edit", {{"target", sheet}, {"set", set}}, [self, fields](const opad::json& out) {
          if (!self || out.is_null()) return;
          self->services().showMessage(fields ? tr("Template fields filled in from the drawing: %1").arg(fields)
                                              : tr("The template has no fields: place them with Title block fields…"), 8000);
        });
      });
}

void DocsArea::templateFields() {
  const std::string sheet = m_page ? m_page->sheet() : std::string();
  if (sheet.empty()) throw opad::Error("Open a sheet first.");
  auto* dialog = new TemplateFieldsDialog(services().document(), services().jobs(), sheet, services().window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this, [this, dialog, sheet] {
    const opad::json set = dialog->change();
    if (!set.is_null()) run("sheet_edit", {{"target", sheet}, {"set", set}});
  });
  dialog->open();
}

void DocsArea::placeView(const std::string& orient) {
  if (!m_page || m_page->sheet().empty()) return newDrawing();
  services().setWorkspace("drawings");
  m_page->canvas()->setFocus();
  m_page->canvas()->placeBase(orient);
}

void DocsArea::placeProjected() {
  const std::vector<std::string> views = m_page ? m_page->canvas()->selectedViews() : std::vector<std::string>{};
  if (views.size() != 1) throw opad::Error("Select the view to project from first.");
  m_page->canvas()->setFocus();
  m_page->canvas()->placeProjected(views[0]);
}

void DocsArea::setViewStyle(const std::vector<std::string>& views, const opad::json& style) {
  const opad::Scene& s = services().document()->scene;
  opad::json ops = opad::json::array();
  for (const auto& id : views) {
    const opad::SheetView* v = s.sheet_view(id);
    if (!v || !v->error.empty()) continue;
    opad::json merged = v->def.value("style", opad::json::object());
    if (!merged.is_object()) merged = opad::json::object();
    merged.update(style);
    if (merged != v->def.value("style", opad::json::object())) ops.push_back({{"op", "edit"}, {"target", id}, {"set", {{"style", merged}}}});
  }
  if (!ops.empty()) run("append", {{"ops", ops}});
}

void DocsArea::openSheet(const std::string& row) {
  const opad::Scene& s = services().document()->scene;
  std::string sheet, view;
  if (row.rfind("drawing:", 0) == 0) {
    for (const auto& x : s.sheets)
      if (x.drawing == row.substr(8)) {
        sheet = x.id;
        break;
      }
  } else if (s.sheet(row)) {
    sheet = row;
  } else if (const opad::SheetView* v = s.sheet_view(row)) {
    sheet = v->sheet, view = row;
  } else if (const opad::SheetItem* t = s.sheet_item(row)) {
    sheet = t->sheet, view = t->view;
  }
  if (sheet.empty() || !m_page) return;
  services().setWorkspace("drawings");
  m_page->showSheet(sheet);
  if (!view.empty()) m_page->canvas()->selectViews({view});
}

void DocsArea::viewMenu(const std::vector<std::string>& views, QMenu& menu) {
  const opad::Scene& s = services().document()->scene;
  if (views.empty()) {  // on the paper: what goes onto it
    for (const char* id : {"drawings.baseView", "drawings.isoView", "drawings.sheetProperties", "drawings.templateFields", "drawings.newSheet", "drawings.print",
                            "drawings.exportSheet", "drawings.issue"})
      if (QAction* a = services().action(id)) menu.addAction(a);
    return;
  }
  if (views.size() == 1) {
    QAction* projected = menu.addAction(icons::themed("viewProjected", 16), tr("Add projected view"), this, [this] { services().guarded([&] { placeProjected(); }); });
    projected->setObjectName("drawings.menu.projected");
  }
  bool hidden = true, base = true;
  std::string tangent;
  for (const auto& id : views)
    if (const opad::SheetView* v = s.sheet_view(id)) {
      try {
        const auto spec = opad::drawing::view_spec(s, *v);
        hidden = hidden && spec.hidden;
      } catch (const std::exception&) {
      }
      base = base && v->kind == "base";
      if (tangent.empty()) {
        const opad::json st = v->def.value("style", opad::json::object());
        tangent = st.is_object() && st.contains("tangent") && st["tangent"].is_string() ? st["tangent"].get<std::string>() : "thin";
      }
    }
  QAction* h = menu.addAction(icons::themed("hiddenLines", 16), tr("Hidden lines"), this, [this, views, hidden] { setViewStyle(views, {{"hidden", !hidden}}); });
  h->setCheckable(true);
  h->setChecked(hidden);
  h->setObjectName("drawings.menu.hidden");
  QMenu* edges = menu.addMenu(icons::themed("tangentEdges", 16), tr("Tangent edges"));
  auto* group = new QActionGroup(edges);
  for (const auto& [value, label] : std::initializer_list<std::pair<const char*, QString>>{{"thin", tr("Thin")}, {"show", tr("As edges")}, {"hide", tr("Hidden")}}) {
    QAction* a = edges->addAction(label, this, [this, views, v = std::string(value)] { setViewStyle(views, {{"tangent", v}}); });
    a->setCheckable(true);
    a->setChecked(tangent == value);
    group->addAction(a);
  }
  if (base) {  // a base view's scale; the views projected from it follow
    QMenu* scale = menu.addMenu(tr("View scale"));
    const opad::Sheet* sheet = s.sheet(m_page->sheet());
    scale->addAction(tr("Sheet scale (%1)").arg(sheet ? QString::fromStdString(opad::drawing::scale_text(sheet->scale)) : QString()), this, [this, views] {
      opad::json ops = opad::json::array();
      for (const auto& id : views) ops.push_back({{"op", "edit"}, {"target", id}, {"set", {{"scale", "sheet"}}}});
      run("append", {{"ops", ops}});
    });
    for (const char* text : {"10:1", "5:1", "2:1", "1:1", "1:2", "1:5", "1:10", "1:20", "1:50", "1:100"})
      scale->addAction(QString::fromLatin1(text), this, [this, views, v = std::string(text)] {
        opad::json ops = opad::json::array();
        for (const auto& id : views) ops.push_back({{"op", "edit"}, {"target", id}, {"set", {{"scale", v}}}});
        run("append", {{"ops", ops}});
      });
  }
  menu.addSeparator();
  if (views.size() == 1)
    menu.addAction(icons::themed("rename", 16), tr("Rename"), this, [this, id = views[0]] { services().browser()->startRename(id); });
  QAction* del = menu.addAction(icons::themed("delete", 16), views.size() == 1 ? tr("Delete view") + "\tDel" : tr("Delete %1 views").arg(views.size()) + "\tDel", this, [this, views] {
    whenFree([this, views] { services().guarded([&] { drawings::remove(services().document(), views); }); });
  });
  del->setObjectName("drawings.menu.delete");
}
