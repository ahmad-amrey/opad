#include "SimulateArea.hpp"

#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <SelectMgr_Selection.hxx>

#include <QAction>
#include <QEventLoop>
#include <QInputDialog>
#include <QLinearGradient>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "AppDocument.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "SimPlot.hpp"
#include "SimulateGuide.hpp"
#include "SimulatePanel.hpp"
#include "SimulatePrint.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Tracking.hpp"
#include "ViewOverlay.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"
#include "opad/sim/fea.hpp"
#include "opad/sim/joints.hpp"

OPAD_ICON_TABLE(simulate,
                {"simulate", R"(<circle cx="7" cy="16" r="4"/><circle cx="17" cy="8" r="4"/><path d="M10 13.5l4-3"/><path d="M3 21h18" opacity=".55"/>)"},
                {"simJoint", R"(<circle cx="12" cy="12" r="3"/><path d="M4 12h5M15 12h5"/><path d="M12 3a9 9 0 0 1 8 5" stroke-dasharray="2 2"/><path d="M20 8l.5-3.5M20 8l-3.5-.5"/>)"},
                {"simGear", R"(<circle cx="8" cy="12" r="4"/><circle cx="17.5" cy="12" r="3"/><path d="M8 5.5v2M8 16.5v2M1.5 12h2M12.5 12h.5M17.5 7.5v1.5M17.5 15v1.5M21.5 12H21" />)"},
                {"simMotion", R"(<path d="M3 18c4 0 5-12 9-12s5 12 9 12"/><circle cx="12" cy="6" r="1.5" fill="currentColor"/>)"},
                {"simPrint", R"(<path d="M8 3h8v5l-2.5 3h-3L8 8z"/><path d="M12 11v3"/><path d="M5 17h14M3 21h18" opacity=".55"/>)"},
                {"simGuide", R"(<path d="M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2z"/><path d="M4 21V5"/><path d="M8 8h7M8 12h5"/>)"},
                {"simDynamic", R"(<rect x="8" y="3" width="8" height="8" rx="1"/><path d="M12 11v9M8.5 16.5L12 20l3.5-3.5"/><path d="M4 21h16" opacity=".55"/>)"},
                {"simStatic", R"(<path d="M3 17h15v-4H3z"/><path d="M3 21V9" /><path d="M19 4v7M16.5 8.5L19 11l2.5-2.5"/>)"},
                {"simModal", R"(<path d="M3 12c2-6 4-6 6 0s4 6 6 0 4-6 6 0"/><path d="M3 12h18" opacity=".45" stroke-dasharray="2 2"/>)"},
                {"simRun", R"(<path d="M7 4l12 8-12 8z"/>)"},
                {"simFixed", R"(<path d="M4 6h16"/><path d="M6 6l-2 4M10 6l-2 4M14 6l-2 4M18 6l-2 4"/><rect x="8" y="10" width="8" height="10" rx="1"/>)"},
                {"simForce", R"(<rect x="3" y="14" width="18" height="6" rx="1"/><path d="M12 3v9M8.5 8.5L12 12l3.5-3.5"/>)"},
                {"simPressure", R"(<rect x="3" y="15" width="18" height="5" rx="1"/><path d="M6 4v8M12 4v8M18 4v8M4.5 10L6 12l1.5-2M10.5 10l1.5 2 1.5-2M16.5 10l1.5 2 1.5-2"/>)"},
                {"simBolt", R"(<path d="M8 3h8v4H8z"/><path d="M10 7v14h4V7"/><path d="M10 11h4M10 14h4M10 17h4" opacity=".55"/><path d="M4 9v6M20 9v6M3 12h2M19 12h2"/>)"},
                {"simGravity", R"(<path d="M12 3v14M7.5 12.5L12 17l4.5-4.5"/><path d="M5 21h14"/>)"},
                {"simResults", R"(<rect x="3" y="4" width="14" height="16" rx="1"/><path d="M20 4v16" stroke-width="3" opacity=".55"/><path d="M6 15c2-4 5-6 8-7" />)"},
                {"simStudy", R"(<path d="M4 20V4"/><path d="M4 20h16"/><path d="M7 15l4-5 3 3 5-7"/>)"},
                {"simLoad", R"(<path d="M12 3v11M8 10l4 4 4-4"/><rect x="4" y="16" width="16" height="4" rx="1"/>)"});

namespace {
using opad::json;

// ---------------------------------------------------------------- the result map in the view
// The study's mesh skin, each corner coloured by the field (opad::result_color, the legend's scale), moved by the
// displacement or mode shape times a scale that makes it visible. Drawn instead of the bodies it covers.
class ResultMap : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(ResultMap, AIS_InteractiveObject)
 public:
  std::vector<opad::Vec3> points;              // deformed node positions
  std::vector<std::array<float, 3>> colours;   // per node
  std::vector<std::array<int, 3>> triangles;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    if (triangles.empty()) return;
    Handle(Graphic3d_ArrayOfTriangles) tris =
        new Graphic3d_ArrayOfTriangles(int(3 * triangles.size()), 0, Graphic3d_ArrayFlags_VertexNormal | Graphic3d_ArrayFlags_VertexColor);
    for (const auto& t : triangles) {
      const opad::Vec3 &a = points[size_t(t[0])], &b = points[size_t(t[1])], &c = points[size_t(t[2])];
      gp_Vec n = gp_Vec(b[0] - a[0], b[1] - a[1], b[2] - a[2]).Crossed(gp_Vec(c[0] - a[0], c[1] - a[1], c[2] - a[2]));
      if (n.Magnitude() < 1e-12) n = gp_Vec(0, 0, 1);
      n.Normalize();
      for (int k = 0; k < 3; ++k) {
        const opad::Vec3& p = points[size_t(t[size_t(k)])];
        const auto& col = colours[size_t(t[size_t(k)])];
        tris->AddVertex(gp_Pnt(p[0], p[1], p[2]), gp_Dir(n), Quantity_Color(col[0], col[1], col[2], Quantity_TOC_sRGB));
      }
    }
    Handle(Graphic3d_AspectFillArea3d) aspect = new Graphic3d_AspectFillArea3d();
    aspect->SetInteriorStyle(Aspect_IS_SOLID);
    aspect->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // the colours are the legend's, not lit
    aspect->SetFaceCulling(Graphic3d_TypeOfBackfacingModel_DoubleSided);
    const Handle(Graphic3d_Group) group = prs->NewGroup();
    group->SetGroupPrimitivesAspect(aspect);
    group->AddPrimitiveArray(tris);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}
};

QString number(double v) { return QString::number(std::fabs(v) < 1e-9 ? 0.0 : v, 'g', std::fabs(v) >= 1000 ? 6 : 4); }

