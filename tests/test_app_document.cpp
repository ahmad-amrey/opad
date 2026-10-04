#include "AppDocument.hpp"
#include "check.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>

// Document tests do not need the GUI's timing sink or widgets.
namespace trace { bool enabled() { return false; } void log(const QString&) {} }

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 2) return 1;
  try {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString step = QString::fromLocal8Bit(argv[1]) + "/box.step";
    AppDocument doc;
    doc.open(step);
    CHECK(doc.hasDocument && !doc.browse && doc.isDirty());
    CHECK(doc.path().isEmpty());
    CHECK(!doc.doc.has_live_bodies());
    const auto bodies = doc.scene.all_bodies().size();
    CHECK(bodies > 0);
    doc.saveAs(tmp.path() + "/opened.opad");
    CHECK(!doc.isDirty());
    doc.importStep(step);
    CHECK_EQ(doc.scene.all_bodies().size(), bodies * 2);
    doc.open(step);
    CHECK_EQ(doc.scene.all_bodies().size(), bodies);
    CHECK_THROWS(doc.open(tmp.path() + "/missing.step"));
    CHECK_EQ(doc.scene.all_bodies().size(), bodies);
    const QString broken = tmp.path() + "/broken.step";
    opad::write_text_file(broken.toStdString(), "ISO-10303-21;\nDATA;\n#1=broken(");
    CHECK_THROWS(doc.open(broken));
    CHECK_EQ(doc.scene.all_bodies().size(), bodies);
    QEventLoop loop;
    bool success = false;
    QObject::connect(&doc, &AppDocument::loadFinished, &loop, [&](bool ok, const QString&) { success = ok; loop.quit(); });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    doc.startOpen(step);  // viewer mode: read-only, nothing to save, view changes allowed
    loop.exec();
    CHECK(success && doc.browse && !doc.isDirty() && doc.doc.has_live_bodies());
    const std::string first = doc.scene.all_bodies().front();
    CHECK_THROWS(doc.run("rename", opad::json{{"target", first}, {"name", "x"}}));
    doc.run("appearance", opad::json{{"target", first}, {"visible", false}});
    CHECK(!doc.isDirty() && !doc.scene.node(first)->visible);
    CHECK_THROWS(doc.saveAs(tmp.path() + "/viewer.opad"));
    doc.viewerOpens = false;  // the setting off: other formats open as editable, unsaved documents
    doc.startOpen(step);
    loop.exec();
    CHECK(success && doc.isDirty() && !doc.browse);
    doc.saveAs(tmp.path() + "/async.opad");
    CHECK_EQ(opad::resolve(opad::Document::load((tmp.path() + "/async.opad").toStdString())).all_bodies().size(), bodies);
    const auto liveCache=doc.doc.shape_cache;
    doc.startImport(step);
    CHECK(doc.doc.shape_cache==liveCache);
    CHECK_EQ(opad::resolve(doc.doc).all_bodies().size(),bodies);
    loop.exec();
    CHECK(success);
    CHECK_EQ(doc.scene.all_bodies().size(),bodies*2);
    // Undo/redo several steps at once (the quick-access Undo ▾ / Redo ▾): the labels the next one first, one refresh.
    {
      const std::string body = doc.scene.all_bodies().front();
      const size_t ops = doc.doc.ops.size();
      doc.run("rename", opad::json{{"target", body}, {"name", "one"}});
      doc.run("rename", opad::json{{"target", body}, {"name", "two"}});
      doc.run("appearance", opad::json{{"target", body}, {"visible", false}});
      std::vector<std::string> ids;
      for (const auto& op : doc.doc.ops) ids.push_back(op.id);
      CHECK(doc.undoLabels().mid(0, 3) == QStringList({AppDocument::tr("hide"), AppDocument::tr("rename"), AppDocument::tr("rename")}) && doc.redoLabels().isEmpty());
      int changes = 0;
      auto counted = QObject::connect(&doc, &AppDocument::changed, &doc, [&changes] { ++changes; });
      doc.undo(2);
      CHECK(changes == 1 && doc.doc.ops.size() == ops + 1 && doc.nodeName(body) == "one" && doc.scene.node(body)->visible);
      CHECK(doc.redoLabels() == QStringList({AppDocument::tr("rename"), AppDocument::tr("hide")}) && doc.undoLabels().first() == AppDocument::tr("rename"));
      doc.redo(5);  // more than there are: all of them
      std::vector<std::string> again;
      for (const auto& op : doc.doc.ops) again.push_back(op.id);
      CHECK(changes == 2 && again == ids && doc.nodeName(body) == "two" && !doc.scene.node(body)->visible && doc.redoLabels().isEmpty());
      QObject::disconnect(counted);
      doc.undo(3);
      CHECK_EQ(doc.doc.ops.size(), ops);
      // Several commands as one step (UI-34, Component from selection): one undo, one refresh; a refused one takes the
      // others back.
      const size_t steps = doc.undoLabels().size(), roots = doc.scene.roots.size();
      const std::string home = doc.scene.node(body)->parent;
      std::string group;
      int refreshes = 0;
      counted = QObject::connect(&doc, &AppDocument::changed, &doc, [&refreshes] { ++refreshes; });
      doc.batch("group", [&] {
        group = doc.run("component", opad::json{{"name", "Group"}}).value("id", "");
        doc.run("reparent", opad::json{{"targets", {body}}, {"parent", group}});
        CHECK(!doc.scene.node(group));  // not resolved in between
      });
      QObject::disconnect(counted);
      CHECK(refreshes == 1 && doc.undoLabels().size() == steps + 1 && doc.undoLabel() == "group" && doc.scene.node(body)->parent == group);
      doc.undo();
      CHECK(doc.doc.ops.size() == ops && !doc.scene.node(group) && doc.scene.node(body)->parent == home);
      doc.redo();
      CHECK(doc.scene.node(body)->parent == group);
      doc.undo();
      doc.run("appearance", opad::json{{"target", body}, {"locked", true}});
      const size_t locked = doc.doc.ops.size();
      CHECK_THROWS(doc.batch("refused", [&] {
        doc.run("component", opad::json{{"name", "Group"}});
        doc.run("reparent", opad::json{{"targets", {body}}, {"parent", nullptr}});  // locked: refused
      }));
      CHECK(doc.doc.ops.size() == locked && doc.undoLabels().size() == steps + 1 && doc.scene.roots.size() == roots);
      doc.undo();
      CHECK_EQ(doc.doc.ops.size(), ops);
    }
    // The active component (UI-33): session state, checked after every change; imports go into it.
    {
      CHECK(doc.activeComponent().empty());
      int changes = 0;
      auto counted = QObject::connect(&doc, &AppDocument::activeComponentChanged, &doc, [&changes] { ++changes; });
      CHECK_THROWS(doc.setActiveComponent(doc.scene.all_bodies().front()));  // a body is no component
      CHECK_THROWS(doc.setActiveComponent("no-such-node"));
      const size_t ops = doc.doc.ops.size();
      const std::string lid = doc.run("component", opad::json{{"name", "Lid"}}).value("id", "");
      doc.setActiveComponent(lid);
      doc.setActiveComponent(lid);  // the same again: no signal
      CHECK(doc.activeComponent() == lid && changes == 1);
      doc.run("rename", opad::json{{"target", lid}, {"name", "Cover"}});  // edits keep it
      doc.run("transform", opad::json{{"target", lid}, {"matrix", opad::Mat4::translation(100, 0, 0).to_json()}});
      CHECK(doc.activeComponent() == lid);
      doc.startImport(step);  // no parent given: the active component
      loop.exec();
      CHECK(success && doc.doc.ops.back().type == "import" && doc.doc.ops.back().data.value("parent", "") == lid);
      // A drawing placed in world coordinates keeps its place under the moved component: its placement is made relative.
      const QString svg = tmp.path() + "/plate.svg";
      opad::write_text_file(svg.toStdString(), R"(<svg width="40mm" viewBox="0 0 40 40"><rect width="20" height="10"/></svg>)");
      doc.startImport(svg, {}, opad::Mat4::translation(0, 0, 5));
      loop.exec();
      const opad::Op& drawing = doc.doc.ops.back();
      CHECK(success && drawing.type == "import" && drawing.data.value("parent", "") == lid);
      const opad::Mat4 placed = doc.scene.world(drawing.data["nodes"][0].value("id", ""));
      CHECK(std::abs(placed.at(0, 3)) < 1e-9 && std::abs(placed.at(2, 3) - 5) < 1e-9);
      doc.setRollback(doc.scene.node(lid)->source_op);  // rolled back to before it was made: still active
      CHECK(doc.activeComponent() == lid && !doc.scene.node(lid) && changes == 1);
      doc.setRollback({});
      doc.undo(int(doc.doc.ops.size() - ops));  // gone: the root is active
      CHECK(doc.activeComponent().empty() && changes == 2 && doc.doc.ops.size() == ops);
      doc.redo(1);
      doc.setActiveComponent(lid);
      doc.newDocument();  // another document: the root
      CHECK(doc.activeComponent().empty() && changes == 4);
      QObject::disconnect(counted);
    }
    // The user's roll-back (the timeline's playhead, UI-99): an editor takes it over and gives it back as it ends, also after
    // committing an edit of an earlier step (shown from there); a step of another kind rolls forward to the end.
    {
      doc.newDocument();
      auto box = [&](const char* name, double z) {
        return doc.run("feature", opad::json{{"kind", "box"}, {"name", name}, {"inputs", {{"plane", {{"origin", {0, 0, z}}, {"normal", {0, 0, 1}}}}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "5 mm"}}}})["feature_id"].get<std::string>();
      };
      const std::string a = box("A", 0), b = box("B", 20), c = box("C", 40);
      doc.rollBackTo(c);
      CHECK(doc.rolledBack() && doc.rollback() == c && !doc.scene.feature(c));
      doc.setRollback(a);  // an editor on A
      CHECK(!doc.rolledBack() && doc.rollback() == a);
      doc.setRollback({});  // cancelled
      CHECK(doc.rolledBack() && doc.rollback() == c);
      doc.setRollback({});  // a second end (the editor's clean-up) leaves the user's roll-back alone
      CHECK(doc.rolledBack() && doc.rollback() == c);
      doc.setRollback(b);
      auto edit = [&](const std::string& op, const char* height) {  // as the feature panel commits it (the op itself rolled back)
        opad::json inputs = doc.doc.find_op(op)->data["inputs"];
        inputs["height"] = height;
        doc.commitPlan(opad::design::plan_ops(doc.doc, {opad::design::make_edit_op(op, {{"inputs", inputs}})}), "edit");
      };
      edit(b, "8 mm");
      doc.setRollback({});
      CHECK(doc.rolledBack() && doc.rollback() == c && doc.scene.feature(b)->inputs["height"] == "8 mm" && !doc.scene.feature(c));
      edit(a, "6 mm");  // rolled back by the user, an edit of an earlier step: still rolled back
      CHECK(doc.rolledBack() && doc.rollback() == c && doc.scene.feature(a)->inputs["height"] == "6 mm");
      doc.run("rename", opad::json{{"target", doc.scene.all_bodies().front()}, {"name", "x"}});  // anything else: at the end
      CHECK(!doc.rolledBack() && doc.rollback().empty() && doc.scene.feature(c));
      // How a body looks is view state: a later hide shows on an earlier state where the body is (one whose body comes later
      // is left out quietly), and hiding while rolled back keeps the roll-back.
      auto bodyOf = [&](const std::string& op) {
        for (const auto& id : doc.scene.all_bodies())
          if (doc.scene.node(id)->source_op == op) return id;
        return std::string();
      };
      const std::string bodyA = bodyOf(a), bodyB = bodyOf(b), bodyC = bodyOf(c);
      CHECK(!bodyA.empty() && !bodyB.empty() && !bodyC.empty());
      doc.run("appearance", opad::json{{"target", bodyA}, {"visible", false}});
      doc.run("appearance", opad::json{{"target", bodyC}, {"visible", false}});
      doc.rollBackTo(c);
      CHECK(doc.rolledBack() && !doc.scene.node(bodyA)->visible && !doc.scene.node(bodyC) && doc.scene.unresolved.empty());
      doc.run("appearance", opad::json{{"target", bodyB}, {"color", {1.0, 0.0, 0.0}}});
      CHECK(doc.rolledBack() && doc.rollback() == c && doc.scene.node(bodyB)->has_color && !doc.scene.feature(c));
      doc.undo(3);
      CHECK(doc.rolledBack() && doc.scene.node(bodyA)->visible && !doc.scene.node(bodyB)->has_color);
      doc.rollBackTo(b);
      doc.setRollback(a);
      doc.commitPlan(opad::design::plan_ops(doc.doc, {opad::design::make_feature_op("box", "D", {{"length", "4 mm"}, {"width", "4 mm"}, {"height", "4 mm"}})}), "box");
      doc.setRollback({});  // the editor committed a new step: it ends at the end
      CHECK(!doc.rolledBack() && doc.rollback().empty() && doc.scene.feature(c));
      doc.rollBackTo(c);
      doc.setRollback(a);
      doc.newDocument();  // another document: neither roll-back is kept
      CHECK(!doc.rolledBack() && doc.rollback().empty());
      doc.setRollback({});
      CHECK(!doc.rolledBack() && doc.rollback().empty());
    }
    // Several commands as one step and one refresh (UI-02); a failing one takes the batch back off the log.
    {
      const auto all = doc.scene.all_bodies();
      const size_t ops = doc.doc.ops.size(), steps = doc.undoLabels().size();
      int changes = 0;
      auto counted = QObject::connect(&doc, &AppDocument::changed, &doc, [&changes] { ++changes; });
      doc.batch(AppDocument::tr("hide others"), [&] {
        doc.run("appearance", opad::json{{"targets", {all[0], all[1]}}, {"visible", false}});
        doc.batch("inner", [&] { doc.run("appearance", opad::json{{"target", all[0]}, {"opacity", 0.5}}); });  // part of the outer one
        doc.run("rename", opad::json{{"target", all[1]}, {"name", "kept"}});
      });
      CHECK(changes == 1 && doc.doc.ops.size() == ops + 4 && doc.undoLabels().size() == steps + 1 && doc.undoLabel() == AppDocument::tr("hide others"));
      CHECK(!doc.scene.node(all[0])->visible && doc.scene.node(all[0])->opacity == 0.5 && doc.nodeName(all[1]) == "kept");
      doc.undo();
      CHECK(changes == 2 && doc.doc.ops.size() == ops && doc.scene.node(all[0])->visible && doc.scene.node(all[1])->visible);
      doc.redo();
      CHECK(doc.doc.ops.size() == ops + 4 && !doc.scene.node(all[1])->visible);
      doc.undo();
      CHECK_THROWS(doc.batch("broken", [&] {
        doc.run("appearance", opad::json{{"target", all[0]}, {"visible", false}});
        doc.run("rename", opad::json{{"target", all[0]}});  // no name: throws
      }));
      CHECK(doc.doc.ops.size() == ops && doc.scene.node(all[0])->visible && doc.undoLabels().size() == steps && doc.canRedo());
      doc.batch("nothing", [] {});
      CHECK(doc.undoLabels().size() == steps && doc.canRedo());  // no step, the redo kept
      QObject::disconnect(counted);
    }
    // What a change changed (UI-40): the appearance, name, placement or parent of the targets of the ops added, undone or
    // redone; anything else is the whole document. A view then looks at the bodies under those nodes alone.
    {
      const auto all = doc.scene.all_bodies();
      const std::vector<std::string> both = {all[0], all[1]};
      auto only = [&doc](const std::vector<std::string>& nodes) { return !doc.lastChange().whole && doc.lastChange().nodes == nodes; };
      doc.run("appearance", opad::json{{"targets", both}, {"visible", false}});
      CHECK(only(both));
      doc.undo();
      CHECK(only(both) && doc.scene.node(all[0])->visible);
      doc.redo();
      CHECK(only(both) && !doc.scene.node(all[1])->visible);
      doc.batch("moved", [&] {
        doc.run("rename", opad::json{{"target", all[1]}, {"name", "moved"}});
        doc.run("transform", opad::json{{"target", all[0]}, {"matrix", opad::Mat4::translation(1, 2, 3).to_json()}});
      });
      CHECK(only({all[1], all[0]}));
      doc.undo(2);
      CHECK(only({all[1], all[0], all[0], all[1]}) && doc.scene.node(all[0])->visible);
      doc.run("section", opad::json{{"name", "Cut"}, {"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}});
      CHECK(doc.lastChange().whole);
      doc.undo();
      CHECK(doc.lastChange().whole);
      doc.redo();
      CHECK(doc.lastChange().whole);
      doc.undo();
    }
    // A worker's document for the nodes a click inspects (UI-51): the shared shape cache, their entries without BREP text.
    {
      const auto all = doc.scene.all_bodies();
      const std::string key = doc.scene.node(all[0])->body_key;
      const auto shapes = doc.shapesOf({all[0]});
      CHECK(shapes->shape_cache == doc.doc.shape_cache && shapes->body_count() == 1 && shapes->bodies()[0].brep.empty() && !doc.doc.body(key)->brep.empty());
      CHECK(!opad::body_shape(*shapes, key).IsNull());
    }
    // Load progress (UI-40): a drawing's read is placed after the scan (it sat at 0-2 % and then jumped to 70).
    {
      const QString dxf = tmp.path() + "/lines.dxf";
      std::string text = "0\nSECTION\n2\nENTITIES\n";
      for (int i = 0; i < 10000; ++i) {
        const std::string x = std::to_string(i % 100 * 5), y = std::to_string(i / 100 * 5), x2 = std::to_string(i % 100 * 5 + 3);
        text += "0\nLINE\n8\nGrid\n10\n" + x + "\n20\n" + y + "\n30\n0\n11\n" + x2 + "\n21\n" + y + "\n31\n0\n";
      }
      opad::write_text_file(dxf.toStdString(), text + "0\nENDSEC\n0\nEOF\n");
      int read = 0, early = 0;
      auto watched = QObject::connect(&doc, &AppDocument::loadProgress, &doc, [&](const QString& phase, int, int overall) {
        if (phase == "reading drawing") ++read, early += overall < 10;
      });
      doc.viewerOpens = true;
      doc.startOpen(dxf);
      loop.exec();
      QObject::disconnect(watched);
      CHECK(success && read > 0 && early == 0);
    }
    int resets=0;
    QObject::connect(&doc,&AppDocument::aboutToReplace,&doc,[&]{++resets;});
    const auto generation=doc.generation;
    doc.newDocument();
    CHECK_EQ(resets,1); CHECK_EQ(doc.generation,generation+1);
    CHECK(doc.scene.all_bodies().empty());
    doc.closeDocument(); CHECK_EQ(resets,2);

    // UI-56: the file changes on disk while it is open; Save never overwrites that silently.
    const QString file = tmp.path() + "/opened.opad";
    const std::filesystem::path fs(file.toStdU16String());
    AppDocument live;
    live.open(file);
    CHECK(!live.isDirty() && QFileInfo(live.diskFile()) == QFileInfo(file) && !live.diskChanged());
    const std::string body = live.scene.all_bodies().front();
    auto external = [&](opad::json op) {  // another program appends to the file
      opad::Document d = opad::Document::load(fs);
      op["target"] = body;
      d.append(op);
      d.save();
    };
    auto fileIds = [&] { std::vector<std::string> ids; for (const auto& o : opad::Document::load(fs).ops) ids.push_back(o.id); return ids; };
    auto liveIds = [&] { std::vector<std::string> ids; for (const auto& o : live.doc.ops) ids.push_back(o.id); return ids; };
    external({{"op", "rename"}, {"name", "Theirs"}});
    CHECK(live.diskChanged());
    const std::string before = opad::read_text_file(fs);
    int blocked = 0;
    QObject::connect(&live, &AppDocument::saveBlocked, &live, [&] { ++blocked; });
    CHECK(!live.save() && blocked == 1);
    CHECK(!live.saveAs(file) && blocked == 2);  // the open file chosen again
    CHECK(opad::read_text_file(fs) == before);
    auto read = AppDocument::readDisk(file, live.diskBase(), live.doc.shape_cache);
    CHECK(read.relation == opad::Relation::extends && read.doc && read.doc->body_count() == 0 && !read.bodies.empty());
    CHECK(live.planDisk(read).mine.empty());
    live.mergeDisk(std::move(read), "from disk");  // nothing unsaved: a fast-forward
    CHECK(!live.isDirty() && live.nodeName(body) == "Theirs" && live.undoLabel() == "from disk" && !live.diskChanged() && liveIds() == fileIds());
    live.run("rename", opad::json{{"target", body}, {"name", "Mine"}});
    external({{"op", "appearance"}, {"visible", false}});
    read = AppDocument::readDisk(file, live.diskBase(), live.doc.shape_cache);
    const auto plan = live.planDisk(read);
    CHECK(plan.error.empty() && plan.mine.size() == 1 && plan.incoming == 1 && plan.conflicts.empty());
    live.mergeDisk(std::move(read), "from disk 2");
    CHECK(live.isDirty() && live.nodeName(body) == "Mine" && !live.scene.node(body)->visible && live.doc.ops.back().type == "rename");
    CHECK(live.save() && !live.isDirty() && liveIds() == fileIds());
    live.undo();  // the unsaved rename, then the merge
    CHECK(live.nodeName(body) == "Theirs" && !live.scene.node(body)->visible);
    live.undo();
    CHECK(live.scene.node(body)->visible && live.undoLabel() == "from disk");
    {  // a reset: the history no longer continues the session's
      opad::Document d = opad::Document::load(fs);
      d.truncate_ops(d.ops.size() - 1);
      opad::write_text_file(fs, d.serialize());
    }
    read = AppDocument::readDisk(file, live.diskBase(), live.doc.shape_cache);
    CHECK(read.relation == opad::Relation::rewritten && !live.planDisk(read).error.empty());
    CHECK_THROWS(live.mergeDisk(std::move(read), "x"));
    read = AppDocument::readDisk(file, live.diskBase(), live.doc.shape_cache);
    live.reloadDisk(std::move(read));
    CHECK(!live.isDirty() && !live.canUndo() && liveIds() == fileIds() && !live.diskChanged());
    live.run("rename", opad::json{{"target", body}, {"name", "Mine again"}});
    external({{"op", "rename"}, {"name", "Theirs again"}});
    CHECK(!live.save() && live.save(true));  // Overwrite, once asked
    CHECK(liveIds() == fileIds() && opad::resolve(opad::Document::load(fs)).node(body)->name == "Mine again");
    std::filesystem::remove(fs);  // deleted: nothing to lose, Save writes it again
    CHECK(!live.diskChanged() && live.save() && std::filesystem::exists(fs));
    // Read with nothing to compare it with (restored into a file that was missing, now back): never taken as the same.
    CHECK(AppDocument::readDisk(file, nullptr, {}).relation == opad::Relation::rewritten);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
