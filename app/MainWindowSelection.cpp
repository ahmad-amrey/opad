// Selection: viewport <-> browser, the Properties panel, selection.json for agents, the context menu.
#include "MainWindow.hpp"

#include <QColorDialog>
#include <QCoreApplication>
#include <QMenu>

#include <algorithm>
#include <functional>
#include <memory>
#include <set>

#include <Bnd_Box.hxx>

#include "Drawing2D.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "SmartRules.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

OPAD_ICON_TABLE(contextmenu,
  {"repeat", R"(<path d="M17 3l3 3-3 3"/><path d="M4 12v-2a4 4 0 0 1 4-4h12"/><path d="M7 21l-3-3 3-3"/><path d="M20 12v2a4 4 0 0 1-4 4H4"/>)"});

// ---------------------------------------------------------------- selection plumbing (F22/F25)
std::vector<std::string> MainWindow::currentNodeIds() const {
  if (!m_selRows.empty() && m_selRefs.empty()) return {};  // an area's rows alone: the view may still hold the last pick
  std::vector<std::string> ids;
  for (const auto& r : m_viewport->selection())
    if (r.kind != opad::Ref::Kind::Point && std::find(ids.begin(), ids.end(), r.body) == ids.end()) ids.push_back(r.body);
  if (ids.empty()) ids = m_browser->selectedIds();
  ids.erase(std::remove_if(ids.begin(), ids.end(), [this](const std::string& id) { return m_browser->isProvided(id); }), ids.end());
  return ids;
}

QColor MainWindow::nodeColour(const std::string& id) const {
  const opad::Node* n = m_doc->node(id);
  return n && n->has_color ? QColor::fromRgbF(n->color[0], n->color[1], n->color[2]) : QColor(190, 190, 195);
}

void MainWindow::onViewportSelection() {
  if (m_design->ownsSelection()) return m_design->viewportSelectionChanged();  // picks for a feature input or a sketch plane
  if (m_syncing) return;
  m_syncing = true;
  auto refs = m_viewport->selection();
  std::vector<std::string> ids;
  std::set<std::string> seen;
  for (const auto& r : refs)
    if (seen.insert(r.body).second) ids.push_back(r.body);
  m_browser->setSelectedIds(ids);
  if (m_section && m_section->picking() && !refs.empty() && refs.front().kind == opad::Ref::Kind::Face) sectionFromFace(refs.front());
  m_selRows.clear();
  selectionMoved(refs);
  if (!m_tool.id.isEmpty()) toolPicksChanged(refs, true);
  if (refs.empty()) m_statusSel->clear();
  else m_statusSel->setText(tr("%1 selected · %2").arg(refs.size()).arg(i18n::t(m_viewport->drawingWords() ? drawing2d::kindWord(refs.front().kind) : opad::Ref::kind_name(refs.front().kind))));  // UI-118: objects and points in 2D
  scheduleSelectionSync();
  m_syncing = false;
}

void MainWindow::onBrowserSelection(const std::vector<std::string>& ids) {
  if (m_syncing) return;
  m_syncing = true;
  // An area's rows (a provided folder's) are no nodes: the view, the edit commands and the tools never see them.
  std::vector<opad::Ref> refs;
  std::vector<std::string> nodes;
  m_selRows.clear();
  for (const auto& id : ids) {
    if (m_browser->isProvided(id)) { m_selRows.push_back(id); continue; }
    opad::Ref r; r.body = id; refs.push_back(r); nodes.push_back(id);
  }
  selectionMoved(refs);
  if (!m_tool.id.isEmpty()) toolPicksChanged(refs, false);
  m_statusSel->setText(!refs.empty() ? (m_viewport->drawingWords() ? tr("%1 selected · %2").arg(refs.size()).arg(i18n::t(drawing2d::nodeWord(m_doc->scene, refs.front().body))) : tr("%1 selected · body").arg(refs.size()))
                                     : ids.empty() ? QString() : tr("%1 selected").arg(ids.size()));
  m_syncing = false;
  m_viewport->selectNodes(nodes);  // sliced; selectionApplied() writes selection.json when it settles
}

