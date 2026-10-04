// Image canvases in the window (UI-70, CanvasArea.hpp).
#include "CanvasArea.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>

#include <atomic>
#include <cmath>
#include <memory>

#include "AppDocument.hpp"
#include "AssetsArea.hpp"
#include "BrowserPanel.hpp"
#include "CanvasEditor.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "DrawingPlacer.hpp"
#include "GuidedTool.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "PanelFooter.hpp"
#include "PropertiesPanel.hpp"
#include "Ribbon.hpp"
#include "SketchBackdrop.hpp"
#include "Theme.hpp"
#include "Toast.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_trace.hpp"
#include "opad/geometry.hpp"

OPAD_ICON_TABLE(canvas,
                {"canvas", R"(<path d="M3 17l4-12h14l-4 12z"/><path d="M6 15l3-3 3 2 2-3 3 4"/>)"},
                {"calibrate", R"(<path d="M3 16l13-13 5 5-13 13z"/><path d="M7 12l2 2M10 9l2 2M13 6l2 2"/><circle cx="4" cy="20" r="1.5"/>)"},
                {"alignto", R"(<rect x="3" y="3" width="10" height="8"/><circle cx="18" cy="17" r="2"/><path d="M13 11l3.5 4.5M3 11l-0 0"/><circle cx="6" cy="20" r="2"/><path d="M3 11l2 7"/>)"},
                {"trace", R"(<rect x="3" y="4" width="18" height="14" stroke-dasharray="2 2"/><path d="M6 15c2-6 5-6 6-2s4 4 6-3"/>)"});

namespace {
bool pictureFile(const QString& path) { return QStringList{"png", "jpg", "jpeg", "bmp", "gif", "webp"}.contains(QFileInfo(path).suffix().toLower()); }
opad::Vec3 unit(const opad::Vec3& v) {
  const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return l > 1e-15 ? opad::Vec3{v[0] / l, v[1] / l, v[2] / l} : v;
}
double distance(const opad::Vec3& a, const opad::Vec3& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
// A cancelled job reports at once while its worker may still read the copy, whose shapes the document shares.
void afterWorker(QObject* ctx, std::shared_ptr<std::atomic<bool>> reading, std::function<void()> fn) {
  if (!*reading) return fn();
  QTimer::singleShot(20, ctx, [ctx, reading, fn] { afterWorker(ctx, reading, fn); });
}
}  // namespace

CanvasArea::CanvasArea(AreaServices& services) : AreaController(services) {}

// ---------------------------------------------------------------- commands, menus, ribbon
void CanvasArea::buildActions() {
  auto command = [this](const char* id, const QString& label, const char* icon, const QStringList& keywords, std::function<bool(const CommandContext&)> when,
                        std::function<void()> fn) {
    CommandInfo info;
    info.id = id;
    info.label = label;
    info.icon = icon;
    info.group = tr("Canvas");
    info.keywords = keywords;
    info.enabledWhen = std::move(when);
    info.editsDocument = std::string_view(id) != "canvas.finish";  // a viewed or read-only file is saved first (requireEditable)
    services().addCommand(info, std::move(fn));
  };
  auto one = [this](const CommandContext& c) { return c.document && !c.viewer && !c.sketching && (!canvasOf(c.selection).empty() || (m_editor && m_editor->active())); };
  auto onTarget = [this](std::function<void()> fn) {
    return [this, fn] {
      if (const std::string id = target(); !id.empty()) {
        if (m_editor->canvas() != id) edit(id);
        if (m_editor->canvas() == id) fn();
      }
    };
  };
  command("canvas.insert", tr("Insert canvas…"), "canvas", {"image", "picture", "photo", "underlay", "reference image"}, [](const CommandContext& c) { return !c.sketching; },
          [this] {  // the file, when a picture was dropped onto the document (the action's "file" property)
            QAction* self = services().action("canvas.insert");
            const QString file = self->property("file").toString();
            self->setProperty("file", QVariant());
            insert(file);
          });
  command("canvas.edit", tr("Edit canvas"), "move", {"image", "picture", "move", "scale", "rotate"}, one, onTarget([] {}));
  command("canvas.calibrate", tr("Calibrate canvas…"), "calibrate", {"scale", "real size", "measure picture"}, one, onTarget([this] { calibrate(); }));
  command("canvas.align", tr("Align canvas to model…"), "alignto", {"scale relative", "match", "fit picture"}, one, onTarget([this] { align(); }));
  command("canvas.trace", tr("Trace canvas to sketch"), "trace", {"vectorize", "bitmap", "outline"}, one, onTarget([this] { trace(); }));
  command("canvas.replace", tr("Replace canvas picture…"), "image", {"swap", "relink", "picture"}, one, onTarget([this] { replace(); }));
  command("canvas.finish", tr("Close canvas"), "finish", {"done", "finish", "leave"}, [this](const CommandContext&) { return m_editor && m_editor->active(); },
          [this] { finish(); });
  command("canvas.fromBackdrop", tr("Backdrop images to canvases"), "canvas", {"sketch image", "convert", "picture"},
          [this](const CommandContext& c) {
            std::string sketch;
            return c.document && !c.viewer && !c.sketching && sketchWithBackdrops(c.selection, sketch);
          },
          [this] {
            std::string sketch;
            if (sketchWithBackdrops(services().selection(), sketch)) fromBackdrop(sketch);
          });
}

void CanvasArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  if (QMenu* design = menus.value("design")) {
    QMenu* sub = design->addMenu(tr("Canvas"));
    sub->setObjectName("canvas");
    for (const char* id : {"canvas.insert", "canvas.edit", "canvas.calibrate", "canvas.align", "canvas.trace", "canvas.replace", "canvas.fromBackdrop"})
      sub->addAction(services().action(id));
  }
}

