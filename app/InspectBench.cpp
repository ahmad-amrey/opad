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

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "CheckPanel.hpp"
#include "Drawing2DBench.hpp"
#include "GuidedTool.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
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

// OPAD_BENCH_CHECK=interference|print (TODO 10 B13, B17; help audit P8): the check runs through its panel as it opens. Print:
// the findings are coloured on the model at once (overhangs in the warning amber, thin walls in the error red, over
// everything in TopOSD, after a body selected whole in Topmost), a clicked finding is selected in the selection's blue and
// taken out of the colours. Interference: nothing is drawn until a row is clicked, then the pair is selected and their
// overlap shown in the error red over them (TopOSD). The findings follow the model while the panel is open: a body moved
// (a transform op) takes the colours or the overlap off at once and the check runs again; print: the same colours, 20 mm
// along X with the body; interference: the pair moved apart, nothing left to list. Closing the panel takes it all away.
// OPAD_BENCH_UISHOT: <shot>.check.png (the panel), <shot>.view.png (after the check), <shot>.finding.png (after the click),
// <shot>.moved.png (after the move and the new check).
OPAD_BENCH(OPAD_BENCH_CHECK, check) {
  const bool print = value == "print";
  auto all = std::make_shared<bool>(true);
  auto require = [all, value](bool ok, const QString& what) {
    trace::log(QStringLiteral("bench: %1 check: %2 %3").arg(value, what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT");
  Viewport* v = w.m_viewport;
  // A colour as the overlays report it (sRGB) against a theme token.
  auto sameColour = [](const opad::json& c, const QColor& q) {
    return c.is_array() && c.size() == 3 && std::abs(c[0].get<double>() - q.redF()) < 0.02 && std::abs(c[1].get<double>() - q.greenF()) < 0.02 &&
           std::abs(c[2].get<double>() - q.blueF()) < 0.02;
  };
  auto overlays = [v] { return v->benchCheckOverlays(); };
  w.startCheck(print);
  auto before = std::make_shared<opad::json>();
  bench2d::pollUntil(&w, [&w, overlays] { return !w.m_checkJob && !overlays().value("tinting", false); }, 30000, [=, &w](bool checked) {
    const int findings = w.m_checks->findingCount();
    require(checked && findings > 0, QString("the check ran as its panel opened: %1 findings").arg(findings));
    *before = overlays();
    const int overhangs = before->value("overhang_triangles", 0), thin = before->value("thin_triangles", 0);
    if (print)
      require(overhangs + thin > 0 && !before->value("overlap", true) &&
                  (!overhangs || (sameColour(before->value("overhang_colour", opad::json()), theme::current().warning) && before->value("overhang_layer", 0) == int(Graphic3d_ZLayerId_TopOSD))) &&
                  (!thin || (sameColour(before->value("thin_colour", opad::json()), theme::current().error) && before->value("thin_layer", 0) == int(Graphic3d_ZLayerId_TopOSD))),
              "the findings are coloured on the model at once, overhangs in the warning amber and thin walls in the error red, on top: " + QString::fromStdString(before->dump()));
    else
      require(!before->value("overlap", true) && before->value("overhang_triangles", 1) == 0, "nothing is drawn before a pair is clicked");
    if (!shot.isEmpty()) v->grabImage().save(shot + ".view.png");
    if (findings > 0) w.m_checks->activate(0);
    bench2d::pollUntil(&w, [=, &w] {
      const opad::json now = overlays();
      return !v->selection().empty() && !w.m_jobs->busy() && !now.value("tinting", false) && (print || now.value("overlap", false));
    }, 20000, [=, &w](bool shown) {
      const opad::json now = overlays();
      if (print)
        require(shown && now.value("overhang_triangles", 0) + now.value("thin_triangles", 0) < overhangs + thin,
                QString("a clicked finding is selected (%1 picks) and leaves the colours to the others: %2 of %3 triangles coloured")
                    .arg(v->selection().size()).arg(now.value("overhang_triangles", 0) + now.value("thin_triangles", 0)).arg(overhangs + thin));
      else
        require(shown && v->selection().size() == 2 && sameColour(now.value("overlap_colour", opad::json()), theme::current().error) &&
                    now.value("overlap_layer", 0) == int(Graphic3d_ZLayerId_TopOSD),
                "a clicked pair is selected and their overlap shown in the error red over them: " + QString::fromStdString(now.dump()));
      if (!shot.isEmpty()) {
        v->grabImage().save(shot + ".finding.png");
        w.m_toolPanel->grab().save(shot + ".check.png");
      }
      // The model changes while the panel stays open: the first finding's body (print) or the pair's second body
      // (interference) moved 20 mm along X.
      const opad::json first = w.m_checks->findings().empty() ? opad::json() : w.m_checks->findings().front();
      const std::string body = print ? first.value("body", "") : first.value("b", "");
      const double shift = print ? 20 : 40;  // the cylinder is 10 mm across, 8 mm into a 20 mm box: 40 mm clears it
      opad::json moved;
      try {
        moved = w.m_doc->run("transform", {{"target", body}, {"matrix", opad::Mat4::translation(shift, 0, 0).to_json()}});
      } catch (const std::exception& e) {
        moved = {{"error", e.what()}};
      }
      const opad::json at = overlays();
      require(!moved.contains("error") && at.value("overhang_triangles", 1) == 0 && at.value("thin_triangles", 1) == 0 && !at.value("overlap", true) &&
                  w.m_checkJob != nullptr,
              "a body moved while the panel is open: the colours and the overlap go at once and the check runs again " + QString::fromStdString(moved.dump()));
      bench2d::pollUntil(&w, [=, &w] {
        const opad::json now = overlays();
        return !w.m_checkJob && !w.m_jobs->busy() && !now.value("tinting", false) && (!print || now.value("overhang_triangles", 0) + now.value("thin_triangles", 0) > 0);
      }, 30000, [=, &w](bool rechecked) {
        const opad::json now = overlays();
        if (print) {
          // The same faces coloured, where the body is now.
          const char* box = overhangs ? "overhang_box" : "thin_box";
          const opad::json was = before->value(box, opad::json()), is = now.value(box, opad::json());
          const bool followed = was.is_array() && is.is_array() && was.size() == 6 && is.size() == 6 && std::abs(is[0].get<double>() - was[0].get<double>() - shift) < 1e-3 &&
                                std::abs(is[3].get<double>() - was[3].get<double>() - shift) < 1e-3 && std::abs(is[1].get<double>() - was[1].get<double>()) < 1e-3;
          require(rechecked && w.m_checks->findingCount() == findings && now.value("overhang_triangles", 0) == overhangs && now.value("thin_triangles", 0) == thin && followed,
                  QString("checked again: %1 findings, the same colours %2 mm along X with the body: %3 -> %4")
                      .arg(w.m_checks->findingCount()).arg(shift).arg(QString::fromStdString(was.dump()), QString::fromStdString(is.dump())));
        } else {
          require(rechecked && w.m_checks->findingCount() == 0 && !now.value("overlap", true),
                  QString("checked again: the pair moved apart, %1 findings and no overlap drawn").arg(w.m_checks->findingCount()));
        }
        if (!shot.isEmpty()) v->grabImage().save(shot + ".moved.png");
        w.m_toolPanel->hide();
        const opad::json after = overlays();
        require(after.value("overhang_triangles", 1) == 0 && after.value("thin_triangles", 1) == 0 && !after.value("overlap", true),
                "closing the panel takes the colours and the overlap away");
        QCoreApplication::exit(*all ? 0 : 2);
      });
    });
  });
  return true;
}
