// Edit point cloud: the mode (CloudEditArea.hpp). Brushes are cylinders along the view through the point picked under the
// pointer (Viewport::pickCloudPoint: the cloud's own pick BVH), as wide as the brush's circle on screen at that depth;
// dabs follow the pointer at a quarter of the radius, filled in along the stroke when the pointer outran the workers.
#include "CloudEditArea.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
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

#include <cmath>

#include "AppDocument.hpp"
#include "BodyShape.hpp"
#include "CommandHelp.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
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
}  // namespace

CloudEditArea::~CloudEditArea() = default;

// ---------------------------------------------------------------- commands
void CloudEditArea::buildActions() {
  CommandInfo edit{"cloud.edit", tr("Edit point cloud"), "cloud_edit"};
  edit.keywords = {tr("sculpt"), tr("push pull"), tr("brush"), tr("clean scan"), tr("delete points"), tr("add points"), tr("point cloud"), tr("reverse engineering")};
  edit.group = tr("Point cloud");
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
  finish.group = tr("Point cloud");
  finish.enabledWhen = inMode;
  m_finish = services().addCommand(finish, [this] { this->finish(); });
  m_finish->setIconText(tr("Finish"));
  CommandInfo cancel{"cloud.edit.cancel", tr("Cancel point cloud edit"), "close"};
  cancel.keywords = {tr("discard"), tr("leave"), tr("point cloud")};
  cancel.group = tr("Point cloud");
  cancel.enabledWhen = inMode;
  m_cancel = services().addCommand(cancel, [this] { this->cancel(true); });
  m_cancel->setIconText(tr("Cancel"));
  struct T {
    Tool tool;
    const char* id;
    const char* label;
    const char* icon;
    QStringList words;
  };
  const T tools[] = {{Tool::Pull, "cloud.edit.pull", QT_TR_NOOP("Push/Pull brush"), "cloud_pull", {tr("sculpt"), tr("push"), tr("pull"), tr("inflate"), tr("dent")}},
                     {Tool::Smooth, "cloud.edit.smooth", QT_TR_NOOP("Smooth brush"), "cloud_smooth", {tr("relax"), tr("flatten"), tr("noise"), tr("sculpt")}},
                     {Tool::Erase, "cloud.edit.erase", QT_TR_NOOP("Delete brush"), "cloud_erase", {tr("erase"), tr("remove points"), tr("clean")}},
                     {Tool::Add, "cloud.edit.add", QT_TR_NOOP("Add points"), "cloud_add", {tr("fill hole"), tr("new point"), tr("patch")}},
                     {Tool::Select, "cloud.edit.select", QT_TR_NOOP("Select to delete"), "cloud_region", {tr("lasso"), tr("box"), tr("keep only"), tr("crop")}}};
  auto* group = new QActionGroup(this);
  group->setExclusive(true);
  for (const T& t : tools) {
    CommandInfo info{t.id, tr(t.label), t.icon};
    info.keywords = t.words;
    info.keywords << tr("point cloud");
    info.group = tr("Point cloud");
    info.checkable = true;
    info.enabledWhen = inMode;
    QAction* a = services().addCommand(info, [this, tool = t.tool] { setTool(tool); });
    a->setCheckable(true);
    group->addAction(a);
    m_toolActions.push_back(a);
  }
  m_toolActions.front()->setChecked(true);
  CommandInfo smaller{"cloud.edit.smaller", tr("Smaller brush"), "brush_smaller", QKeySequence("Ctrl+[")};
  smaller.keywords = {tr("brush size"), tr("radius"), tr("point cloud")};
  smaller.group = tr("Point cloud");
  smaller.enabledWhen = inMode;
  services().addCommand(smaller, [this] { setRadiusPixels(int(std::lround(radiusPixels() / 1.25))); });
  CommandInfo larger{"cloud.edit.larger", tr("Larger brush"), "brush_larger", QKeySequence("Ctrl+]")};
  larger.keywords = {tr("brush size"), tr("radius"), tr("point cloud")};
  larger.group = tr("Point cloud");
  larger.enabledWhen = inMode;
  services().addCommand(larger, [this] { setRadiusPixels(int(std::lround(radiusPixels() * 1.25 + 0.5))); });
}