void CanvasArea::ribbon(RibbonLayout& layout) {
  layout.addAction("design.construct.construct", services().action("canvas.insert"));
  // While a canvas is edited its own tab comes first in Design (edit shows it, finish hides it).
  layout.addContextualTab("design", "design.canvas", tr("Canvas"));
  auto group = [&](const char* id, const QString& title, std::initializer_list<const char*> commands) {
    const QString groupId = QString("design.canvas.") + id;
    layout.addGroup("design.canvas", groupId, title);
    for (const char* command : commands) layout.addAction(groupId, services().action(command));
  };
  group("place", tr("Place"), {"canvas.calibrate", "canvas.align"});
  group("picture", tr("Picture"), {"canvas.trace", "canvas.replace", "canvas.insert"});
  group("close", tr("Close"), {"canvas.finish"});
}

// ---------------------------------------------------------------- the panel
void CanvasArea::ready() {
  AppDocument* doc = services().document();
  Viewport* view = services().viewport();
  m_editor = new CanvasEditor(doc, view, services().jobs(), this);
  m_placer = new DrawingPlacer(doc, view, services().jobs(), services().window());
  buildPanel();
  m_prompt = new PromptBar(view);
  m_prompt->setObjectName("canvasPrompt");
  m_prompt->setAttribute(Qt::WA_NativeWindow);
  m_prompt->hide();
  connect(m_editor, &CanvasEditor::dragged, this, [this](const opad::CanvasPlace&) { fillPanel(); });
  connect(m_editor, &CanvasEditor::placed, this, [this](const opad::CanvasPlace& p) {
    if (!run({{"action", "place"}, {"set", {{"x", p.x}, {"y", p.y}, {"width", p.width}, {"height", p.height}, {"angle", p.angle * 180 / M_PI}}}}))
      services().viewport()->endPlacementPreview(m_editor->canvas());
  });
  connect(m_editor, &CanvasEditor::picked, this, &CanvasArea::flowPicked);
  connect(m_editor, &CanvasEditor::pickCancelled, this, &CanvasArea::flowBack);
  connect(m_editor, &CanvasEditor::status, this, [this](const QString& text) { services().showMessage(text, 3000); });
  connect(units::notifier(), &units::Notifier::changed, this, [this] { fillPanel(true); });
  view->installEventFilter(this);
  services().browser()->addDecorator([this](const browser::Row& row, browser::Decoration& d) {
    if (!row.node || !opad::is_canvas(*row.node)) return;
    if (d.typeIcon.isEmpty()) d.typeIcon = "image";  // a linked one keeps its link
    d.tooltip = tr("Image canvas: double-click it in the view to move, size or turn it");
  });
  services().properties()->addSectionProvider([this](const PropertySubject& s, const opad::json&, QList<PropertySection>& out) { section(s, out); });
}

void CanvasArea::section(const PropertySubject& subject, QList<PropertySection>& out) {
  if (subject.refs.size() != 1) return;
  const std::string id = subject.refs.front().body;
  const opad::Node* n = services().document()->node(id);
  if (!n || !opad::is_canvas(*n) || n->body_missing) return;
  const opad::CanvasPlace p = opad::canvas_place(services().document()->scene, id);
  const opad::CanvasFlags flags = opad::CanvasFlags::of(n->canvas);
  PropertySection sec;
  sec.title = tr("Canvas");
  auto length = [](double mm) { return units::format(units::Kind::Length, mm); };
  sec.rows << qMakePair(tr("Size"), QString("%1 × %2").arg(length(p.width), length(p.height)))
           << qMakePair(tr("Centre on its plane"), QString("%1, %2").arg(length(p.x), length(p.y)))
           << qMakePair(tr("Angle"), units::format(units::Kind::Angle, p.angle * 180 / M_PI));
  if (const opad::json px = n->raster.value("px", opad::json()); px.is_array() && px.size() == 2 && p.width > 0)
    sec.rows << qMakePair(tr("Picture"), tr("%1 × %2 px, %3 dpi as placed").arg(px[0].get<long>()).arg(px[1].get<long>()).arg(px[0].get<double>() / (p.width / 25.4), 0, 'f', 0));
  QStringList shown;
  if (p.stretched()) shown << tr("stretched out of its proportions");
  if (flags.flip[0]) shown << tr("flipped left-right");
  if (flags.flip[1]) shown << tr("flipped upside down");
  if (flags.through) shown << tr("through the model");
  if (!flags.selectable) shown << tr("not selectable in the view");
  if (n->locked) shown << tr("locked");
  if (!shown.isEmpty()) sec.rows << qMakePair(tr("Display"), shown.join(", "));
  sec.actions << qMakePair(tr("Edit canvas"), std::function<void()>([this, id] { QTimer::singleShot(0, this, [this, id] { edit(id); }); }));
  out << sec;
}

