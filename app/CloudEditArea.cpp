// Edit point cloud: the mode (CloudEditArea.hpp). Brushes are cylinders along the view through the point picked under the
// pointer (Viewport::pickCloudPoint: the cloud's own pick BVH), as wide as the brush's circle on screen at that depth;
// dabs follow the pointer at a quarter of the radius, filled in along the stroke when the pointer outran the workers.
#include "CloudEditArea.hpp"

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "AppDocument.hpp"
#include "BodyShape.hpp"
#include "CommandHelp.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "GuidedTool.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "KeyText.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"
#include "Toast.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"

OPAD_ICON_TABLE(cloudedit,
                {"cloud_edit", R"(<circle cx="5" cy="17" r="1"/><circle cx="9" cy="15" r="1"/><circle cx="13" cy="16" r="1"/><circle cx="7" cy="20" r="1"/><circle cx="11" cy="19.5" r="1"/><path d="M14 13l6-6 2 2-6 6h-2z"/><path d="M18 9l2 2"/>)"},
                {"cloud_pull", R"(<path d="M3 18c3-1 4-6 9-6s6 5 9 6"/><circle cx="6" cy="16" r="0.9"/><circle cx="12" cy="12" r="0.9"/><circle cx="18" cy="16" r="0.9"/><path d="M12 9V3m-2.5 2.5L12 3l2.5 2.5"/>)"},
                {"cloud_smooth", R"(<path d="M3 15c2-3 3 2 5-1s3 2 5-1 3 2 5-1 2 1 3 1"/><path d="M3 19h18"/><circle cx="8" cy="9" r="3" stroke-dasharray="2 1.5"/>)"},
                {"cloud_erase", R"(<circle cx="6" cy="16" r="1"/><circle cx="10" cy="18" r="1"/><circle cx="18" cy="17" r="1"/><circle cx="14" cy="9" r="4.5" stroke-dasharray="2 1.5"/><path d="M12 7l4 4M16 7l-4 4"/>)"},
                {"cloud_add", R"(<circle cx="5" cy="16" r="1"/><circle cx="9" cy="17" r="1"/><circle cx="15" cy="17" r="1"/><circle cx="19" cy="16" r="1"/><circle cx="12" cy="17.5" r="1.4" fill="currentColor"/><path d="M12 4v8M8 8h8"/>)"},
                {"cloud_paint", R"(<circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="2" fill="currentColor"/><circle cx="5" cy="20" r="0.9"/><circle cx="19" cy="20" r="0.9"/>)"},
                {"cloud_box", R"(<rect x="4" y="5" width="16" height="13" rx="1" stroke-dasharray="2.5 2"/><circle cx="9" cy="10" r="1"/><circle cx="14" cy="12" r="1"/><circle cx="10" cy="14.5" r="1"/>)"},
                {"cloud_grow", R"(<circle cx="12" cy="12" r="1.6" fill="currentColor"/><circle cx="12" cy="12" r="5" stroke-dasharray="2 1.5"/><circle cx="12" cy="12" r="9" stroke-dasharray="2 2"/>)"},
                {"brush_smaller", R"(<circle cx="12" cy="12" r="8" stroke-dasharray="2 1.5"/><circle cx="12" cy="12" r="4"/><path d="M9.5 12h5"/>)"},
                {"brush_larger", R"(<circle cx="12" cy="12" r="4" stroke-dasharray="2 1.5"/><circle cx="12" cy="12" r="8"/><path d="M9.5 12h5M12 9.5v5"/>)"});

namespace {
constexpr int kMinRadius = 4, kMaxRadius = 240;
constexpr double kPullPerDab = 0.05;   // of the brush's radius, at full strength and the brush's middle
constexpr double kSmoothPerDab = 0.5;  // of the way onto the local plane, at full strength
const char* kTabId = "design.cloudEdit";

bool benchRun() {
  for (const QString& key : QProcessEnvironment::systemEnvironment().keys())
    if (key.startsWith(QLatin1String("OPAD_BENCH"))) return true;
  return false;
}

struct ToolInfo {
  CloudEditArea::Tool tool;
  const char* id;
  const char* label;
  const char* icon;
  const char* hint;
};
const ToolInfo kTools[] = {
    {CloudEditArea::Tool::Pull, "cloud.edit.pull", QT_TRANSLATE_NOOP("CloudEdit", "Push/Pull brush"), "cloud_pull",
     QT_TRANSLATE_NOOP("CloudEdit", "Drag over the points to pull them out of the surface; {fixed:ctrl} or {fixed:alt} held pushes them in.")},
    {CloudEditArea::Tool::Smooth, "cloud.edit.smooth", QT_TRANSLATE_NOOP("CloudEdit", "Smooth brush"), "cloud_smooth",
     QT_TRANSLATE_NOOP("CloudEdit", "Drag over the points to settle them onto their surface.")},
    {CloudEditArea::Tool::Erase, "cloud.edit.erase", QT_TRANSLATE_NOOP("CloudEdit", "Delete brush"), "cloud_erase",
     QT_TRANSLATE_NOOP("CloudEdit", "Drag over the points to delete them.")},
    {CloudEditArea::Tool::Add, "cloud.edit.add", QT_TRANSLATE_NOOP("CloudEdit", "Add points"), "cloud_add",
     QT_TRANSLATE_NOOP("CloudEdit", "Click on the surface to add a point there; drag to add a row of them.")},
    {CloudEditArea::Tool::Paint, "cloud.edit.paint", QT_TRANSLATE_NOOP("CloudEdit", "Paint select"), "cloud_paint",
     QT_TRANSLATE_NOOP("CloudEdit", "Paint over the points to select them; {fixed:shift} adds, {fixed:ctrl} takes away.")},
    {CloudEditArea::Tool::Box, "cloud.edit.box", QT_TRANSLATE_NOOP("CloudEdit", "Box select"), "cloud_box",
     QT_TRANSLATE_NOOP("CloudEdit", "Drag a box over the points to select them; {fixed:shift} adds, {fixed:ctrl} takes away.")},
    {CloudEditArea::Tool::Lasso, "cloud.edit.lasso", QT_TRANSLATE_NOOP("CloudEdit", "Lasso select"), "cloud_region",
     QT_TRANSLATE_NOOP("CloudEdit", "Drag a lasso round the points to select them; {fixed:shift} adds, {fixed:ctrl} takes away.")},
    {CloudEditArea::Tool::Grow, "cloud.edit.grow", QT_TRANSLATE_NOOP("CloudEdit", "Grow region"), "cloud_grow",
     QT_TRANSLATE_NOOP("CloudEdit", "Click a point: the selection spreads over its surface and stops at edges; {fixed:shift} adds, {fixed:ctrl} takes away.")},
};
}  // namespace

CloudEditArea::~CloudEditArea() = default;