QString kindLabel(const std::string& kind) {
  if (kind == "motion") return Simulate::tr("Motion");
  if (kind == "dynamic") return Simulate::tr("Dynamic");
  if (kind == "static") return Simulate::tr("Static stress");
  if (kind == "modal") return Simulate::tr("Vibration modes");
  return QString::fromStdString(kind);
}

QString iconForJoint(const std::string& kind) { return opad::sim::is_relation(kind) ? "simGear" : "simJoint"; }

QString iconForStudy(const std::string& kind) {
  if (kind == "motion") return "simMotion";
  if (kind == "dynamic") return "simDynamic";
  if (kind == "modal") return "simModal";
  return "simStatic";
}

QString iconForLoad(const std::string& kind) {
  if (kind == "fixed" || kind == "displacement") return "simFixed";
  if (kind == "pressure") return "simPressure";
  if (kind == "bolt_preload") return "simBolt";
  if (kind == "gravity") return "simGravity";
  return "simForce";
}

// How a printed body fails ("between layers (wall)", sim/printing.hpp) in the user's words.
QString failsText(const std::string& fails) {
  static const std::map<std::string, const char*> words = {
      {"along the roads", QT_TRANSLATE_NOOP("Simulate", "along the roads")}, {"across the roads", QT_TRANSLATE_NOOP("Simulate", "across the roads")},
      {"between layers", QT_TRANSLATE_NOOP("Simulate", "between layers")},   {"shear", QT_TRANSLATE_NOOP("Simulate", "in shear")},
      {"in compression", QT_TRANSLATE_NOOP("Simulate", "in compression")},
      {"wall", QT_TRANSLATE_NOOP("Simulate", "walls")},                       {"top/bottom", QT_TRANSLATE_NOOP("Simulate", "top/bottom skin")},
      {"infill", QT_TRANSLATE_NOOP("Simulate", "infill")}};
  auto word = [&](const std::string& w) {
    const auto it = words.find(w);
    return it == words.end() ? QString::fromStdString(w) : Simulate::tr(it->second);
  };
  const size_t open = fails.find(" (");
  if (open == std::string::npos || fails.back() != ')') return word(fails);
  return Simulate::tr("%1, in the %2").arg(word(fails.substr(0, open)), word(fails.substr(open + 2, fails.size() - open - 3)));
}

std::string stateOf(const AppDocument* doc) { return std::to_string(doc->doc.ops.size()) + (doc->doc.ops.empty() ? "" : doc->doc.ops.back().id); }

// A study's summary in a few lines for the panel.
QString summaryText(const json& s) {
  QStringList lines;
  auto add = [&](const QString& label, const json& v, const QString& unit) {
    if (v.is_number()) lines << label + " " + number(v.get<double>()) + (unit.isEmpty() ? "" : " " + unit);
  };
  add(Simulate::tr("Peak von Mises"), s.value("max_von_mises_MPa", json()), "MPa");
  add(Simulate::tr("Largest displacement"), s.value("max_displacement_mm", json()), "mm");
  if (const json f = s.value("frequencies_Hz", json()); f.is_array() && !f.empty()) {
    QStringList fs;
    for (const auto& x : f) fs << number(x.get<double>());
    lines << Simulate::tr("Frequencies %1 Hz").arg(fs.join(", "));
  }
  if (const json b = s.value("bodies", json()); b.is_object())
    for (const auto& [name, v] : b.items())
      if (v.is_object() && v.contains("safety_factor") && v["safety_factor"].is_number())
        lines << Simulate::tr("%1: safety factor %2").arg(QString::fromStdString(name), number(v["safety_factor"].get<double>()));
  if (const json b = s.value("bolts", json()); b.is_array())
    for (const auto& v : b)
      if (v.is_object() && v.contains("axial_stress_MPa"))
        lines << Simulate::tr("%1: shank stress %2 MPa (preload / area %3 MPa)")
                     .arg(QString::fromStdString(v.value("bolt", "")), number(v["axial_stress_MPa"].get<double>()), number(v.value("nominal_stress_MPa", 0.0)));
  if (const json p = s.value("print", json()); p.is_object())
    for (const auto& [name, v] : p.items()) {
      if (!v.is_object()) continue;
      QString line = Simulate::tr("%1 printed: %2 g of plastic").arg(QString::fromStdString(name), number(v.value("printed_mass_g", 0.0)));
      if (v.contains("fails") && v["fails"].is_string())
        line += "; " + Simulate::tr("weakest %1, layer %2").arg(failsText(v["fails"].get<std::string>())).arg(v.value("weakest_layer", 0));
      lines << line;
    }
  if (const json o = s.value("outputs", json()); o.is_object()) {
    int n = 0;
    for (const auto& [name, v] : o.items()) {
      if (!v.is_object() || ++n > 6) continue;
      const QString unit = QString::fromStdString(v.value("unit", ""));
      if (v.contains("max") && v.contains("min"))
        lines << QString::fromStdString(name) + " " + number(v["min"].get<double>()) + " … " + number(v["max"].get<double>()) + " " + unit;
      else if (v.contains("max"))
        lines << QString::fromStdString(name) + " " + Simulate::tr("up to") + " " + number(v["max"].get<double>()) + " " + unit;
    }
  }
  if (const json w = s.value("warnings", json()); w.is_array())
    for (const auto& x : w) lines << "⚠ " + QString::fromStdString(x.get<std::string>());
  return lines.join("\n");
}
}  // namespace

// ---------------------------------------------------------------- the legend over the view
class SimLegend : public QWidget {
 public:
  explicit SimLegend(QWidget* parent) : QWidget(parent) {
    setObjectName("simLegend");
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(150, 220);
    hide();
  }
  void setScale(const QString& title, double lo, double hi, const QString& unit, const QString& note) {
    m_title = title, m_lo = lo, m_hi = hi, m_unit = unit, m_note = note;
    update();
  }
  QString title() const { return m_title; }

 protected:
  void paintEvent(QPaintEvent*) override {
    const Tokens& k = theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(k.line, 1));
    p.setBrush(k.bg2);
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);
    const QFontMetrics fm = fontMetrics();
    p.setPen(k.fg);
    p.drawText(QRect(8, 6, width() - 16, fm.height()), Qt::AlignLeft, fm.elidedText(m_title, Qt::ElideRight, width() - 16));
    const QRect bar(10, 14 + 2 * fm.height(), 14, height() - 30 - 3 * fm.height());  // its end labels clear of the title
    QLinearGradient g(bar.bottomLeft(), bar.topLeft());
    for (int i = 0; i <= 10; ++i) {
      const auto c = opad::result_color(i / 10.0);
      g.setColorAt(i / 10.0, QColor::fromRgbF(c[0], c[1], c[2]));
    }
    p.fillRect(bar, g);
    p.setPen(k.fg2);
    for (int i = 0; i <= 4; ++i) {
      const double v = m_lo + (m_hi - m_lo) * i / 4.0;
      const int y = bar.bottom() - int(std::lround(bar.height() * i / 4.0));
      p.drawLine(bar.right() + 1, y, bar.right() + 4, y);
      p.drawText(QRect(bar.right() + 8, y - fm.height() / 2, width() - bar.right() - 12, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                 number(v) + " " + m_unit);
    }
    p.setPen(k.fg3);
    p.drawText(QRect(8, height() - 6 - fm.height(), width() - 16, fm.height()), Qt::AlignLeft, fm.elidedText(m_note, Qt::ElideRight, width() - 16));
  }

 private:
  QString m_title, m_unit, m_note;
  double m_lo = 0, m_hi = 1;
};