void CanvasArea::buildPanel() {
  auto* body = new QWidget;
  auto* outer = new QVBoxLayout(body);
  m_main = new QWidget(body);  // the canvas's place and flags; Calibrate and Align swap it for their steps
  auto* layout = new QVBoxLayout(m_main);
  layout->setContentsMargins(0, 0, 0, 0);
  m_hint = new QLabel(body);
  m_hint->setObjectName("secondary");
  m_hint->setWordWrap(true);
  layout->addWidget(m_hint);
  auto* form = new QFormLayout;
  const QStringList labels = {tr("X"), tr("Y"), tr("Width"), tr("Height"), tr("Angle")};
  const QStringList names = {"canvasX", "canvasY", "canvasWidth", "canvasHeight", "canvasAngle"};
  for (int i = 0; i < 5; ++i) {
    m_fields[size_t(i)] = new QLineEdit(body);
    m_fields[size_t(i)]->setObjectName(names[i]);
    m_fields[size_t(i)]->installEventFilter(this);
    connect(m_fields[size_t(i)], &QLineEdit::returnPressed, this, &CanvasArea::applyFields);
    form->addRow(labels[i], m_fields[size_t(i)]);
  }
  m_fields[0]->setToolTip(tr("Its centre on the plane it was put on (an expression in the shown unit)"));
  m_fields[3]->setToolTip(tr("Typed alone, width or height keeps its proportions; typed together, both apply (stretched)"));
  m_fields[2]->setToolTip(m_fields[3]->toolTip());
  m_opacity = new QSlider(Qt::Horizontal, body);
  m_opacity->setObjectName("canvasOpacity");
  m_opacity->setRange(5, 100);
  m_opacity->setTracking(false);  // one appearance op when let go
  connect(m_opacity, &QSlider::valueChanged, this, [this](int value) {
    if (m_filling) return;
    if (const std::string id = target(); !id.empty()) services().guarded([&] { services().document()->run("appearance", {{"target", id}, {"opacity", value / 100.0}}); });
  });
  form->addRow(tr("Opacity"), m_opacity);
  layout->addLayout(form);
  auto check = [&](QCheckBox*& box, const QString& text, const char* name) {
    box = new QCheckBox(text, body);
    box->setObjectName(name);
    return box;
  };
  auto* flips = new QHBoxLayout;
  flips->addWidget(check(m_flipH, tr("Flip left-right"), "canvasFlipH"));
  flips->addWidget(check(m_flipV, tr("Flip upside down"), "canvasFlipV"));
  layout->addLayout(flips);
  layout->addWidget(check(m_through, tr("Show through the model"), "canvasThrough"));
  layout->addWidget(check(m_selectable, tr("Selectable in the view"), "canvasSelectable"));
  layout->addWidget(check(m_lock, tr("Locked"), "canvasLock"));
  m_through->setToolTip(tr("Drawn over the model, as a reference to trace or compare"));
  m_selectable->setToolTip(tr("Off: clicks in the view go through it; it is still picked in the browser"));
  connect(m_flipH, &QCheckBox::toggled, this, [this](bool on) { if (!m_filling) setFlags({{"flip", {on, m_flipV->isChecked()}}}); });
  connect(m_flipV, &QCheckBox::toggled, this, [this](bool on) { if (!m_filling) setFlags({{"flip", {m_flipH->isChecked(), on}}}); });
  connect(m_through, &QCheckBox::toggled, this, [this](bool on) { if (!m_filling) setFlags({{"display_through", on}}); });
  connect(m_selectable, &QCheckBox::toggled, this, [this](bool on) { if (!m_filling) setFlags({{"selectable", on}}); });
  connect(m_lock, &QCheckBox::toggled, this, [this](bool on) {
    if (m_filling) return;
    if (const std::string id = target(); !id.empty()) services().guarded([&] { services().document()->run("appearance", {{"target", id}, {"locked", on}}); });
  });
  auto button = [&](const QString& text, const char* icon, const char* name, const QString& tip, std::function<void()> fn) {
    auto* b = new QPushButton(icons::icon(icon, theme::current().fg), text, body);
    b->setObjectName(name);
    b->setToolTip(tip);
    connect(b, &QPushButton::clicked, this, std::move(fn));
    return b;
  };
  m_proportions = button(tr("Picture proportions"), "image", "canvasProportions", tr("Its height from its width as the picture has them: no longer stretched"), [this] {
    const opad::CanvasPlace p = m_editor->place();
    run({{"action", "place"}, {"set", {{"width", p.width}, {"height", p.width * p.body_h / p.body_w}}}});
  });
  layout->addWidget(m_proportions);
  auto* tools = new QHBoxLayout;
  m_moves << button(tr("Calibrate"), "calibrate", "canvasCalibrate", tr("Pick two points on the picture and type their real distance"), [this] { calibrate(); })
          << button(tr("Align to model"), "alignto", "canvasAlign", tr("Pick two points on the picture and the two points of the model they belong on"), [this] { align(); });
  tools->addWidget(m_moves[0]);
  tools->addWidget(m_moves[1]);
  layout->addLayout(tools);
  auto* more = new QHBoxLayout;
  more->addWidget(button(tr("Trace to sketch"), "trace", "canvasTrace", tr("A new sketch on the canvas's plane with the picture's dark shapes as curves"), [this] { trace(); }));
  more->addWidget(button(tr("Replace picture…"), "image", "canvasReplace", tr("Another picture in this place, as wide as this one"), [this] { replace(); }));
  layout->addLayout(more);
  outer->addWidget(m_main);
  // Calibrate and Align as guided tools (GuidedTool.hpp): the steps with what was picked, then the real distance; the
  // footer's Back takes the last pick back (as Esc does), Cancel leaves, Apply calibrates.
  m_flowPage = new QWidget(body);
  auto* flowLayout = new QVBoxLayout(m_flowPage);
  flowLayout->setContentsMargins(0, 0, 0, 0);
  m_steps = new ToolStepsPanel(m_flowPage);
  m_steps->setObjectName("canvasSteps");
  m_steps->setFooter(false, false);
  flowLayout->addWidget(m_steps, 1);
  m_distanceRow = new QWidget(m_flowPage);
  auto* row = new QHBoxLayout(m_distanceRow);
  row->setContentsMargins(12, 0, 12, 0);
  row->addWidget(new QLabel(tr("Real distance"), m_distanceRow));
  m_distance = new QLineEdit(m_distanceRow);
  m_distance->setObjectName("canvasDistance");
  m_distance->installEventFilter(this);
  row->addWidget(m_distance);
  connect(m_distance, &QLineEdit::returnPressed, this, &CanvasArea::applyCalibration);
  connect(m_distance, &QLineEdit::textEdited, this, [this] { refreshPrompt(); });
  m_distanceRow->hide();
  flowLayout->addWidget(m_distanceRow);
  m_flowPage->hide();
  outer->addWidget(m_flowPage, 1);
  outer->addStretch();
  m_footer = new PanelFooter(body);
  m_footer->setBack(tr("Back"), "Esc");
  m_footer->setCancel(tr("Cancel"), QString());
  connect(m_footer, &PanelFooter::accepted, this, [this] {
    if (m_flow == Flow::Calibrate) applyCalibration();
    else if (m_flow == Flow::None) finish();
  });
  connect(m_footer, &PanelFooter::cancelled, this, [this] { endFlow(); });
  connect(m_footer, &PanelFooter::backRequested, this, &CanvasArea::flowBack);
  outer->addWidget(m_footer);
  footerForFlow();
  m_panel = new ToolPanel("canvas", "canvas", &Tokens::sel, tr("Canvas"), body, 520, services().window());
  m_panel->setObjectName("canvasPanel");
  m_panel->setHelpId("canvas.edit");  // its "?": the canvas editor's card and clip
  m_panel->setEscapeHandler([this] {
    if (m_flow != Flow::None) flowBack();
    else finish();
  });
  connect(m_panel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on && m_editor->active()) {
      endFlow();
      m_editor->stop();
      services().setContextualTab("design.canvas", false);
    }
  });
  services().addPanel(m_panel);
}