// ---------------------------------------------------------------- commands
void CloudEditArea::buildActions() {
  const QString group = tr("Point cloud");
  CommandInfo edit{"cloud.edit", tr("Edit point cloud"), "cloud_edit"};
  edit.keywords = {tr("sculpt"), tr("push pull"), tr("brush"), tr("clean scan"), tr("delete points"), tr("add points"), tr("point cloud"), tr("reverse engineering")};
  edit.group = group;
  edit.editsDocument = true;
  edit.enabledWhen = [this](const CommandContext& c) { return c.document && !c.sketching && !m_active; };
  m_edit = services().addCommand(edit, [this] {
    const std::string body = chosenCloud(services().selection());
    if (body.empty()) throw opad::UserHint("Select a point cloud first.", true);
    this->edit(body);
  });
  auto inMode = [this](const CommandContext&) { return m_active; };
  CommandInfo finish{"cloud.edit.finish", tr("Finish point cloud edit"), "finish"};
  finish.keywords = {tr("done"), tr("commit"), tr("leave"), tr("point cloud")};
  finish.group = group;
  finish.enabledWhen = inMode;
  m_finish = services().addCommand(finish, [this] { this->finish(); });
  CommandInfo cancel{"cloud.edit.cancel", tr("Cancel point cloud edit"), "close"};
  cancel.keywords = {tr("discard"), tr("leave"), tr("point cloud")};
  cancel.group = group;
  cancel.enabledWhen = inMode;
  m_cancel = services().addCommand(cancel, [this] { this->cancel(true); });
  auto* tools = new QActionGroup(this);
  tools->setExclusive(true);
  for (const ToolInfo& t : kTools) {
    CommandInfo info{t.id, i18n::t(QString::fromLatin1(t.label)), t.icon};
    info.keywords = {tr("point cloud"), tr("scan")};
    if (t.tool == Tool::Pull) info.keywords << tr("sculpt") << tr("push") << tr("pull") << tr("inflate") << tr("dent");
    if (t.tool == Tool::Smooth) info.keywords << tr("relax") << tr("flatten") << tr("noise");
    if (t.tool == Tool::Erase) info.keywords << tr("erase") << tr("remove points");
    if (t.tool == Tool::Add) info.keywords << tr("fill hole") << tr("new point") << tr("patch");
    if (selecting(t.tool)) info.keywords << tr("select points") << tr("region");
    if (t.tool == Tool::Grow) info.keywords << tr("flood fill") << tr("auto select") << tr("surface");
    info.group = group;
    info.checkable = true;
    info.enabledWhen = inMode;
    QAction* a = services().addCommand(info, [this, tool = t.tool] { setTool(tool); });
    a->setCheckable(true);
    tools->addAction(a);
    m_toolActions.push_back(a);
  }
  m_toolActions.front()->setChecked(true);
  auto withSelection = [this](const CommandContext&) { return m_active && !m_selected.empty(); };
  CommandInfo del{"cloud.edit.deleteSelected", tr("Delete selected points"), "delete"};
  del.keywords = {tr("erase"), tr("remove points"), tr("point cloud")};
  del.group = group;
  del.enabledWhen = withSelection;
  m_deleteSelected = services().addCommand(del, [this] { changeSelection(false); });
  CommandInfo keep{"cloud.edit.keepSelected", tr("Keep only selected points"), "cloud_crop"};
  keep.keywords = {tr("crop"), tr("keep points"), tr("point cloud")};
  keep.group = group;
  keep.enabledWhen = withSelection;
  m_keepSelected = services().addCommand(keep, [this] { changeSelection(true); });
  CommandInfo smaller{"cloud.edit.smaller", tr("Smaller brush"), "brush_smaller", QKeySequence("Ctrl+[")};
  smaller.keywords = {tr("brush size"), tr("radius"), tr("point cloud")};
  smaller.group = group;
  smaller.enabledWhen = inMode;
  services().addCommand(smaller, [this] { setRadiusPixels(int(std::lround(radiusPixels() / 1.25))); });
  CommandInfo larger{"cloud.edit.larger", tr("Larger brush"), "brush_larger", QKeySequence("Ctrl+]")};
  larger.keywords = {tr("brush size"), tr("radius"), tr("point cloud")};
  larger.group = group;
  larger.enabledWhen = inMode;
  services().addCommand(larger, [this] { setRadiusPixels(int(std::lround(radiusPixels() * 1.25 + 0.5))); });
}

void CloudEditArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  if (QMenu* design = menus.value("design"))
    for (QMenu* sub : design->findChildren<QMenu*>("cloudMenu")) sub->addAction(m_edit);
}

void CloudEditArea::ribbon(RibbonLayout& layout) {
  // Design > Scan: Edit point cloud beside the region tools.
  if (layout.addGroup("design.scan", "design.scan.edit", tr("Edit"))) layout.addAction("design.scan.edit", m_edit);
  // The mode's own tab, first in Design while it runs.
  if (!layout.addContextualTab("design", kTabId, tr("Edit points"))) return;
  const QString tab = QString::fromLatin1(kTabId);
  auto small = RibbonLayout::Size::Small;
  layout.addGroup(tab, tab + ".brush", tr("Brush"));
  for (Tool t : {Tool::Pull, Tool::Smooth, Tool::Erase, Tool::Add}) layout.addAction(tab + ".brush", toolAction(t), t == Tool::Pull ? RibbonLayout::Size::Large : small);
  layout.addGroup(tab, tab + ".size", tr("Size"));
  layout.addAction(tab + ".size", services().action("cloud.edit.smaller"), small);
  layout.addAction(tab + ".size", services().action("cloud.edit.larger"), small);
  layout.addGroup(tab, tab + ".select", tr("Select"));
  for (Tool t : {Tool::Paint, Tool::Box, Tool::Lasso, Tool::Grow}) layout.addAction(tab + ".select", toolAction(t), t == Tool::Paint ? RibbonLayout::Size::Large : small);
  layout.addGroup(tab, tab + ".selection", tr("Selection"));
  layout.addAction(tab + ".selection", m_deleteSelected, small);
  layout.addAction(tab + ".selection", m_keepSelected, small);
  layout.addGroup(tab, tab + ".finish", tr("Finish"));
  layout.addAction(tab + ".finish", m_finish, RibbonLayout::Size::Large, {}, true);
  layout.addAction(tab + ".finish", m_cancel, RibbonLayout::Size::Large);
}

void CloudEditArea::ready() {
  buildPanel();
  Viewport* view = services().viewport();
  view->installEventFilter(this);
  view->setMouseTracking(true);
  m_prompt = new PromptBar(view);
  m_prompt->setObjectName("cloudEditPrompt");
  m_prompt->setAttribute(Qt::WA_NativeWindow);
  m_prompt->hide();
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] { refresh(); });
  connect(units::notifier(), &units::Notifier::changed, this, [this] { refresh(); });
  // Edit feature on a cloud_edit step (the timeline, its menu): this mode on that step, rolled back to it.
  services().design()->setKindEditor("cloud_edit", [this](const std::string& opId) {
    const opad::Feature* f = services().document()->scene.feature(opId);
    if (!f) return;
    std::string body;
    for (const auto& b : f->inputs.value("bodies", opad::json::array()))
      if (b.is_object()) body = b.value("body", std::string());
    if (!body.empty()) services().guarded([&] { edit(body, opId); });
  });
}

void CloudEditArea::positionOverlays(const QRect&) {
  if (!m_prompt || !m_prompt->isVisible()) return;
  Viewport* view = services().viewport();
  m_prompt->adjustSize();
  m_prompt->move(std::max(8, (view->width() - m_prompt->width()) / 2), 44);
  m_prompt->raise();
}