QAction* CloudEditArea::toolAction(Tool tool) const { return m_toolActions[size_t(tool)]; }

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
  layout.addGroup(tab, tab + ".brush", tr("Brush"));
  for (Tool t : {Tool::Pull, Tool::Smooth, Tool::Erase, Tool::Add}) layout.addAction(tab + ".brush", toolAction(t), t == Tool::Pull ? RibbonLayout::Size::Large : RibbonLayout::Size::Small);
  layout.addGroup(tab, tab + ".size", tr("Size"));
  layout.addAction(tab + ".size", services().action("cloud.edit.smaller"), RibbonLayout::Size::Small);
  layout.addAction(tab + ".size", services().action("cloud.edit.larger"), RibbonLayout::Size::Small);
  layout.addGroup(tab, tab + ".select", tr("Select"));
  layout.addAction(tab + ".select", toolAction(Tool::Select));
  layout.addGroup(tab, tab + ".finish", tr("Finish"));
  layout.addAction(tab + ".finish", m_finish, RibbonLayout::Size::Large, {}, true);
  layout.addAction(tab + ".finish", m_cancel, RibbonLayout::Size::Large);
}

void CloudEditArea::ready() {
  buildPanel();
  services().viewport()->installEventFilter(this);
  services().viewport()->setMouseTracking(true);
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
  auto start = [this, body, opId] {
    AppDocument* d = services().document();
    std::shared_ptr<const opad::cloud::Edit> begun = std::make_shared<opad::cloud::Edit>();
    if (!opId.empty()) {
      const opad::Feature* f = d->scene.feature(opId);
      if (!f || f->kind != "cloud_edit") return;
      begun = std::make_shared<opad::cloud::Edit>(opad::cloud::Edit::from_json(f->inputs.value("edit", opad::json::object())));
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
    services().select({});
    refresh();
    services().updateCommands();
    services().updateUndo();
    // The arrays drawn for the cloud as it is at this step (rolled back: drawn anew first), then the editor on a worker.
    const int session = m_session;
    auto wait = std::make_shared<std::function<void()>>();
    *wait = [this, session, wait, begun] {
      if (session != m_session || !m_active) return;
      AppDocument* now = services().document();
      const opad::Node* n = now->node(m_body);
      opad::Mat4 world;
      const auto prs = services().viewport()->cloudPrs(m_body, &world);
      if (!n || !prs || services().viewport()->displayArrays(n->body_key) != prs) {
        QTimer::singleShot(50, this, *wait);
        return;
      }
      enterPrepared(m_body, m_op, begun);
    };
    (*wait)();
  };
  if (services().requireEditable(start)) start();
}

void CloudEditArea::enterPrepared(const std::string& body, const std::string&, std::shared_ptr<const opad::cloud::Edit> begun) {
  opad::Mat4 world;
  m_base = services().viewport()->cloudPrs(body, &world);
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
    services().viewport()->setCloudEdit(m_body, begun->empty() ? nullptr : begun, false);
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
  m_editor.reset();
  m_base.reset();
  m_history.clear();
  m_index = 0;
  m_selected = {};
  if (!m_op.empty()) services().document()->setRollback({});
  m_op.clear();
  services().setContextualTab(kTabId, false);
  if (!m_workspaceBefore.isEmpty() && m_workspaceBefore != services().workspace()) services().setWorkspace(m_workspaceBefore);
  m_workspaceBefore.clear();
  if (m_panel->isVisible()) m_panel->hide();
  services().updateCommands();
  services().updateUndo();
}

void CloudEditArea::finish() {
  if (!m_active) return;
  if (m_job || !m_tasks.empty() || m_dragging || !m_editor) {  // the strokes under way first
    m_finishing = true;
    return refresh();
  }
  m_finishing = false;
  const auto edit = current();
  const bool changed = m_op.empty() ? edit && !edit->empty() : m_index != 0;
  const std::string body = m_body, opId = m_op;
  if (!changed) {
    services().viewport()->setCloudEdit(body, nullptr);
    return leave();
  }
  // The op made on the worker (an edit's text can be megabytes), planned and committed as one step; the edited points stay
  // drawn until the body's new arrays replace them.
  const QString label = opId.empty() ? tr("Edit point cloud") : tr("Edit point cloud again");
  AppDocument* doc = services().document();
  const std::string name = opId.empty() ? opad::design::next_name(doc->scene, "EditPoints") : std::string();
  m_finishing = true;
  refresh();
  const int session = m_session;
  services().design()->applyOps({}, label, [this, session, body](bool ok, const QString& error) {
    if (session != m_session) return;
    m_finishing = false;
    if (!ok) {
      services().showMessage(error, 8000);
      return refresh();
    }
    leave();
    services().undoToast(tr("Point cloud edited"));
  }, [edit, body, opId, name](opad::Document& d, opad::design::Plan& plan) {
    const opad::json inputs = {{"bodies", opad::json::array({{{"body", body}}})}, {"edit", edit->to_json()}};
    const opad::json op = opId.empty() ? opad::design::make_feature_op("cloud_edit", name, inputs) : opad::design::make_edit_op(opId, {{"inputs", inputs}});
    plan = opad::design::plan_ops(d, {op});
  });
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
  if (!m_active) return;
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
  // The tools, as the ribbon's (the same actions).
  auto* tools = new QHBoxLayout;
  for (QAction* a : m_toolActions) {
    auto* b = new QPushButton(a->icon(), QString(), body);
    b->setToolTip(a->text());
    b->setCheckable(true);
    b->setObjectName("cloudEditTool." + a->objectName().section('.', 2));
    connect(b, &QPushButton::clicked, a, &QAction::trigger);
    connect(a, &QAction::toggled, b, &QPushButton::setChecked);
    b->setChecked(a->isChecked());
    tools->addWidget(b);
  }
  tools->addStretch();
  outer->addLayout(tools);
  m_hint = new QLabel(body);
  m_hint->setObjectName("secondary");
  m_hint->setWordWrap(true);
  outer->addWidget(m_hint);
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
  // Size and strength.
  auto* size = new QHBoxLayout;
  size->addWidget(new QLabel(tr("Size"), body));
  m_radius = new QSlider(Qt::Horizontal, body);
  m_radius->setObjectName("cloudEditRadius");
  m_radius->setRange(kMinRadius, kMaxRadius);
  m_radius->setValue(std::clamp(QSettings().value("cloudEdit/radius", 40).toInt(), kMinRadius, kMaxRadius));
  size->addWidget(m_radius, 1);
  m_radiusLabel = new QLabel(body);
  m_radiusLabel->setObjectName("cloudEditRadiusText");
  size->addWidget(m_radiusLabel);
  outer->addLayout(size);
  connect(m_radius, &QSlider::valueChanged, this, [this](int v) {
    QSettings().setValue("cloudEdit/radius", v);
    refresh();
  });
  m_strengthRow = new QWidget(body);
  auto* strength = new QHBoxLayout(m_strengthRow);
  strength->setContentsMargins(0, 0, 0, 0);
  strength->addWidget(new QLabel(tr("Strength"), m_strengthRow));
  m_strength = new QSlider(Qt::Horizontal, m_strengthRow);
  m_strength->setObjectName("cloudEditStrength");
  m_strength->setRange(1, 100);
  m_strength->setValue(std::clamp(QSettings().value("cloudEdit/strength", 50).toInt(), 1, 100));
  connect(m_strength, &QSlider::valueChanged, this, [](int v) { QSettings().setValue("cloudEdit/strength", v); });
  strength->addWidget(m_strength, 1);
  outer->addWidget(m_strengthRow);
  // Select: how, and what then.
  m_selectRow = new QWidget(body);
  auto* sel = new QVBoxLayout(m_selectRow);
  sel->setContentsMargins(0, 0, 0, 0);
  auto* modes = new QHBoxLayout;
  auto* group = new QButtonGroup(m_selectRow);
  const QString names[] = {tr("Lasso"), tr("Box"), tr("Brush")};
  for (int i = 0; i < 3; ++i) {
    auto* b = new QPushButton(names[i], m_selectRow);
    b->setCheckable(true);
    b->setObjectName(QStringLiteral("cloudEditSelect%1").arg(i));
    group->addButton(b, i);
    modes->addWidget(b);
    m_selectModes.push_back(b);
  }
  m_selectModes.front()->setChecked(true);
  connect(group, &QButtonGroup::idClicked, this, [this](int id) {
    m_selectMode = id;
    refresh();
  });
  sel->addLayout(modes);
  auto* acts = new QHBoxLayout;
  auto* del = new QPushButton(tr("Delete these"), m_selectRow);
  auto* keep = new QPushButton(tr("Keep only these"), m_selectRow);
  del->setObjectName("cloudEditDeleteSelected");
  keep->setObjectName("cloudEditKeepSelected");
  connect(del, &QPushButton::clicked, this, [this] { changeSelection(false); });
  connect(keep, &QPushButton::clicked, this, [this] { changeSelection(true); });
  m_deleteSel = del;
  m_keepSel = keep;
  acts->addWidget(del);
  acts->addWidget(keep);
  sel->addLayout(acts);
  outer->addWidget(m_selectRow);
  m_status = new QLabel(body);
  m_status->setObjectName("cloudEditStatus");
  m_status->setWordWrap(true);
  outer->addWidget(m_status);
  m_footer = new PanelFooter(body);
  m_footer->setPrimary(tr("Finish"), QString());
  m_footer->setCancel(tr("Cancel"), QString());
  connect(m_footer, &PanelFooter::accepted, this, [this] { finish(); });
  connect(m_footer, &PanelFooter::cancelled, this, [this] { cancel(true); });
  outer->addWidget(m_footer);
  m_panel = new ToolPanel(QStringLiteral("cloud-edit"), QStringLiteral("cloud_edit"), &Tokens::sel, tr("Edit point cloud"), body, 320, services().window());
  m_panel->setObjectName("cloudEditPanel");
  m_panel->setHelpId("cloud.edit");
  // Esc lets go of what is under way (a selection, a drag); the mode itself ends only by Finish or Cancel.
  m_panel->setEscapeHandler([this] {
    if (m_dragging) {
      m_dragging = false;
      services().viewport()->setCloudLasso({}, false);
    } else if (!m_selected.empty()) {
      m_selected = {};
      services().viewport()->showCloudRegion(m_body, nullptr);
    } else {
      services().showMessage(help::expand(tr("Finish or Cancel ends the point cloud edit ({key:cloud.edit.finish}).")), 4000);
    }
    refresh();
  });
  services().addPanel(m_panel);
}

void CloudEditArea::setTool(Tool tool) {
  m_tool = tool;
  if (QAction* a = toolAction(tool); !a->isChecked()) a->setChecked(true);
  if (tool != Tool::Select && !m_selected.empty()) {
    m_selected = {};
    services().viewport()->showCloudRegion(m_body, nullptr);
  }
  services().viewport()->setCloudLasso({}, false);
  refresh();
}

int CloudEditArea::radiusPixels() const { return m_radius ? m_radius->value() : 40; }
void CloudEditArea::setRadiusPixels(int pixels) {
  if (m_radius) m_radius->setValue(std::clamp(pixels, kMinRadius, kMaxRadius));
}
double CloudEditArea::strength() const { return m_strength ? m_strength->value() / 100.0 : 0.5; }

opad::json CloudEditArea::summary() const {
  const auto e = current();
  return e ? e->summary() : opad::json::object();
}

void CloudEditArea::refresh() {
  if (!m_status) return;
  const bool brush = m_tool != Tool::Select || m_selectMode == 2;
  m_directionRow->setVisible(m_tool == Tool::Pull);
  m_strengthRow->setVisible(m_tool == Tool::Pull || m_tool == Tool::Smooth);
  m_selectRow->setVisible(m_tool == Tool::Select);
  m_deleteSel->setEnabled(!m_selected.empty());
  m_keepSel->setEnabled(!m_selected.empty());
  static const char* hints[] = {QT_TR_NOOP("Drag over the points to pull them out of the surface; {fixed:ctrl} or {fixed:alt} held pushes them in."),
                                QT_TR_NOOP("Drag over the points to settle them onto their surface."),
                                QT_TR_NOOP("Drag over the points to delete them."),
                                QT_TR_NOOP("Click on the surface to add a point there; drag to add a row of them."),
                                QT_TR_NOOP("Drag round the points to select them; {fixed:shift} adds, {fixed:ctrl} takes away.")};
  QString hint = help::expand(i18n::t(QString::fromLatin1(hints[int(m_tool)])));
  if (brush) {
    const QString smaller = keys::text("cloud.edit.smaller"), larger = keys::text("cloud.edit.larger");
    if (!smaller.isEmpty() && !larger.isEmpty()) hint += QStringLiteral(" ") + tr("%1 and %2 change the size.").arg(smaller, larger);
  }
  m_hint->setText(hint);
  // The size on screen and what it spans on the cloud at the middle of the view.
  QString size = tr("%1 px").arg(radiusPixels());
  if (m_base && m_base->cloudSpacing > 0) {
    opad::Vec3 toward;
    double mm = 0;
    const auto box = m_base->box;
    if (!box.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      const opad::Vec3 mid = m_world.apply({(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2});
      if (services().viewport()->cloudBrushFrame({}, mid, toward, mm)) size += QStringLiteral(" · ≈") + units::format(units::Kind::Length, mm * radiusPixels());
    }
  }
  m_radiusLabel->setText(size);
  QString status;
  if (!m_active) status.clear();
  else if (!m_editor) status = tr("Preparing the points…");
  else {
    const opad::json s = summary();
    const QLocale l;
    status = tr("%1 moved · %2 deleted · %3 added").arg(l.toString(qulonglong(s.value("moved", 0))), l.toString(qulonglong(s.value("deleted", 0))), l.toString(qulonglong(s.value("added", 0))));
    if (!m_selected.empty()) status += QStringLiteral("\n") + tr("%1 points selected").arg(l.toString(qulonglong(m_selected.count())));
    if (m_finishing) status += QStringLiteral("\n") + tr("Finishing…");
  }
  m_status->setText(status);
  m_footer->setPrimaryEnabled(m_editor != nullptr && !m_finishing);
}

// ---------------------------------------------------------------- the worker queue
void CloudEditArea::queue(Task task) {
  // A brush's dabs join those still waiting: one task does them all.
  if (!task.dabs.empty() && !task.work && !m_tasks.empty() && !m_tasks.back().dabs.empty() && !m_tasks.back().work && m_tasks.back().stroke == task.stroke) {
    auto& last = m_tasks.back().dabs;
    last.insert(last.end(), task.dabs.begin(), task.dabs.end());
  } else {
    m_tasks.push_back(std::move(task));
  }
  pump();
}

void CloudEditArea::pump() {
  if (m_taskRunning || !m_editor || m_tasks.empty()) {
    if (!m_taskRunning && m_finishing && m_tasks.empty() && !m_dragging && m_editor) QTimer::singleShot(0, this, [this] { if (m_finishing) finish(); });
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
  m_job = services().jobs()->quiet(tr("Editing the points"), [editor, out, dabs, work, stroke, mode](Progress) {
    if (work) return work(*editor, *out);
    // A brush's dabs, then the stroke's points so far drawn where they are now (deleted ones in red).
    bool erase = false;
    for (const Dab& d : dabs) {
      opad::cloud::Brush b;
      b.centre = d.centre;
      b.toward = d.toward;
      b.radius = d.radius;
      std::vector<uint32_t> ids;
      std::vector<float> w;
      switch (d.tool) {
        case Tool::Pull: ids = editor->push(b, d.amount); break;
        case Tool::Smooth: ids = editor->smooth(b, d.amount); break;
        case Tool::Erase: ids = editor->erase(b), erase = true; break;
        case Tool::Add:
          if (const auto added = editor->add(d.centre, d.toward)) ids = {*added};
          break;
        case Tool::Select: editor->under(b, ids, w); break;
      }
      if (!stroke) continue;
      std::vector<uint32_t> merged;
      merged.reserve(stroke->size() + ids.size());
      std::set_union(stroke->begin(), stroke->end(), ids.begin(), ids.end(), std::back_inserter(merged));
      stroke->swap(merged);
    }
    if (!stroke || stroke->empty()) return;
    // The points of the stroke as the editor has them now: a small edit of just those (no deletions: erased points show).
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
    const bool selecting = !dabs.empty() && dabs.front().tool == Tool::Select;
    const std::array<double, 3> red{0.9, 0.2, 0.15}, orange{1.0, 0.55, 0.1};
    out->stroke = BodyPrs::cloudArray(base, stroke.get(), erase || selecting ? 4 : mode, stroke->size() + 1, nullptr, nullptr, erase ? &red : selecting ? &orange : nullptr, &now);
    out->count = stroke->size();
  }, [this, out, session, done, clock](bool ok, const QString& error) {
    m_job = nullptr;
    m_taskRunning = false;
    if (session != m_session || !m_active) return;
    if (!ok) services().showMessage(error, 6000);
    else if (done) done(*out);
    else if (!out->stroke.IsNull()) services().viewport()->showCloudStroke(m_body, out->stroke);
    if (trace::enabled() && clock.elapsed() > 100) trace::log(QStringLiteral("cloud edit: a task took %1 ms").arg(clock.elapsed()));
    pump();
  });
}

// ---------------------------------------------------------------- strokes
bool CloudEditArea::frameAt(const QPointF& at, opad::Vec3& local, opad::Vec3& toward, double& mmPerPixel) {
  opad::Vec3 world;
  Viewport* view = services().viewport();
  if (!view->pickCloudPoint(at, world)) return false;
  opad::Vec3 eye;
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
    d.centre = c;
    d.toward = toward;
    d.radius = radius;
    if (m_tool == Tool::Pull) d.amount = (m_inverted ? -1 : 1) * (m_push && m_push->isChecked() ? -1 : 1) * kPullPerDab * strength() * radius;
    else if (m_tool == Tool::Smooth) d.amount = kSmoothPerDab * strength();
    task.dabs.push_back(d);
  };
  if (!m_haveLast) {
    dab(p);
  } else {
    const opad::Vec3 d{p[0] - m_last[0], p[1] - m_last[1], p[2] - m_last[2]};
    const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length < step) return;
    // A jump of more than a few brushes (onto another part, across a gap): a new start, not a line through the air.
    const int count = length > 6 * radius ? 1 : std::min(64, int(length / step));
    for (int k = 1; k <= count; ++k) {
      const double t = count == 1 && length > 6 * radius ? 1.0 : k * step / length;
      dab({m_last[0] + d[0] * t, m_last[1] + d[1] * t, m_last[2] + d[2] * t});
    }
    if (count >= 1 && length <= 6 * radius) p = {m_last[0] + d[0] * count * step / length, m_last[1] + d[1] * count * step / length, m_last[2] + d[2] * count * step / length};
  }
  m_last = p;
  m_haveLast = true;
  queue(std::move(task));
}

void CloudEditArea::endStroke() {
  m_dragging = false;
  m_haveLast = false;
  const auto stroke = std::exchange(m_strokeIds, nullptr);
  if (m_tool == Tool::Select) return finishSelect();
  Task task;
  task.work = [](opad::cloud::Editor& e, Out& out) {
    e.countStroke();
    out.snapshot = std::make_shared<const opad::cloud::Edit>(e.edit());
  };
  task.done = [this](Out& out) {
    m_history.resize(m_index + 1);
    m_history.push_back(out.snapshot);
    m_index = m_history.size() - 1;
    services().viewport()->setCloudEdit(m_body, out.snapshot, true);
    services().updateUndo();
    refresh();
  };
  (void)stroke;
  queue(std::move(task));
}

void CloudEditArea::finishSelect() {
  // The region: a lasso or box on the worker (where the points are now), or the brush's points; then joined with the
  // selection (Shift), taken from it (Ctrl) or replacing it.
  const auto stroke = m_strokeIds;
  std::vector<std::array<double, 2>> polygon;
  if (m_selectMode == 0 || m_selectMode == 1) {
    QPolygonF shape = m_lasso;
    if (m_selectMode == 1 && shape.size() >= 2) {
      const QRectF r = QRectF(shape.first(), shape.last()).normalized();
      if (r.width() < 2 || r.height() < 2) return;
      shape = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    }
    for (const QPointF& p : shape) {
      const QPointF n = services().viewport()->widgetToNdc(p);
      polygon.push_back({n.x(), n.y()});
    }
    if (polygon.size() < 3) return;
  }
  services().viewport()->setCloudLasso({}, false);
  opad::Mat4 local = m_world;
  const auto& offset = m_base->cloudData->offset;
  local = m_world * opad::Mat4::translation(offset[0], offset[1], offset[2]);
  const auto ndc = services().viewport()->worldToNdc(local);
  const int combine = m_combine;
  const opad::cloud::Region before = m_selected;
  Task task;
  task.work = [polygon, ndc, combine, before](opad::cloud::Editor& e, Out& out) {
    opad::cloud::Region r = polygon.empty() ? opad::cloud::Region() : e.inPolygon(ndc, polygon);
    out.region = combine > 0 ? before.unite(r) : combine < 0 ? before.subtract(r) : r;
  };
  if (m_selectMode == 2) {
    // The brush's points were gathered dab by dab (m_strokeIds of this drag, kept by the last dab task).
    auto ids = m_lastSelectIds;
    task.work = [ids, combine, before](opad::cloud::Editor&, Out& out) {
      const opad::cloud::Region r = ids ? opad::cloud::Region::of(*ids) : opad::cloud::Region();
      out.region = combine > 0 ? before.unite(r) : combine < 0 ? before.subtract(r) : r;
    };
  }
  task.done = [this](Out& out) {
    m_selected = out.region;
    services().viewport()->showCloudStroke(m_body, nullptr);
    services().viewport()->showCloudRegion(m_body, std::make_shared<const std::vector<uint32_t>>(m_selected.indices()));
    trace::log(QStringLiteral("cloud edit: %1 points selected").arg(m_selected.count()));
    refresh();
  };
  (void)stroke;
  queue(std::move(task));
}

void CloudEditArea::changeSelection(bool keep) {
  if (m_selected.empty() || !m_active) return;
  const opad::cloud::Region region = std::exchange(m_selected, {});
  services().viewport()->showCloudRegion(m_body, nullptr);
  Task task;
  task.work = [region, keep](opad::cloud::Editor& e, Out& out) {
    out.count = keep ? e.keep(region) : e.erase(region);
    e.countStroke();
    out.snapshot = std::make_shared<const opad::cloud::Edit>(e.edit());
  };
  task.done = [this](Out& out) {
    m_history.resize(m_index + 1);
    m_history.push_back(out.snapshot);
    m_index = m_history.size() - 1;
    services().viewport()->setCloudEdit(m_body, out.snapshot, true);
    services().updateUndo();
    refresh();
  };
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
    if (type == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && !services().selection().sketching)
      QTimer::singleShot(0, this, [this] {
        if (m_active || !services().document()->hasDocument || services().document()->browse || !services().activeCommand().isEmpty()) return;
        const SelectionContext s = services().selection();
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
  const bool brush = m_tool != Tool::Select || m_selectMode == 2;
  if (type == QEvent::MouseButtonPress && e->button() == Qt::LeftButton) {
    if (view->cubeAt(e->position())) return false;
    if (!m_editor) return true;  // still preparing
    m_dragging = true;
    m_inverted = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
    m_combine = e->modifiers() & Qt::ShiftModifier ? 1 : e->modifiers() & Qt::ControlModifier ? -1 : 0;
    m_haveLast = false;
    m_strokeIds = std::make_shared<std::vector<uint32_t>>();
    if (m_tool == Tool::Select && m_selectMode == 2) m_lastSelectIds = m_strokeIds;
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
    } else if (m_selectMode == 0) {
      m_lasso << e->position();
      view->setCloudLasso(m_lasso, false);
    } else {
      m_lasso = {m_lasso.first(), e->position()};
      const QRectF r = QRectF(m_lasso[0], m_lasso[1]).normalized();
      view->setCloudLasso({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}, true);
    }
    return true;
  }
  if (type == QEvent::MouseButtonRelease && e->button() == Qt::LeftButton && m_dragging) {
    if (brush) queueDab(e->position());
    endStroke();
    if (brush) showBrush(e->position());
    return true;
  }
  return m_dragging || (e->button() == Qt::LeftButton && type == QEvent::MouseButtonDblClick);
}

OPAD_AREA(CloudEditArea)
