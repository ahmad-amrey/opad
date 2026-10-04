// OPAD_BENCH_INSPECT (TODO 11 wave 3, help audit P8): the measuring tools do what their guides show, on two boxes apart.
// Distance started from the Body filter with nothing selected picks faces (its guide clicks two faces): the filter turns
// to Faces, the steps ask for faces and two clicks on the boxes measure face to face; with a body selected first the
// Body filter stays and the body is the first pick. Bounding box takes picks one by one: its step says each click adds,
// a second box clicked grows the box around both, Back (Esc) takes the last pick back and the box shrinks to the first.
#include <QCoreApplication>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "BenchRegistry.hpp"
#include "Drawing2DBench.hpp"
#include "GuidedTool.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "Viewport.hpp"

OPAD_BENCH(OPAD_BENCH_INSPECT, inspect) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: inspect: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  auto script = std::make_shared<bench2d::Script>();
  auto bodies = std::make_shared<std::vector<std::string>>();
  auto click = [v](const std::string& body) {
    int x = 0, y = 0;
    if (v->benchBodyPoint(body, x, y)) v->benchClickAt(x, y);
  };
  auto idle = [&w] { return !w.m_jobs->busy(); };
  script->add("the boxes are displayed", [] {}, [v, &w, bodies] {
    if (v->pumpJob() || v->remainingBodies() > 0 || v->displayedCount() < 2) return false;
    *bodies = w.m_doc->scene.all_bodies();
    return bodies->size() >= 2;
  });
  // Distance: faces from the Body filter.
  script->add("Distance from the Body filter", [&w, v] {
    w.action("select.bodies")->trigger();
    v->clearSelection();
    w.startTool("distance");
  }, [v, idle] { return v->selectionFilter() == Viewport::SelFilter::Face && idle(); });
  script->add("two faces clicked", [&w, v, require, click, bodies] {
    const QList<ToolStep> steps = w.toolSteps();
    require(w.action("select.faces")->isChecked() && !w.action("select.bodies")->isChecked() && steps.size() == 2 &&
                steps[0].label == MainWindow::tr("Select first %1").arg(i18n::t("face")),
            "Distance started from the Body filter picks faces: the Faces filter is on, its steps ask for faces (" + (steps.isEmpty() ? QString() : steps[0].label) + ")");
    click((*bodies)[0]);
    click((*bodies)[1]);
  }, [&w] { return w.m_lastMeasure.is_object() && w.m_toolPicks.size() == 2; });
  script->add("bodies selected first", [&w, v, require, bodies] {
    require(w.m_toolPicks[0].kind == opad::Ref::Kind::Face && w.m_toolPicks[1].kind == opad::Ref::Kind::Face && w.m_lastMeasure.value("value", -1.0) > 0,
            QString("two clicks on the boxes measure face to face: %1 mm").arg(w.m_lastMeasure.value("value", -1.0)));
    w.cancelTool();
    w.action("select.bodies")->trigger();
    v->selectNodes({(*bodies)[0]});
  }, [v, idle] { return v->selection().size() == 1 && v->selectionFilter() == Viewport::SelFilter::Body && idle(); });
  script->add("Distance with a body selected first", [&w] { w.startTool("distance"); }, [&w, idle] { return w.m_toolPicks.size() == 1 && idle(); });
  script->add("Bounding box", [&w, v, require, bodies] {
    require(v->selectionFilter() == Viewport::SelFilter::Body && w.m_toolPicks[0].kind == opad::Ref::Kind::Body && w.m_toolPicks[0].body == (*bodies)[0],
            "with a body selected first the Body filter stays and the body is the first pick");
    w.cancelTool();
    v->clearSelection();
    w.startTool("bbox");
  }, [&w, v, idle] { return w.m_tool.id == "bbox" && v->selection().empty() && idle(); });
  // The box's X, Y and Z sizes, mm.
  auto size = [&w](double x, double y, double z) {
    const opad::json s = w.m_lastMeasure.value("size", opad::json());
    return s.is_array() && s.size() == 3 && std::abs(s[0].get<double>() - x) < 1e-3 && std::abs(s[1].get<double>() - y) < 1e-3 && std::abs(s[2].get<double>() - z) < 1e-3;
  };
  auto sizes = [&w] { return QString::fromStdString(w.m_lastMeasure.value("size", opad::json()).dump()); };
  script->add("the cube clicked", [&w, require, click, bodies] {
    const QList<ToolStep> steps = w.toolSteps();
    require(steps.size() == 1 && steps[0].label == MainWindow::tr("Select bodies, faces or edges · each click adds"),
            "Bounding box asks for picks one after another: " + (steps.isEmpty() ? QString() : steps[0].label));
    click((*bodies)[0]);
  }, [&w] { return w.m_lastMeasure.is_object() && w.m_toolPicks.size() == 1; });
  script->add("the block clicked", [require, click, bodies, size, sizes] {
    require(size(10, 10, 10), "one click: the cube's box, 10 x 10 x 10 mm " + sizes());
    click((*bodies)[1]);
  }, [&w] { return w.m_lastMeasure.is_object() && w.m_toolPicks.size() == 2; });
  script->add("Back", [&w, require, size, sizes] {
    require(size(50, 40, 20) && w.m_pinAction->isEnabled(), "a second click adds: the box around both, 50 x 40 x 20 mm " + sizes() + ", pinnable");
    require(w.m_toolSteps->footer()->cancelText() == MainWindow::tr("Back"), "its panel's Esc button is Back: " + w.m_toolSteps->footer()->cancelText());
    w.toolEscape();
  }, [&w] { return w.m_lastMeasure.is_object() && w.m_toolPicks.size() == 1; });
  script->add("done", [&w, require, size, sizes] {
    require(size(10, 10, 10), "Back (Esc) takes the last pick back: the cube's box again " + sizes());
    w.cancelTool();
  });
  bench2d::Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
