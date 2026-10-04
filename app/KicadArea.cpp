// KiCad boards in the window (UI-72 UI, UI-134; KicadArea.hpp): Insert KiCad PCB, the sync preview, projecting a board into a
// sketch, the board's Properties section and the small-part filter.
#include "KicadArea.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
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
#include <tuple>

#include "AppDocument.hpp"
#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BrowserPanel.hpp"
#include "CheckPanel.hpp"
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
#include "opad/assets.hpp"
#include "opad/checks.hpp"
#include "opad/kicad_pcb.hpp"
#include "opad/scene.hpp"

OPAD_ICON_TABLE(kicad,
                {"kicadboard", R"(<rect x="3" y="5" width="18" height="14" rx="1"/><rect x="9" y="9" width="6" height="6"/><path d="M15 12h3M6 9h3M6 15h3"/><circle cx="18" cy="8" r="1"/>)"},
                {"smallparts", R"(<rect x="3" y="11" width="9" height="9"/><rect x="15" y="5" width="2" height="2"/><rect x="18" y="10" width="2" height="2"/><rect x="15" y="15" width="2" height="2"/><path d="M14 3l7 18"/>)"});

namespace {
std::filesystem::path fsPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }
constexpr double kSmallPartSize = 3.0;  // mm: chip resistors and capacitors, small diodes
constexpr double kDegrees = 180 / 3.14159265358979323846;
constexpr double kClearance = 1.0;  // mm: a board's gap to its enclosure, until one is typed (setting kicad/clearance)
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
                    std::function<bool(const CommandContext&)> when, std::function<void()> fn, bool edits = false, bool checkable = false) {
    CommandInfo info;
    info.id = id;
    info.label = label;
    info.icon = icon;
    info.group = group;
    info.keywords = keywords;
    info.enabledWhen = std::move(when);
    info.editsDocument = edits;
    info.checkable = checkable;
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
  add("kicad.clearance", tr("Check clearance to board…"), "interference", kicad, {"kicad", "clearance", "gap", "enclosure", "interference", "fit", "pcb"},
      [this](const CommandContext& c) { return c.document && !c.sketching && !m_boards.empty(); }, [this] {
        const std::string b = boardOf(services().selection());
        clearance(b.empty() ? m_boards.front() : b);
      });
  QAction* small = add("view.hideSmallParts", tr("Hide small parts while navigating"), "smallparts", QString(), {"performance", "fast", "orbit", "pcb", "level of detail"},
                       {}, [this] { setSmallParts(services().action("view.hideSmallParts")->isChecked()); }, false, true);
  small->setChecked(QSettings().value("view/hideSmallParts", false).toBool());
  add("view.smallPartSize", tr("Small part size…"), "", QString(), {"performance", "hide", "navigating"}, {}, [this] { askSmallPartSize(); });
}

void KicadArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  if (QMenu* file = menus.value("file")) {  // after Link as asset…
    const QList<QAction*> entries = file->actions();
    QAction* before = nullptr;
    for (int i = 0; i < entries.size(); ++i)
      if (entries[i]->objectName() == "assets.link" || (!before && entries[i]->objectName() == "file.import")) before = entries.value(i + 1);
    file->insertAction(before, services().action("kicad.insert"));
  }
  QMenu* insert = menus.value("insert");  // Insert KiCad PCB… with the other inserts, the board's commands in a submenu after them
  if (insert) insert->insertAction(areas::firstSeparator(insert), services().action("kicad.insert"));
  if (QMenu* design = insert ? insert : menus.value("design")) {
    QMenu* sub = design->addMenu(icons::themed("kicadboard", 16), tr("KiCad"));
    sub->setObjectName("kicad");
    for (const char* id : {"kicad.insert", "kicad.previewSync", "kicad.project", "kicad.clearance"}) sub->addAction(services().action(id));
  }
  if (QMenu* view = menus.value("view")) {
    view->addSeparator();
    view->addAction(services().action("view.hideSmallParts"));
    view->addAction(services().action("view.smallPartSize"));
  }
}