void CloudEditArea::contextMenu(const SelectionContext& selection, QMenu& menu) {
  if (m_active || selection.sketching) return;
  bool one = false;
  for (const auto& id : selection.ids) one = one || cloudBody(id);
  for (const auto& r : selection.refs) one = one || cloudBody(r.body);
  if (one) menu.addAction(m_edit);
}

std::string CloudEditArea::chosenCloud(const SelectionContext& selection) const {
  for (const auto& id : selection.ids)
    if (cloudBody(id)) return id;
  for (const auto& r : selection.refs)
    if (cloudBody(r.body)) return r.body;
  for (const auto& r : services().viewport()->selection())
    if (cloudBody(r.body)) return r.body;
  std::string only;
  AppDocument* doc = services().document();
  for (const auto& id : doc->scene.all_bodies())
    if (cloudBody(id) && doc->scene.effectively_visible(id)) {
      if (!only.empty()) return {};
      only = id;
    }
  return only;
}

bool CloudEditArea::cloudBody(const std::string& id) const {
  const opad::Node* n = id.empty() ? nullptr : services().document()->node(id);
  return n && n->kind == opad::Node::Kind::Body && n->representation == "points";
}

// ---------------------------------------------------------------- entering and leaving
void CloudEditArea::edit(const std::string& body, const std::string& opId) {
  if (m_active) return;
  AppDocument* doc = services().document();
  if (!doc->hasDocument || doc->designBusy) return;
  // A linked file's parts are read-only (the engine refuses to change them): say so now, not at Finish.
  if (const opad::Node* n = doc->node(body); n && n->linked)
    throw opad::UserHint("This point cloud is part of a linked file: embed it first (Embed as editable), then edit its points.", true);
  auto start = [this, body, opId] {
    AppDocument* d = services().document();
    auto begun = std::make_shared<opad::cloud::Edit>();
    if (!opId.empty()) {
      const opad::Feature* f = d->scene.feature(opId);
      if (!f || f->kind != "cloud_edit") return;
      *begun = opad::cloud::Edit::from_json(f->inputs.value("edit", opad::json::object()));
      d->setRollback(opId);  // the cloud as it was when this step was made: what its point numbers mean
    } else if (d->rolledBack()) {
      throw opad::UserHint("Roll the timeline forward to the end first: a new edit goes after the last step.", true);
    }
    m_active = true;
    ++m_session;
    m_body = body;
    m_op = opId;
    m_history = {begun};
    m_index = 0;
    m_tasks.clear();
    m_selected = {};
    m_workspaceBefore = services().workspace();
    if (m_workspaceBefore != "design") services().setWorkspace("design");
    services().setContextualTab(kTabId, true);
    services().openPanel(m_panel);
    QElapsedTimer cleared;
    cleared.start();
    services().select({});  // the cloud is drawn as itself while it is edited, not in the selection's colour
    if (trace::enabled()) trace::log(QStringLiteral("cloud edit: selection cleared in %1 ms").arg(cleared.elapsed()));
    m_prompt->show();
    refresh();
    services().updateCommands();
    services().updateUndo();
    // The arrays drawn for the cloud as it is at this step (rolled back: drawn anew first), then the editor on a worker.
    const int session = m_session;
    auto wait = std::make_shared<std::function<void()>>();
    *wait = [this, session, wait, begun] {
      if (session != m_session || !m_active) return;
      const opad::Node* n = services().document()->node(m_body);
      const auto prs = services().viewport()->cloudPrs(m_body);
      if (!n || !prs || services().viewport()->displayArrays(n->body_key) != prs) {
        QTimer::singleShot(50, this, [wait] { (*wait)(); });
        return;
      }
      enterPrepared(begun);
    };
    (*wait)();
  };
  if (services().requireEditable(start)) start();
}

void CloudEditArea::enterPrepared(std::shared_ptr<const opad::cloud::Edit> begun) {
  opad::Mat4 world;
  m_base = services().viewport()->cloudPrs(m_body, &world);
  m_world = world;
  if (!m_base) return leave();
  auto made = std::make_shared<std::shared_ptr<opad::cloud::Editor>>();
  const auto base = m_base->cloudData;
  const int session = m_session;
  QElapsedTimer clock;
  clock.start();
  m_taskRunning = true;
  m_job = services().jobs()->async(tr("Preparing the points for editing"), [made, base, begun](Progress p) {
    auto e = std::make_shared<opad::cloud::Editor>(base, [&] { return p.cancelled(); });
    e->reset(*begun);
    *made = std::move(e);
  }, [this, made, session, clock, begun](bool ok, const QString& error) {
    m_job = nullptr;
    m_taskRunning = false;
    if (session != m_session || !m_active) return;
    if (!ok) {
      services().showMessage(error, 6000);
      return leave();
    }
    m_editor = *made;
    if (!begun->empty()) services().viewport()->setCloudEdit(m_body, begun, false);
    trace::log(QStringLiteral("cloud edit: prepared %1 points in %2 ms").arg(m_editor->base().size()).arg(clock.elapsed()));
    refresh();
    pump();
  });
  refresh();
}

void CloudEditArea::leave() {
  if (!m_active) return;
  m_active = false;
  ++m_session;
  m_dragging = false;
  m_finishing = false;
  Viewport* view = services().viewport();
  view->setCloudLasso({}, false);
  view->showCloudStroke(m_body, nullptr);
  view->showCloudRegion(m_body, nullptr);
  m_tasks.clear();
  // The editor (a grid over every point) and the strokes' snapshots are freed on a worker: hundreds of MB on a big scan.
  struct Leftovers {
    std::shared_ptr<opad::cloud::Editor> editor;
    std::shared_ptr<const BodyPrs> base;
    std::vector<std::shared_ptr<const opad::cloud::Edit>> history;
  };
  auto left = std::make_shared<Leftovers>(Leftovers{std::move(m_editor), std::move(m_base), std::move(m_history)});
  m_editor.reset();
  m_base.reset();
  m_history.clear();
  services().jobs()->quiet(tr("Editing the points"), [left](Progress) { *left = Leftovers(); });
  m_index = 0;
  m_selected = {};
  m_strokeIds.reset();
  if (!m_op.empty()) services().document()->setRollback({});
  m_op.clear();
  services().setContextualTab(kTabId, false);
  if (!m_workspaceBefore.isEmpty() && m_workspaceBefore != services().workspace()) services().setWorkspace(m_workspaceBefore);
  m_workspaceBefore.clear();
  if (m_panel->isVisible()) m_panel->hide();
  m_prompt->hide();
  services().showMessage(QString(), 1);
  services().updateCommands();
  services().updateUndo();
}