// ---------------------------------------------------------------- the area
Simulate::Simulate(AreaServices& services) : AreaController(services) {
  m_tick.setInterval(16);
  m_tick.setTimerType(Qt::PreciseTimer);
  connect(&m_tick, &QTimer::timeout, this, &Simulate::tick);
}

Simulate::~Simulate() = default;

void Simulate::buildActions() {
  const QPointer<Simulate> self(this);
  const auto doc = [](const CommandContext& c) { return c.document && !c.viewer; };
  const auto add = [&](const char* id, const QString& label, const char* icon, std::function<void()> fn, std::function<bool(const CommandContext&)> when,
                       QStringList keywords, bool checkable = false) {
    CommandInfo info;
    info.id = id;
    info.label = label;
    info.icon = icon;
    info.group = tr("Simulate");
    info.keywords = std::move(keywords);
    info.workspaces = {"simulate"};
    info.editsDocument = true;
    info.checkable = checkable;
    info.enabledWhen = std::move(when);
    return services().addCommand(info, [self, fn] {
      if (self) self->services().guarded(fn);
    });
  };
  add("simulate.panel", tr("Simulation panel"), "simulate", [this] { open(); }, doc, {"mechanism", "joints", "studies", "physics"});
  {
    CommandInfo info;  // any time, any workspace: it is how to start
    info.id = "simulate.guide";
    info.label = tr("Simulation guide");
    info.icon = "simGuide";
    info.group = tr("Simulate");
    info.keywords = {"how to", "tutorial", "step by step", "use cases", "learn", "simulation", "FEA", "mechanism"};
    services().addCommand(info, [self] {
      if (self) self->openGuide(0);
    });
  }
  add("simulate.joint", tr("Joint"), "simJoint", [this] {
        open();
        addJoint(m_form->jointKind());
      },
      doc, {"revolute", "hinge", "slider", "prismatic", "ball", "constraint", "mate", "gear", "rack", "lead screw"});
  add("simulate.motion", tr("Motion study"), "simMotion", [this] { newStudy("motion"); }, doc, {"kinematics", "animate", "drive", "trace"});
  add("simulate.dynamic", tr("Dynamic study"), "simDynamic", [this] { newStudy("dynamic"); }, doc,
      {"gravity", "physics", "chrono", "torque", "motor", "contact", "friction", "spring"});
  add("simulate.static", tr("Static stress study"), "simStatic", [this] { newStudy("static"); }, doc,
      {"FEA", "finite element", "von Mises", "stress", "deflection", "calculix", "safety factor"});
  add("simulate.modal", tr("Vibration modes study"), "simModal", [this] { newStudy("modal"); }, doc,
      {"frequency", "natural frequency", "eigen", "resonance", "modal analysis"});
  add("simulate.print", tr("Printed part"), "simPrint", [this] { printSettings(); }, [self, doc](const CommandContext& c) { return doc(c) && self && !self->m_job; },
      {"3D print", "FDM", "FFF", "infill", "layers", "walls", "slicer", "filament", "PLA", "PETG", "anisotropic"});
  add("simulate.run", tr("Run study"), "simRun", [this] { runStudy(); },
      [self, doc](const CommandContext& c) { return doc(c) && self && !self->m_study.empty() && !self->m_job; }, {"solve", "compute", "simulate"});
  add("simulate.play", tr("Play"), "explodePlay", [this] { play(); },
      [self](const CommandContext& c) { return c.document && self && self->m_run && self->m_run->t.size() > 1 && !self->m_run->fea; },
      {"animate", "playback", "pause"});
  add("simulate.fixed", tr("Fixed support"), "simFixed", [this] { addLoad("fixed"); }, doc, {"restraint", "clamp", "boundary condition", "support"});
  add("simulate.force", tr("Force"), "simForce", [this] { addLoad("force"); }, doc, {"load", "newton", "push"});
  add("simulate.pressure", tr("Pressure"), "simPressure", [this] { addLoad("pressure"); }, doc, {"load", "MPa", "distributed"});
  add("simulate.boltPreload", tr("Bolt preload"), "simBolt", [this] { addLoad("bolt_preload"); }, doc, {"pretension", "fastener", "tightening", "clamp force"});
  add("simulate.gravity", tr("Gravity load"), "simGravity", [this] { addLoad("gravity"); }, doc, {"self weight", "acceleration", "weight"});
  add("simulate.results", tr("Result map"), "simResults", [this] { showResults(!resultShown()); },
      [self](const CommandContext& c) { return c.document && self && self->m_run && self->m_run->fea; }, {"contour", "stress map", "colour map", "deformed"}, true);
}

void Simulate::ribbon(RibbonLayout& layout) {
  layout.addWorkspace("simulate", {tr("Simulate"), "simulate", "Ctrl+5",
                                   tr("Joints and mechanisms, motion and dynamics with Project Chrono, stress and vibration with Netgen and CalculiX."),
                                   tr("ops: joint · pose · load · study")});
  using Size = RibbonLayout::Size;
  const auto add = [&](const QString& group, const char* id, Size size = Size::Large) {
    if (QAction* a = services().action(id)) layout.addAction(group, a, size);
  };
  layout.addTab("simulate", "simulate.mechanism", tr("Mechanism"));
  layout.addGroup("simulate.mechanism", "simulate.mechanism.joints", tr("Joints"));
  add("simulate.mechanism.joints", "simulate.joint");
  add("simulate.mechanism.joints", "simulate.panel");
  layout.addGroup("simulate.mechanism", "simulate.mechanism.studies", tr("Studies"));
  add("simulate.mechanism.studies", "simulate.motion");
  add("simulate.mechanism.studies", "simulate.dynamic");
  layout.addGroup("simulate.mechanism", "simulate.mechanism.results", tr("Results"));
  add("simulate.mechanism.results", "simulate.run");
  add("simulate.mechanism.results", "simulate.play");
  layout.addGroup("simulate.mechanism", "simulate.mechanism.help", tr("Help"));
  add("simulate.mechanism.help", "simulate.guide");
  layout.addTab("simulate", "simulate.structure", tr("Structure"));
  layout.addGroup("simulate.structure", "simulate.structure.loads", tr("Loads and supports"));
  add("simulate.structure.loads", "simulate.fixed");
  add("simulate.structure.loads", "simulate.force");
  for (const char* id : {"simulate.pressure", "simulate.boltPreload", "simulate.gravity"}) add("simulate.structure.loads", id, Size::Small);
  layout.addGroup("simulate.structure", "simulate.structure.studies", tr("Studies"));
  add("simulate.structure.studies", "simulate.static");
  add("simulate.structure.studies", "simulate.modal");
  add("simulate.structure.studies", "simulate.print");
  layout.addGroup("simulate.structure", "simulate.structure.results", tr("Results"));
  add("simulate.structure.results", "simulate.run");
  add("simulate.structure.results", "simulate.results");
  layout.addGroup("simulate.structure", "simulate.structure.help", tr("Help"));
  add("simulate.structure.help", "simulate.guide");
}

