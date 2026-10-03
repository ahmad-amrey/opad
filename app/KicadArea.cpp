// KiCad boards in the window (UI-72 UI, UI-134; KicadArea.hpp): Insert KiCad PCB, the sync preview, projecting a board into a
// sketch and the board's Properties section.
#include "KicadArea.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <set>

#include "AppDocument.hpp"
#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "SketchEditor.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/kicad_pcb.hpp"
#include "opad/scene.hpp"

OPAD_ICON_TABLE(kicad,
                {"kicadboard", R"(<rect x="3" y="5" width="18" height="14" rx="1"/><rect x="9" y="9" width="6" height="6"/><path d="M15 12h3M6 9h3M6 15h3"/><circle cx="18" cy="8" r="1"/>)"});

namespace {
std::filesystem::path fsPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }
QString refOf(const QString& name) { return name.section(' ', 0, 0); }  // "J1 USB_C" -> "J1"

// The node ids of an import's KiCad records, by reference designator (the parts, and the mounting holes apart).
void nodesByRef(const opad::json& nodes, std::map<std::string, std::string>& parts, std::map<std::string, std::string>& holes) {
  for (const auto& n : nodes) {
    if (n.contains("kicad") && n["kicad"].is_object() && !n["kicad"].value("ref", "").empty())
      (n["kicad"].value("hole", false) ? holes : parts)[n["kicad"].value("ref", "")] = n.value("id", "");
    if (n.contains("children")) nodesByRef(n["children"], parts, holes);
  }
}
}  // namespace

KicadArea::KicadArea(AreaServices& services) : AreaController(services) {}

AssetsArea* KicadArea::assets() const { return services().window()->findChild<AssetsArea*>(); }

void KicadArea::buildActions() {
  auto add = [this](const char* id, const QString& label, const char* icon, const QString& group, const QStringList& keywords,
                    std::function<bool(const CommandContext&)> when, std::function<void()> fn, bool edits = false) {
    CommandInfo info;
    info.id = id;
    info.label = label;
    info.icon = icon;
    info.group = group;
    info.keywords = keywords;
    info.enabledWhen = std::move(when);
    info.editsDocument = edits;
    return services().addCommand(info, std::move(fn));
  };
  const QString kicad = tr("KiCad");
  add("kicad.insert", tr("Insert KiCad PCB…"), "kicadboard", kicad, {"kicad", "pcb", "board", "electronics", "link", "import"},
      [](const CommandContext& c) { return c.document && !c.sketching; }, [this] { insert(); });  // insert() asks a viewed file to be saved first
  add("kicad.previewSync", tr("Preview KiCad sync…"), "regen", kicad, {"kicad", "changes", "compare", "update", "pcb"},
      [this](const CommandContext& c) {
        const std::string b = c.document && !c.viewer ? boardOf(c.selection) : std::string();
        const AssetsArea* a = b.empty() ? nullptr : assets();
        const AssetMonitor::Asset* asset = a && a->monitor() ? a->monitor()->asset(b) : nullptr;
        return asset && asset->asset.value("storage", "linked") != "embedded";
      },
      [this] { preview(boardOf(services().selection())); });
  add("kicad.project", tr("Project KiCad board…"), "project", kicad, {"kicad", "outline", "mounting holes", "connector", "enclosure", "pcb"},
      [this](const CommandContext& c) { return c.sketching && !m_boards.empty(); }, [this] { project(); });
}

void KicadArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  if (QMenu* file = menus.value("file")) {  // after Link as asset…
    const QList<QAction*> entries = file->actions();
    QAction* before = nullptr;
    for (int i = 0; i < entries.size(); ++i)
      if (entries[i]->objectName() == "assets.link" || (!before && entries[i]->objectName() == "file.import")) before = entries.value(i + 1);
    file->insertAction(before, services().action("kicad.insert"));
  }
  if (QMenu* design = menus.value("design")) {
    QMenu* sub = design->addMenu(icons::themed("kicadboard", 16), tr("KiCad"));
    sub->setObjectName("kicad");
    for (const char* id : {"kicad.insert", "kicad.previewSync", "kicad.project"}) sub->addAction(services().action(id));
  }
}

