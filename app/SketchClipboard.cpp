// The clipboard (TODO 11 UI-129): Ctrl+C, Ctrl+X and Ctrl+V. In a sketch they copy the selected curves with the constraints
// among them (core copy_entities / paste_entities), as application/x-opad+json so another OPAD window pastes them too; the
// paste follows the pointer by its base point until a click places it, in one undo step. Outside a sketch they copy bodies
// and components (core opad/clipboard.hpp) as new bodies or, with Ctrl+Shift+V, linked instances; the timeline's marker
// gives its op id, as its menu says.
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QMainWindow>
#include <QMenuBar>
#include <QMimeData>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <limits>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "CurveSamples.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "Ribbon.hpp"
#include "SketchEditor.hpp"
#include "TimelineWidget.hpp"
#include "Units.hpp"
#include "opad/clipboard.hpp"
#include "opad/design/sketch_edit.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"

using namespace opad::design;

OPAD_ICON_TABLE(clipboard, {"cut", R"(<circle cx="6" cy="18" r="3"/><circle cx="18" cy="18" r="3"/><path d="M8 16L18 4M16 16L6 4"/>)"},
                {"pasteLinked", R"(<path d="M8 4H5v17h6M16 4h3v5"/><rect x="8" y="2" width="8" height="4"/><path d="M14 17a2.5 2.5 0 0 1 0-5h2M18 12h2a2.5 2.5 0 0 1 0 5h-2M15.5 14.5h3"/>)"},
                {"copyBase", R"(<rect x="9" y="5" width="11" height="11"/><path d="M5 11v8h8" opacity=".6"/><circle cx="9" cy="16" r="2" fill="currentColor"/>)"},
                {"paste", R"(<path d="M8 4H5v17h14V4h-3"/><rect x="8" y="2" width="8" height="4"/><path d="M9 11h6M9 15h6"/>)"});

namespace {
ParamTable parameters(const opad::Scene& scene) {
  std::vector<ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return ParamTable(defs, scene.units);
}
}  // namespace

bool SketchEditor::copySelection(bool cut) {
  if (!m_active || m_editJob) return false;
  // The base point: the lower left of what is copied (as drawn: an arc's bulge, a circle's rim).
  std::vector<int> ids;
  double bu = std::numeric_limits<double>::infinity(), bv = bu;
  auto extend = [&](double u, double v) { bu = std::min(bu, u), bv = std::min(bv, v); };
  for (int id : m_sel) {
    if (const SkEntity* e = m_sk.entity(id)) {
      ids.push_back(id);
      if (e->type == SkEntity::Type::Point) {
        if (const SkPoint* p = e->p.empty() ? nullptr : m_sk.point(e->p[0])) extend(p->x, p->y);
      } else
        for (const auto& [u, v] : sampled(*e)) extend(u, v);
    } else if (const SkPoint* p = m_sk.point(id)) {
      ids.push_back(id);
      extend(p->x, p->y);
    }
  }
  if (ids.empty() || !std::isfinite(bu) || !std::isfinite(bv)) {
    emit status(tr("Select the curves to copy first"));
    return false;
  }
  return copyFrom(ids, bu, bv, cut);
}

void SketchEditor::copyWithBase() {
  if (!m_active || m_editJob) return;
  if (std::none_of(m_sel.begin(), m_sel.end(), [this](int id) { return m_sk.entity(id) || m_sk.point(id); })) return emit status(tr("Select the curves to copy first"));
  setTool("copybase");
}

bool SketchEditor::copyFrom(const std::vector<int>& ids, double bu, double bv, bool cut) {
  try {
    const opad::json clip = copy_entities(m_sk, ids, bu, bv);
    auto* mime = new QMimeData;
    mime->setData(kClipMime, QByteArray::fromStdString(clip.dump()));
    QApplication::clipboard()->setMimeData(mime);
    const int curves = int(clip.at("sketch").at("entities").size());
    emit status(cut ? tr("Cut %1 curves; Ctrl+V pastes them").arg(curves) : tr("Copied %1 curves; Ctrl+V pastes them").arg(curves));
  } catch (const std::exception& e) {
    emit status(i18n::t(QString::fromUtf8(e.what())));
    return false;
  }
  if (cut) deleteSelection();
  return true;
}