void KicadArea::ready() {
  buildPanel();
  buildClearancePanel();
  if (AssetsArea* a = assets()) {
    a->setPreviewer([this](const std::string& import) { preview(import); });
    if (a->monitor())  // the previewed board changed again: read again what is shown
      connect(a->monitor(), &AssetMonitor::filesChanged, this, [this](const std::vector<std::string>& imports) {
        if (std::find(imports.begin(), imports.end(), m_import) == imports.end()) return;
        m_plan.reset();
        if (m_panel->isVisible()) preview(m_import);
      });
  }
  services().properties()->addSectionProvider([this](const PropertySubject& s, const opad::json&, QList<PropertySection>& out) { section(s, out); });
  setSmallParts(QSettings().value("view/hideSmallParts", false).toBool());
  documentChanged(true);
}

void KicadArea::documentChanged(bool replaced) {
  m_boards.clear();
  const AppDocument* doc = services().document();
  std::set<std::string> deleted(doc->scene.deleted_ops.begin(), doc->scene.deleted_ops.end());
  for (const auto& o : doc->doc.ops)
    if (o.type == "import" && o.data.contains("kicad") && !deleted.count(o.id)) m_boards.push_back(o.id);
  if (m_panel && m_panel->isVisible() && (replaced || std::find(m_boards.begin(), m_boards.end(), m_import) == m_boards.end())) closePreview();
  if (m_plan && doc->revision != m_planRevision) m_plan.reset();  // planned on what the document no longer is: Sync plans again
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
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {  // a part, a sketch, a feature's bodies
    const std::string id = item->data(0, Qt::UserRole).toString().toStdString();
    const AppDocument* doc = services().document();
    if (id.empty()) return;
    if (doc->node(id) || doc->scene.sketch(id)) return services().browser()->selectIds({id});
    std::vector<std::string> bodies;
    if (const opad::Feature* f = doc->scene.feature(id))
      for (const auto& b : f->result.value("bodies", opad::json::array()))
        if (doc->node(b.value("id", ""))) bodies.push_back(b.value("id", ""));
    if (!bodies.empty()) services().browser()->selectIds(bodies);
  });
  layout->addWidget(m_list, 1);
  m_footer = new PanelFooter(body);
  m_footer->setPrimary(tr("Sync"));
  connect(m_footer, &PanelFooter::accepted, this, &KicadArea::sync);
  connect(m_footer, &PanelFooter::cancelled, this, &KicadArea::closePreview);
  layout->addWidget(m_footer);
  m_panel = new ToolPanel("kicadSync", "regen", &Tokens::sel, tr("Sync preview"), body, 420, services().window());
  m_panel->setObjectName("kicadSyncPanel");
  m_panel->setEscapeHandler([this] { closePreview(); });
  connect(m_panel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on) services().viewport()->clearLookLayer(LookSource::Compare);
  });
  services().addPanel(m_panel);
}