void CloudEditArea::finish() {
  if (!m_active) return;
  if (m_taskRunning || !m_tasks.empty() || m_dragging || !m_editor) {  // the strokes under way first
    m_finishing = m_editor != nullptr;
    return refresh();
  }
  const auto edit = current();
  const bool changed = m_op.empty() ? edit && !edit->empty() : m_index != 0;
  const std::string body = m_body, opId = m_op;
  if (!changed) {
    services().viewport()->setCloudEdit(body, nullptr);
    return leave();
  }
  // The op made on the worker (an edit's text can be megabytes), planned and committed as one step; the edited points stay
  // drawn until the body's new arrays replace them (Viewport drops an edit made on arrays the body no longer has).
  const QString label = opId.empty() ? tr("Edit point cloud") : tr("Edit point cloud again");
  const std::string name = opId.empty() ? opad::design::next_name(services().document()->scene, "EditPoints") : std::string();
  m_finishing = true;
  refresh();
  const int session = m_session;
  QElapsedTimer clock;
  clock.start();
  // Planned on a worker that reads the document in place (readAsync: nothing changes it meanwhile): the op's text and the
  // edited cloud's body are made there, and an embedded scan's body store is never copied on the UI thread.
  auto plan = std::make_shared<opad::design::Plan>();
  auto committed = [this, session, clock](bool ok, const QString& error) {
    if (session != m_session) return;
    m_finishing = false;
    if (!ok) {
      services().showMessage(i18n::t(error), 8000);
      return refresh();
    }
    trace::log(QStringLiteral("cloud edit: finished in %1 ms").arg(clock.elapsed()));
    leave();
    services().undoToast(tr("Point cloud edited"));
  };
  Job* job = services().document()->readAsync(services().jobs(), label, [plan, edit, body, opId, name](const opad::Document& d, const opad::Scene&, Progress p) {
    const opad::json inputs = {{"bodies", opad::json::array({{{"body", body}}})}, {"edit", edit->to_json()}};
    const opad::json op = opId.empty() ? opad::design::make_feature_op("cloud_edit", name, inputs) : opad::design::make_edit_op(opId, {{"inputs", inputs}});
    *plan = opad::design::plan_ops(d, {op}, true, [p] { return p.cancelled(); });
  }, [this, plan, label, committed](bool ok, const QString& error) {
    if (!ok) return committed(false, error);
    services().design()->commitPlanned(plan, label, committed);
  });
  if (!job) committed(false, tr("The design is still being recomputed; try again in a moment."));
}

void CloudEditArea::cancel(bool ask) {
  if (!m_active) return;
  const bool changed = m_history.size() > 1;
  auto drop = [this] {
    services().viewport()->setCloudEdit(m_body, nullptr);
    leave();
  };
  if (!ask || !changed) return drop();
  const QString name = services().document()->nodeName(m_body);
  services().toasts()->ask(tr("Discard the changes to %1?").arg(name), tr("The points go back to how they were when the edit began."),
                           {{tr("Discard"), [this, drop] { if (m_active) drop(); }}, {tr("Keep editing"), [] {}}});
}

void CloudEditArea::documentChanged(bool replaced) {
  if (!m_active || m_finishing) return;
  if (replaced || !cloudBody(m_body)) {
    services().showMessage(tr("The point cloud edit ended: the cloud is no longer there."), 6000);
    leave();
  }
}