void SketchEditor::paste() {
  if (!m_active || m_editJob) return;
  const QMimeData* mime = QApplication::clipboard()->mimeData();
  if (!mime || !mime->hasFormat(kClipMime)) return emit status(tr("The clipboard holds no sketch curves: copy some first (Ctrl+C)"));
  // Read and checked on a worker (a clip of a converted drawing is megabytes), with its outline for the preview.
  const QByteArray bytes = mime->data(kClipMime);
  const int revision = ++m_clipRevision, session = m_session;
  const double deflection = std::max(1e-7, m_viewport->pixelSize() * 0.5);
  auto clip = std::make_shared<Clip>();
  QPointer<SketchEditor> guard(this);
  m_jobs->async(tr("Reading the clipboard"), [bytes, clip, deflection](Progress progress) {
    clip->data = opad::json::parse(bytes.begin(), bytes.end());
    if (!clip->data.is_object() || clip->data.value("format", std::string()) != "opad.sketch.clipboard") throw opad::Error("the clipboard holds no sketch curves");
    const Sketch sk = Sketch::from_json(clip->data.at("sketch"));
    clip->bu = clip->data.at("base").at(0).get<double>();
    clip->bv = clip->data.at("base").at(1).get<double>();
    clip->curves = sk.entities.size();
    if (sk.entities.size() > 2000) {  // its extent only
      Bnd_Box box;
      for (const auto& e : sk.entities)
        if (const auto edge = entity_edge(sk, e, opad::Frame()); !edge.IsNull()) BRepBndLib::Add(edge, box);
      if (box.IsVoid()) return;
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      x0 -= clip->bu, x1 -= clip->bu, y0 -= clip->bv, y1 -= clip->bv;
      clip->outline.push_back({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}});
      return;
    }
    for (const auto& e : sk.entities) {
      if (progress.cancelled()) return;
      const auto edge = entity_edge(sk, e, opad::Frame());
      if (edge.IsNull()) continue;
      std::vector<std::pair<double, double>> line;
      for (const auto& p : curveSamples(edge, deflection)) line.push_back({p.X() - clip->bu, p.Y() - clip->bv});
      clip->outline.push_back(std::move(line));
    }
  }, [this, guard, clip, revision, session](bool ok, const QString& error) {
    if (!guard || !m_active || revision != m_clipRevision || session != m_session) return;
    if (!ok) return emit status(i18n::t(error));
    if (m_editJob) return emit status(tr("The sketch is busy; paste again"));
    setTool("paste");
    m_clip = clip;
    toolPrompt();
    updateTransient();
    emit changed();
  });
}

// The paste tool's click: the copy's base point there (one undo step), selected after. The copybase tool's: the selection
// copied about it.
bool SketchEditor::clipClick(const Snap& s) {
  if (m_tool == "copybase") {
    const std::vector<int> selection = m_sel;
    copyFrom(selection, s.u, s.v, false);
    setTool("select");
    m_sel = selection;
    rebuild();
    emit changed();
    return true;
  }
  if (m_tool != "paste") return false;
  const auto clip = m_clip;
  if (!clip) {
    setTool("select");
    return true;
  }
  begin_change();
  std::vector<int> made;
  try {
    made = paste_entities(m_sk, clip->data, s.u, s.v, parameters(m_doc->scene));
  } catch (const std::exception& e) {
    cancel_change();
    emit status(i18n::t(QString::fromUtf8(e.what())));
    return true;
  }
  if (!end_change(tr("Paste"))) return true;
  setTool("select");
  m_sel = made;
  rebuild();
  emit changed();
  emit status(tr("Pasted %1 curves").arg(made.size()));
  return true;
}