QLineEdit* CanvasArea::field(int i) const { return i == 5 ? m_distance : i >= 0 && i < 5 ? m_fields[size_t(i)] : nullptr; }

void CanvasArea::fillPanel(bool force) {
  if (!m_editor || !m_editor->active()) return;
  const opad::Node* n = services().document()->node(m_editor->canvas());
  if (!n) return;
  if (force) m_typing = nullptr;
  const opad::CanvasPlace p = m_editor->place();
  const opad::CanvasFlags flags = opad::CanvasFlags::of(n->canvas);
  m_filling = true;
  const double values[5] = {p.x, p.y, p.width, p.height, p.angle * 180 / M_PI};
  for (int i = 0; i < 5; ++i) {
    QLineEdit* f = m_fields[size_t(i)];
    if (!force && f->hasFocus() && f->isModified()) continue;
    f->setText(units::editable(i == 4 ? units::Kind::Angle : units::Kind::Length, values[i]));
    f->setModified(false);
    f->setEnabled(!n->locked);
  }
  m_opacity->setValue(int(std::lround(n->opacity * 100)));
  m_flipH->setChecked(flags.flip[0]);
  m_flipV->setChecked(flags.flip[1]);
  m_through->setChecked(flags.through);
  m_selectable->setChecked(flags.selectable);
  m_lock->setChecked(n->locked);
  for (QPushButton* b : m_moves) b->setEnabled(!n->locked);
  m_proportions->setVisible(p.stretched());
  m_proportions->setEnabled(!n->locked);
  m_filling = false;
  m_panel->setContext(QString::fromStdString(n->name));
  m_hint->setText(n->locked ? tr("Locked: unlock it to move it.")
                  : p.on_plane ? tr("X and Y place its centre on the plane it was put on.")
                               : tr("Moved off the plane it was put on: X and Y are in its own plane now."));
}

void CanvasArea::applyFields() {
  if (!m_editor->active()) return;
  opad::json args = {{"action", "place"}, {"set", opad::json::object()}};
  bool any = false;
  for (int i = 0; i < 5; ++i) {
    QLineEdit* f = m_fields[size_t(i)];
    if (!f->isModified()) continue;
    const auto value = units::parse(i == 4 ? units::Kind::Angle : units::Kind::Length, f->text());
    if (!value || ((i == 2 || i == 3) && !(*value > 0))) {
      toast(tr("Not a value: %1").arg(f->text()));
      f->setFocus();
      f->selectAll();
      return;
    }
    args["set"][std::array<const char*, 5>{"x", "y", "width", "height", "angle"}[size_t(i)]] = *value;
    any = true;
  }
  if (any) run(args);
  fillPanel(true);
}

void CanvasArea::setFlags(const opad::json& change) { run({{"action", "flags"}, {"set", change}}); }

bool CanvasArea::run(opad::json args) {
  const std::string id = target();
  if (id.empty()) return false;
  args["target"] = id;
  try {
    services().document()->run("canvas", args);
    return true;
  } catch (const std::exception& e) {
    toast(i18n::t(QString::fromUtf8(e.what())));
    fillPanel(true);
    return false;
  }
}

void CanvasArea::toast(const QString& text, int ms) { services().toast(text, QString(), {}, ms); }

// ---------------------------------------------------------------- selection, document, view
std::string CanvasArea::canvasOf(const SelectionContext& selection) const {
  if (selection.ids.size() != 1) return {};
  const opad::Node* n = services().document()->node(selection.ids.front());
  return n && opad::is_canvas(*n) && !n->body_missing ? n->id : std::string();
}

std::string CanvasArea::target() const { return m_editor && m_editor->active() ? m_editor->canvas() : canvasOf(services().selection()); }

bool CanvasArea::sketchWithBackdrops(const SelectionContext& selection, std::string& sketch) const {
  if (selection.ids.size() != 1) return false;
  const opad::SketchItem* s = services().document()->scene.sketch(selection.ids.front());
  if (!s || !s->geometry.contains("images") || !s->geometry["images"].is_array() || s->geometry["images"].empty()) return false;
  sketch = s->id;
  return true;
}

void CanvasArea::selectionChanged(const SelectionContext& selection) {
  if (!m_panel || !m_panel->isVisible() || m_flow != Flow::None) return;
  if (const std::string id = canvasOf(selection); !id.empty() && id != m_editor->canvas()) edit(id);  // the panel follows the canvas picked
}

void CanvasArea::documentChanged(bool replaced) {
  if (!m_editor) return;
  if (replaced) return finish();
  m_editor->refresh();
  if (!m_editor->active()) return finish();
  fillPanel();
}

void CanvasArea::positionOverlays(const QRect&) {
  if (m_prompt && m_prompt->isVisible()) {
    Viewport* view = services().viewport();
    m_prompt->move(std::max(8, (view->width() - m_prompt->width()) / 2), 44);
    m_prompt->raise();
  }
}

void CanvasArea::contextMenu(const SelectionContext& selection, QMenu& menu) {
  if (selection.sketching) return;
  std::string sketch;
  if (sketchWithBackdrops(selection, sketch)) {
    menu.addSeparator();
    menu.addAction(services().action("canvas.fromBackdrop"));
  }
  const std::string id = canvasOf(selection);
  if (id.empty()) return;
  menu.addSeparator();
  for (const char* command : {"canvas.edit", "canvas.calibrate", "canvas.align", "canvas.trace", "canvas.replace"}) menu.addAction(services().action(command));
  const opad::Node* n = services().document()->node(id);
  const opad::CanvasFlags flags = opad::CanvasFlags::of(n->canvas);
  QMenu* look = menu.addMenu(tr("Canvas display"));
  auto add = [&](const QString& text, bool on, opad::json change) {
    QAction* a = look->addAction(text);
    a->setCheckable(true);
    a->setChecked(on);
    const opad::json args = {{"action", "flags"}, {"target", id}, {"set", change}};
    connect(a, &QAction::triggered, this, [this, args] { services().guarded([&] { services().document()->run("canvas", args); }); });
  };
  add(tr("Flip left-right"), flags.flip[0], {{"flip", {!flags.flip[0], flags.flip[1]}}});
  add(tr("Flip upside down"), flags.flip[1], {{"flip", {flags.flip[0], !flags.flip[1]}}});
  add(tr("Show through the model"), flags.through, {{"display_through", !flags.through}});
  add(tr("Selectable in the view"), flags.selectable, {{"selectable", !flags.selectable}});
}