void Simulate::ready() {
  m_form = new SimulatePanel;
  m_panel = new ToolPanel("simulate", "simulate", &Tokens::sel, tr("Simulation"), m_form, 760, services().window());
  m_panel->setDefaultWidth(400);
  m_panel->setObjectName("simulateToolPanel");
  m_panel->setHelpId("simulate.panel");
  auto escape = [this] {
    if (playing()) pause();
    else m_panel->hide();
  };
  m_panel->setEscapeHandler(escape);
  services().addPanel(m_panel);
  connect(m_panel, &ToolPanel::visibilityChanged, this, [this](bool shown) {
    if (shown) refresh();
    else {
      pause();
      clearMotion();
    }
  });
  connect(m_form->footer(), &PanelFooter::cancelled, this, escape);
  connect(m_form, &SimulatePanel::jointChosen, this, [this](const QString& id) { chooseJoint(id.toStdString()); });
  connect(m_form, &SimulatePanel::jointValueChanging, this, &Simulate::previewJoint);
  connect(m_form, &SimulatePanel::jointValueChosen, this, [this](double v) { services().guarded([&] { setJoint(v); }); });
  connect(m_form, &SimulatePanel::addJointRequested, this, [this](const QString& kind) { services().guarded([&] { addJoint(kind); }); });
  connect(m_form, &SimulatePanel::studyChosen, this, [this](const QString& id) { chooseStudy(id.toStdString()); });
  connect(m_form, &SimulatePanel::newStudyRequested, this, [this](const QString& kind) { services().guarded([&] { newStudy(kind); }); });
  connect(m_form, &SimulatePanel::runRequested, this, [this] { services().guarded([&] { runStudy(); }); });
  connect(m_form, &SimulatePanel::guideRequested, this, [this] {
    // Where the document is: nothing yet, the hinge; joints, their motion; loads, the stress study.
    const opad::Scene& s = services().document()->scene;
    openGuide(!s.loads.empty() ? 4 : !s.joints.empty() ? 1 : 0);
  });
  connect(m_form, &SimulatePanel::playRequested, this, &Simulate::play);
  connect(m_form, &SimulatePanel::frameChosen, this, [this](int f) {
    pause();
    showFrame(f);
  });
  connect(m_form, &SimulatePanel::seriesChosen, this, &Simulate::showSeries);
  connect(m_form, &SimulatePanel::fieldChosen, this, [this](const QString& field) {
    m_field = field;
    if (resultShown()) showResults(true);
  });
  connect(m_form, &SimulatePanel::resultsToggled, this, [this](bool on) { services().guarded([&] { showResults(on); }); });
  m_legend = new SimLegend(services().viewport());
  browser::Folder folder;
  folder.id = "simulation";
  folder.title = tr("Simulation");
  folder.icon = "simulate";
  folder.startsClosed = true;  // the parts stay the browser's first rows
  folder.items = [this] { return folderItems(); };
  folder.contextMenu = [this](const std::string& id, QMenu& menu) { folderMenu(id, menu); };
  folder.activated = [this](const std::string& id) {
    services().guarded([&] {
      open();
      if (id.rfind("sim:j:", 0) == 0) chooseJoint(id.substr(6));
      else if (id.rfind("sim:s:", 0) == 0) {
        chooseStudy(id.substr(6));
        if (!m_run) runStudy();
      }
    });
  };
  services().browser()->addFolder(folder);
}

void Simulate::workspaceChanged(const QString& id) {
  if (id != "simulate") {
    pause();
    if (m_panel && !m_panel->pinned()) m_panel->hide();
  }
}

void Simulate::positionOverlays(const QRect&) {
  if (!m_legend || m_legend->isHidden()) return;
  const QWidget* view = services().viewport();
  m_legend->move(16, std::max(8, view->height() - m_legend->height() - 56));
  m_legend->raise();
}

void Simulate::selectionChanged(const SelectionContext& selection) {
  // Rows of the Simulation folder picked in the browser, in the order picked: the joints a relation couples.
  std::vector<std::string> now;
  for (const auto& id : selection.ids)
    if (id.rfind("sim:", 0) == 0) now.push_back(id);
  std::vector<std::string> kept;
  for (const auto& id : m_picked)
    if (std::find(now.begin(), now.end(), id) != now.end()) kept.push_back(id);
  for (const auto& id : now)
    if (std::find(kept.begin(), kept.end(), id) == kept.end()) kept.push_back(id);
  m_picked = kept;
  if (m_panel && m_panel->isVisible() && now.size() == 1 && now[0].rfind("sim:j:", 0) == 0) chooseJoint(now[0].substr(6));
}

void Simulate::documentChanged(bool replaced) {
  if (replaced) {
    pause();
    clearMotion();
    showResults(false);
    m_run.reset();
    m_study.clear();
    m_joint.clear();
    m_picked.clear();
  }
  m_mech.reset();
  if (m_run && stateOf(services().document()) != m_runState) {
    // The model changed under the results: they stay shown until a run or another study, but the motion goes.
    pause();
    clearMotion();
  }
  if (m_panel && m_panel->isVisible()) refresh();
}

// A write waits for what reads the document now (a recovery snapshot, a design being recomputed: a few hundred ms)
// instead of failing with "try again in a moment"; after 10 s it goes ahead and fails as it would.
opad::json Simulate::write(const std::string& command, const opad::json& args, const QString& label) {
  AppDocument* doc = services().document();
  QElapsedTimer clock;
  clock.start();
  while ((doc->designBusy || doc->loading || doc->snapshotBusy() || doc->converting()) && clock.elapsed() < 10000) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return doc->run(command, args, label);
}

void Simulate::open() {
  services().openPanel(m_panel);
  refresh();
}

std::vector<std::string> Simulate::pickedJoints() const {
  std::vector<std::string> out;
  for (const auto& id : m_picked)
    if (id.rfind("sim:j:", 0) == 0) out.push_back(id.substr(6));
  return out;
}