bool CloudEditArea::maybeClose() {
  if (!m_active) return true;
  if (m_history.size() <= 1 || benchRun()) {
    cancel(false);
    return true;
  }
  const auto answer = QMessageBox::question(services().window(), tr("Unfinished point cloud edit"), tr("Finish the point cloud edit before continuing?"),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (answer == QMessageBox::Discard) {
    cancel(false);
    return true;
  }
  if (answer == QMessageBox::Save) finish();
  return false;
}

bool CloudEditArea::history(bool& canUndo, bool& canRedo) const {
  if (!m_active) return false;
  canUndo = m_index > 0;
  canRedo = m_index + 1 < m_history.size();
  return true;
}

bool CloudEditArea::command(const QString& id, const SelectionContext&) {
  if (!m_active) return false;
  if (id == "edit.undo") return undo(), true;
  if (id == "edit.redo") return redo(), true;
  if (id == "edit.delete") {  // Del: the selected points
    if (!m_selected.empty()) changeSelection(false);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- the panel
void CloudEditArea::buildPanel() {
  auto* body = new QWidget;
  auto* outer = new QVBoxLayout(body);
  outer->setContentsMargins(12, 8, 12, 8);
  outer->setSpacing(10);
  // The tools: four brushes and Select (the selection tool used last).
  auto* tools = new QHBoxLayout;
  const QString toolNames[] = {tr("Push/Pull"), tr("Smooth"), tr("Delete"), tr("Add"), tr("Select")};
  for (int i = 0; i < 5; ++i) {
    QAction* a = toolAction(i < 4 ? Tool(i) : Tool::Paint);
    auto* b = new QPushButton(a->icon(), toolNames[i], body);
    b->setToolTip(i < 4 ? a->text() : tr("Select points: Paint, Box, Lasso or Grow region"));
    b->setCheckable(true);
    b->setObjectName(QStringLiteral("cloudEditTool%1").arg(i));
    connect(b, &QPushButton::clicked, this, [this, i] { setTool(i < 4 ? Tool(i) : m_lastSelect); });
    tools->addWidget(b);
    m_toolButtons.push_back(b);
  }
  outer->addLayout(tools);
  // Push/Pull's way.
  m_directionRow = new QWidget(body);
  auto* way = new QHBoxLayout(m_directionRow);
  way->setContentsMargins(0, 0, 0, 0);
  auto* pull = new QRadioButton(tr("Pull out"), m_directionRow);
  auto* push = new QRadioButton(tr("Push in"), m_directionRow);
  pull->setObjectName("cloudEditPull");
  push->setObjectName("cloudEditPush");
  pull->setChecked(true);
  m_pull = pull;
  m_push = push;
  way->addWidget(pull);
  way->addWidget(push);
  way->addStretch();
  outer->addWidget(m_directionRow);
  // Select's ways.
  m_selectRow = new QWidget(body);
  auto* sel = new QHBoxLayout(m_selectRow);
  sel->setContentsMargins(0, 0, 0, 0);
  const QString selectNames[] = {tr("Paint"), tr("Box"), tr("Lasso"), tr("Grow")};
  for (int i = 0; i < 4; ++i) {
    auto* b = new QPushButton(selectNames[i], m_selectRow);
    b->setCheckable(true);
    b->setObjectName(QStringLiteral("cloudEditSelect%1").arg(i));
    connect(b, &QPushButton::clicked, this, [this, i] { setTool(Tool(int(Tool::Paint) + i)); });
    sel->addWidget(b);
    m_selectButtons.push_back(b);
  }
  outer->addWidget(m_selectRow);
  // Size and strength.
  m_sizeRow = new QWidget(body);
  auto* size = new QHBoxLayout(m_sizeRow);
  size->setContentsMargins(0, 0, 0, 0);
  size->addWidget(new QLabel(tr("Size"), m_sizeRow));
  m_radius = new QSlider(Qt::Horizontal, m_sizeRow);
  m_radius->setObjectName("cloudEditRadius");
  m_radius->setRange(kMinRadius, kMaxRadius);
  m_radius->setValue(std::clamp(QSettings().value("cloudEdit/radius", 40).toInt(), kMinRadius, kMaxRadius));
  size->addWidget(m_radius, 1);
  m_radiusLabel = new QLabel(m_sizeRow);
  m_radiusLabel->setObjectName("cloudEditRadiusText");
  size->addWidget(m_radiusLabel);
  outer->addWidget(m_sizeRow);
  connect(m_radius, &QSlider::valueChanged, this, [this](int v) {
    QSettings().setValue("cloudEdit/radius", v);
    refresh();
  });
  m_strengthRow = new QWidget(body);
  auto* strength = new QHBoxLayout(m_strengthRow);
  strength->setContentsMargins(0, 0, 0, 0);
  strength->addWidget(new QLabel(tr("Brush strength"), m_strengthRow));
  m_strength = new QSlider(Qt::Horizontal, m_strengthRow);
  m_strength->setObjectName("cloudEditStrength");
  m_strength->setRange(1, 100);
  m_strength->setValue(std::clamp(QSettings().value("cloudEdit/strength", 50).toInt(), 1, 100));
  auto* strengthText = new QLabel(m_strengthRow);
  connect(m_strength, &QSlider::valueChanged, this, [strengthText](int v) {
    QSettings().setValue("cloudEdit/strength", v);
    strengthText->setText(QStringLiteral("%1 %").arg(v));
  });
  strengthText->setText(QStringLiteral("%1 %").arg(m_strength->value()));
  strength->addWidget(m_strength, 1);
  strength->addWidget(strengthText);
  outer->addWidget(m_strengthRow);
  // Falloff: three presets.
  m_falloffRow = new QWidget(body);
  auto* fall = new QHBoxLayout(m_falloffRow);
  fall->setContentsMargins(0, 0, 0, 0);
  fall->addWidget(new QLabel(tr("Falloff"), m_falloffRow));
  auto* falls = new QButtonGroup(m_falloffRow);
  const QString fallNames[] = {tr("Smooth"), tr("Sharp"), tr("Flat")};
  const int chosen = std::clamp(QSettings().value("cloudEdit/falloff", 0).toInt(), 0, 2);
  for (int i = 0; i < 3; ++i) {
    auto* b = new QPushButton(fallNames[i], m_falloffRow);
    b->setCheckable(true);
    b->setChecked(i == chosen);
    b->setObjectName(QStringLiteral("cloudEditFalloff%1").arg(i));
    falls->addButton(b, i);
    fall->addWidget(b);
    m_falloffButtons.push_back(b);
  }
  connect(falls, &QButtonGroup::idClicked, this, [](int id) { QSettings().setValue("cloudEdit/falloff", id); });
  outer->addWidget(m_falloffRow);
  m_front = new QCheckBox(tr("Front surface only (not through thin walls)"), body);
  m_front->setObjectName("cloudEditFront");
  m_front->setChecked(QSettings().value("cloudEdit/front", true).toBool());
  connect(m_front, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("cloudEdit/front", on); });
  outer->addWidget(m_front);
  m_addSpacing = new QLabel(body);
  m_addSpacing->setObjectName("secondary");
  outer->addWidget(m_addSpacing);
  // Grow region's limits.
  m_growBox = new QWidget(body);
  auto* grow = new QVBoxLayout(m_growBox);
  grow->setContentsMargins(0, 0, 0, 0);
  auto* angleRow = new QHBoxLayout;
  angleRow->addWidget(new QLabel(tr("Surface bends at most"), m_growBox), 1);
  m_growAngle = new QDoubleSpinBox(m_growBox);
  m_growAngle->setObjectName("cloudEditGrowAngle");
  m_growAngle->setRange(0.5, 90);
  m_growAngle->setDecimals(1);
  m_growAngle->setSuffix(QStringLiteral(" °"));
  m_growAngle->setValue(QSettings().value("cloudEdit/growAngle", 10.0).toDouble());
  connect(m_growAngle, &QDoubleSpinBox::valueChanged, this, [](double v) { QSettings().setValue("cloudEdit/growAngle", v); });
  angleRow->addWidget(m_growAngle);
  grow->addLayout(angleRow);
  auto* offRow = new QHBoxLayout;
  offRow->addWidget(new QLabel(tr("Off its surface at most"), m_growBox), 1);
  m_growOffset = new QDoubleSpinBox(m_growBox);
  m_growOffset->setObjectName("cloudEditGrowOffset");
  m_growOffset->setDecimals(3);
  m_growOffset->setSingleStep(0.01);
  offRow->addWidget(m_growOffset);
  grow->addLayout(offRow);
  connect(m_growOffset, &QDoubleSpinBox::valueChanged, this, [](double shown) {
    QSettings().setValue("cloudEdit/growOffset", units::fromDisplay(units::Kind::Length, shown));
  });
  auto* growHint = new QLabel(tr("From the clicked point over its neighbours on the same surface: a flat patch, a fillet, a bridge over a bore."), m_growBox);
  growHint->setObjectName("secondary");
  growHint->setWordWrap(true);
  grow->addWidget(growHint);
  outer->addWidget(m_growBox);
  // What is selected and what then.
  m_selectedRow = new QWidget(body);
  auto* selected = new QVBoxLayout(m_selectedRow);
  selected->setContentsMargins(0, 0, 0, 0);
  m_selectedText = new QLabel(m_selectedRow);
  m_selectedText->setObjectName("cloudEditSelected");
  selected->addWidget(m_selectedText);
  auto* acts = new QHBoxLayout;
  auto* del = new QPushButton(m_selectedRow);
  auto* keep = new QPushButton(tr("Keep only these"), m_selectedRow);
  del->setObjectName("cloudEditDeleteSelected");
  keep->setObjectName("cloudEditKeepSelected");
  connect(del, &QPushButton::clicked, this, [this] { changeSelection(false); });
  connect(keep, &QPushButton::clicked, this, [this] { changeSelection(true); });
  m_deleteSel = del;
  m_keepSel = keep;
  acts->addWidget(del);
  acts->addWidget(keep);
  selected->addLayout(acts);
  outer->addWidget(m_selectedRow);
  m_hint = new QLabel(body);
  m_hint->setObjectName("secondary");
  m_hint->setWordWrap(true);
  outer->addWidget(m_hint);
  m_status = new QLabel(body);
  m_status->setObjectName("cloudEditStatus");
  m_status->setWordWrap(true);
  outer->addWidget(m_status);
  outer->addStretch();
  m_footer = new PanelFooter(body);
  m_footer->setPrimary(tr("Finish"), QString());
  m_footer->setCancel(tr("Cancel"), QString());
  connect(m_footer, &PanelFooter::accepted, this, [this] { finish(); });
  connect(m_footer, &PanelFooter::cancelled, this, [this] { cancel(true); });
  outer->addWidget(m_footer);
  m_panel = new ToolPanel(QStringLiteral("cloud-edit"), QStringLiteral("cloud_edit"), &Tokens::sel, tr("Edit point cloud"), body, 460, services().window());
  m_panel->setObjectName("cloudEditPanel");
  m_panel->setHelpId("cloud.edit");
  // Esc lets go of what is under way (a drag, then the selection); the mode itself ends only by Finish or Cancel.
  m_panel->setEscapeHandler([this] {
    if (m_dragging) {
      m_dragging = false;
      m_haveLast = false;
      services().viewport()->setCloudLasso({}, false);
    } else if (!m_selected.empty()) {
      m_selected = {};
      services().viewport()->showCloudRegion(m_body, nullptr);
      services().updateCommands();
    } else {
      services().showMessage(help::expand(tr("Finish ({key:cloud.edit.finish}) or Cancel ends the point cloud edit.")), 4000);
    }
    refresh();
  });
  services().addPanel(m_panel);
}

void CloudEditArea::setTool(Tool tool) {
  m_tool = tool;
  if (selecting(tool)) m_lastSelect = tool;
  if (QAction* a = toolAction(tool); !a->isChecked()) a->setChecked(true);
  services().viewport()->setCloudLasso({}, false);
  refresh();
}

int CloudEditArea::radiusPixels() const { return m_radius ? m_radius->value() : 40; }
void CloudEditArea::setRadiusPixels(int pixels) {
  if (m_radius) m_radius->setValue(std::clamp(pixels, kMinRadius, kMaxRadius));
}
double CloudEditArea::strength() const { return m_strength ? m_strength->value() / 100.0 : 0.5; }
opad::cloud::Falloff CloudEditArea::falloff() const {
  for (size_t i = 0; i < m_falloffButtons.size(); ++i)
    if (m_falloffButtons[i]->isChecked()) return opad::cloud::Falloff(i);
  return opad::cloud::Falloff::Smooth;
}
bool CloudEditArea::frontOnly() const { return !m_front || m_front->isChecked(); }

opad::json CloudEditArea::summary() const {
  const auto e = current();
  return e ? e->summary() : opad::json::object();
}

void CloudEditArea::refresh() {
  if (!m_status) return;
  const int t = int(m_tool);
  for (int i = 0; i < int(m_toolButtons.size()); ++i) m_toolButtons[size_t(i)]->setChecked(i < 4 ? i == t : selecting(m_tool));
  for (int i = 0; i < int(m_selectButtons.size()); ++i) m_selectButtons[size_t(i)]->setChecked(int(Tool::Paint) + i == t);
  m_directionRow->setVisible(m_tool == Tool::Pull);
  m_selectRow->setVisible(selecting(m_tool));
  m_sizeRow->setVisible(brushes(m_tool));
  m_strengthRow->setVisible(m_tool == Tool::Pull || m_tool == Tool::Smooth);
  m_falloffRow->setVisible(m_tool == Tool::Pull || m_tool == Tool::Smooth);
  m_front->setVisible(m_tool == Tool::Pull || m_tool == Tool::Smooth || m_tool == Tool::Erase || m_tool == Tool::Paint);
  m_addSpacing->setVisible(m_tool == Tool::Add);
  m_growBox->setVisible(m_tool == Tool::Grow);
  m_selectedRow->setVisible(selecting(m_tool) || !m_selected.empty());
  if (m_growOffset && !m_growOffset->hasFocus()) {
    const QSignalBlocker block(m_growOffset);
    m_growOffset->setSuffix(QStringLiteral(" ") + units::symbol(units::Kind::Length));
    m_growOffset->setRange(units::toDisplay(units::Kind::Length, 0.0005), units::toDisplay(units::Kind::Length, 50));
    m_growOffset->setValue(units::toDisplay(units::Kind::Length, QSettings().value("cloudEdit/growOffset", 0.05).toDouble()));
  }
  // The hint of the tool, keys as they are bound now.
  QString hint = help::expand(i18n::t(QString::fromLatin1(kTools[t].hint)));
  if (brushes(m_tool)) {
    const QString smaller = keys::text("cloud.edit.smaller"), larger = keys::text("cloud.edit.larger");
    if (!smaller.isEmpty() && !larger.isEmpty()) hint += QStringLiteral(" ") + tr("%1 and %2 change the size.").arg(smaller, larger);
  }
  m_hint->setText(hint);
  // The size on screen and what it spans on the cloud at the middle of its box.
  double mm = 0;
  QString size = tr("%1 px").arg(radiusPixels());
  if (m_base && !m_base->box.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    m_base->box.Get(x0, y0, z0, x1, y1, z1);
    opad::Vec3 toward;
    const opad::Vec3 mid = m_world.apply({(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2});
    if (services().viewport()->cloudBrushFrame({}, mid, toward, mm)) size += QStringLiteral(" · ≈ ") + units::format(units::Kind::Length, mm * radiusPixels());
  }
  m_radiusLabel->setText(size);
  const double spacing = m_editor ? m_editor->spacing() : m_base ? m_base->cloudSpacing : 0;
  m_addSpacing->setText(tr("Spacing along a drag: twice the scan's (%1)").arg(units::format(units::Kind::Length, 2 * spacing)));
  const QLocale l;
  m_selectedText->setText(m_selected.empty() ? tr("Nothing selected") : tr("%1 points selected").arg(l.toString(qulonglong(m_selected.count()))));
  const QString delKey = keys::text("edit.delete");
  m_deleteSel->setText(delKey.isEmpty() ? tr("Delete these") : tr("Delete these (%1)").arg(delKey));
  m_deleteSel->setEnabled(!m_selected.empty());
  m_keepSel->setEnabled(!m_selected.empty());
  QString status;
  if (m_active && !m_editor) status = tr("Preparing the points…");
  else if (m_active) {
    const opad::json s = summary();
    status = tr("%1 moved · %2 deleted · %3 added · %4 strokes")
                 .arg(l.toString(qulonglong(s.value("moved", 0))), l.toString(qulonglong(s.value("deleted", 0))), l.toString(qulonglong(s.value("added", 0))),
                      l.toString(qulonglong(m_index)));
    if (m_finishing) status += QStringLiteral("\n") + tr("Finishing…");
  }
  m_status->setText(status);
  m_footer->setPrimaryEnabled(m_editor != nullptr && !m_finishing);
  if (m_active) {
    const QString name = services().document()->nodeName(m_body);
    services().showMessage(tr("Editing %1 · %2").arg(name, status.section('\n', 0, 0)), 0);
    QString keysHint;
    if (brushes(m_tool)) {
      const QString smaller = keys::text("cloud.edit.smaller"), larger = keys::text("cloud.edit.larger");
      if (!smaller.isEmpty()) keysHint = tr("%1 %2 size").arg(smaller, larger);
    }
    m_prompt->setText("cloud_edit", tr("Edit points"), i18n::t(QString::fromLatin1(kTools[t].label)) + (keysHint.isEmpty() ? QString() : QStringLiteral(" · ") + keysHint) +
                                                           QStringLiteral(" · ") + help::expand(tr("{fixed:esc} lets go")));
    positionOverlays({});
  }
  services().updateCommands();
}

// ---------------------------------------------------------------- the worker queue
void CloudEditArea::queue(Task task) {
  // A brush's dabs join those of the same stroke still waiting: one task does them all.
  if (!task.dabs.empty() && !task.work && !m_tasks.empty() && !m_tasks.back().dabs.empty() && !m_tasks.back().work && m_tasks.back().stroke == task.stroke) {
    auto& last = m_tasks.back().dabs;
    last.insert(last.end(), task.dabs.begin(), task.dabs.end());
  } else {
    m_tasks.push_back(std::move(task));
  }
  pump();
}

void CloudEditArea::pump() {
  if (m_taskRunning || !m_editor) return;
  if (m_tasks.empty()) {
    if (m_finishing && !m_dragging) QTimer::singleShot(0, this, [this] { if (m_finishing && !m_taskRunning) { m_finishing = false; finish(); } });
    return;
  }
  Task task = std::move(m_tasks.front());
  m_tasks.pop_front();
  auto editor = m_editor;
  auto out = std::make_shared<Out>();
  const int session = m_session;
  const int mode = BodyPrs::cloudColorMode.load();
  auto stroke = task.stroke;
  std::vector<Dab> dabs = std::move(task.dabs);
  auto work = std::move(task.work);
  auto done = std::move(task.done);
  QElapsedTimer clock;
  clock.start();
  m_taskRunning = true;
  const QString shown = task.shown;
  auto run = [editor, out, dabs, work, stroke, mode](Progress p) {
    out->cancelled = [p]() mutable { return p.cancelled(); };
    if (work) return work(*editor, *out);
    // A brush's dabs, then the stroke's points so far drawn where they are now (deleted ones red, selected ones orange).
    for (const Dab& d : dabs) {
      std::vector<uint32_t> ids;
      std::vector<float> w;
      switch (d.tool) {
        case Tool::Pull: ids = editor->push(d.brush, d.amount); break;
        case Tool::Smooth: ids = editor->smooth(d.brush, d.amount); break;
        case Tool::Erase: ids = editor->erase(d.brush); break;
        case Tool::Add:
          if (const auto added = editor->add(d.brush.centre, d.brush.toward)) ids = {*added};
          break;
        default: editor->under(d.brush, ids, w); break;  // Paint select
      }
      if (!stroke) continue;
      std::vector<uint32_t> merged;
      merged.reserve(stroke->size() + ids.size());
      std::set_union(stroke->begin(), stroke->end(), ids.begin(), ids.end(), std::back_inserter(merged));
      stroke->swap(merged);
    }
    if (!stroke || stroke->empty() || dabs.empty()) return;
    // The stroke's points as the editor has them now: a small edit of just those (no deletions: erased points show).
    opad::cloud::Edit now;
    const auto& base = editor->base();
    now.base = base.size();
    now.added = editor->added();
    for (uint32_t i : *stroke) {
      if (i >= base.size()) continue;
      const opad::Vec3 p = editor->at(i), q = base.point(i);
      now.moved.push_back(i);
      for (int k = 0; k < 3; ++k) now.offsets.push_back(float(p[size_t(k)] - q[size_t(k)]));
    }
    const Tool kind = dabs.front().tool;
    const std::array<double, 3> red{0.89, 0.35, 0.3}, orange{1.0, 0.6, 0.2};
    const bool flat = kind == Tool::Erase || kind == Tool::Paint;
    out->stroke = BodyPrs::cloudArray(base, stroke.get(), flat ? 4 : mode, stroke->size() + 1, nullptr, nullptr, kind == Tool::Erase ? &red : kind == Tool::Paint ? &orange : nullptr, &now);
    out->count = stroke->size();
  };
  auto finished = [this, out, session, done, clock](bool ok, const QString& error) {
    m_job = nullptr;
    m_taskRunning = false;
    if (session != m_session || !m_active) return;
    if (!ok) services().showMessage(error, 6000);
    else if (done) done(*out);
    else if (!out->stroke.IsNull()) services().viewport()->showCloudStroke(m_body, out->stroke);
    if (trace::enabled() && clock.elapsed() > 150) trace::log(QStringLiteral("cloud edit: a task took %1 ms").arg(clock.elapsed()));
    pump();
  };
  m_job = shown.isEmpty() ? services().jobs()->quiet(tr("Editing the points"), run, finished) : services().jobs()->async(shown, run, finished);
}

// ---------------------------------------------------------------- strokes
bool CloudEditArea::frameAt(const QPointF& at, opad::Vec3& local, opad::Vec3& toward, double& mmPerPixel) {
  opad::Vec3 world, eye;
  Viewport* view = services().viewport();
  if (!m_base || !view->pickCloudPoint(at, world)) return false;
  if (!view->cloudBrushFrame(at, world, eye, mmPerPixel)) return false;
  const opad::Mat4 inverse = m_world.inverse();
  local = inverse.apply(world);
  const auto& offset = m_base->cloudData->offset;
  for (int k = 0; k < 3; ++k) local[size_t(k)] -= offset[size_t(k)];
  toward = inverse.apply_dir(eye);
  const double l = std::sqrt(toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2]);
  if (l < 1e-12) return false;
  for (auto& v : toward) v /= l;
  return true;
}

void CloudEditArea::queueDab(const QPointF& at) {
  opad::Vec3 p, toward;
  double mm = 0;
  if (!frameAt(at, p, toward, mm)) return;
  const double radius = std::max(1e-3, mm * radiusPixels());
  const double spacing = m_editor ? std::max(m_editor->spacing(), 1e-3) : radius / 10;
  // How far apart dabs fall: a quarter of the brush; Add's points twice the scan's spacing (at least four pixels) apart.
  const double step = m_tool == Tool::Add ? std::max(2 * spacing, 4 * mm) : 0.25 * radius;
  Task task;
  task.stroke = m_strokeIds;
  auto dab = [&](const opad::Vec3& c) {
    Dab d;
    d.tool = m_tool;
    d.brush.centre = c;
    d.brush.toward = toward;
    d.brush.radius = radius;
    d.brush.falloff = falloff();
    d.brush.front = frontOnly();
    if (m_tool == Tool::Pull) d.amount = (m_inverted ? -1 : 1) * (m_push->isChecked() ? -1 : 1) * kPullPerDab * strength() * radius;
    else if (m_tool == Tool::Smooth) d.amount = kSmoothPerDab * strength();
    task.dabs.push_back(d);
  };
  if (!m_haveLast) {
    dab(p);
  } else {
    const opad::Vec3 d{p[0] - m_last[0], p[1] - m_last[1], p[2] - m_last[2]};
    const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length < step) return;
    if (length > 6 * radius) {
      dab(p);  // a jump onto another part or across a gap: a new start, not a line through the air
    } else {
      const int count = std::min(64, int(length / step));
      for (int k = 1; k <= count; ++k) {
        const double s = k * step / length;
        dab({m_last[0] + d[0] * s, m_last[1] + d[1] * s, m_last[2] + d[2] * s});
      }
      const double s = count * step / length;
      p = {m_last[0] + d[0] * s, m_last[1] + d[1] * s, m_last[2] + d[2] * s};
    }
  }
  m_last = p;
  m_haveLast = true;
  queue(std::move(task));
}

void CloudEditArea::kept(const std::shared_ptr<const opad::cloud::Edit>& snapshot) {
  m_history.resize(m_index + 1);
  m_history.push_back(snapshot);
  m_index = m_history.size() - 1;
  services().viewport()->setCloudEdit(m_body, snapshot, true);
  services().updateUndo();
  refresh();
}

void CloudEditArea::endStroke() {
  m_dragging = false;
  m_haveLast = false;
  m_strokeIds.reset();
  Task task;
  task.work = [](opad::cloud::Editor& e, Out& out) {
    e.countStroke();
    out.snapshot = std::make_shared<const opad::cloud::Edit>(e.edit());
  };
  task.done = [this](Out& out) { kept(out.snapshot); };
  queue(std::move(task));
}

void CloudEditArea::finishSelect(const QPointF& at) {
  // The region: the painted points, a box or lasso (where the points are now), or grown from the clicked point; then joined
  // with the selection (Shift), taken from it (Ctrl) or replacing it.
  const auto painted = std::exchange(m_strokeIds, nullptr);
  m_dragging = false;
  m_haveLast = false;
  Viewport* view = services().viewport();
  std::vector<std::array<double, 2>> polygon;
  if (m_tool == Tool::Box || m_tool == Tool::Lasso) {
    QPolygonF shape = m_lasso;
    if (m_tool == Tool::Box) {
      if (shape.size() < 2) return view->setCloudLasso({}, false);
      const QRectF r = QRectF(shape.first(), shape.last()).normalized();
      if (r.width() < 2 || r.height() < 2) return view->setCloudLasso({}, false);
      shape = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    }
    for (const QPointF& p : shape) {
      const QPointF n = view->widgetToNdc(p);
      polygon.push_back({n.x(), n.y()});
    }
    view->setCloudLasso({}, false);
    if (polygon.size() < 3) return;
  }
  bool seeded = false;
  opad::Vec3 seed, toward;
  double mm = 0;
  if (m_tool == Tool::Grow && !(seeded = frameAt(at, seed, toward, mm))) return services().showMessage(tr("Click on a point of the cloud to grow from."), 3000);
  const auto& offset = m_base->cloudData->offset;
  const auto ndc = view->worldToNdc(m_world * opad::Mat4::translation(offset[0], offset[1], offset[2]));
  const int combine = m_combine;
  const opad::cloud::Region before = m_selected;
  const Tool tool = m_tool;
  const double angle = m_growAngle->value(), off = units::fromDisplay(units::Kind::Length, m_growOffset->value());
  Task task;
  task.work = [=](opad::cloud::Editor& e, Out& out) {
    opad::cloud::Region r;
    if (tool == Tool::Paint) r = painted ? opad::cloud::Region::of(*painted) : opad::cloud::Region();
    else if (tool == Tool::Grow && seeded) {
      std::vector<uint32_t> nearest;
      e.nearest(seed, 1, nearest);
      if (!nearest.empty()) r = e.grow(nearest.front(), angle, off, 2000000, &out.stopped, out.cancelled);
    } else r = e.inPolygon(ndc, polygon);
    out.region = combine > 0 ? before.unite(r) : combine < 0 ? before.subtract(r) : r;
  };
  if (tool == Tool::Grow) task.shown = tr("Growing the region");
  task.done = [this](Out& out) {
    m_selected = out.region;
    services().viewport()->showCloudStroke(m_body, nullptr);
    services().viewport()->showCloudRegion(m_body, m_selected.empty() ? nullptr : std::make_shared<const std::vector<uint32_t>>(m_selected.indices()));
    if (out.stopped) services().showMessage(tr("Grow region stopped at %1 points: lower its limits to keep to one surface.").arg(QLocale().toString(qulonglong(m_selected.count()))), 6000);
    trace::log(QStringLiteral("cloud edit: %1 points selected").arg(m_selected.count()));
    refresh();
  };
  queue(std::move(task));
}

void CloudEditArea::changeSelection(bool keep) {
  if (m_selected.empty() || !m_active || !m_editor) return;
  const opad::cloud::Region region = std::exchange(m_selected, {});
  services().viewport()->showCloudRegion(m_body, nullptr);
  Task task;
  task.work = [region, keep](opad::cloud::Editor& e, Out& out) {
    out.count = keep ? e.keep(region) : e.erase(region);
    e.countStroke();
    out.snapshot = std::make_shared<const opad::cloud::Edit>(e.edit());
  };
  task.done = [this](Out& out) { kept(out.snapshot); };
  queue(std::move(task));
  refresh();
}

void CloudEditArea::undo() {
  if (!m_active || m_index == 0 || m_dragging) return;
  --m_index;
  const auto to = m_history[m_index];
  Task task;
  task.work = [to](opad::cloud::Editor& e, Out&) { e.reset(*to); };
  task.done = [this, to](Out&) {
    services().viewport()->setCloudEdit(m_body, to->empty() ? nullptr : to, true);
    services().viewport()->showCloudStroke(m_body, nullptr);
    refresh();
  };
  queue(std::move(task));
  services().updateUndo();
  refresh();
}

void CloudEditArea::redo() {
  if (!m_active || m_index + 1 >= m_history.size() || m_dragging) return;
  ++m_index;
  const auto to = m_history[m_index];
  Task task;
  task.work = [to](opad::cloud::Editor& e, Out&) { e.reset(*to); };
  task.done = [this, to](Out&) {
    services().viewport()->setCloudEdit(m_body, to, true);
    refresh();
  };
  queue(std::move(task));
  services().updateUndo();
  refresh();
}

void CloudEditArea::showBrush(const QPointF& at) {
  QPolygonF circle;
  const double r = radiusPixels();
  for (int k = 0; k < 40; ++k) circle << at + QPointF(r * std::cos(k * M_PI / 20), r * std::sin(k * M_PI / 20));
  services().viewport()->setCloudLasso(circle, true);
}

bool CloudEditArea::eventFilter(QObject* object, QEvent* event) {
  Viewport* view = services().viewport();
  if (object != view) return AreaController::eventFilter(object, event);
  const auto type = event->type();
  // Double-click on a cloud (outside the mode): edit it, once the view has picked what was clicked.
  if (!m_active) {
    if (type == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
      QTimer::singleShot(0, this, [this] {
        AppDocument* doc = services().document();
        if (m_active || !doc->hasDocument || doc->browse || !services().activeCommand().isEmpty()) return;
        const SelectionContext s = services().selection();
        if (s.sketching) return;
        std::string body;
        for (const auto& r : s.refs)
          if (cloudBody(r.body)) body = r.body;
        for (const auto& id : s.ids)
          if (cloudBody(id)) body = id;
        if (!body.empty()) services().guarded([&] { edit(body); });
      });
    return false;
  }
  if (type == QEvent::Leave) {
    if (!m_dragging) view->setCloudLasso({}, false);
    return false;
  }
  if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease && type != QEvent::MouseButtonDblClick) return false;
  auto* e = static_cast<QMouseEvent*>(event);
  const bool brush = brushes(m_tool);
  if (type == QEvent::MouseButtonPress && e->button() == Qt::LeftButton) {
    if (view->cubeAt(e->position())) return false;
    if (!m_editor || m_finishing) return true;  // still preparing, or finishing
    m_dragging = true;
    m_inverted = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
    m_combine = e->modifiers() & Qt::ShiftModifier ? 1 : e->modifiers() & Qt::ControlModifier ? -1 : 0;
    m_haveLast = false;
    m_strokeIds = std::make_shared<std::vector<uint32_t>>();
    m_lasso = {e->position()};
    if (brush) {
      showBrush(e->position());
      queueDab(e->position());
    }
    return true;
  }
  if (type == QEvent::MouseMove) {
    if (!m_dragging) {
      if (brush && !(e->buttons() & (Qt::MiddleButton | Qt::RightButton))) showBrush(e->position());
      return false;  // the view still hovers, orbits and pans
    }
    m_inverted = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
    if (brush) {
      showBrush(e->position());
      queueDab(e->position());
    } else if (m_tool == Tool::Lasso) {
      m_lasso << e->position();
      view->setCloudLasso(m_lasso, false);
    } else if (m_tool == Tool::Box) {
      m_lasso = {m_lasso.first(), e->position()};
      const QRectF r = QRectF(m_lasso[0], m_lasso[1]).normalized();
      view->setCloudLasso({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}, true);
    }
    return true;
  }
  if (type == QEvent::MouseButtonRelease && e->button() == Qt::LeftButton && m_dragging) {
    if (brush) queueDab(e->position());
    if (selecting(m_tool)) finishSelect(e->position());
    else endStroke();
    if (brush) showBrush(e->position());
    return true;
  }
  return m_dragging || (e->button() == Qt::LeftButton && type == QEvent::MouseButtonDblClick);
}

OPAD_AREA(CloudEditArea)
