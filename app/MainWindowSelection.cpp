// Selection: viewport <-> browser, the Properties panel, selection.json for agents, the context menu.
#include "MainWindow.hpp"
#include "AgentBridge.hpp"

#include <QColorDialog>
#include <QCoreApplication>
#include <QMenu>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>

#include <Bnd_Box.hxx>

#include "I18n.hpp"
#include "Icons.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

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
  else m_statusSel->setText(tr("%1 selected · %2").arg(refs.size()).arg(i18n::t(opad::Ref::kind_name(refs.front().kind))));
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
  m_statusSel->setText(!refs.empty() ? tr("%1 selected · body").arg(refs.size()) : ids.empty() ? QString() : tr("%1 selected").arg(ids.size()));
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
    // A sub-shape's details walk its body (inspect_ref): the kind and the body now, the rest from a worker (UI-51).
    const bool subShape = r.kind != opad::Ref::Kind::Body && r.kind != opad::Ref::Kind::Point;
    opad::json j = r.kind == opad::Ref::Kind::Body ? opad::node_properties(m_doc->doc, m_doc->scene, r.body, false)
                 : subShape ? opad::json{{"ref", r.str()}, {"type", opad::Ref::kind_name(r.kind)}, {"body", r.body}, {"body_name", m_doc->nodeName(r.body).toStdString()}, {"index", r.index}}
                            : opad::inspect_ref(m_doc->doc, m_doc->scene, r);
    QString title, subtitle, id;
    if (r.kind == opad::Ref::Kind::Point) {
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
    if (subShape) showRefGeometry(r, subtitle, id);
  } catch (const std::exception& e) {
    m_props->showEntity(tr("Error"), QString::fromUtf8(e.what()), QString(), opad::json::object());
  }
}

// The live selection is published for agents (F25: opad-cli selection, opad.run("selection")) only while agent access is
// on (UI-06): with none, a rubber band over 71,818 faces spent seconds of the UI thread inspecting them for nobody. At most
// kPublishedRefs refs, each with what is known at once (ref, node, type, body key, the cached world box of a body); an
// agent inspects what it needs. The JSON is built and written on a worker; a newer selection's write wins.
void MainWindow::writeSelectionFile() {
  if (m_selFileJob) m_selFileJob->cancel();
  if (m_doc->browse || !m_agent || !m_agent->publishesSelection()) return;
  constexpr size_t kPublishedRefs = 2000;
  struct Entry {
    std::string ref, node, type, key;
    opad::Mat4 world;
    Bnd_Box box;  // body-local, cached by the load worker
  };
  auto entries = std::make_shared<std::vector<Entry>>();
  const size_t total = m_selRefs.size();
  for (size_t i = 0; i < std::min(total, kPublishedRefs); ++i) {
    const opad::Ref& r = m_selRefs[i];
    Entry e;
    e.ref = r.str();
    e.node = m_doc->nodeName(r.body).toStdString();
    e.type = opad::Ref::kind_name(r.kind);
    if (const opad::Node* n = m_doc->node(r.body); n && r.kind == opad::Ref::Kind::Body) {
      e.type = n->kind == opad::Node::Kind::Body ? "body" : "component";
      if (n->kind == opad::Node::Kind::Body && !n->body_missing) {
        e.key = n->body_key;
        e.world = m_doc->scene.world(r.body);
        try {
          e.box = opad::body_bbox(m_doc->doc, n->body_key);
        } catch (const std::exception&) {
        }
      }
    } else if (r.kind == opad::Ref::Kind::Body && m_doc->scene.sketch(r.body)) {
      e.type = "sketch";
    }
    entries->push_back(std::move(e));
  }
  opad::json head{{"pid", static_cast<long long>(QCoreApplication::applicationPid())}, {"document", m_doc->path().toStdString()},
                  {"browse", m_doc->browse}, {"total", total}, {"truncated", total > kPublishedRefs}};
  static std::mutex writing;  // one write at a time, and only the newest selection's
  static std::atomic<unsigned> newest{0};
  const unsigned mine = ++newest;
  m_selFileJob = m_jobs->async(tr("Publishing selection"), [entries, head, mine](Progress p) {
    opad::json j = head;
    j["ts"] = opad::now_iso8601();
    opad::json& sel = j["selection"] = opad::json::array();
    for (const Entry& e : *entries) {
      if (p.cancelled()) return;
      opad::json out{{"ref", e.ref}, {"node", e.node}, {"type", e.type}};
      if (!e.key.empty()) out["key"] = e.key;
      if (!e.box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        e.box.Get(x0, y0, z0, x1, y1, z1);
        opad::Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        for (int c = 0; c < 8; ++c) {
          const opad::Vec3 q = e.world.apply({(c & 1) ? x1 : x0, (c & 2) ? y1 : y0, (c & 4) ? z1 : z0});
          for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
        }
        out["bbox"] = {{"min", lo}, {"max", hi}, {"size", {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}},
                       {"center", {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2}}};
      }
      sel.push_back(std::move(out));
    }
    const std::string text = j.dump(2);
    std::lock_guard<std::mutex> lock(writing);
    if (mine == newest) opad::write_text_file(opad::cache_dir() / "selection.json", text);
  }, [this](bool, const QString&) { m_selFileJob = nullptr; }, JobKind::Background);
}