// ---------------------------------------------------------------- the mechanism
void Simulate::refresh() {
  if (!m_form) return;
  AppDocument* doc = services().document();
  const opad::Scene& s = doc->scene;
  SimulatePanel::Entries joints;
  for (const auto& j : s.joints) {
    const opad::sim::JointKind* k = opad::sim::joint_kind(j.kind);
    if (!k || k->relation || k->coords.empty()) continue;
    joints.push_back({QString::fromStdString(j.id), QString::fromStdString(j.name) + "  ·  " + i18n::t(QString::fromStdString(j.kind))});
  }
  if (m_joint.empty() || !s.joint(m_joint)) m_joint = joints.empty() ? std::string() : joints.front().first.toStdString();
  m_form->setJoints(joints, QString::fromStdString(m_joint));
  if (s.joints.empty()) {
    m_form->setMechanism(tr("No joints yet: pick a circular edge or a face of a part and press Add, or Joint on the ribbon."));
  } else {
    try {
      if (!m_mech) m_mech = std::make_unique<opad::sim::Mechanism>(s);
      const json a = m_mech->analysis();
      QString text = tr("%n joint(s), %1 degree(s) of freedom", "", int(s.joints.size())).arg(a.value("dof", 0));
      if (a.value("redundant", 0) > 0) text += " · " + tr("%n redundant equation(s)", "", a.value("redundant", 0));
      for (const auto& p : a.value("problems", json::array())) text += "\n⚠ " + QString::fromStdString(p.get<std::string>());
      m_form->setMechanism(text);
    } catch (const std::exception& e) {
      m_form->setMechanism("⚠ " + QString::fromStdString(e.what()));
    }
  }
  chooseJoint(m_joint);
  SimulatePanel::Entries studies;
  for (const auto& st : s.studies) studies.push_back({QString::fromStdString(st.id), QString::fromStdString(st.name) + "  ·  " + kindLabel(st.kind)});
  if (!m_study.empty() && !s.study(m_study)) {
    m_study.clear();
    m_run.reset();
    showResults(false);
  }
  if (m_study.empty() && !studies.empty()) m_study = studies.back().first.toStdString();
  m_form->setStudies(studies, QString::fromStdString(m_study));
  showRun();
  services().updateCommands();
}

void Simulate::chooseJoint(const std::string& id) {
  m_joint = id;
  const opad::Joint* j = services().document()->scene.joint(id);
  const opad::sim::JointKind* k = j ? opad::sim::joint_kind(j->kind) : nullptr;
  if (!j || !k || k->relation || k->coords.empty() || j->values.empty()) {
    m_form->setJointValue(false, 0, 1, 0, {});
    return;
  }
  const bool angle = k->coords[0].angle;
  const double v = j->values[0];
  double lo = angle ? -360 : -100, hi = angle ? 360 : 100;
  if (const json lim = j->def.value("limits", json::object()); lim.is_object())
    if (const json r = lim.value(k->coords[0].name, json()); r.is_array() && r.size() == 2) lo = r[0].get<double>(), hi = r[1].get<double>();
  lo = std::min(lo, v), hi = std::max(hi, v);
  m_form->setJointValue(!j->def.value("locked", false), lo, hi, v, angle ? QStringLiteral("°") : QStringLiteral("mm"));
}

void Simulate::previewJoint(double value) {
  if (m_joint.empty()) return;
  const opad::Scene& s = services().document()->scene;
  try {
    if (!m_mech) m_mech = std::make_unique<opad::sim::Mechanism>(s);
    opad::sim::Mechanism m = *m_mech;
    const opad::Joint* j = s.joint(m_joint);
    std::vector<double> v(j ? j->values.size() : 1, NAN);
    v[0] = value;
    m.drive({{m_joint, v}});
    std::vector<std::pair<std::string, gp_Trsf>> motion;
    for (const auto& [part, world] : m.part_worlds()) {
      const opad::Mat4 delta = world * s.world(part).inverse();
      const gp_Trsf t = opad::trsf_from_mat(delta);
      for (const auto& b : s.bodies_under(part)) motion.push_back({b, t});
    }
    services().viewport()->setPreviewMotion(motion);
  } catch (const std::exception&) {
  }
}

void Simulate::setJoint(double value) {
  if (m_joint.empty()) return;
  if (!services().requireEditable()) return;
  AppDocument* doc = services().document();
  const opad::Joint* j = doc->scene.joint(m_joint);
  if (!j || j->values.empty() || std::fabs(j->values[0] - value) < 1e-9) {
    clearMotion();
    return;
  }
  std::vector<double> v(j->values.begin(), j->values.end());
  v[0] = value;
  const std::string name = j->name;
  clearMotion();
  const json out = write("joint_set", {{"values", {{m_joint, v}}}}, tr("Move %1").arg(QString::fromStdString(name)));
  for (const auto& n : out.value("notes", json::array())) services().toast(QString::fromStdString(n.get<std::string>()));
}

void Simulate::addJoint(const QString& kind) {
  if (!services().requireEditable()) return;
  const std::string k = kind.toStdString();
  json args = {{"kind", k}};
  if (opad::sim::is_relation(k)) {
    const auto js = pickedJoints();
    if (js.size() != 2) throw opad::UserHint("Pick the two joints it couples in the browser's Simulation folder (Ctrl+click the second).");
    args["joints"] = js;
    const double v = m_form->relationValue();
    if (k == "gear") args["ratio"] = v == 0 ? -1.0 : v;
    else if (k == "rack_pinion") args["radius"] = v;
    else args["lead"] = v;
  } else {
    const SelectionContext sel = services().selection();
    std::vector<opad::Ref> refs;
    for (const auto& r : sel.refs)
      if (!r.body.empty()) refs.push_back(r);
    if (refs.empty())
      wantPicks("select.edges", "Click where the joint is (the Edges filter is on now): a circular edge of the part that moves, then the part it is fixed to, and Add again.");
    args["part"] = refs[0].body;
    args["at"] = refs[0].kind == opad::Ref::Kind::Body ? json{{"origin", {0, 0, 0}}, {"z", {0, 0, 1}}} : refs[0].to_json();
    if (k != "ground" && refs.size() > 1 && refs[1].body != refs[0].body) args["base"] = refs[1].body;
    else if (k != "ground") args["base"] = nullptr;
    if (k == "screw") args["pitch"] = m_form->relationValue() == 0 ? 1.0 : m_form->relationValue();
  }
  const json out = write("joint", args, tr("Add joint"));
  m_joint = opad::sim::is_relation(k) ? m_joint : out.value("id", "");
  services().toast(tr("%1 added: the mechanism has %2 degree(s) of freedom").arg(QString::fromStdString(out.value("name", "")), QString::number(out.value("dof", 0))));
  if (m_panel->isVisible()) refresh();
}