// The board is read and its sync planned, as Sync plans it, on a worker from a copy of the document (taken on a worker): what
// changes on the board, and what that does to the sketches and features built on it. Sync then commits that very plan while
// the document stays as it was.
void KicadArea::preview(const std::string& import, int waited) {
  AssetsArea* a = assets();
  AssetMonitor* monitor = a ? a->monitor() : nullptr;
  const AssetMonitor::Asset* asset = monitor ? monitor->asset(import) : nullptr;
  if (!asset || asset->asset.value("storage", "linked") == "embedded") {
    services().toast(tr("Only a linked file is synced with its file"));
    return;
  }
  if (m_job || (m_reading && !waited)) return;
  AppDocument* doc = services().document();
  const bool board = asset->asset.value("kind", "") == "kicad_pcb";  // per footprint; any other file per body
  const QString file = monitor->file(import), title = QString::fromStdString(asset->name);
  const opad::AssetOptions options = AssetMonitor::options(doc);
  const unsigned long long revision = doc->revision;
  QPointer<KicadArea> self(this);
  auto failed = [self, title](const QString& error) {
    if (!self) return;
    self->m_reading = false;
    self->services().toast(tr("%1 could not be read: %2").arg(title, i18n::t(error)), {}, {}, 8000);
    emit self->previewReady(false, error);
  };
  m_reading = true;
  const bool started = !doc->loading && !doc->converting() && !a->busy() &&
                       doc->captureSnapshot(services().jobs(), [self, import, file, title, options, revision, failed, board](std::shared_ptr<opad::Document> copy, const QString& error) {
    if (!self) return;
    if (!copy) return failed(error);
    struct Out {
      opad::json report;
      opad::design::Plan plan;
    };
    auto out = std::make_shared<Out>();
    self->m_job = self->services().jobs()->async(tr("Reading %1").arg(title), [copy, import, file, options, out, board](Progress p) {
      if (board) out->report = opad::kicad_sync_preview(*copy, import, fsPath(file));
      opad::AssetOptions o = options;
      o.progress = [p](double, const std::string&) { return !p.cancelled(); };
      try {
        out->plan = opad::plan_asset_sync(*copy, import, o);
        out->report["affects"] = opad::asset_sync_affects(*copy, import, out->plan);
        if (!board) {
          out->report["parts"] = opad::asset_sync_parts(*copy, import, out->plan);
          out->report["changed"] = !out->plan.ops.empty() && !out->plan.report.value("up_to_date", false);
          out->report["unchanged"] = out->report["parts"].value("kept", 0);
        }
      } catch (const std::exception& e) {
        if (p.cancelled() || !board) throw;
        out->plan = {};
        out->report["affects_error"] = e.what();
      }
    }, [self, import, title, out, revision, failed](bool ok, const QString& error) {
      if (!self) return;
      self->m_job = nullptr;
      if (!ok) return failed(error);
      self->m_reading = false;
      self->m_import = import;
      self->m_report = std::move(out->report);
      self->m_plan = out->plan.ops.empty() ? nullptr : std::make_shared<opad::design::Plan>(std::move(out->plan));
      self->m_planRevision = revision;
      self->fill();
      self->m_panel->setContext(title);
      self->services().openPanel(self->m_panel);
      emit self->previewReady(true, {});
    });
  });
  if (started) return;
  if (waited >= 300) return failed(tr("The document is busy; try again in a moment."));
  QTimer::singleShot(100, this, [this, import, waited] { preview(import, waited + 1); });
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
      row->setToolTip(1, row->text(1));
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
  const opad::json bodies = m_report.value("parts", opad::json::object());  // any other linked file: its bodies by node
  for (const auto& [key, title, color] : {std::tuple{"changed", tr("Changed"), t.diffModified}, {"added", tr("Added"), t.diffAdded}, {"removed", tr("Removed"), t.diffRemoved}}) {
    const opad::json& list = bodies.value(key, opad::json::array());
    if (list.empty()) continue;
    auto* top = new QTreeWidgetItem(m_list, {QString("%1 (%2)").arg(title).arg(list.size())});
    top->setForeground(0, color);
    top->setFirstColumnSpanned(true);
    for (const auto& b : list) {
      auto* row = new QTreeWidgetItem(top, {QString::fromStdString(b.value("name", ""))});
      row->setData(0, Qt::UserRole, QString::fromStdString(b.value("node", "")));
      if (std::string(key) != "added") tint(b.value("node", ""), color);
      ++count;
    }
    top->setExpanded(true);
  }
  QList<QStringList> board;  // the board itself: what, before → after
  auto change = [](const QString& before, const QString& after) { return before + " → " + after; };
  if (m_report.contains("thickness"))
    board << QStringList{tr("Thickness"), change(length(m_report["thickness"].value("before", 0.0)), length(m_report["thickness"].value("after", 0.0)))};
  if (m_report.contains("holes"))
    board << QStringList{tr("Drills"), change(QString::number(m_report["holes"].value("before", 0)), QString::number(m_report["holes"].value("after", 0)))};
  if (m_report.contains("outline")) {
    const opad::json& o = m_report["outline"];
    board << QStringList{tr("Outline area"), change(units::format(units::Kind::Area, o["before"].value("area", 0.0)), units::format(units::Kind::Area, o["after"].value("area", 0.0)))};
  }
  if (!board.isEmpty()) {
    auto* top = new QTreeWidgetItem(m_list, {tr("Board")});
    top->setForeground(0, t.diffModified);
    top->setFirstColumnSpanned(true);
    for (const QStringList& line : board) new QTreeWidgetItem(top, line);
    top->setExpanded(true);
  }
  fillAffected();
  const bool changed = m_report.value("changed", false);
  m_footer->setHint(changed ? tr("%1 changes · %2 unchanged").arg(count + int(board.size())).arg(m_report.value("unchanged", 0))
                            : m_plan ? tr("No part moved; the file is newer") : m_report.contains("parts") ? tr("The file is as last synced") : tr("The board is as last synced"));
  m_footer->hint()->setToolTip(m_footer->hint()->text());  // the footer may cut it short
  m_footer->setPrimaryEnabled(changed || m_plan);
  services().viewport()->setLookLayer(LookSource::Compare, std::move(tints));
}