void MainWindow::showContextMenu(const QPoint& globalPos, std::vector<std::string> ids) {
  SelectionContext context = selectionContext();  // for the areas' entries: the objects the menu is about
  context.ids = ids;
  if(auto* instances=findChild<ToolPanel*>("instanceBrowser"))instances->hide();
  if(m_design->sketchActive()) {
    QMenu menu(this);
    if(!ids.empty()) {
      auto* sketch=m_design->sketch();
      menu.addAction(sketch->visible()?tr("Hide sketch"):tr("Show sketch"),this,[sketch]{sketch->setVisible(!sketch->visible());});
      menu.addAction(action("sketch.replane"));menu.addAction(action("view.alignPlane"));
      forEachArea([&](AreaController* area) { area->contextMenu(context, menu); });
      menu.exec(globalPos);return;
    }
    for(const char* id:{"sketch.construction","sketch.dimension","sketch.c.horizontal","sketch.c.vertical","sketch.c.coincident","sketch.c.tangent","sketch.c.fix","sketch.node","sketch.openEnds"})menu.addAction(action(id));
    menu.addSeparator();menu.addAction(tr("Driving / reference"),m_design->sketch(),&SketchEditor::toggleReference);
    menu.addAction(tr("Delete"),m_design->sketch(),&SketchEditor::deleteSelection);
    forEachArea([&](AreaController* area) { area->contextMenu(context, menu); });
    menu.exec(globalPos);return;
  }
  QMenu menu(this);
  auto add = [&](const char* id) { if (QAction* a = action(id)) menu.addAction(a); };  // viewer mode: editing entries ask to save first
  if (!ids.empty()) {
    menu.addSection(ids.size() == 1 ? m_doc->nodeName(ids.front()) : tr("%1 objects").arg(ids.size()));
    QAction* fit = menu.addAction(icons::themed("fit", 16), tr("Fit to"));
    connect(fit, &QAction::triggered, this, [this, ids] { m_viewport->fitNodes(ids, true); });
    if(std::any_of(ids.begin(),ids.end(),[this](const auto& id){for(const auto& body:m_doc->scene.bodies_under(id))if(m_doc->scene.node(body)->representation=="drawing2d")return true;return false;}))add("design.convertDrawing");
    if(ids.size()==1 && m_doc->scene.sketch(ids.front())) {
      menu.addAction(tr("Redefine sketch plane"),this,[this,id=ids.front()]{m_design->editOp(id);m_design->redefineSketchPlane();});
    }
    auto* exportObject=menu.addAction(icons::themed("export",16),tr("Export selected objects"));
    connect(exportObject,&QAction::triggered,this,[this,ids] { guarded([&] { exportDialog(ids); }); });
    add("edit.selectparent");
    add("view.isolate");
    QAction* others = menu.addAction(icons::themed("hide", 16), tr("Hide others"));
    connect(others, &QAction::triggered, this, [this, ids] { guarded([&] { hideOthers(ids); }); });
    add("edit.hide");
    add("edit.rename");
    QAction* color = menu.addAction(icons::themed("dot", 16), tr("Colour…"));  // a view setting in viewer mode too
    connect(color, &QAction::triggered, this, [this, ids] {
      // From the object's own colour, and one step to undo for all of them (it was one per object).
      QColor c = QColorDialog::getColor(nodeColour(ids.front()), this, tr("Colour"));
      if (!c.isValid()) return;
      m_doc->run("appearance", opad::json{{"targets", ids}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
    });
    const opad::Node* n = m_doc->node(ids.front());
    QAction* lock = menu.addAction(icons::themed("lock", 16), n && n->locked ? tr("Unlock") : tr("Lock"));
    if(ids.size()==1 && n && !n->body_key.empty() && m_doc->scene.instance_count[n->body_key]>1)
      menu.addAction(tr("Browse linked instances"),this,[this,id=ids.front()]{browseInstances(id);});
    connect(lock, &QAction::triggered, this, [this, ids, locked = n && n->locked] {
      m_doc->run("appearance", opad::json{{"targets", ids}, {"locked", !locked}});
    });
    menu.addSeparator();
    add("annotate.add");
    add("annotate.draw");
    add("inspect.distance");
    add("inspect.radius");
    add("inspect.properties");
    menu.addSeparator();
    QAction* del = menu.addAction(icons::themed("delete", 16), tr("Delete (tombstone import)"));
    connect(del, &QAction::triggered, this, [this, ids] {
      if (!requireEditable()) return;
      std::set<std::string> ops;
      for (const auto& id : ids) if (const opad::Node* nn = m_doc->node(id)) ops.insert(nn->source_op);
      deleteOps({ops.begin(), ops.end()});
    });
  } else {
    add("view.fit");
    add("view.home");
    add("view.unisolate");
    add("edit.showall");
    menu.addSeparator();
    add("file.import");
  }
  forEachArea([&](AreaController* area) { area->contextMenu(context, menu); });
  menu.exec(globalPos);
}

// The bbox of a component walks every body under it; it is added to the panel by a sliced job.
void MainWindow::showNodeGeometry(const std::string& id, const QString& title, const QString& subtitle, const QString& nid) {
  if (m_propsJob) m_propsJob->cancel();
  auto document = m_doc->shapesOf({id});
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  auto result = std::make_shared<opad::json>();
  const auto generation = m_doc->generation;
  m_propsJob = m_jobs->async(tr("Measuring %1").arg(title), [document, scene, id, result](Progress p) {
    *result = opad::node_properties(*document, *scene, id, true, [p] { return p.cancelled(); });
  }, [this, result, title, subtitle, nid, generation](bool ok, const QString&) {
    m_propsJob = nullptr;
    if (!ok || generation != m_doc->generation) return;
    m_props->showEntity(title, subtitle, nid, *result);
  });
}

void MainWindow::showRefGeometry(const opad::Ref& ref, const QString& subtitle, const QString& nid) {
  if (m_propsJob) m_propsJob->cancel();
  auto document = m_doc->shapesOf({ref.body});
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  auto result = std::make_shared<opad::json>();
  const auto generation = m_doc->generation;
  m_propsJob = m_jobs->async(tr("Inspecting %1").arg(subtitle), [document, scene, ref, result](Progress) {
    *result = opad::inspect_ref(*document, *scene, ref);
  }, [this, ref, result, subtitle, nid, generation](bool ok, const QString& error) {
    m_propsJob = nullptr;
    if (generation != m_doc->generation || error == "cancelled") return;
    if (!ok) return m_props->showEntity(tr("Error"), error, QString(), opad::json::object());
    const QString kind = i18n::t(opad::Ref::kind_name(ref.kind));
    const QString geo = QString::fromStdString(result->value("surface", result->value("curve", std::string())));
    m_props->showEntity(QString::fromUtf8("%1%2%3").arg(kind.left(1).toUpper() + kind.mid(1), geo.isEmpty() ? QString() : QString::fromUtf8(" · "), geo), subtitle, nid, *result);
  });
}

void MainWindow::scheduleSelectionSync() { m_selFileTimer.start(); }