void KicadArea::ribbon(RibbonLayout& layout) {
  if (RibbonLayout::Group* g = layout.group("design.assemble.components")) {  // after Link as asset, else after Import
    int at = 0;
    for (int i = 0; i < g->items.size(); ++i)
      if (g->items[i].action == services().action("assets.link") || g->items[i].action == services().action("file.import")) at = i + 1;
    RibbonLayout::Item item;
    item.action = services().action("kicad.insert");
    g->items.insert(std::min(at, int(g->items.size())), item);
  }
  layout.addAction("sketch.reference.reference", services().action("kicad.project"));
}

void KicadArea::ready() {
  buildPanel();
  if (AssetsArea* a = assets()) a->setPreviewer([this](const std::string& import) { preview(import); });
  services().properties()->addSectionProvider([this](const PropertySubject& s, const opad::json&, QList<PropertySection>& out) { section(s, out); });
  documentChanged(true);
}

void KicadArea::documentChanged(bool replaced) {
  m_boards.clear();
  const AppDocument* doc = services().document();
  std::set<std::string> deleted(doc->scene.deleted_ops.begin(), doc->scene.deleted_ops.end());
  for (const auto& o : doc->doc.ops)
    if (o.type == "import" && o.data.contains("kicad") && !deleted.count(o.id)) m_boards.push_back(o.id);
  if (m_panel && m_panel->isVisible() && (replaced || std::find(m_boards.begin(), m_boards.end(), m_import) == m_boards.end())) closePreview();
}

KicadArea::Board KicadArea::board(const std::string& import) const {
  Board b;
  b.import = import;
  const opad::Scene& scene = services().document()->scene;
  for (const auto& [id, n] : scene.nodes)
    if (n.source_op == import && (n.parent.empty() || scene.node(n.parent)->source_op != import)) b.root = id;
  const opad::Node* root = scene.node(b.root);
  if (!root) return b;
  b.name = QString::fromStdString(root->name);
  const AssetsArea* a = assets();
  const AssetMonitor::Asset* asset = a && a->monitor() ? a->monitor()->asset(import) : nullptr;
  b.linked = asset && asset->asset.value("storage", "linked") != "embedded";
  for (const auto& id : root->children) {
    const opad::Node* n = scene.node(id);
    if (!n || n->kind != opad::Node::Kind::Component) continue;
    if (n->name != "Layers") {
      b.parts.push_back({id, QString::fromStdString(n->name)});
      continue;
    }
    for (const auto& layer : n->children)
      if (const opad::Node* l = scene.node(layer); l && l->name == "Outline") b.outline = layer;
      else if (l && l->name == "Mounting holes") {
        b.holes = layer;
        for (const auto& h : l->children) b.holeNodes.push_back({h, QString::fromStdString(scene.node(h)->name)});
      }
  }
  std::sort(b.parts.begin(), b.parts.end(), [](const auto& x, const auto& y) { return QString::localeAwareCompare(x.second, y.second) < 0; });
  return b;
}

std::string KicadArea::boardOf(const SelectionContext& selection) const {
  for (const auto& id : selection.ids)
    if (const opad::Node* n = services().document()->node(id); n && std::find(m_boards.begin(), m_boards.end(), n->source_op) != m_boards.end()) return n->source_op;
  return {};
}

// ---------------------------------------------------------------- Insert KiCad PCB
void KicadArea::insert(const QString& file) {
  if (!services().requireEditable([this, file] { insert(file); })) return;
  QString path = file;
  if (path.isEmpty())
    path = QFileDialog::getOpenFileName(services().window(), tr("Insert KiCad PCB"), QSettings().value("ui/lastDir").toString(), tr("KiCad boards (*.kicad_pcb)"));
  if (!path.isEmpty()) services().importFile(path, true);  // the board's options first, then linked: watched with its 3D models
}

// ---------------------------------------------------------------- sync preview
void KicadArea::buildPanel() {
  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  m_list = new QTreeWidget(body);
  m_list->setObjectName("kicadSyncList");
  m_list->setHeaderHidden(true);
  m_list->setRootIsDecorated(true);
  m_list->setColumnCount(2);
  m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_list->header()->setStretchLastSection(true);
  connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
    const std::string id = item->data(0, Qt::UserRole).toString().toStdString();
    if (!id.empty() && services().document()->node(id)) services().browser()->selectIds({id});
  });
  layout->addWidget(m_list, 1);
  m_footer = new PanelFooter(body);
  m_footer->setPrimary(tr("Sync"));
  connect(m_footer, &PanelFooter::accepted, this, &KicadArea::sync);
  connect(m_footer, &PanelFooter::cancelled, this, &KicadArea::closePreview);
  layout->addWidget(m_footer);
  m_panel = new ToolPanel("kicadSync", "regen", &Tokens::sel, tr("Sync preview"), body, 360, services().window());
  m_panel->setObjectName("kicadSyncPanel");
  m_panel->setEscapeHandler([this] { closePreview(); });
  connect(m_panel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on) services().viewport()->clearLookLayer(LookSource::Compare);
  });
  services().addPanel(m_panel);
}