// What the sync does to the design built on the board (UI-134): per sketch the references that move or are projected again and
// the dimensions they take along, lose or measure anew; per feature recomputed whether its bodies change; new errors in red.
void KicadArea::fillAffected() {
  const opad::json affects = m_report.value("affects", opad::json::object());
  const opad::json sketches = affects.value("sketches", opad::json::array()), features = affects.value("features", opad::json::array());
  const Tokens& t = theme::current();
  if (m_report.contains("affects_error")) {
    auto* top = new QTreeWidgetItem(m_list, {tr("Design affected"), i18n::t(QString::fromStdString(m_report.value("affects_error", "")))});
    top->setForeground(1, t.diffRemoved);
    return;
  }
  if (sketches.empty() && features.empty()) return;
  auto* top = new QTreeWidgetItem(m_list, {tr("Design affected (%1)").arg(sketches.size() + features.size())});
  top->setForeground(0, t.diffModified);
  top->setFirstColumnSpanned(true);
  auto phrase = [](const opad::json& r) {
    const bool moved = r.value("change", "") == "moved";
    const std::string kind = r.value("kicad", ""), ref = r.value("ref", "");
    if (kind == "outline") return moved ? tr("The board outline moves") : tr("The board outline is projected again");
    if (kind == "holes") return moved ? tr("The mounting holes move") : tr("The mounting holes are projected again");
    const QString name = !ref.empty() ? QString::fromStdString(ref) : kind == "hole" ? tr("A mounting hole") : tr("A part");
    return (moved ? tr("%1 moves") : tr("%1 is projected again")).arg(name);
  };
  auto names = [](const opad::json& list) {
    QStringList out;
    for (const auto& d : list) out << QString::fromStdString(d.value("name", ""));
    return out.join(", ");
  };
  auto value = [](const opad::json& d, const char* key) {
    const double v = d.value(key, 0.0);
    return d.value("type", "") == "angle" ? units::compact(units::Kind::Angle, v * kDegrees) : units::compact(units::Kind::Length, v);
  };
  auto row = [&](const opad::json& e, QStringList parts) {  // the op's name on the first of its lines (rows are one line high)
    if (e.contains("error")) parts = QStringList{i18n::t(QString::fromStdString(e.value("error", "")))};
    for (int i = 0; i < parts.size(); ++i) {
      auto* item = new QTreeWidgetItem(top, {i ? QString() : QString::fromStdString(e.value("name", "")), parts[i]});
      item->setData(0, Qt::UserRole, QString::fromStdString(e.value("op", "")));
      item->setToolTip(1, parts[i]);
      if (e.contains("error")) item->setForeground(1, t.diffRemoved);
    }
  };
  for (const auto& s : sketches) {
    QStringList parts;
    for (const auto& r : s.value("references", opad::json::array())) parts << phrase(r);
    if (const opad::json& d = s.value("dimensions_moved", opad::json::array()); !d.empty()) parts << tr("Follow the board: %1").arg(names(d));
    for (const auto& d : s.value("dimensions_changed", opad::json::array()))
      parts << tr("%1 measures %2 → %3").arg(QString::fromStdString(d.value("name", "")), value(d, "before"), value(d, "after"));
    if (const opad::json& d = s.value("dimensions_removed", opad::json::array()); !d.empty()) parts << tr("Removed: %1").arg(names(d));
    row(s, parts.isEmpty() ? QStringList{tr("Recomputed")} : parts);
  }
  for (const auto& f : features) {
    const int bodies = f.value("bodies_changed", 0);
    row(f, {bodies == 1 ? tr("Recomputed: its body changes") : bodies ? tr("Recomputed: %1 bodies change").arg(bodies) : tr("Recomputed")});
  }
  top->setExpanded(true);
}