// ---------------------------------------------------------------- studies
void Simulate::newStudy(const QString& kind) {
  if (!services().requireEditable()) return;
  open();
  AppDocument* doc = services().document();
  const opad::Scene& s = doc->scene;
  const std::string k = kind.toStdString();
  json settings = json::object();
  if (k == "motion") {
    const opad::Joint* j = s.joint(m_joint);
    if (!j) throw opad::UserHint("A motion study drives a joint: add one first (Joint on the ribbon).");
    const opad::sim::JointKind* jk = opad::sim::joint_kind(j->kind);
    const bool angle = jk && !jk->coords.empty() && jk->coords[0].angle;
    settings = {{"duration", 2.0}, {"frames", 121}, {"drivers", {{{"joint", m_joint}, {"to", (j->values.empty() ? 0.0 : j->values[0]) + (angle ? 360.0 : 50.0)}}}}};
  } else if (k == "dynamic") {
    settings = {{"duration", 1.0}, {"frames", 101}, {"gravity", true}};
  } else {
    if (s.loads.empty()) throw opad::UserHint("A structural study needs a load case: add a Fixed support and a Force or Pressure on faces first.");
    settings = {{"case", s.loads.front().load_case}};
  }
  const json out = write("study", {{"kind", k}, {"settings", settings}, {"run", false}}, tr("New study"));
  m_study = out.value("id", "");
  m_run.reset();
  refresh();
  runStudy();
}

void Simulate::openGuide(int useCase) {
  QWidget* window = services().window();
  auto* guide = window->findChild<SimulateGuide*>();
  if (!guide)
    guide = new SimulateGuide([this](const QString& id) { return services().action(id); },
                              [this](const QString& id) {
                                QAction* a = services().action(id);
                                if (!a || !a->isEnabled()) return;
                                // A tool of Simulate (or the selection filter it asks for) in Simulate; Gear in Design.
                                if (id == "design.gear") services().setWorkspace("design");
                                else if (services().workspace() != "simulate" && id != "inspect.material") services().setWorkspace("simulate");
                                services().window()->activateWindow();
                                a->trigger();
                              },
                              window);
  guide->open(useCase);
}

// Nothing picked for a tool that needs picks: the selection filter it wants is turned on, and the hint says what to click.
void Simulate::wantPicks(const char* filter, const char* hint) {
  if (QAction* f = services().action(filter); f && !f->isChecked()) f->trigger();
  throw opad::UserHint(hint);
}

void Simulate::printSettings() {
  if (!services().requireEditable()) return;
  AppDocument* doc = services().document();
  const opad::Study* st = doc->scene.study(m_study);
  if (!st || (st->kind != "static" && st->kind != "modal")) {
    st = nullptr;
    for (const auto& x : doc->scene.studies)
      if (x.kind == "static" || x.kind == "modal") st = &x;
  }
  if (!st) throw opad::UserHint("Printed part settings belong to a structural study: make a Static stress or Vibration modes study first.");
  const std::string id = st->id;
  json settings = st->def.value("settings", json::object());
  PrintDialog dialog(settings.value("print", json()), services().window());
  if (dialog.exec() != QDialog::Accepted) return;
  const json print = dialog.print();
  if (print.is_null()) settings.erase("print");
  else settings["print"] = print;
  if (settings == st->def.value("settings", json::object())) return;
  write("study", {{"id", id}, {"settings", settings}, {"run", false}}, tr("Printed part"));
  if (m_study != id) chooseStudy(id);
  m_run.reset();
  refresh();
  runStudy();
}

void Simulate::chooseStudy(const std::string& id) {
  if (id == m_study) return;
  pause();
  clearMotion();
  showResults(false);
  m_study = id;
  m_run.reset();
  m_frame = 0;
  showRun();
  services().updateCommands();
}

void Simulate::runStudy() {
  AppDocument* doc = services().document();
  const opad::Study* st = doc->scene.study(m_study);
  if (!st) throw opad::UserHint("Choose a study first, or make one: Motion, Dynamic, Static stress or Vibration modes.");
  if (m_job) return;
  pause();
  clearMotion();
  showResults(false);
  const json def = st->def;
  const std::string id = m_study;
  auto result = std::make_shared<std::shared_ptr<const opad::sim::StudyRun>>();
  auto error = std::make_shared<std::string>();
  m_job = doc->readAsync(
      services().jobs(), tr("Running %1").arg(QString::fromStdString(st->name)),
      [def, result, error](const opad::Document& d, const opad::Scene& s, Progress p) {
        try {
          *result = opad::sim::run_study_cached(d, s, def, [p](double f, const std::string& phase) {
            p.setPhase(QString::fromStdString(phase), int(std::lround(100 * f)));
            return !p.cancelled();
          });
        } catch (const std::exception& e) {
          *error = e.what();
        }
      },
      [this, self = QPointer<Simulate>(this), id, result, error](bool ok, const QString& why) {
        if (!self) return;
        m_job = nullptr;
        services().updateCommands();
        if (!ok || !error->empty() || !*result) {
          const QString text = !error->empty() ? QString::fromStdString(*error) : why;
          if (!text.isEmpty()) services().toast(tr("The study did not run: %1").arg(i18n::t(text)), {}, {}, 8000);
          return;
        }
        // The run's summary into the study op (the cache answers at once).
        services().guarded([&] { write("study", {{"id", id}}, tr("Run study")); });
        m_study = id;
        m_run = *result;
        m_runState = stateOf(services().document());
        m_frame = 0;
        refresh();
        if (m_run->fea) showResults(true);
        else play();
      });
  if (!m_job) services().toast(tr("The document is busy: try again in a moment"));
  services().updateCommands();
}

void Simulate::showRun() {
  if (!m_form) return;
  const opad::Study* st = services().document()->scene.study(m_study);
  if (!m_run) {
    QString text;
    if (st && st->result.is_object() && !st->result.empty()) text = summaryText(st->result) + "\n" + tr("(from its last run: Run shows it again)");
    m_form->setRun(text, 0, false);
    m_form->plot()->setData({}, {});
    return;
  }
  json summary = m_run->summary;
  if (!m_run->warnings.empty()) summary["warnings"] = m_run->warnings;
  const bool structural = bool(m_run->fea);
  m_form->setRun(summaryText(summary), int(m_run->t.size()), structural);
  if (structural) {
    SimulatePanel::Entries fields;
    if (m_run->kind == "static") {
      fields = {{"von_mises", tr("von Mises stress (MPa)")}, {"displacement", tr("Displacement (mm)")}};
      if (!m_run->fea->failure_index.empty()) fields.push_back({"failure_index", tr("Failure index, printed (1 fails)")});
    } else {
      for (size_t i = 0; i < m_run->fea->frequencies.size(); ++i)
        fields.push_back({QString("mode:%1").arg(i), tr("Mode %1: %2 Hz").arg(i + 1).arg(number(m_run->fea->frequencies[i]))});
    }
    if (std::none_of(fields.begin(), fields.end(), [&](const auto& f) { return f.first == m_field; }) && !fields.empty()) m_field = fields.front().first;
    m_form->setFields(fields, m_field);
    m_form->setResultsShown(resultShown());
    return;
  }
  QStringList names;
  for (const auto& s : m_run->series) names << QString::fromStdString(s.name) + (s.unit.empty() ? "" : " (" + QString::fromStdString(s.unit) + ")");
  m_seriesIndex = std::clamp(m_seriesIndex, 0, std::max(0, int(names.size()) - 1));
  m_form->setSeries(names, m_seriesIndex);
  showSeries(m_seriesIndex);
  showFrame(std::min(m_frame, int(m_run->t.size()) - 1));
}

