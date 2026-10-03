#include "DocsArea.hpp"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QSettings>

#include <algorithm>

#include "AppDocument.hpp"
#include "BomExport.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DrawingsFolder.hpp"
#include "ExportJob.hpp"
#include "Icons.hpp"
#include "PartProperties.hpp"
#include "PropertiesPanel.hpp"
#include "Ribbon.hpp"
#include "SheetDialogs.hpp"

OPAD_ICON_TABLE(docs,
                {"drawingSheet", R"(<rect x="3" y="5" width="18" height="14" rx="1"/><path d="M13 19v-4h8"/><path d="M6 8h4" opacity=".55"/>)"},
                {"billOfMaterials", R"(<rect x="3" y="4" width="18" height="16" rx="1"/><path d="M3 8.5h18M3 12.5h18M3 16.5h18M8 4v16"/>)"},
                {"partProperties", R"(<path d="M3 4h8l10 10-7 7L4 11z"/><circle cx="7.5" cy="8" r="1.4"/>)"});

namespace {
void insertAfter(QMenu* menu, QAction* anchor, QAction* action) {
  const QList<QAction*> all = menu->actions();
  const qsizetype at = all.indexOf(anchor);
  if (at >= 0 && at + 1 < all.size()) menu->insertAction(all[at + 1], action);
  else menu->addAction(action);
}

std::vector<browser::Item> items(const std::vector<drawings::Row>& rows) {
  std::vector<browser::Item> out;
  for (const auto& r : rows) {
    browser::Item item{r.id, r.name, r.icon, r.tooltip, items(r.children)};
    item.editable = r.editable;
    item.error = r.error;
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<std::string> nodesOf(AppDocument* doc, std::vector<std::string> ids) {  // bodies and components
  ids.erase(std::remove_if(ids.begin(), ids.end(), [doc](const std::string& id) { return !doc->node(id); }), ids.end());
  return ids;
}
}  // namespace

void DocsArea::buildActions() {
  CommandInfo bom;
  bom.id = "file.exportBom";
  bom.label = tr("Export &bill of materials…");
  bom.icon = "billOfMaterials";
  bom.keywords = {"BoM", "parts list", "CSV"};
  bom.enabledWhen = [](const CommandContext& c) { return c.document; };
  services().addCommand(bom, [this] { exportBom(); });
  CommandInfo part;
  part.id = "inspect.partProperties";
  part.label = tr("Part properties…");
  part.icon = "partProperties";
  part.keywords = {"part number", "material", "vendor", "density"};
  part.editsDocument = true;
  services().addCommand(part, [this] { editPartProperties(services().selection().ids); });
  CommandInfo props;
  props.id = "file.documentProperties";
  props.label = tr("Document properties…");
  props.icon = "sheetProperties";
  props.keywords = {"title block", "company", "owner", "project", "approved", "revision"};
  props.editsDocument = true;
  props.enabledWhen = [](const CommandContext& c) { return c.document; };
  services().addCommand(props, [this] { services().guarded([&] { documentProperties(); }); });
  buildDrawingCommands();
}

void DocsArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  if (QMenu* file = menus.value("file")) {
    insertAfter(file, services().action("file.export"), services().action("file.exportBom"));
    insertAfter(file, services().action("file.exportBom"), services().action("file.documentProperties"));
  }
  if (QMenu* inspect = menus.value("inspect")) insertAfter(inspect, services().action("inspect.properties"), services().action("inspect.partProperties"));
}

void DocsArea::ribbon(RibbonLayout& layout) {
  const auto after = [&](const QString& group, const char* anchor, const char* id) {
    RibbonLayout::Group* g = layout.group(group);
    if (!g) return;
    qsizetype at = g->items.size();
    for (qsizetype i = 0; i < g->items.size(); ++i)
      if (g->items[i].action == services().action(anchor)) at = i + 1;
    RibbonLayout::Item item;
    item.action = services().action(id);
    g->items.insert(at, item);
  };
  after("review.inspect.results", "inspect.properties", "inspect.partProperties");
  after("review.export.export", "file.export", "file.exportBom");
  after("design.export.export", "file.export", "file.exportBom");
  drawingsRibbon(layout);
}

void DocsArea::ready() {
  AppDocument* doc = services().document();
  browser::Folder folder;
  folder.id = "drawings";
  folder.title = tr("Drawings");
  folder.items = [doc] { return items(drawings::rows(*doc)); };
  folder.contextMenu = [this](const std::string& id, QMenu& menu) {
    if (!id.empty()) rowMenu(id, menu);
  };
  // While the sheet canvas reads the document, renames and deletes wait for it (whenFree stops it).
  folder.rename = [this, doc](const std::string& id, const QString& name) {
    whenFree([this, doc, id, name] { services().guarded([&] { drawings::rename(doc, id, name); }); });
  };
  folder.remove = [this, doc](const std::vector<std::string>& ids) {
    whenFree([this, doc, ids] { services().guarded([&] { drawings::remove(doc, ids); }); });
    return true;
  };
  folder.activated = [this](const std::string& id) { services().guarded([&] { openSheet(id); }); };
  services().browser()->addFolder(folder);
  // A body's or component's part properties (what it sets itself) and the link to edit them.
  services().properties()->addSectionProvider([this](const PropertySubject& subject, const opad::json& props, QList<PropertySection>& out) {
    const parts::Section part = parts::section(props);
    if (part.title.isEmpty()) return;
    std::vector<std::string> ids;
    for (const auto& r : subject.refs)
      if (r.kind == opad::Ref::Kind::Body && std::find(ids.begin(), ids.end(), r.body) == ids.end()) ids.push_back(r.body);
    PropertySection section{part.title, part.rows, {}};
    section.actions.append({tr("Edit part properties…"), [this, ids] { services().guarded([&] { editPartProperties(ids); }); }});
    out << section;
  });
  readyDrawings();
}

void DocsArea::contextMenu(const SelectionContext& selection, QMenu& menu) {
  QAction* properties = services().action("inspect.properties");
  if (menu.actions().contains(properties)) insertAfter(&menu, properties, services().action("inspect.partProperties"));
  const std::vector<std::string> nodes = nodesOf(services().document(), selection.ids);
  if (!selection.sketching && !nodes.empty()) {  // bodies or components: a drawing of them
    QAction* draw = menu.addAction(icons::themed("drawingSheet", 16), tr("Create drawing…"), this, [this, nodes] {
      services().guarded([&] { newDrawing(nodes); });
    });
    draw->setObjectName("drawings.createFrom");
  }
}

void DocsArea::rowMenu(const std::string& id, QMenu& menu) {
  drawings::contextMenu(services().document(), id, menu, [this, id] { services().browser()->startRename(id); },
                        [this, id] { services().guarded([&] { exportSheet(id); }); });
}

void DocsArea::editPartProperties(std::vector<std::string> ids) {
  AppDocument* doc = services().document();
  ids = nodesOf(doc, std::move(ids));
  if (ids.empty()) throw opad::Error("Select bodies or components to give them part properties.");
  if (!services().requireEditable([this, ids] { services().guarded([&] { editPartProperties(ids); }); })) return;
  auto* dialog = new PartPropertiesDialog(doc, ids, services().window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &PartPropertiesDialog::applied, this, [this] {  // the panel shows the new fields
    if (services().properties()->isVisible()) services().action("inspect.properties")->trigger();
  });
  dialog->open();
}

void DocsArea::documentProperties() {
  AppDocument* doc = services().document();
  if (!doc->hasDocument) throw opad::Error("Open or create a document first.");
  if (!services().requireEditable([this] { services().guarded([&] { documentProperties(); }); })) return;
  auto* dialog = new DocumentPropertiesDialog(doc, services().window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this, [this, dialog] {
    const opad::json set = dialog->change();
    if (!set.is_null()) run("part_properties", {{"document", true}, {"set", set}});
  });
  dialog->open();
}

void DocsArea::exportBom(std::vector<std::string> ids) {
  AppDocument* doc = services().document();
  if (!doc->hasDocument) throw opad::Error("Nothing to export.");
  if (ids.empty()) ids = nodesOf(doc, services().selection().ids);
  auto* dialog = new BomDialog(doc, services().jobs(), ids, services().window());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  const QString stem = doc->doc.path.empty() ? (doc->browse ? QFileInfo(doc->viewing).completeBaseName() : tr("Untitled"))
                                             : QString::fromStdU16String(doc->doc.path.stem().u16string());
  connect(dialog, &BomDialog::exportRequested, this, [dialog, stem] {
    QSettings settings;
    const QString suggested = QDir(settings.value("ui/lastDir", QDir::homePath()).toString()).filePath(tr("%1 bill of materials.csv").arg(stem));
    QString out = QFileDialog::getSaveFileName(dialog, tr("Export bill of materials"), suggested, tr("CSV files (*.csv)"));
    if (out.isEmpty()) return;
    if (QFileInfo(out).suffix().isEmpty()) out += ".csv";
    settings.setValue("ui/lastDir", QFileInfo(out).absolutePath());
    dialog->exportTo(out);
  });
  connect(dialog, &BomDialog::exported, this, [this](const QString& path, const QString& error) {
    if (error.isEmpty()) services().showMessage(tr("Bill of materials written to %1").arg(QDir::toNativeSeparators(path)), 8000);
  });
  dialog->open();
}

void DocsArea::exportSheet(const std::string& id) {
  AppDocument* doc = services().document();
  QString stem;
  int sheets = 0;
  if (id.rfind("drawing:", 0) == 0) {
    for (const auto& s : doc->scene.sheets) sheets += s.drawing == id.substr(8);
    stem = QString::fromStdString(id.substr(8));
  } else if (const opad::Sheet* sheet = doc->scene.sheet(id)) {
    sheets = 1;
    stem = QString::fromStdString(sheet->name);
  }
  if (!sheets) throw opad::Error("That sheet is gone.");
  for (const QChar c : QString("<>:\"/\\|?*")) stem.replace(c, '_');
  QSettings settings;
  const QString last = sheets > 1 ? "pdf" : settings.value("export/sheetFormat", "pdf").toString();
  QString out = qEnvironmentVariable("OPAD_BENCH_EXPORT_OUT");  // benches: no file dialog
  if (out.isEmpty()) {
    QStringList filters;
    QString chosen;
    for (const auto& [f, label] : std::initializer_list<std::pair<QString, QString>>{
             {"pdf", tr("PDF files (*.pdf)")}, {"svg", tr("SVG files (*.svg)")}, {"dxf", tr("DXF files (*.dxf)")}, {"dwg", tr("DWG files (*.dwg)")}, {"png", tr("PNG pictures (*.png)")}}) {
      if (sheets > 1 && f != "pdf") continue;  // several sheets: the pages of one PDF
      filters << label;
      if (f == last) chosen = label;
    }
    out = QFileDialog::getSaveFileName(services().window(), sheets > 1 ? tr("Export drawing") : tr("Export sheet"),
                                       QDir(settings.value("ui/lastDir", QDir::homePath()).toString()).filePath(stem + "." + last), filters.join(";;"), &chosen);
    if (out.isEmpty()) return;
  }
  QString format = QFileInfo(out).suffix().toLower();
  if (format != "pdf" && format != "svg" && format != "dxf" && format != "dwg" && format != "png") out += "." + (format = last);
  settings.setValue("ui/lastDir", QFileInfo(out).absolutePath());
  if (sheets == 1) settings.setValue("export/sheetFormat", format);
  QPointer<DocsArea> self(this);
  whenFree([this, self, doc, format, out, id] {  // after the sheet canvas's worker
    services().guarded([&] {
      exportJob(doc, services().jobs(), services().window(), {{"format", format.toStdString()}, {"out", out.toStdString()}, {"sheet", id}}, out,
                [self](const opad::json& result) {
                  if (self) self->lastExport = result;
                });
    });
  });
}

DocsArea* DocsArea::of(const std::vector<AreaController*>& areas) {
  for (AreaController* area : areas)
    if (auto* docs = qobject_cast<DocsArea*>(area)) return docs;
  return nullptr;
}

OPAD_AREA(DocsArea)