void KicadArea::sync() {
  const std::string import = m_import;
  std::shared_ptr<opad::design::Plan> plan = std::exchange(m_plan, nullptr);
  closePreview();
  AssetsArea* a = assets();
  if (!a || import.empty()) return;
  if (plan) a->syncPlanned(import, std::move(*plan), m_planRevision);
  else a->sync({import});
}

void KicadArea::closePreview() {
  m_plan.reset();
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

// ---------------------------------------------------------------- clearance to the enclosure (UI-134)
void KicadArea::buildClearancePanel() {
  m_checks = new CheckPanel;
  m_checks->setObjectName("kicadClearanceChecks");
  m_clearancePanel = new ToolPanel("kicadClearance", "interference", &Tokens::sel, tr("Board clearance"), m_checks, 220, services().window());
  m_clearancePanel->setObjectName("kicadClearancePanel");
  m_clearancePanel->setContentSizeHint([this](int width) { return m_checks->preferredSize(width); });
  connect(m_checks, &CheckPanel::contentResized, m_clearancePanel, &ToolPanel::requestContentFit);
  connect(m_checks, &CheckPanel::runRequested, this, [this] { runClearance(); });
  connect(m_checks, &CheckPanel::findingActivated, this, [this](const opad::json& f) {  // the pair selected, the gap measured
    services().viewport()->selectNodes({f.value("a", ""), f.value("b", "")});
    if (f.value("kind", "") != "clearance" || !f.contains("point_a")) return;
    services().viewport()->showMeasurement({{"kind", "distance"}, {"value", f.value("distance_mm", 0.0)}, {"unit", "mm"}, {"point_a", f["point_a"]}, {"point_b", f["point_b"]}});
    m_measured = true;
  });
  connect(m_clearancePanel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (on) return;
    if (Job* job = std::exchange(m_checkJob, nullptr)) job->cancel();
    if (std::exchange(m_measured, false)) services().viewport()->clearDimension();
  });
  services().addPanel(m_clearancePanel);
}

void KicadArea::clearance(const std::string& import) {
  if (import.empty() || !services().document()->hasDocument) return;
  m_clearanceImport = import;
  m_enclosure.clear();  // the selection outside the board, else every other visible solid
  for (const auto& id : services().selection().ids)
    if (const opad::Node* n = services().document()->node(id); n && n->source_op != import) m_enclosure.push_back(id);
  m_checks->begin(CheckPanel::Mode::Interference);
  m_checks->setClearance(QSettings().value("kicad/clearance", kClearance).toDouble());
  m_clearancePanel->setContext(board(import).name);
  services().openPanel(m_clearancePanel);
  runClearance();
}