void Simulate::showSeries(int index) {
  if (!m_run || index < 0 || size_t(index) >= m_run->series.size()) return;
  m_seriesIndex = index;
  const auto& s = m_run->series[size_t(index)];
  m_form->plot()->setData(m_run->t, {{QString::fromStdString(s.name), QString::fromStdString(s.unit), s.v}});
  m_form->plot()->setCursor(m_frame);
}

void Simulate::showFrame(int frame) {
  if (!m_run || m_run->t.empty() || m_run->fea) return;
  m_frame = std::clamp(frame, 0, int(m_run->t.size()) - 1);
  m_form->setFrame(m_frame, QString::number(m_run->t[size_t(m_frame)], 'f', 3) + " s");
  if (stateOf(services().document()) != m_runState || size_t(m_frame) >= m_run->poses.size()) return;
  const opad::Scene& s = services().document()->scene;
  std::vector<std::pair<std::string, gp_Trsf>> motion;
  for (size_t k = 0; k < m_run->parts.size() && k < m_run->poses[size_t(m_frame)].size(); ++k) {
    const std::string& part = m_run->parts[k];
    if (!s.node(part)) continue;
    try {
      const gp_Trsf t = opad::trsf_from_mat(m_run->poses[size_t(m_frame)][k] * s.world(part).inverse());
      for (const auto& b : s.bodies_under(part)) motion.push_back({b, t});
    } catch (const std::exception&) {
    }
  }
  services().viewport()->setPreviewMotion(motion);
}

void Simulate::clearMotion() {
  if (Viewport* v = services().viewport()) v->setPreviewMotion({});
}

void Simulate::play() {
  if (playing()) return pause();
  if (!m_run || m_run->t.size() < 2 || m_run->fea) return;
  if (m_frame >= int(m_run->t.size()) - 1) m_frame = 0;
  m_playFrom = m_run->t[size_t(m_frame)];
  m_clock.start();
  m_tick.start();
  m_form->setPlaying(true);
}

void Simulate::pause() {
  m_tick.stop();
  if (m_form) m_form->setPlaying(false);
}

void Simulate::tick() {
  if (!m_run || m_run->t.size() < 2) return pause();
  // Real time for a study of a second or more; a short one (an engine's two turns in 40 ms) is stretched to 3 s.
  const double span = m_run->t.back() - m_run->t.front();
  const double rate = span >= 1 ? 1.0 : span / 3.0;
  const double t = m_playFrom + rate * m_clock.elapsed() / 1000.0;
  const auto it = std::lower_bound(m_run->t.begin(), m_run->t.end(), t);
  const int frame = int(std::min<size_t>(size_t(it - m_run->t.begin()), m_run->t.size() - 1));
  showFrame(frame);
  if (t >= m_run->t.back()) pause();
}

// ---------------------------------------------------------------- loads and the result map
void Simulate::addLoad(const QString& kind) {
  if (!services().requireEditable()) return;
  AppDocument* doc = services().document();
  const std::string k = kind.toStdString();
  json on = json::array();
  const SelectionContext sel = services().selection();
  for (const auto& r : sel.refs) {
    if (r.body.empty()) continue;
    if (k == "bolt_preload") on.push_back(r.body);
    else if (r.kind == opad::Ref::Kind::Face) on.push_back(r.to_json());
  }
  if (k != "gravity" && on.empty()) {
    if (k == "bolt_preload") wantPicks("select.bodies", "Click the bolt (the Bodies filter is on now), then Bolt preload again.");
    wantPicks("select.faces", "Click the faces it acts on (the Faces filter is on now), then choose it again.");
  }
  json args = {{"kind", k}, {"on", on}};
  if (!doc->scene.loads.empty()) args["case"] = doc->scene.loads.back().load_case;
  bool ok = true;
  if (k == "force") {
    const QString text = QInputDialog::getText(services().window(), tr("Force"), tr("Force vector in N (x, y, z):"), QLineEdit::Normal, "0, 0, -1000", &ok);
    if (!ok) return;
    const QStringList parts = text.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
    if (parts.size() != 3) throw opad::UserHint("Type the force as three numbers: x, y, z in N.");
    args["vector"] = {parts[0].toDouble(), parts[1].toDouble(), parts[2].toDouble()};
  } else if (k == "pressure") {
    args["value"] = QInputDialog::getDouble(services().window(), tr("Pressure"), tr("Pressure in MPa (positive pushes into the face):"), 1.0, -1e4, 1e4, 3, &ok);
  } else if (k == "bolt_preload") {
    args["value"] = QInputDialog::getDouble(services().window(), tr("Bolt preload"), tr("Preload in N:"), 10000, 0, 1e7, 0, &ok);
  } else if (k == "gravity") {
    args["vector"] = {0, 0, -9806.65};
  }
  if (!ok) return;
  json out;
  try {
    out = write("load", args, tr("Add load"));
  } catch (const std::exception& e) {
    if (trace::enabled()) trace::log(QString("simulate: load refused: ") + e.what());
    throw;
  }
  services().toast(tr("%1 added to %2").arg(QString::fromStdString(out.value("name", "")), QString::fromStdString(out.value("case", ""))));
}