void KicadArea::preview(const std::string& import) {
  AssetsArea* a = assets();
  AssetMonitor* monitor = a ? a->monitor() : nullptr;
  const AssetMonitor::Asset* asset = monitor ? monitor->asset(import) : nullptr;
  if (!asset || asset->asset.value("storage", "linked") == "embedded") {
    services().toast(tr("Only a linked board is synced with its file"));
    return;
  }
  if (m_job) return;
  opad::json data;  // the import as its syncs left it (a copy: the worker reads the board, not the document)
  for (const auto& e : opad::effective_ops(services().document()->doc))
    if (e.op->id == import) data = e.data();
  const QString file = monitor->file(import), title = QString::fromStdString(asset->name);
  auto report = std::make_shared<opad::json>();
  m_job = services().jobs()->async(tr("Reading %1").arg(title), [data, import, file, report](Progress) { *report = opad::kicad_sync_preview(data, import, fsPath(file)); },
                                   [this, import, report, title](bool ok, const QString& error) {
                                     m_job = nullptr;
                                     if (!ok) {
                                       services().toast(tr("%1 could not be read: %2").arg(title, i18n::t(error)), {}, {}, 8000);
                                       return emit previewReady(false, error);
                                     }
                                     m_import = import;
                                     m_report = *report;
                                     fill();
                                     m_panel->setContext(title);
                                     services().openPanel(m_panel);
                                     emit previewReady(true, {});
                                   });
}

// The changes grouped (moved, flipped, models, footprints, added, removed, the board), each part's row selecting it; the parts
// that are on the board now tinted as the compare colours have them (moved blue, changed amber, removed red).
void KicadArea::fill() {
  m_list->clear();
  std::map<std::string, std::string> parts, holes;
  for (const auto& e : opad::effective_ops(services().document()->doc))
    if (e.op->id == m_import) nodesByRef(e.data().value("nodes", opad::json::array()), parts, holes);
  const Tokens& t = theme::current();
  std::map<std::string, LookDelta> tints;
  auto tint = [&](const std::string& id, const QColor& c) {
    if (id.empty()) return;
    LookDelta d;
    d.color = std::array<double, 3>{c.redF(), c.greenF(), c.blueF()};
    tints[id] = d;
  };
  auto length = [](double v) { return units::format(units::Kind::Length, v); };
  int count = 0;
  auto group = [&](const QString& title, const char* key, const QColor& color, std::function<QString(const opad::json&)> detail) {
    const opad::json& list = m_report.value(key, opad::json::array());
    if (list.empty()) return;
    auto* top = new QTreeWidgetItem(m_list, {QString("%1 (%2)").arg(title).arg(list.size())});
    top->setForeground(0, color);
    top->setFirstColumnSpanned(true);
    for (const auto& c : list) {
      const std::string ref = c.value("ref", "");
      const bool hole = c.value("hole", false);
      const std::string id = hole ? (holes.count(ref) ? holes[ref] : std::string()) : (parts.count(ref) ? parts[ref] : std::string());
      auto* row = new QTreeWidgetItem(top, {ref.empty() ? tr("(no reference)") : QString::fromStdString(ref), detail(c)});
      row->setData(0, Qt::UserRole, QString::fromStdString(id));
      if (hole) row->setToolTip(0, tr("Mounting hole"));
      if (std::string(key) != "added") tint(id, color);
      ++count;
    }
    top->setExpanded(true);
  };
  group(tr("Moved"), "moved", t.diffMoved, [&](const opad::json& c) {
    QString text = tr("by %1, %2").arg(length(c.value("dx", 0.0)), length(c.value("dy", 0.0)));
    if (const double turn = c.value("drot", 0.0); std::abs(turn) > 1e-6) text += ", " + tr("turned %1").arg(units::format(units::Kind::Angle, turn));
    return c.value("hole", false) ? tr("mounting hole %1").arg(text) : text;
  });
  group(tr("Flipped"), "flipped", t.diffMoved, [](const opad::json& c) { return c.value("side", "") == "bottom" ? tr("to the bottom") : tr("to the top"); });
  auto names = [](const opad::json& list) {
    QStringList out;
    for (const auto& m : list) out << QFileInfo(QString::fromStdString(m.get<std::string>())).fileName();
    return out.isEmpty() ? tr("none") : out.join(", ");
  };
  group(tr("3D model changed"), "models_changed", t.diffModified, [&](const opad::json& c) { return names(c.value("before", opad::json::array())) + " → " + names(c.value("after", opad::json::array())); });
  group(tr("Footprint changed"), "footprint_changed", t.diffModified,
        [](const opad::json& c) { return QString::fromStdString(c.value("before", "")) + " → " + QString::fromStdString(c.value("after", "")); });
  group(tr("Added"), "added", t.diffAdded, [](const opad::json& c) { return (c.value("hole", false) ? tr("mounting hole") + " " : QString()) + QString::fromStdString(c.value("footprint", "")); });
  group(tr("Removed"), "removed", t.diffRemoved, [](const opad::json& c) { return (c.value("hole", false) ? tr("mounting hole") + " " : QString()) + QString::fromStdString(c.value("footprint", "")); });
  QStringList board;  // the board itself
  if (m_report.contains("thickness"))
    board << tr("Thickness %1 → %2").arg(length(m_report["thickness"].value("before", 0.0)), length(m_report["thickness"].value("after", 0.0)));
  if (m_report.contains("holes")) board << tr("Drills %1 → %2").arg(m_report["holes"].value("before", 0)).arg(m_report["holes"].value("after", 0));
  if (m_report.contains("outline")) {
    const opad::json& o = m_report["outline"];
    board << tr("Outline area %1 → %2").arg(units::format(units::Kind::Area, o["before"].value("area", 0.0)), units::format(units::Kind::Area, o["after"].value("area", 0.0)));
  }
  if (!board.isEmpty()) {
    auto* top = new QTreeWidgetItem(m_list, {tr("Board")});
    top->setForeground(0, t.diffModified);
    top->setFirstColumnSpanned(true);
    for (const QString& line : board) new QTreeWidgetItem(top, {line});
    top->setExpanded(true);
  }
  const bool changed = m_report.value("changed", false);
  m_footer->setHint(changed ? tr("%1 changes · %2 unchanged").arg(count + int(board.size())).arg(m_report.value("unchanged", 0))
                            : tr("The board is as last synced"));
  m_footer->setPrimaryEnabled(changed);
  services().viewport()->setLookLayer(LookSource::Compare, std::move(tints));
}