// The board's solids against the enclosure's (opad::check_interference "against"), on a worker from a copy of the document.
void KicadArea::runClearance(int waited) {
  if (Job* job = std::exchange(m_checkJob, nullptr)) job->cancel();
  AppDocument* doc = services().document();
  const Board b = board(m_clearanceImport);
  if (b.root.empty()) return m_checks->setFailed(tr("The board is no longer in the document."));
  opad::json args = m_checks->options();
  QSettings().setValue("kicad/clearance", args.value("clearance_mm", kClearance));
  args["against"] = {b.root};
  if (!m_enclosure.empty()) args["select"] = m_enclosure;
  args["limit"] = 200;
  m_checks->setRunning(tr("Checking…"));
  QPointer<KicadArea> self(this);
  auto failed = [self](const QString& error) {
    if (!self) return;
    self->m_checks->setFailed(error);
    emit self->clearanceReady(false);
  };
  const bool started = !doc->loading && !doc->converting() && doc->captureSnapshot(services().jobs(), [self, args, failed](std::shared_ptr<opad::Document> copy, const QString& error) {
    if (!self) return;
    if (!copy) return failed(i18n::t(error));
    auto result = std::make_shared<opad::json>();
    self->m_checkJob = self->services().jobs()->async(tr("Board clearance"), [copy, args, result](Progress p) {
      const opad::Scene scene = opad::resolve(*copy);
      *result = opad::check_interference(*copy, scene, args, [p] { return p.cancelled(); });
    }, [self, result, failed](bool ok, const QString& error) {
      if (!self) return;
      self->m_checkJob = nullptr;
      if (!ok) return failed(error == "cancelled" ? tr("Cancelled.") : i18n::t(error));
      const AppDocument* doc = self->services().document();
      for (auto& item : (*result)["items"])  // a board part by its footprint ("J1 USB_C"), not its model's body
        if (const opad::Node* n = doc->node(item.value("b", "")); n && !n->parent.empty())
          if (const opad::Node* p = doc->node(n->parent); p && p->source_op == n->source_op && !p->parent.empty()) item["b_name"] = p->name;
      self->m_lastClearance = *result;
      self->m_checks->setResult(*result);
      emit self->clearanceReady(true);
    });
  });
  if (started) return;
  if (waited >= 300) return failed(tr("The document is busy; try again in a moment."));
  QTimer::singleShot(100, this, [this, waited] { runClearance(waited + 1); });
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
  // The board's root is marked to stay together in exploded views (explode_keep_defaults), said here once explode reads it.
  sec.rows << qMakePair(tr("Footprints placed"), QString::number(b.parts.size())) << qMakePair(tr("Mounting holes"), QString::number(b.holeNodes.size()));
  if (b.linked) sec.actions << qMakePair(tr("Preview sync…"), std::function<void()>([this, import] { QTimer::singleShot(0, this, [this, import] { preview(import); }); }));
  sec.actions << qMakePair(tr("Check clearance…"), std::function<void()>([this, import] { QTimer::singleShot(0, this, [this, import] { clearance(import); }); }));
  out << sec;
}

void KicadArea::contextMenu(const SelectionContext& selection, QMenu& menu) {
  const std::string import = selection.sketching ? std::string() : boardOf(selection);
  if (import.empty()) return;
  menu.addAction(icons::themed("interference", 16), tr("Check clearance to board…"), this, [this, import] { clearance(import); });
  if (services().document()->browse) return;
  if (const AssetsArea* a = assets(); a && a->monitor() && a->monitor()->asset(import))
    menu.addAction(icons::themed("regen", 16), tr("Preview KiCad sync…"), this, [this, import] { preview(import); });
}

// ---------------------------------------------------------------- small parts while navigating
void KicadArea::setSmallParts(bool on) {
  QSettings settings;
  settings.setValue("view/hideSmallParts", on);
  if (QAction* a = services().action("view.hideSmallParts"); a && a->isChecked() != on) a->setChecked(on);
  services().viewport()->setSmallPartFilter(on ? settings.value("view/smallPartSize", kSmallPartSize).toDouble() : 0);
}

void KicadArea::askSmallPartSize() {
  QSettings settings;
  bool ok = false;
  const double was = settings.value("view/smallPartSize", kSmallPartSize).toDouble();
  const double shown = QInputDialog::getDouble(services().window(), tr("Small part size"),
                                               tr("While the view moves, hide parts smaller than (%1):").arg(units::symbol(units::Kind::Length)),
                                               units::toDisplay(units::Kind::Length, was), 0, 1e6, 3, &ok);
  if (!ok) return;
  settings.setValue("view/smallPartSize", units::fromDisplay(units::Kind::Length, shown));
  setSmallParts(true);
}

OPAD_AREA(KicadArea)