void Simulate::showResults(bool on) {
  Viewport* view = services().viewport();
  if (!view) return;
  if (!m_map.IsNull()) {
    view->removeOverlay(m_map);
    m_map.Nullify();
  }
  view->clearLookLayer(LookSource::Simulate);
  if (m_legend && !m_legend->isHidden()) {
    m_legend->hide();
    viewoverlay::uncovered(view);
  }
  if (!on || !m_run || !m_run->fea) {
    if (m_form) m_form->setResultsShown(false);
    services().updateCommands();
    return;
  }
  const opad::sim::FeaResult& r = *m_run->fea;
  const size_t n = r.nodes.size();
  std::vector<double> value(n, 0.0);
  std::vector<opad::Vec3> shape(n, opad::Vec3{0, 0, 0});
  QString title, unit;
  int mode = -1;
  if (m_field.startsWith("mode:")) mode = m_field.mid(5).toInt();
  if (mode >= 0 && size_t(mode) < r.modes.size()) {
    shape = r.modes[size_t(mode)];
    for (size_t i = 0; i < n; ++i) value[i] = std::sqrt(shape[i][0] * shape[i][0] + shape[i][1] * shape[i][1] + shape[i][2] * shape[i][2]);
    title = tr("Mode %1: %2 Hz").arg(mode + 1).arg(number(r.frequencies[size_t(mode)]));
    unit = tr("rel.");
  } else if (!r.displacement.empty()) {
    shape = r.displacement;
    if (m_field == "displacement") {
      for (size_t i = 0; i < n; ++i) value[i] = std::sqrt(shape[i][0] * shape[i][0] + shape[i][1] * shape[i][1] + shape[i][2] * shape[i][2]);
      title = tr("Displacement");
      unit = "mm";
    } else if (m_field == "failure_index" && r.failure_index.size() == n) {
      value = r.failure_index;
      title = tr("Failure index (Tsai-Hill)");
      unit = "";
    } else {
      value = r.von_mises;
      value.resize(n, 0.0);
      title = tr("von Mises stress");
      unit = "MPa";
    }
  }
  // The deformation scaled to 5 % of the model's size at its largest.
  double size = 0, umax = 0;
  opad::Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c) lo[size_t(c)] = std::min(lo[size_t(c)], r.nodes[i][size_t(c)]), hi[size_t(c)] = std::max(hi[size_t(c)], r.nodes[i][size_t(c)]);
  for (int c = 0; c < 3; ++c) size = std::max(size, hi[size_t(c)] - lo[size_t(c)]);
  for (const auto& u : shape) umax = std::max(umax, std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]));
  const double scale = umax > 0 ? 0.05 * size / umax : 0;
  double vlo = 1e300, vhi = -1e300;
  for (double v : value) vlo = std::min(vlo, v), vhi = std::max(vhi, v);
  if (!(vhi > vlo)) vhi = vlo + 1;
  // A stress singularity (bonded parts' re-entrant corner) would leave the rest in the bottom colour: the scale stops at
  // the 99.5th percentile when the peak is far above it, and the legend gives the peak.
  const double peak = vhi;
  if (mode < 0 && m_field != "displacement" && value.size() > 10) {
    std::vector<double> sorted = value;
    const size_t at = size_t(0.995 * double(sorted.size() - 1));
    std::nth_element(sorted.begin(), sorted.begin() + long(at), sorted.end());
    if (peak > 1.5 * sorted[at] && sorted[at] > vlo) vhi = sorted[at];
  }
  Handle(ResultMap) map = new ResultMap();
  map->points.resize(n);
  map->colours.resize(n);
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c) map->points[i][size_t(c)] = r.nodes[i][size_t(c)] + scale * shape[i][size_t(c)];
    map->colours[i] = opad::result_color(std::clamp((value[i] - vlo) / (vhi - vlo), 0.0, 1.0));
  }
  map->triangles = r.skin;
  m_map = map;
  std::map<std::string, LookDelta> hide;
  for (const auto& b : r.bodies) {
    LookDelta d;
    d.visible = false;
    hide[b] = d;
  }
  view->setLookLayer(LookSource::Simulate, hide);
  view->showOverlay(m_map);
  m_map->SetZLayer(Graphic3d_ZLayerId_Default);  // parts in front hide it, as they would the bodies
  view->updateOverlay(m_map);
  QString note = scale > 0 ? tr("deformation × %1").arg(number(scale)) : QString();
  if (peak > vhi) note = tr("peak %1 %2 above the scale").arg(number(peak), unit);
  m_legend->setScale(title, vlo, vhi, unit, note);
  m_legend->show();
  positionOverlays({});
  if (m_form) m_form->setResultsShown(true);
  services().updateCommands();
}

// ---------------------------------------------------------------- the browser's Simulation folder
std::vector<browser::Item> Simulate::folderItems() const {
  std::vector<browser::Item> rows;
  const AppDocument* doc = services().document();
  if (!doc->hasDocument) return rows;
  const opad::Scene& s = doc->scene;
  for (const auto& j : s.joints) {
    QString tip = i18n::t(QString::fromStdString(j.kind));
    if (!j.values.empty() && !opad::sim::is_relation(j.kind)) {
      QStringList vs;
      for (double v : j.values) vs << number(v);
      tip += " · " + vs.join(", ");
    }
    if (!j.error.empty()) tip += "\n" + i18n::t(QString::fromStdString(j.error));
    browser::Item item{"sim:j:" + j.id, QString::fromStdString(j.name), iconForJoint(j.kind), tip, {}};
    item.error = !j.error.empty();
    rows.push_back(item);
  }
  std::map<std::string, std::vector<browser::Item>> cases;
  for (const auto& l : s.loads) {
    browser::Item item{"sim:l:" + l.id, QString::fromStdString(l.name), iconForLoad(l.kind), i18n::t(QString::fromStdString(l.kind)), {}};
    item.error = !l.error.empty();
    cases[l.load_case].push_back(item);
  }
  for (auto& [name, items] : cases) {
    browser::Item c{"sim:c:" + name, QString::fromStdString(name), "simLoad", tr("A load case: the loads and supports a static or modal study takes together"), {}};
    c.children = std::move(items);
    rows.push_back(c);
  }
  for (const auto& st : s.studies) {
    browser::Item item{"sim:s:" + st.id, QString::fromStdString(st.name), iconForStudy(st.kind), kindLabel(st.kind) + " · " + tr("double-click to run and show it"), {}};
    item.error = !st.error.empty();
    rows.push_back(item);
  }
  return rows;
}

void Simulate::folderMenu(const std::string& id, QMenu& menu) {
  if (id.rfind("sim:s:", 0) == 0) {
    const std::string st = id.substr(6);
    menu.addAction(icons::themed("simRun", 16), tr("Run study"), this, [this, st] {
      services().guarded([&] {
        open();
        chooseStudy(st);
        runStudy();
      });
    });
    const opad::Study* study = services().document()->scene.study(st);
    if (study && (study->kind == "static" || study->kind == "modal"))
      menu.addAction(icons::themed("simPrint", 16), tr("Printed part…"), this, [this, st] {
        services().guarded([&] {
          open();
          chooseStudy(st);
          printSettings();
        });
      });
  } else if (id.rfind("sim:j:", 0) == 0) {
    const std::string j = id.substr(6);
    const opad::Joint* joint = services().document()->scene.joint(j);
    if (!joint || opad::sim::is_relation(joint->kind)) return;
    const bool locked = joint->def.value("locked", false);
    menu.addAction(icons::themed(locked ? "unlock" : "lock", 16), locked ? tr("Unlock joint") : tr("Lock joint"), this, [this, j, locked] {
      services().guarded([&] { write("joint", {{"id", j}, {"locked", !locked}}, locked ? tr("Unlock joint") : tr("Lock joint")); });
    });
    menu.addAction(icons::themed("simJoint", 16), tr("Move with the slider"), this, [this, j] {
      open();
      chooseJoint(j);
    });
  } else if (id.empty()) {
    menu.addAction(icons::themed("simulate", 16), tr("Simulation panel"), this, [this] { open(); });
  }
}

OPAD_AREA(Simulate)