void KicadArea::sync() {
  const std::string import = m_import;
  closePreview();
  if (AssetsArea* a = assets(); a && !import.empty()) a->sync({import});
}

void KicadArea::closePreview() {
  if (m_panel) m_panel->hide();
  services().viewport()->clearLookLayer(LookSource::Compare);
}

// ---------------------------------------------------------------- projecting into a sketch (UI-134)
QDialog* KicadArea::project() {
  if (!services().design() || !services().design()->sketchActive() || m_boards.empty()) return nullptr;
  auto* dialog = new QDialog(services().window());
  dialog->setObjectName("kicadProject");
  dialog->setWindowTitle(tr("Project KiCad board"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto* layout = new QVBoxLayout(dialog);
  auto* boards = new QComboBox(dialog);
  boards->setObjectName("kicadBoard");
  for (const auto& b : m_boards) boards->addItem(board(b).name, QString::fromStdString(b));
  boards->setVisible(m_boards.size() > 1);
  layout->addWidget(boards);
  auto* outline = new QCheckBox(dialog);
  outline->setObjectName("kicadOutline");
  auto* holes = new QCheckBox(dialog);
  holes->setObjectName("kicadHoles");
  layout->addWidget(outline);
  layout->addWidget(holes);
  auto* label = new QLabel(tr("Parts (their outline as this sketch sees them):"), dialog);
  layout->addWidget(label);
  auto* filter = new QLineEdit(dialog);
  filter->setObjectName("kicadPartFilter");
  filter->setPlaceholderText(tr("Filter by reference or footprint"));
  filter->setClearButtonEnabled(true);
  layout->addWidget(filter);
  auto* list = new QListWidget(dialog);
  list->setObjectName("kicadParts");
  layout->addWidget(list, 1);
  auto* linked = new QCheckBox(tr("Linked: follows the board when it is synced"), dialog);
  linked->setObjectName("kicadLinked");
  linked->setChecked(true);
  layout->addWidget(linked);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
  QPushButton* ok = buttons->addButton(tr("Project"), QDialogButtonBox::AcceptRole);
  ok->setObjectName("kicadProjectOk");
  ok->setDefault(true);
  layout->addWidget(buttons);
  auto fillBoard = [this, boards, outline, holes, list, filter] {
    const Board b = board(boards->currentData().toString().toStdString());
    outline->setText(tr("Board outline"));
    outline->setEnabled(!b.outline.empty());
    outline->setChecked(!b.outline.empty());
    holes->setText(b.holeNodes.empty() ? tr("Mounting holes (none)") : tr("Mounting holes (%1), also ones added later").arg(b.holeNodes.size()));
    holes->setEnabled(!b.holes.empty());
    holes->setChecked(!b.holes.empty());
    list->clear();
    for (const auto& [id, name] : b.parts) {
      auto* item = new QListWidgetItem(name, list);
      item->setData(Qt::UserRole, QString::fromStdString(id));
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      item->setCheckState(Qt::Unchecked);
    }
    filter->clear();
  };
  connect(boards, &QComboBox::currentIndexChanged, dialog, fillBoard);
  connect(filter, &QLineEdit::textChanged, dialog, [list](const QString& text) {
    for (int i = 0; i < list->count(); ++i) list->item(i)->setHidden(!text.isEmpty() && !list->item(i)->text().contains(text, Qt::CaseInsensitive));
  });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, boards, outline, holes, list, linked] {
    std::vector<std::string> parts;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked) parts.push_back(list->item(i)->data(Qt::UserRole).toString().toStdString());
    if (!outline->isChecked() && !holes->isChecked() && parts.empty()) return dialog->reject();
    if (!projectInto(boards->currentData().toString().toStdString(), outline->isChecked(), holes->isChecked(), parts, linked->isChecked()))
      services().toast(tr("The sketch is busy; try again in a moment."));
    dialog->accept();
  });
  fillBoard();
  dialog->resize(420, 520);
  dialog->open();
  return dialog;
}