// The Properties panel belongs to one selection: it is opened from the context menu (or Ctrl+P), and a new
// selection closes it. Pinned, it stays and follows the selection. Nothing is inspected while it is closed.
void MainWindow::selectionMoved(const std::vector<opad::Ref>& refs) {
  m_selRefs = refs;
  if(auto* panel=findChild<ToolPanel*>("instanceBrowser"); panel && panel->isVisible()) {
    const auto current=panel->property("instanceCurrent").toString().toStdString();
    if(refs.size()!=1 || refs.front().body!=current)panel->hide();
  }
  if (m_areasReady) {
    const SelectionContext selection = selectionContext();
    for (AreaController* area : m_areas) area->selectionChanged(selection);
  }
  updateCommands();
  if (!m_propsPanel->isVisible()) return;
  if (m_propsPanel->pinned()) showProperties(refs);  // O(1): only the first ref is inspected and geometry walks are deferred to a job
  else m_propsPanel->hide();
}

void MainWindow::showProperties(const std::vector<opad::Ref>& refs) {
  if (m_propsJob) m_propsJob->cancel();  // what is measured for the previous entity must not overwrite this one
  if (refs.empty() && !m_selRows.empty()) {  // an area's browser row: the areas' sections alone
    opad::Ref row;
    row.body = m_selRows.front();
    m_props->setSubject({{row}, {}});
    const QString title = m_browser->rowName(row.body);
    m_propsPanel->setContext(title);
    m_props->showEntity(title, m_selRows.size() > 1 ? tr("  (+%1 more)").arg(m_selRows.size() - 1).trimmed() : QString(), QString(), opad::json::object());
    return;
  }
  if (refs.empty()) {
    m_propsPanel->setContext(QString());
    m_props->clear();
    return;
  }
  m_props->setSubject({refs, {}});  // for the areas' sections
  try {
    const opad::Ref& r = refs.front();
    const opad::Node* node = r.kind == opad::Ref::Kind::Body ? m_doc->node(r.body) : nullptr;
    const bool component = node && node->kind == opad::Node::Kind::Component;
    // What walks the geometry (volume, area, the tight box; for a component every body under it) is measured on a
    // worker and filled in afterwards.
    opad::json j = r.kind == opad::Ref::Kind::Body ? opad::node_properties(m_doc->doc, m_doc->scene, r.body, false) : opad::inspect_ref(m_doc->doc, m_doc->scene, r);
    const opad::Node* subject = m_doc->node(r.body);
    const bool drawing = m_viewport->drawingWords() && subject && subject->representation == "drawing2d";  // 2D words (UI-118)
    if (drawing) j = drawing2d::properties(std::move(j));
    QString title, subtitle, id;
    if (drawing && r.kind != opad::Ref::Kind::Body && !drawing2d::entityType(j).empty()) {  // "Line", "Walls › object 3"
      title = Viewport::drawingWord(drawing2d::entityType(j));
      subtitle = QString::fromUtf8("%1 › %2 %3").arg(m_doc->nodeName(subject->parent.empty() ? r.body : subject->parent), i18n::t(drawing2d::kindWord(r.kind))).arg(r.index);
      id = QString::fromStdString(r.body.substr(0, 8));
    } else if (r.kind == opad::Ref::Kind::Point) {
      title = tr("Point");
      subtitle = QString::fromStdString(r.str());
    } else if (r.kind == opad::Ref::Kind::Body) {
      const opad::Node* n = m_doc->node(r.body);
      title = m_doc->nodeName(r.body);
      // Where it sits: the components above it, or its kind at the top (the path ended in its own name, so a top-level
      // body read "Loft1 / Loft1").
      QStringList path;
      for (const auto& p : m_doc->scene.path_to(r.body)) if (p != r.body) path << m_doc->nodeName(p);
      subtitle = !path.isEmpty() ? path.join(QString::fromUtf8(" › ")) : n && n->kind == opad::Node::Kind::Component ? tr("Component") : tr("Body");
      if (n && n->kind == opad::Node::Kind::Body) {
        auto it = m_doc->scene.instance_count.find(n->body_key);
        if (it != m_doc->scene.instance_count.end() && it->second > 1) subtitle += tr(" · %1 instances").arg(it->second);
      }
      id = QString::fromStdString(r.body.substr(0, 8));
    } else {
      QString kind = i18n::t(opad::Ref::kind_name(r.kind));
      QString geo = QString::fromStdString(j.value("surface", j.value("curve", std::string())));
      title = QString::fromUtf8("%1%2%3").arg(kind.left(1).toUpper() + kind.mid(1), geo.isEmpty() ? QString() : QString::fromUtf8(" · "), geo);
      subtitle = QString::fromUtf8("%1 › %2 %3").arg(m_doc->nodeName(r.body), kind).arg(r.index);
      id = QString::fromStdString(r.body.substr(0, 8));
    }
    if (refs.size() > 1) subtitle += tr("  (+%1 more)").arg(refs.size() - 1);
    m_propsPanel->setContext(r.kind == opad::Ref::Kind::Body || r.kind == opad::Ref::Kind::Point ? title : subtitle);
    m_props->showEntity(title, subtitle, id, j);
    if (r.kind == opad::Ref::Kind::Body && node && !node->body_missing) showNodeGeometry(r.body, title, subtitle, id);
  } catch (const std::exception& e) {
    m_props->showEntity(tr("Error"), QString::fromUtf8(e.what()), QString(), opad::json::object());
  }
}

