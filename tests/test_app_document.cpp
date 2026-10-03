#include "AppDocument.hpp"
#include "check.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

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
    int resets=0;
    QObject::connect(&doc,&AppDocument::aboutToReplace,&doc,[&]{++resets;});
    const auto generation=doc.generation;
    doc.newDocument();
    CHECK_EQ(resets,1); CHECK_EQ(doc.generation,generation+1);
    CHECK(doc.scene.all_bodies().empty());
    doc.closeDocument(); CHECK_EQ(resets,2);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