bool KicadArea::projectInto(const std::string& import, bool outline, bool holes, const std::vector<std::string>& parts, bool linked) {
  SketchEditor* sketch = services().design() ? services().design()->sketch() : nullptr;
  if (!sketch || !sketch->active()) return false;
  const Board b = board(import);
  std::vector<opad::json> sources;
  auto source = [&](const char* what, const std::string& node, const QString& ref = {}) {
    opad::json s = {{"asset", import}, {"kicad", what}, {"node", node}};
    if (!ref.isEmpty()) s["ref"] = ref.toStdString();
    sources.push_back(s);
  };
  if (outline && !b.outline.empty()) source("outline", b.outline);
  if (holes && !b.holes.empty()) source("holes", b.holes);
  for (const auto& id : parts)
    if (const opad::Node* n = services().document()->node(id)) source("part", id, refOf(QString::fromStdString(n->name)));
  return !sources.empty() && sketch->projectSources(sources, linked);
}

// ---------------------------------------------------------------- Properties, context menu
void KicadArea::section(const PropertySubject& subject, QList<PropertySection>& out) {
  if (subject.refs.empty() || m_boards.empty()) return;
  const opad::Node* n = services().document()->node(subject.refs.front().body);
  if (!n || std::find(m_boards.begin(), m_boards.end(), n->source_op) == m_boards.end()) return;
  const std::string import = n->source_op;
  const Board b = board(import);
  if (subject.refs.front().body != b.root) return;  // the board's own node: its parts have the Linked file section's
  PropertySection sec;
  sec.title = tr("KiCad board");
  sec.rows << qMakePair(tr("Footprints placed"), QString::number(b.parts.size())) << qMakePair(tr("Mounting holes"), QString::number(b.holeNodes.size()))
           << qMakePair(tr("Exploded views"), tr("Kept together"));
  if (b.linked) sec.actions << qMakePair(tr("Preview sync…"), std::function<void()>([this, import] { QTimer::singleShot(0, this, [this, import] { preview(import); }); }));
  out << sec;
}

void KicadArea::contextMenu(const SelectionContext& selection, QMenu& menu) {
  const std::string import = selection.sketching ? std::string() : boardOf(selection);
  if (import.empty() || services().document()->browse) return;
  if (const AssetsArea* a = assets(); a && a->monitor() && a->monitor()->asset(import))
    menu.addAction(icons::themed("regen", 16), tr("Preview KiCad sync…"), this, [this, import] { preview(import); });
}

OPAD_AREA(KicadArea)