// The live selection is published for agents (F25): opad-cli selection / opad.run("selection").
void MainWindow::writeSelectionFile() {
  if (m_selFileJob) m_selFileJob->cancel();
  if (m_doc->browse) return;  // viewer mode: no agent channel
  struct State {
    std::vector<opad::Ref> refs;
    opad::json sel = opad::json::array();
    size_t i = 0;
  };
  auto st = std::make_shared<State>();
  st->refs = m_viewport->selection();
  const size_t kDetailCap = 200;  // inspect geometry for at most this many; the rest are listed by ref only
  m_selFileJob = m_jobs->sliced(tr("Publishing selection"), [this, st, kDetailCap](Job&) {
    if (st->i >= st->refs.size()) return false;
    const opad::Ref& r = st->refs[st->i];
    opad::json e;
    e["ref"] = r.str();
    e["node"] = m_doc->nodeName(r.body).toStdString();
    if (st->i < kDetailCap) {
      try {
        opad::json info;
        if (r.kind == opad::Ref::Kind::Body) {
          // Bodies get O(1) descriptors: volume/area need exact integration (seconds for a heavy body),
          // so agents ask `inspect` for those on demand. Faces/edges are cheap to inspect fully.
          info = opad::node_properties(m_doc->doc, m_doc->scene, r.body, false);
          Bnd_Box b = opad::node_world_bbox(m_doc->doc, m_doc->scene, r.body);
          if (!b.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            b.Get(x0, y0, z0, x1, y1, z1);
            info["bbox"] = {{"min", {x0, y0, z0}}, {"max", {x1, y1, z1}}, {"size", {x1 - x0, y1 - y0, z1 - z0}}, {"center", {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2}}};
          }
        } else {
          info = opad::inspect_ref(m_doc->doc, m_doc->scene, r);
        }
        for (const char* k : {"type", "surface", "curve", "bbox", "normal", "axis", "radius", "center", "area", "volume", "length", "key"})
          if (info.contains(k)) e[k] = info[k];
      } catch (const std::exception&) {
      }
    }
    st->sel.push_back(std::move(e));
    return ++st->i < st->refs.size();
  }, [this, st](bool completed) {
    m_selFileJob = nullptr;
    if (!completed) return;  // a newer selection superseded this one
    try {
      opad::json j;
      j["pid"] = static_cast<long long>(QCoreApplication::applicationPid());
      j["document"] = m_doc->path().toStdString();
      j["browse"] = m_doc->browse;
      j["ts"] = opad::now_iso8601();
      j["selection"] = std::move(st->sel);
      opad::write_text_file(opad::cache_dir() / "selection.json", j.dump(2));
    } catch (const std::exception&) {
    }
  });
}

void MainWindow::showContextMenu(const QPoint& globalPos, std::vector<std::string> ids) {
  if(auto* instances=findChild<ToolPanel*>("instanceBrowser"))instances->hide();
  QMenu menu(this);
  buildContextMenu(menu, ids);
  menu.exec(globalPos);
}