bool CanvasArea::eventFilter(QObject* object, QEvent* event) {
  QLineEdit* line = qobject_cast<QLineEdit*>(object);
  if (line && event->type() == QEvent::KeyPress) {  // the fields: Tab goes round them, Esc gives the view its keys back
    auto* k = static_cast<QKeyEvent*>(event);
    const int at = int(std::find(m_fields.begin(), m_fields.end(), line) - m_fields.begin());
    if ((k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) && at < 5) {
      QLineEdit* next = m_fields[size_t((at + (k->key() == Qt::Key_Backtab ? 4 : 1)) % 5)];
      next->setFocus(Qt::TabFocusReason);
      next->selectAll();
      return true;
    }
    if (k->key() == Qt::Key_Escape) {
      if (line == m_distance) flowBack();
      else fillPanel(true);
      services().viewport()->setFocus();
      return true;
    }
    return false;
  }
  if (object != services().viewport()) return false;
  if (event->type() == QEvent::MouseButtonPress) m_typing = nullptr;  // typed again later: a new value
  if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && m_flow == Flow::None) {
    QTimer::singleShot(0, this, [this] {  // after the view has picked what was clicked
      if (const std::string id = canvasOf(services().selection()); !id.empty() && !services().document()->browse) edit(id);
    });
    return false;
  }
  // Typed values while the panel is open (the DynamicInput contract): digits and Tab never reach the window's shortcuts
  // (filters 1-4, styles 5-7); they go into the first field, or into the real distance while Calibrate waits for it. The
  // shortcut stage claims the key and gives the field the focus (the first key replaces what it shows); the press then
  // reaches the field, or the view when the focus stayed there (an inactive window), which hands it on.
  if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) && m_panel && m_panel->isVisible() && m_editor->active()) {
    auto* k = static_cast<QKeyEvent*>(event);
    const QString text = k->text();
    const bool value = text.size() == 1 && (text[0].isDigit() || QString(".,-+(").contains(text[0]));
    if (event->type() == QEvent::KeyPress) {
      if (m_typed != k->key() || !m_typing) return false;
      m_typed = 0;
      if (value) m_typing->insert(text);
      return true;
    }
    if ((k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) || (!value && k->key() != Qt::Key_Tab)) return false;
    QLineEdit* into = m_flow == Flow::Calibrate && m_points.size() == 2 ? m_distance : m_flow == Flow::None ? m_fields[0] : nullptr;
    if (!into || !into->isEnabled()) return false;
    event->accept();
    if (m_typing != into) {
      into->setFocus(Qt::OtherFocusReason);
      into->selectAll();
      m_typing = into;
    }
    m_typed = k->key();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- insert, edit
void CanvasArea::insert(const QString& given) {
  if (!services().requireEditable([this, given] { insert(given); })) return;
  QString path = given;
  QSettings settings;
  if (path.isEmpty()) {
    path = QFileDialog::getOpenFileName(services().window(), tr("Insert canvas"), settings.value("ui/lastDir").toString(), tr("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
    if (path.isEmpty()) return;
    settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
  }
  if (!pictureFile(path)) return toast(tr("Not a picture OPAD places as a canvas: %1").arg(QFileInfo(path).fileName()));
  const assets::Mode mode = assets::askImport(services().window(), path);
  if (mode == assets::Mode::Cancel) return;
  const bool link = mode == assets::Mode::Link;
  finish();
  AppDocument* doc = services().document();
  if (!doc->hasDocument) return services().importFile(path, link);  // nothing to place it among: a document of its own
  const auto refs = services().viewport()->selection();
  auto pickPlane = [this, path, link] { services().design()->pickSketchPlane([this, path, link](opad::json, opad::Frame frame) { placeOn(path, frame, 0, 0, link); }, false); };
  const opad::Node* body = refs.size() == 1 && refs.front().kind == opad::Ref::Kind::Face ? doc->node(refs.front().body) : nullptr;
  if (!body || body->body_missing) return pickPlane();
  // On the selected face: its plane (the normal outward, x along a world axis where one lies in it), centred on it. The
  // body's shape (cached since the load) and place are taken here; the face is read on a worker.
  auto shape = std::make_shared<const TopoDS_Shape>(opad::body_shape(doc->doc, body->body_key));
  auto frame = std::make_shared<opad::Frame>();
  const opad::Mat4 world = doc->scene.world(body->id);
  const int index = refs.front().index;
  QPointer<CanvasArea> self(this);
  services().jobs()->async(tr("Reading the face"), [shape, world, index, frame](Progress) {
    TopoDS_Shape face = opad::subshape(*shape, opad::Ref::Kind::Face, index);
    if (face.IsNull() || !(world.is_identity() || opad::mat_is_rigid(world))) throw opad::Error("the plane must be a planar face");
    if (!world.is_identity()) face = face.Moved(TopLoc_Location(opad::trsf_from_mat(world)));
    BRepAdaptor_Surface surface(TopoDS::Face(face));
    if (surface.GetType() != GeomAbs_Plane) throw opad::Error("that face is not planar");
    const gp_Ax3 ax = surface.Plane().Position();
    gp_Dir n = ax.Direction();
    if (!ax.Direct()) n.Reverse();
    if (face.Orientation() == TopAbs_REVERSED) n.Reverse();  // outward from the body
    gp_Dir x = ax.XDirection();
    for (const gp_Dir& axis : {gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1)})
      if (std::fabs(axis.Dot(n)) < 1e-6) {
        x = axis;
        break;
      }
    GProp_GProps g;
    BRepGProp::SurfaceProperties(face, g);
    *frame = opad::design::frame_from_ax3(gp_Ax3(g.CentreOfMass(), n, x));
  }, [self, frame, path, link, pickPlane](bool ok, const QString& error) {
    if (!self) return;
    if (ok) return self->placeOn(path, *frame, 0, 0, link);
    self->toast(i18n::t(error));
    pickPlane();
  });
}

void CanvasArea::placeOn(const QString& file, const opad::Frame& plane, double u, double v, bool link) {
  m_placer->placed = [this, file, plane, link](const opad::Mat4& placement) {
    const opad::json options = {{"plane", plane.to_json()}, {"width", m_placer->imageWidth()}, {"center", true}};
    if (link) return services().importPlaced(file, placement, options, link, [this] { afterInsert(); });  // a linked picture: an asset's read
    // A copy: read on a worker without the document (a big model is never copied for it), committed as one undo step.
    opad::ImportOptions o;
    o.placement = placement;
    // Into the active component (UI-33), placed relative to it, as the linked read and every import go (startImport).
    const AppDocument* doc = services().document();
    if (const opad::Node* in = doc->scene.node(doc->activeComponent()); in && in->kind == opad::Node::Kind::Component) {
      o.parent = in->id;
      o.placement = doc->scene.world(in->id).inverse() * placement;
    }
    o.canvas = options;
    o.author = QSettings().value("user/name").toString().trimmed().toStdString();
    auto plan = std::make_shared<opad::design::Plan>();
    const std::filesystem::path path(file.toStdU16String());
    QPointer<CanvasArea> self(this);
    services().jobs()->async(tr("Reading %1").arg(QFileInfo(file).fileName()), [plan, path, o](Progress) { *plan = opad::plan_canvas_import(path, o); },
                             [self, plan](bool ok, const QString& error) {
                               if (!self) return;
                               if (!ok) return self->toast(i18n::t(error));
                               self->commitInsert(plan);
                             });
  };
  m_placer->back = [this, file] { QTimer::singleShot(0, this, [this, file] { insert(file); }); };
  m_placer->start(file, plane, [this](ToolPanel* panel) { services().openPanel(panel); });
  if (u != 0 || v != 0) m_placer->setOffset(u, v);
}

void CanvasArea::commitInsert(std::shared_ptr<opad::design::Plan> plan, int waited) {
  AppDocument* doc = services().document();
  if (doc->designBusy || doc->loading || doc->snapshotBusy()) {  // a change being planned against the document as it is: after it
    if (waited < 300) QTimer::singleShot(100, this, [this, plan, waited] { commitInsert(plan, waited + 1); });
    else toast(tr("The document is busy; try again in a moment."));
    return;
  }
  const std::string canvas = plan->report.value("canvas", "");
  try {
    doc->commitPlan(std::move(*plan), tr("insert canvas"));
  } catch (const std::exception& e) {
    return toast(i18n::t(QString::fromUtf8(e.what())));
  }
  edit(canvas);
}

void CanvasArea::afterInsert() {
  AppDocument* doc = services().document();
  const std::string op = doc->lastLoad.value("op", "");
  for (const auto& id : doc->scene.all_bodies())
    if (const opad::Node* n = doc->scene.node(id); n && n->source_op == op && opad::is_canvas(*n)) return edit(id);
}

void CanvasArea::edit(const std::string& canvas) {
  if (!services().requireEditable([this, canvas] { edit(canvas); })) return;
  const opad::Node* n = services().document()->node(canvas);
  if (!n || !opad::is_canvas(*n)) return;
  endFlow();
  m_editor->start(canvas);
  if (!m_editor->active()) return;
  fillPanel(true);
  services().openPanel(m_panel);
  services().setContextualTab("design.canvas", true);
}

void CanvasArea::finish() {
  endFlow();
  if (m_editor) m_editor->stop();
  if (m_panel && m_panel->isVisible()) m_panel->hide();
  services().setContextualTab("design.canvas", false);
}

// ---------------------------------------------------------------- Calibrate, Align to model
void CanvasArea::calibrate() { startFlow(Flow::Calibrate); }
void CanvasArea::align() { startFlow(Flow::Align); }

void CanvasArea::startFlow(Flow flow) {
  if (!m_editor->active()) return;
  if (m_editor->locked()) return toast(tr("The canvas is locked: unlock it to move it"));
  endFlow();
  m_flow = flow;
  m_points.clear();
  if (flow == Flow::Align) m_editor->vertexFilter(true);
  nextPick();
}

void CanvasArea::applyCalibration() {
  const auto value = units::parse(units::Kind::Length, m_distance->text());
  if (m_flow != Flow::Calibrate || !value || !(*value > 0) || m_points.size() != 2) return toast(tr("Type the real distance between the two points"));
  const auto a = m_points[0], b = m_points[1];
  const bool ok = run({{"action", "calibrate"}, {"points", {a, b}}, {"distance", *value}});
  if (ok) toast(tr("Calibrated: the two points are %1 apart").arg(units::format(units::Kind::Length, *value)));
  endFlow(ok);
}

void CanvasArea::footerForFlow() {
  const bool flow = m_flow != Flow::None, waits = m_flow == Flow::Calibrate && m_points.size() == 2;
  m_main->setVisible(!flow);
  m_flowPage->setVisible(flow);
  m_footer->setBackVisible(flow && !m_points.empty());
  m_footer->setCancelVisible(flow);
  m_footer->setHint(flow ? QString() : tr("Drag it, a corner (Shift: free proportions) or the knob"));
  if (flow) m_footer->setPrimary(PanelFooter::Primary::Stay);
  else m_footer->setPrimary(PanelFooter::Primary::Close);
  m_footer->setPrimaryVisible(!flow || waits);
  const auto value = waits ? units::parse(units::Kind::Length, m_distance->text()) : std::nullopt;
  m_footer->setPrimaryEnabled(!flow || (value && *value > 0));
}

void CanvasArea::nextPick() {
  const size_t n = m_points.size();
  const bool waitsDistance = m_flow == Flow::Calibrate && n == 2;
  m_editor->pick(m_flow == Flow::None || waitsDistance ? CanvasEditor::Pick::None
                 : m_flow == Flow::Align && n % 2 == 1   ? CanvasEditor::Pick::Model
                                                         : CanvasEditor::Pick::Canvas);
  services().viewport()->showPickMarkers(m_points);
  m_distanceRow->setVisible(waitsDistance);
  if (waitsDistance) {
    m_distance->setText(units::editable(units::Kind::Length, distance(m_points[0], m_points[1])));
    m_distance->setFocus(Qt::OtherFocusReason);
    m_distance->selectAll();
  }
  refreshPrompt();
}

void CanvasArea::flowPicked(const opad::Vec3& point, bool model) {
  if (m_flow == Flow::None) return;
  (void)model;
  m_points.push_back(point);
  if (m_flow == Flow::Align && m_points.size() == 4) {
    const auto p = m_points;
    try {
      const opad::json r = services().document()->run("canvas", {{"action", "align"}, {"points", {p[0], p[1], p[2], p[3]}}, {"target", target()}});
      const double residual = r.value("residual", 0.0);
      toast(residual > 1e-3 ? tr("Aligned. The model points lie up to %1 off the canvas's plane").arg(units::format(units::Kind::Length, residual))
                            : tr("Aligned to the model"));
      endFlow(true);
    } catch (const std::exception& e) {
      toast(i18n::t(QString::fromUtf8(e.what())));
      m_points.pop_back();
      nextPick();
    }
    return;
  }
  nextPick();
}

void CanvasArea::flowBack() {
  if (m_flow == Flow::None) return;
  if (m_points.empty()) return endFlow();
  m_points.pop_back();
  nextPick();
}

void CanvasArea::endFlow(bool done) {
  const bool was = m_flow != Flow::None;
  m_flow = Flow::None;
  m_points.clear();
  if (m_editor) m_editor->pick(CanvasEditor::Pick::None);
  if (was) services().viewport()->showPickMarkers({});
  if (m_distanceRow) m_distanceRow->hide();
  if (m_prompt) m_prompt->hide();
  if (m_footer) footerForFlow();
  if (was) {
    services().viewport()->setFocus();
    fillPanel(true);
    emit flowEnded(done);
  }
}

void CanvasArea::refreshPrompt() {
  if (m_flow == Flow::None || !m_prompt) return;
  const size_t n = m_points.size();
  QList<ToolStep> steps;
  QStringList labels = m_flow == Flow::Calibrate ? QStringList{tr("Pick a point on the picture"), tr("Pick a second point"), tr("Type the real distance")}
                                                 : QStringList{tr("Pick a point on the picture"), tr("Pick where it belongs on the model"),
                                                               tr("Pick a second point on the picture"), tr("Pick where that one belongs")};
  auto point = [](const opad::Vec3& p) {
    return QString("%1, %2, %3").arg(units::format(units::Kind::Length, p[0]), units::format(units::Kind::Length, p[1]), units::format(units::Kind::Length, p[2]));
  };
  for (int i = 0; i < labels.size(); ++i) steps << ToolStep{labels[i], size_t(i) < n ? point(m_points[size_t(i)]) : QString()};
  const QString title = m_flow == Flow::Calibrate ? tr("Calibrate canvas") : tr("Align canvas to model");
  m_prompt->set(m_flow == Flow::Calibrate ? "calibrate" : "alignto", title, steps,
                m_flow == Flow::Align && n % 2 == 1 ? tr("A vertex or a circle's centre · Esc steps back") : tr("Esc steps back"));
  m_prompt->adjustSize();
  m_prompt->show();
  // The panel lists the same steps, what each picked, and what it comes to.
  m_steps->setSteps(steps, QString());
  const opad::Node* canvas = services().document()->node(target());
  m_steps->setSummary(title, canvas ? QString::fromStdString(canvas->name) : QString(), tr("Step %1 of %2").arg(std::min<qsizetype>(qsizetype(n) + 1, labels.size())).arg(labels.size()));
  QList<QPair<QString, QString>> rows;
  if (m_flow == Flow::Calibrate && n == 2) {
    const double shown = distance(m_points[0], m_points[1]);
    rows << qMakePair(tr("Apart now"), units::format(units::Kind::Length, shown));
    if (const auto typed = units::parse(units::Kind::Length, m_distance->text()); typed && *typed > 0 && shown > 0)
      rows << qMakePair(tr("Scaled by"), QString::number(*typed / shown, 'g', 6));
  }
  if (m_flow == Flow::Align && n >= 2) rows << qMakePair(tr("First pair apart"), units::format(units::Kind::Length, distance(m_points[0], m_points[1])));
  m_steps->setResult(rows);
  footerForFlow();
  positionOverlays({});
}

// ---------------------------------------------------------------- Trace, Replace, backdrops
void CanvasArea::trace() {
  const std::string id = target();
  const opad::Node* n = id.empty() ? nullptr : services().document()->node(id);
  if (!n || !n->raster.contains("href")) return toast(tr("The canvas's picture is not loaded"));
  if (services().design()->busy() || services().design()->sketchActive()) return toast(tr("Finish what the design is doing first"));
  const opad::CanvasPlace place = opad::canvas_place(services().document()->scene, id);
  const auto flip = opad::CanvasFlags::of(n->canvas).flip;
  auto data = std::make_shared<const std::string>(n->raster["href"].get<std::string>());  // the scene moves on meanwhile
  auto out = std::make_shared<opad::json>();
  const QString name = tr("%1 trace").arg(QString::fromStdString(n->name));
  QPointer<CanvasArea> self(this);
  services().jobs()->async(tr("Tracing %1").arg(QString::fromStdString(n->name)), [data, out, place, flip](Progress progress) {
    QImage image = decodePicture(QByteArray::fromBase64(QByteArray::fromStdString(data->substr(data->find(',') + 1))), 2048).convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) throw opad::Error("the picture could not be decoded");
    if (flip[0] || flip[1]) image = image.flipped((flip[0] ? Qt::Horizontal : Qt::Orientations()) | (flip[1] ? Qt::Vertical : Qt::Orientations()));
    std::vector<unsigned char> grey(size_t(image.width()) * size_t(image.height()));
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x) {
        const QRgb pixel = image.pixel(x, y);
        grey[size_t(y) * size_t(image.width()) + size_t(x)] = static_cast<unsigned char>((qGray(pixel) * qAlpha(pixel) + 255 * (255 - qAlpha(pixel))) / 255);
      }
    const opad::design::TraceOptions options;
    opad::design::Sketch traced = opad::design::trace_bitmap(grey, image.width(), image.height(), options, [progress] { return progress.cancelled(); });
    const double sx = place.width / image.width(), sy = place.height / image.height();  // shown millimetres per pixel
    for (auto& p : traced.points) p.x *= sx, p.y *= sy;
    opad::design::simplify_sketch(traced, std::max(1e-6, options.tolerance * std::min(sx, sy)));
    if (traced.entities.empty()) throw opad::Error("nothing to trace: the picture has no dark shapes");
    *out = traced.to_json();
  }, [self, out, place, name](bool ok, const QString& error) {
    if (!self) return;
    if (!ok) {
      self->toast(i18n::t(error));
      return emit self->traced(false, error);
    }
    // The sketch's plane: the canvas's own, its origin at the picture's lower left corner (where the trace's is).
    const opad::Mat4 world = opad::canvas_world(place);
    const opad::Frame frame{world.apply({0, 0, 0}), unit(world.apply_dir({1, 0, 0})), unit(world.apply_dir({0, 1, 0}))};
    const size_t curves = out->value("entities", opad::json::array()).size();
    self->services().design()->applyOps({opad::design::make_sketch_op(name.toStdString(), {{"frame", frame.to_json()}}, *out)}, tr("trace canvas"),
                                        [self, name, curves](bool ok, const QString& error) {
                                          if (!self) return;
                                          self->toast(ok ? tr("Traced %1 curves into “%2”").arg(curves).arg(name) : i18n::t(error));
                                          emit self->traced(ok, error);
                                        });
  });
}

