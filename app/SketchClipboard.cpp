// The clipboard (TODO 11 UI-129): Ctrl+C, Ctrl+X and Ctrl+V. In a sketch they copy the selected curves with the constraints
// among them (core copy_entities / paste_entities), as application/x-opad+json so another OPAD window pastes them too; the
// paste follows the pointer by its base point until a click places it, in one undo step. Outside a sketch Ctrl+C copies
// the timeline marker's op id, as its menu says.
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
#include <cmath>
#include <limits>

#include "AreaController.hpp"
#include "Commands.hpp"
#include "CurveSamples.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "Ribbon.hpp"
#include "SketchEditor.hpp"
#include "TimelineWidget.hpp"
#include "opad/design/sketch_edit.hpp"
#include "opad/design/sketch_geom.hpp"

using namespace opad::design;

OPAD_ICON_TABLE(clipboard, {"cut", R"(<circle cx="6" cy="18" r="3"/><circle cx="18" cy="18" r="3"/><path d="M8 16L18 4M16 16L6 4"/>)"},
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
// Edit's Copy, Cut and Paste (Ctrl+C, Ctrl+X, Ctrl+V), and the sketch's Copy with base point (Ctrl+Shift+C). A text field
// with the keyboard keeps its own clipboard keys.
class ClipboardArea : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    auto add = [this](const char* id, const QString& label, const char* icon, const QKeySequence& key, const QStringList& words, std::function<void()> fn) {
      CommandInfo info;
      info.id = id, info.label = label, info.icon = icon, info.key = key;
      info.keywords = words;
      info.editsDocument = QString(id).startsWith("sketch.");  // as the built-in commands (MainWindow::isEditAction)
      return services().addCommand(info, std::move(fn));
    };
    m_copy = add("edit.copy", tr("Copy"), "copy", QKeySequence::Copy, {tr("clipboard"), tr("op id")}, [this] { copy(false); });
    m_cut = add("edit.cut", tr("Cut"), "cut", QKeySequence::Cut, {tr("clipboard")}, [this] { copy(true); });
    m_paste = add("edit.paste", tr("Paste"), "paste", QKeySequence::Paste, {tr("clipboard")}, [this] { paste(); });
    m_base = add("sketch.copybase", tr("Copy with base point"), "copyBase", QKeySequence("Ctrl+Shift+C"), {tr("clipboard")}, [this] { services().design()->sketch()->copyWithBase(); });
  }
  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* edit = menus.value("edit");
    if (!edit) return;
    const QList<QAction*> entries = edit->actions();
    QAction* after = nullptr;  // after Undo and Redo
    for (int i = 0; i + 1 < entries.size(); ++i)
      if (entries[i]->objectName() == "edit.redo") after = entries[i + 1];
    edit->insertActions(after, {m_cut, m_copy, m_paste});
    edit->insertSeparator(after);
  }
  void ribbon(RibbonLayout& layout) override {
    if (!layout.addGroup("sketch.modify", "sketch.modify.clipboard", tr("Clipboard"))) return;
    for (QAction* a : {m_copy, m_cut, m_paste, m_base}) layout.addAction("sketch.modify.clipboard", a, RibbonLayout::Size::Small);
  }

 private:
  // In a sketch, its curves; else the timeline marker's op id (the marker picked and nothing selected, or the timeline with
  // the keyboard).
  void copy(bool cut) {
    if (services().design()->sketchActive()) {
      services().design()->sketch()->copySelection(cut);
      return;
    }
    auto* timeline = services().window()->findChild<TimelineWidget*>();
    QWidget* keys = services().window()->focusWidget();
    const bool focused = timeline && keys && (keys == timeline || timeline->isAncestorOf(keys));
    if (!cut && timeline && !timeline->currentOp().empty() && (focused || services().selection().empty())) {
      QApplication::clipboard()->setText(QString::fromStdString(timeline->currentOp()));
      return services().showMessage(tr("Copied the op id %1").arg(QString::fromStdString(timeline->currentOp().substr(0, 8))));
    }
    services().showMessage(cut ? tr("Cut works on sketch curves: open a sketch and select them") : tr("Nothing to copy: select sketch curves, or a marker on the timeline"));
  }
  void paste() {
    if (services().design()->sketchActive()) return services().design()->sketch()->paste();
    services().showMessage(tr("Paste works in a sketch: open one, then Ctrl+V"));
  }
  QAction *m_copy = nullptr, *m_cut = nullptr, *m_paste = nullptr, *m_base = nullptr;
};
OPAD_AREA(ClipboardArea)