// ---------------------------------------------------------------- the commands
// Edit's Copy, Cut, Paste (Ctrl+C, Ctrl+X, Ctrl+V) and Paste as linked instances (Ctrl+Shift+V), and the sketch's Copy with
// base point (Ctrl+Shift+C). In a sketch they are the sketch's (curves). Elsewhere Copy takes the selected bodies and
// components (core copy_nodes: their entries ride along for another window while they are small enough), Cut copies them
// and then deletes them as Delete does, and Paste makes new bodies: in the document they came from, Move / copy with Make
// a copy on, beside the originals (parametric copies, the panel open to place them; Enter makes them); from another
// document, or once the originals are gone, imported as they were. Paste as linked instances puts the same parts again
// beside them (the same body entries, the Linked instances browser lists them), one undo step. The timeline with the
// keyboard, or its marker with nothing selected, copies the op id as its menu says. A text field keeps its own keys.
class ClipboardArea : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    auto add = [this](const char* id, const QString& label, const char* icon, const QKeySequence& key, const QStringList& words, std::function<void()> fn) {
      CommandInfo info;
      info.id = id, info.label = label, info.icon = icon, info.key = key;
      info.keywords = words;
      info.editsDocument = QString(id) != "edit.copy";  // as the built-in commands (MainWindow::isEditAction)
      return services().addCommand(info, std::move(fn));
    };
    m_copy = add("edit.copy", tr("Copy"), "copy", QKeySequence::Copy, {tr("clipboard"), tr("op id"), tr("bodies")}, [this] { copy(false); });
    m_cut = add("edit.cut", tr("Cut"), "cut", QKeySequence::Cut, {tr("clipboard")}, [this] { copy(true); });
    m_paste = add("edit.paste", tr("Paste"), "paste", QKeySequence::Paste, {tr("clipboard"), tr("new bodies")}, [this] { paste(false); });
    m_linked = add("edit.pastelinked", tr("Paste as linked instances"), "pasteLinked", QKeySequence("Ctrl+Shift+V"), {tr("clipboard"), tr("instances")},
                   [this] { paste(true); });
    m_base = add("sketch.copybase", tr("Copy with base point"), "copyBase", QKeySequence("Ctrl+Shift+C"), {tr("clipboard")}, [this] { services().design()->sketch()->copyWithBase(); });
  }
  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* edit = menus.value("edit");
    if (!edit) return;
    const QList<QAction*> entries = edit->actions();
    QAction* after = nullptr;  // after Undo and Redo
    for (int i = 0; i + 1 < entries.size(); ++i)
      if (entries[i]->objectName() == "edit.redo") after = entries[i + 1];
    edit->insertActions(after, {m_cut, m_copy, m_paste, m_linked});
    edit->insertSeparator(after);
  }
  void ribbon(RibbonLayout& layout) override {
    if (layout.addGroup("sketch.modify", "sketch.modify.clipboard", tr("Clipboard")))
      for (QAction* a : {m_copy, m_cut, m_paste, m_base}) layout.addAction("sketch.modify.clipboard", a, RibbonLayout::Size::Small);
    if (layout.addGroup("design.assemble", "design.assemble.clipboard", tr("Clipboard")))
      for (QAction* a : {m_copy, m_paste, m_linked}) layout.addAction("design.assemble.clipboard", a, RibbonLayout::Size::Small);
  }

 private:
  void copy(bool cut) {
    if (services().design()->sketchActive()) {
      services().design()->sketch()->copySelection(cut);
      return;
    }
    auto* timeline = services().window()->findChild<TimelineWidget*>();
    QWidget* keys = services().window()->focusWidget();
    const bool marker = timeline && !timeline->currentOp().empty(), focused = marker && keys && (keys == timeline || timeline->isAncestorOf(keys));
    if (!cut && focused) return copyOpId(timeline->currentOp());
    std::vector<std::string> nodes;
    for (const auto& id : services().selection().ids)
      if (services().document()->scene.node(id)) nodes.push_back(id);
    if (!nodes.empty()) return copyNodes(nodes, cut);
    if (!cut && marker && services().selection().empty()) return copyOpId(timeline->currentOp());
    services().showMessage(cut ? tr("Nothing to cut: select bodies, components or sketch curves") : tr("Nothing to copy: select bodies, components or sketch curves, or a marker on the timeline"));
  }
  void copyOpId(const std::string& op) {
    QApplication::clipboard()->setText(QString::fromStdString(op));
    services().showMessage(tr("Copied the op id %1").arg(QString::fromStdString(op.substr(0, 8))));
  }
  // The JSON is made here (the nodes, and the BREP text of their entries up to the limit: a copy of that much memory), its
  // text on a worker; the last copy started is the one the clipboard gets.
  void copyNodes(const std::vector<std::string>& ids, bool cut) {
    const AppDocument* doc = services().document();
    auto clip = std::make_shared<opad::json>();
    try {
      *clip = opad::copy_nodes(doc->doc, doc->scene, ids);
    } catch (const std::exception& e) {
      return services().showMessage(i18n::t(QString::fromUtf8(e.what())));
    }
    const int count = int(clip->at("nodes").size()), serial = ++m_copies;
    auto bytes = std::make_shared<QByteArray>();
    services().jobs()->async(tr("Copying to the clipboard"), [clip, bytes](Progress) {
      *bytes = QByteArray::fromStdString(clip->dump());
      *clip = opad::json();
    }, [this, bytes, count, serial](bool ok, const QString& error) {
      if (!ok) return services().showMessage(i18n::t(error));
      if (serial != m_copies) return;
      auto* mime = new QMimeData;
      mime->setData(SketchEditor::kClipMime, *bytes);
      QApplication::clipboard()->setMimeData(mime);
      services().showMessage(tr("Copied %1 objects: Ctrl+V pastes new bodies, Ctrl+Shift+V linked instances").arg(count));
    });
    if (cut)
      if (QAction* remove = services().action("edit.delete")) remove->trigger();
  }

  // Read and checked on a worker (a clip with BREP text is megabytes), then placed.
  void paste(bool linked) {
    if (services().design()->sketchActive()) return services().design()->sketch()->paste();
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(SketchEditor::kClipMime)) return services().showMessage(tr("The clipboard holds no bodies or components: copy some first (Ctrl+C)"));
    const QByteArray bytes = mime->data(SketchEditor::kClipMime);
    auto clip = std::make_shared<opad::json>();
    const int serial = ++m_pastes;
    services().jobs()->async(tr("Reading the clipboard"), [bytes, clip](Progress) {
      *clip = opad::json::parse(bytes.begin(), bytes.end());
      if (!clip->is_object()) throw opad::Error("the clipboard holds no bodies or components");
    }, [this, clip, linked, serial](bool ok, const QString& error) {
      if (serial != m_pastes) return;
      if (!ok) return services().showMessage(i18n::t(error));
      services().guarded([&] { place(clip, linked); });
    });
  }
  void place(const std::shared_ptr<opad::json>& clip, bool linked) {
    AppDocument* doc = services().document();
    const std::string format = clip->value("format", std::string());
    if (format == "opad.sketch.clipboard") return services().showMessage(tr("The clipboard holds sketch curves: open a sketch to paste them"));
    if (!doc->hasDocument || doc->browse) return;  // the command asked to save a viewed file first; not done
    if (doc->loading || doc->designBusy || services().design()->sketchActive()) return services().showMessage(tr("The document is busy; paste again in a moment"));
    if (services().design()->featureActive() || !doc->rollback().empty()) return services().showMessage(tr("Finish what is open first."));
    const bool home = clip->value("document", std::string()) == doc->doc.header.uuid;
    std::vector<std::string> sources;
    std::string identity = clip->value("document", std::string());
    bool present = home && clip->contains("nodes") && (*clip)["nodes"].is_array(), solid = true;
    if (present)
      for (const auto& n : (*clip)["nodes"]) {
        const std::string id = n.value("id", std::string());
        identity += "/" + id;
        if (!doc->scene.node(id)) present = false;
        else sources.push_back(id);
      }
    // Beside the originals; pasted again, beside the last paste (the same clip: one step further each time).
    m_repeat = identity == m_lastClip ? m_repeat + 1 : 0;
    m_lastClip = identity;
    const double dx = present ? besideOriginals(sources, solid) * (m_repeat + 1) : 0;
    if (!linked && present && solid) return newBodies(sources, dx);
    opad::PastePlan plan = opad::plan_paste(doc->doc, doc->scene, *clip, {dx, 0, 0});  // throws what is wrong with it
    const QString what = linked && home ? tr("Paste as linked instances") : tr("Paste");
    if (!plan.missing.empty()) return pasteEntries(clip, plan.missing.size(), what);
    opad::json ops = opad::json::array();
    for (auto& op : plan.ops) ops.push_back(std::move(op));
    doc->run("append", {{"ops", ops}}, what);
    select(plan.nodes);
    services().showMessage(!home ? tr("Pasted %1 objects where they were").arg(plan.nodes.size())
                           : present ? tr("Pasted %1 linked instances %2 along X").arg(plan.nodes.size()).arg(units::format(units::Kind::Length, dx))
                                     : tr("Pasted %1 objects where the originals were").arg(plan.nodes.size()));
  }
  // How far along X the copies go to sit beside the originals: their extent's width and a little more, a round amount (view
  // boxes from the cache, as the viewport has them). `solid`: every body is a solid (Move / copy can copy it).
  double besideOriginals(const std::vector<std::string>& sources, bool& solid) const {
    const AppDocument* doc = services().document();
    Bnd_Box box;
    size_t bodies = 0;
    for (const auto& id : sources)
      for (const auto& b : doc->scene.bodies_under(id)) {
        if (const opad::Node* n = doc->scene.node(b); n && n->representation != "solid") solid = false;
        if (++bodies > 5000) continue;  // enough to know how wide
        try {
          box.Add(opad::node_world_bbox(doc->doc, doc->scene, b));
        } catch (const std::exception&) {
        }
      }
    if (box.IsVoid()) return 10;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const double width = std::max(1e-3, (x1 - x0) * 1.15), step = std::pow(10.0, std::floor(std::log10(width)) - 1);
    return std::ceil(width / step) * step;
  }
  void newBodies(const std::vector<std::string>& sources, double dx) {
    opad::json picks = opad::json::array();
    for (const auto& id : sources) picks.push_back(opad::Ref{id}.to_json());
    services().design()->startFeature("move", {{"bodies", picks}, {"copy", true}, {"dx", units::editable(units::Kind::Length, dx).toStdString()}});
    if (services().design()->featureActive())
      services().showMessage(tr("Paste: the copies are shown %1 along X; change X, Y or Z, then Enter makes them").arg(units::format(units::Kind::Length, dx)), 8000);
  }
  // Entries the document lacks (copied in another window): added to a copy of the document on a worker, with their shapes
  // read and boxed there, which then replaces the document as one undo step.
  void pasteEntries(const std::shared_ptr<opad::json>& clip, size_t entries, const QString& what) {
    AppDocument* doc = services().document();
    const QString author = QSettings().value("user/name").toString().trimmed();
    const bool started = doc->captureSnapshot(services().jobs(), [this, clip, author, what, entries](std::shared_ptr<opad::Document> copy, const QString& error) {
      AppDocument* doc = services().document();
      if (!copy) return services().showMessage(error.isEmpty() ? tr("The document is busy; paste again in a moment") : error);
      struct Out {
        opad::Scene scene;
        opad::json result;
      };
      auto out = std::make_shared<Out>();
      const auto revision = doc->revision;
      doc->designBusy = true;  // nothing changes the document meanwhile: the copy replaces it
      emit doc->undoChanged();
      services().jobs()->async(tr("Pasting %1 bodies").arg(entries), [copy, clip, author, out](Progress progress) {
        out->result = opad::paste_nodes(*copy, *clip, {0, 0, 0}, author.toStdString());
        for (const auto& b : (*clip)["bodies"]) {
          if (progress.cancelled()) throw opad::Error("cancelled");
          if (copy->has_body(b.value("key", std::string()))) opad::body_bbox(*copy, b.value("key", std::string()));  // parsed and boxed here
        }
        out->scene = opad::resolve(*copy);
      }, [this, copy, out, revision, what](bool ok, const QString& error) {
        AppDocument* doc = services().document();
        doc->designBusy = false;
        emit doc->undoChanged();
        if (!ok) return services().showMessage(i18n::t(error));
        try {
          doc->commitSnapshot(*copy, out->scene, revision, what);
        } catch (const std::exception&) {
          return services().showMessage(tr("The document changed meanwhile; paste again"));
        }
        std::vector<std::string> ids;
        for (const auto& id : out->result.value("ids", opad::json::array())) ids.push_back(id.get<std::string>());
        select(ids);
        services().showMessage(tr("Pasted %1 objects where they were, with %2 new bodies").arg(ids.size()).arg(out->result.value("bodies_added", 0)));
      });
    });
    if (!started) services().showMessage(tr("The document is busy; paste again in a moment"));
  }
  // The pasted nodes selected once the browser has them.
  void select(const std::vector<std::string>& ids) {
    QTimer::singleShot(0, this, [this, ids] {
      std::vector<std::string> there;
      for (const auto& id : ids)
        if (services().document()->scene.node(id)) there.push_back(id);
      if (!there.empty()) services().browser()->selectIds(there);
    });
  }
  QAction *m_copy = nullptr, *m_cut = nullptr, *m_paste = nullptr, *m_linked = nullptr, *m_base = nullptr;
  int m_copies = 0, m_pastes = 0;  // the last copy and paste started win
  std::string m_lastClip;  // what was pasted last (its document and nodes), pasted m_repeat + 1 times in a row
  int m_repeat = 0;
};
OPAD_AREA(ClipboardArea)