// What the menu is about (UI-100): faces, edges or vertices picked in the view, else the objects (bodies, components,
// sketches), else nothing. Each kind gets its own entries; Repeat of the last tool comes first in every one.
void MainWindow::buildContextMenu(QMenu& menu, const std::vector<std::string>& ids) {
  SelectionContext context = selectionContext();  // for the areas' entries: the objects the menu is about
  context.ids = ids;
  auto add = [&](const char* id) { if (QAction* a = action(id)) menu.addAction(a); };  // viewer mode: editing entries ask to save first
  auto entry = [&](const QString& icon, const QString& text, const char* name, std::function<void()> fn) {
    QAction* a = icon.isEmpty() ? menu.addAction(text) : menu.addAction(icons::themed(icon, 16), text);
    a->setObjectName(name);
    connect(a, &QAction::triggered, this, [this, fn] { guarded(fn); });
    return a;
  };
  auto title = [&](const QString& text) { menu.addSection(text)->setObjectName("contextTitle"); };  // areas insert after it
  if (QAction* repeat = repeatAction()) {
    menu.addAction(repeat);
    menu.addSeparator();
  }
  if(m_design->sketchActive()) {
    add("sketch.finish");  // a sketch is left from its menu too (it was only in the ribbon and the panel)
    add("sketch.cancel");
    menu.addSeparator();
    if(!ids.empty()) {
      auto* sketch=m_design->sketch();
      menu.addAction(sketch->visible()?tr("Hide sketch"):tr("Show sketch"),this,[sketch]{sketch->setVisible(!sketch->visible());});
      menu.addAction(action("sketch.replane"));menu.addAction(action("view.alignPlane"));
      forEachArea([&](AreaController* area) { area->contextMenu(context, menu); });
      return;
    }
    for(const char* id:{"sketch.construction","sketch.dimension","sketch.c.horizontal","sketch.c.vertical","sketch.c.coincident","sketch.c.tangent","sketch.c.fix","sketch.node","sketch.openEnds"})menu.addAction(action(id));
    menu.addSeparator();menu.addAction(tr("Driving / reference"),m_design->sketch(),&SketchEditor::toggleReference);
    menu.addAction(tr("Delete"),m_design->sketch(),&SketchEditor::deleteSelection);
    forEachArea([&](AreaController* area) { area->contextMenu(context, menu); });
    return;
  }
  const opad::Scene& scene = m_doc->scene;
  auto all = [&](auto pred) { return !ids.empty() && std::all_of(ids.begin(), ids.end(), pred); };
  auto picked = [&](opad::Ref::Kind kind) {
    return !context.refs.empty() && std::all_of(context.refs.begin(), context.refs.end(), [kind](const opad::Ref& r) { return r.kind == kind; });
  };
  const bool faces = picked(opad::Ref::Kind::Face), edges = picked(opad::Ref::Kind::Edge), vertices = picked(opad::Ref::Kind::Vertex);
  const bool sketches = !faces && !edges && !vertices && all([&](const std::string& id) { return scene.sketch(id) != nullptr; });
  const bool components = !faces && !edges && !vertices && all([&](const std::string& id) { const opad::Node* n = scene.node(id); return n && n->kind == opad::Node::Kind::Component; });
  const QString one = ids.size() == 1 ? m_doc->nodeName(ids.front()) : QString();
  // The op that made the objects (one for all of them): Edit it when it is a feature or a sketch, find it on the timeline.
  std::string source;
  for (const auto& id : ids) {
    const opad::Node* n = scene.node(id);
    const std::string op = n ? n->source_op : scene.sketch(id) ? id : std::string();
    source = source.empty() || source == op ? op : std::string("-");
  }
  const opad::Op* sourceOp = source.empty() || source == "-" ? nullptr : m_doc->doc.find_op(source);
  auto history = [&] {
    if (!sourceOp || m_doc->browse) return;
    const QString name = m_timeline->label(*sourceOp);
    if ((scene.feature(source) || scene.sketch(source)) && !sketches)
      entry("rename", tr("Edit %1").arg(name), "contextEditSource", [this, source] { if (requireEditable()) m_design->editOp(source); });
    entry("locate", tr("Find %1 in the timeline").arg(name), "contextFind", [this, source] {
      if (!m_timelineDock->isVisible()) m_timelineDock->show();
      m_timeline->setCurrentOp(source);
      m_timeline->pulse(source);
    });
  };
  auto hideOthers = [&] {
    entry("hide", tr("Hide others"), "contextHideOthers", [this, ids] {
      std::set<std::string> keep;
      for (const auto& id : ids) for (const auto& b : m_doc->scene.bodies_under(id)) keep.insert(b);
      std::vector<std::string> hide;
      for (const auto& b : m_doc->scene.all_bodies())
        if (!keep.count(b) && m_doc->node(b)->visible) hide.push_back(b);
      if (!hide.empty()) m_doc->run("appearance", opad::json{{"targets", hide}, {"visible", false}});  // one step for all of them
    });
  };
  auto looks = [&] {
    entry("dot", tr("Colour…"), "contextColour", [this, ids] {  // a view setting in viewer mode too
      // From the object's own colour, and one step to undo for all of them (it was one per object).
      QColor c = QColorDialog::getColor(nodeColour(ids.front()), this, tr("Colour"));
      if (!c.isValid()) return;
      m_doc->run("appearance", opad::json{{"targets", ids}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
    });
    const opad::Node* n = m_doc->node(ids.front());
    entry("lock", n && n->locked ? tr("Unlock") : tr("Lock"), "contextLock", [this, ids, locked = n && n->locked] {
      m_doc->run("appearance", opad::json{{"targets", ids}, {"locked", !locked}});
    });
  };
  // Picked faces and edges as Del does (UI-04), through smart selection; objects by what the selection covers.
  auto remove = [&](bool picks) {
    const QString key = "\t" + action("edit.delete")->shortcut().toString(QKeySequence::NativeText);
    QString text = tr("Delete") + key;
    if (!picks) {
      const smart::Deletion d = smart::routeDelete(scene, ids);
      const QString what = sketches && ids.size() == 1 ? QString::fromStdString(scene.sketch(ids.front())->name) : !one.isEmpty() ? one : tr("%1 objects").arg(ids.size());
      text = (d.remove.empty() ? tr("Delete %1") : tr("Remove %1")).arg(what) + key;
    }
    QAction* del = entry("delete", text, "contextDelete", [this, ids, picks] {
      if (!requireEditable()) return;
      if (picks) {
        if (!areaCommand("edit.delete")) throw opad::Error("Faces and edges are deleted through the feature that made them: select it with Ctrl+Up, or use Remove faces.");
        return;
      }
      deleteNodes(ids);
    });
    if (!picks && !smart::routeDelete(scene, ids).remove.empty()) del->setToolTip(tr("A Remove step at the end of the timeline takes it out; the history that made it stays"));
  };
  auto ofBody = [&] {  // what a pick is on
    entry("body", ids.size() == 1 ? tr("Select %1").arg(one) : tr("Select the %1 bodies").arg(ids.size()), "contextSelectBody", [this, ids] {
      std::vector<opad::Ref> bodies(ids.size());
      for (size_t i = 0; i < ids.size(); ++i) bodies[i].body = ids[i];
      m_areaServices.select(bodies);
    });
    add("view.isolate");
    add("edit.hide");
  };
  if (faces || edges || vertices) {
    const size_t n = context.refs.size();
    if (faces) title(n == 1 ? tr("Face of %1").arg(m_doc->nodeName(context.refs.front().body)) : tr("%1 faces").arg(n));
    else if (edges) title(n == 1 ? tr("Edge of %1").arg(m_doc->nodeName(context.refs.front().body)) : tr("%1 edges").arg(n));
    else title(n == 1 ? tr("Vertex of %1").arg(m_doc->nodeName(context.refs.front().body)) : tr("%1 vertices").arg(n));
    menu.addSeparator();
    if (faces && n == 1) {  // a sketch on it, the view square to it (both take the picked face as their plane)
      entry("sketch", tr("Sketch on this face"), "contextSketchOn", [this] { action("design.sketch")->trigger(); });
      entry("plane", tr("Look at this face"), "contextLookAt", [this] { action("view.alignPlane")->trigger(); });
    }
    if (faces) add("design.offset_face");
    if (edges) {
      add("design.fillet");
      add("design.chamfer");
    }
    menu.addSeparator();
    add("inspect.distance");
    if (!vertices) add("inspect.angle");
    if (!vertices) add("inspect.radius");
    add("inspect.properties");
    add("annotate.add");
    menu.addSeparator();
    ofBody();
    if (!vertices) {
      menu.addSeparator();
      remove(true);
    }
  } else if (!ids.empty() && sketches) {
    title(ids.size() == 1 ? QString::fromStdString(scene.sketch(ids.front())->name) : tr("%1 sketches").arg(ids.size()));
    if (ids.size() == 1 && !m_doc->browse) {
      entry("rename", tr("Edit sketch"), "contextEditSketch", [this, id = ids.front()] { if (requireEditable()) m_design->editOp(id); });
      entry("plane", tr("Redefine sketch plane"), "contextReplane", [this, id = ids.front()] {
        m_design->editOp(id);
        m_design->redefineSketchPlane();
      });
    }
    history();
    entry("export", tr("Export sketch"), "contextExportSketch", [this, ids] { exportDialog(ids); });
    menu.addSeparator();
    remove(false);
  } else if (!ids.empty()) {
    title(!one.isEmpty() ? one : components ? tr("%1 components").arg(ids.size()) : tr("%1 objects").arg(ids.size()));
    entry("fit", tr("Fit to"), "contextFit", [this, ids] { m_viewport->fitNodes(ids); });
    add("view.isolate");
    hideOthers();
    add("edit.hide");
    menu.addSeparator();
    history();
    if(std::any_of(ids.begin(),ids.end(),[this](const auto& id){for(const auto& body:m_doc->scene.bodies_under(id))if(m_doc->scene.node(body)->representation=="drawing2d")return true;return false;}))add("design.convertDrawing");
    menu.addSeparator();
    add("edit.rename");
    looks();
    if (!components) add("design.move");
    if (components && ids.size() == 1) add("design.newcomponent");
    const opad::Node* n = m_doc->node(ids.front());
    if (n && !n->parent.empty()) add("edit.selectparent");
    if(ids.size()==1 && n && !n->body_key.empty() && m_doc->scene.instance_count[n->body_key]>1)
      menu.addAction(tr("Browse linked instances"),this,[this,id=ids.front()]{browseInstances(id);});
    entry("export", tr("Export selected objects"), "contextExport", [this, ids] { exportDialog(ids); });
    menu.addSeparator();
    add("annotate.add");
    add("annotate.draw");
    add("inspect.distance");
    if (!components) add("inspect.radius");
    add("inspect.properties");
    menu.addSeparator();
    remove(false);
  } else {
    add("view.fit");
    add("view.home");
    add("view.unisolate");
    add("edit.showall");
    menu.addSeparator();
    add("design.sketch");
    add("file.import");
  }
  forEachArea([&](AreaController* area) { area->contextMenu(context, menu); });
}

// The last tool started (a feature, a sketch tool, a measurement, a note): offered first in the context menus as
// "Repeat Fillet". Null when there is none, it is off now or still running.
QAction* MainWindow::repeatAction() {
  QAction* last = m_lastCommand.isEmpty() ? nullptr : action(m_lastCommand);
  QAction* repeat = action("edit.repeat");
  if (!last || !repeat || !last->isEnabled() || (last->isCheckable() && last->isChecked())) return nullptr;
  QString label = last->text().split('\t').front();
  label.remove('&');
  repeat->setText(tr("Repeat %1").arg(label));
  repeat->setIcon(last->icon());
  return repeat;
}

bool MainWindow::repeatable(const QString& id) const {
  if (id.startsWith("sketch.")) {
    const QAction* a = action(id);
    return a && a->property("sketchTool").isValid() && a->property("sketchTool").toString() != "select";
  }
  if (id.startsWith("design.")) return id == "design.sketch" || opad::design::feature_spec(id.mid(7).toStdString()) != nullptr;
  static const QStringList tools = {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.printcheck", "inspect.interference",
                                    "annotate.add", "annotate.draw", "select.similar"};
  return tools.contains(id);
}

// The bbox of a component walks every body under it; it is added to the panel by a sliced job.
void MainWindow::showNodeGeometry(const std::string& id, const QString& title, const QString& subtitle, const QString& nid) {
  if (m_propsJob) m_propsJob->cancel();
  auto document = std::make_shared<opad::Document>(m_doc->doc);
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  auto result = std::make_shared<opad::json>();
  const auto generation = m_doc->generation;
  const opad::Node* node = m_doc->node(id);
  const bool drawing = m_viewport->drawingWords() && node && node->representation == "drawing2d";  // 2D words (UI-118)
  m_propsJob = m_jobs->async(tr("Measuring %1").arg(title), [document, scene, id, result, drawing](Progress p) {
    *result = opad::node_properties(*document, *scene, id, true, [p] { return p.cancelled(); });
    if (drawing) *result = drawing2d::properties(std::move(*result));
  }, [this, result, title, subtitle, nid, generation](bool ok, const QString&) {
    m_propsJob = nullptr;
    if (!ok || generation != m_doc->generation) return;
    m_props->showEntity(title, subtitle, nid, *result);
  });
}

void MainWindow::scheduleSelectionSync() { m_selFileTimer.start(); }