void CanvasArea::replace(const QString& given) {
  const std::string id = target();
  const opad::Node* n = id.empty() ? nullptr : services().document()->node(id);
  if (!n) return;
  if (n->linked) {  // a linked picture is replaced through its link
    if (auto* assets = services().window()->findChild<AssetsArea*>()) assets->replace(n->source_op);
    return;
  }
  QString path = given;
  if (path.isEmpty()) {
    QSettings settings;
    path = QFileDialog::getOpenFileName(services().window(), tr("Replace canvas picture"), settings.value("ui/lastDir").toString(), tr("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
    if (path.isEmpty()) return;
    settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
  }
  const std::filesystem::path file(path.toStdU16String());
  planned(tr("Replacing %1").arg(QString::fromStdString(n->name)), tr("replace picture"), [id, file](opad::Document& doc) { return opad::plan_canvas_replace(doc, id, file); },
          [this, path](bool ok, const QString& error, const opad::json&) {
            toast(ok ? tr("The canvas shows %1").arg(QFileInfo(path).fileName()) : i18n::t(error));
          });
}

void CanvasArea::fromBackdrop(const std::string& sketch) {
  if (!services().requireEditable([this, sketch] { fromBackdrop(sketch); })) return;
  if (services().design()->sketchActive()) return toast(tr("Finish the sketch first"));
  planned(tr("Backdrop images to canvases"), tr("backdrop to canvas"), [sketch](opad::Document& doc) { return opad::plan_canvas_from_backdrop(doc, sketch); },
          [this](bool ok, const QString& error, const opad::json& report) {
            toast(ok ? tr("%n canvas(es) made from the sketch's backdrop", nullptr, int(report.value("canvases", opad::json::array()).size())) : i18n::t(error));
          });
}

void CanvasArea::planned(const QString& title, const QString& label, std::function<opad::design::Plan(opad::Document&)> plan,
                         std::function<void(bool, const QString&, const opad::json&)> then) {
  AppDocument* doc = services().document();
  if (m_busy || !doc->hasDocument || doc->browse || doc->loading) return then(false, tr("The document is busy; try again in a moment."), {});
  m_busy = true;
  QPointer<CanvasArea> self(this);
  auto fail = [self, then](const QString& error) {
    if (!self) return;
    self->m_busy = false;
    then(false, error, {});
    emit self->planDone(false, error);
  };
  const auto generation = doc->generation;
  const bool started = doc->captureSnapshot(services().jobs(), [self, doc, title, label, plan, then, fail, generation](std::shared_ptr<opad::Document> copy, const QString& error) {
    if (!self) return;
    if (!copy) return fail(error);
    if (generation != doc->generation) return fail(tr("The document changed meanwhile; try again."));
    doc->designBusy = true;
    emit doc->undoChanged();
    auto result = std::make_shared<opad::design::Plan>();
    auto reading = std::make_shared<std::atomic<bool>>(true);
    self->services().jobs()->async(title, [copy, result, plan, reading](Progress) {
      struct Done {
        std::shared_ptr<std::atomic<bool>> flag;
        ~Done() { *flag = false; }
      } done{reading};
      *result = plan(*copy);
    }, [self, doc, result, reading, generation, label, then, fail](bool ok, const QString& error) {
      if (!self) return;
      afterWorker(self, reading, [self, doc, result, generation, label, then, fail, ok, error] {
        if (!self) return;
        doc->designBusy = false;
        emit doc->undoChanged();
        if (generation != doc->generation) return fail(tr("The document changed meanwhile; try again."));
        if (!ok) return fail(i18n::t(error));
        self->m_busy = false;
        try {
          const opad::json report = doc->commitPlan(std::move(*result), label);
          then(true, {}, report);
          emit self->planDone(true, {});
        } catch (const std::exception& e) {
          then(false, i18n::t(QString::fromUtf8(e.what())), {});
          emit self->planDone(false, QString::fromUtf8(e.what()));
        }
      });
    });
  });
  if (!started) fail(tr("The document is busy; try again in a moment."));
}

OPAD_AREA(CanvasArea)
