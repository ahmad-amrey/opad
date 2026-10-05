// OPAD_BENCH_INSPECT (TODO 11 wave 3, help audit P8): the measuring tools do what their guides show, on two boxes apart.
// Distance started from the Body filter with nothing selected picks faces (its guide clicks two faces): the filter turns
// to Faces, the steps ask for faces and two clicks on the boxes measure face to face; the tool ended, the Body filter is
// back; with a body selected first the Body filter stays and the body is the first pick. Bounding box takes picks one by
// one: its step says each click adds bodies (the filter's), a second box clicked grows the box around both, Back (Esc)
// takes the last pick back and the box shrinks to the first. Distance then Angle straight after it: the Body filter is back
// when Angle ends.
#include <QCoreApplication>
#include <QHideEvent>

#include <cmath>
#include <functional>
#include <map>
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
  script->add("Distance ended", [&w, require] {
    require(w.m_toolPicks[0].kind == opad::Ref::Kind::Face && w.m_toolPicks[1].kind == opad::Ref::Kind::Face && w.m_lastMeasure.value("value", -1.0) > 0,
            QString("two clicks on the boxes measure face to face: %1 mm").arg(w.m_lastMeasure.value("value", -1.0)));
    w.cancelTool();
  }, [&w, v, idle] { return v->selectionFilter() == Viewport::SelFilter::Body && w.action("select.bodies")->isChecked() && idle(); });
  script->add("bodies selected first", [&w, v, require, bodies] {
    require(!w.action("select.faces")->isChecked(), "the tool ended, the Body filter it switched from is back");
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
    require(steps.size() == 1 && steps[0].label == MainWindow::tr("Select bodies · each click adds"),
            "Bounding box asks for picks one after another, what the filter picks: " + (steps.isEmpty() ? QString() : steps[0].label));
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
  script->add("Bounding box ended", [&w, require, size, sizes] {
    require(size(10, 10, 10), "Back (Esc) takes the last pick back: the cube's box again " + sizes());
    w.cancelTool();
  }, [v, idle] { return v->selection().empty() && idle(); });
  script->add("Distance then Angle", [&w] { w.startTool("distance"); },
              [&w, v, idle] { return w.m_tool.id == "distance" && v->selectionFilter() == Viewport::SelFilter::Face && idle(); });
  script->add("Angle ended", [&w] {
    w.startTool("angle");  // straight from Distance, in the Faces filter it set
    w.cancelTool();
  }, [&w, v, idle] { return w.m_tool.id.isEmpty() && v->selectionFilter() == Viewport::SelFilter::Body && w.action("select.bodies")->isChecked() && idle(); });
  script->add("done", [require] { require(true, "a tool started straight from another: the Body filter is back when the second ends"); });
  bench2d::Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}

// OPAD_BENCH_CHECK=interference|print (TODO 10 B13, B17; help audit P8): the check runs through its panel as it opens. Print:
// the findings are coloured on the model at once (overhangs in the warning amber, thin walls in the error red, over
// everything in TopOSD, after a body selected whole in Topmost), a clicked finding is selected in the selection's blue and
// taken out of the colours. Interference: nothing is drawn until a row is clicked, then the pair is selected and their
// overlap shown in the error red over them (TopOSD). Minimising the window (the panel's spontaneous hide) leaves the
// colours, the overlap and the check as they were. Interference: a row clicked and Check pressed at once never draws that
// row's overlap later; the overlap goes with its pair when one of them is pulled away in an exploded view, and is drawn
// where the pair is when both are. The findings follow the model while the panel is open: a body moved (a transform op)
// takes the colours or the overlap off at once and the check runs again; print: the same colours, 20 mm along X with the
// body; interference: the pair moved apart, nothing left to list, then with a clearance a "too close" row shows the gap
// as a dimension, which a move takes away with the rest. A print check on more than one body: isolating another one takes
// the colours off the hidden body, ending the isolation brings them back (the view's own change, no new check). Print: an
// exploded view takes the colours along with the body, a look hiding it takes them off, both ended brings them back where
// they were. Closing the panel takes it all away.
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
  auto coloured = [overlays] { return overlays().value("overhang_triangles", 0) + overlays().value("thin_triangles", 0); };
  auto settled = [&w, overlays] { return !w.m_checkJob && !w.m_overlapJob && !w.m_jobs->busy() && !overlays().value("tinting", false); };
  // A look layer set and every displayed body showing it (the looks pass done, the colours made again after it).
  auto look = [&w, v, settled](LookSource source, std::map<std::string, LookDelta> deltas, std::function<void(bool)> then) {
    auto applied = std::make_shared<bool>(false);
    QObject::connect(v, &Viewport::looksApplied, &w, [applied] { *applied = true; }, Qt::SingleShotConnection);
    v->setLookLayer(source, std::move(deltas));
    bench2d::pollUntil(&w, [applied, settled] { return *applied && settled(); }, 20000, std::move(then));
  };
  auto offsetBy = [](const opad::json& was, const opad::json& is, double dz) {
    return was.is_array() && is.is_array() && was.size() == 6 && is.size() == 6 && std::abs(is[0].get<double>() - was[0].get<double>()) < 1e-3 &&
           std::abs(is[2].get<double>() - was[2].get<double>() - dz) < 1e-3 && std::abs(is[5].get<double>() - was[5].get<double>() - dz) < 1e-3;
  };
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
    const opad::json first = w.m_checks->findings().empty() ? opad::json() : w.m_checks->findings().front();
    // The first finding's body (print) or the pair's second body (interference).
    const std::string body = print ? first.value("body", "") : first.value("b", "");
    auto shown = [=, &w] {
      const opad::json now = overlays();
      return !v->selection().empty() && settled() && (print || now.value("overlap", false));
    };
    // The model changes while the panel stays open: that body moved 20 mm (print) or 40 mm (interference) along X.
    auto move = std::make_shared<std::function<void()>>();
    *move = [=, &w] {
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
        const char* boxName = overhangs ? "overhang_box" : "thin_box";
        if (print) {
          // The same faces coloured, where the body is now.
          const opad::json was = before->value(boxName, opad::json()), is = now.value(boxName, opad::json());
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
        auto close = [=, &w] {
          w.m_toolPanel->hide();
          const opad::json after = overlays();
          require(after.value("overhang_triangles", 1) == 0 && after.value("thin_triangles", 1) == 0 && !after.value("overlap", true) && !after.value("gap", true),
                  "closing the panel takes the colours, the overlap and the gap away");
          QCoreApplication::exit(*all ? 0 : 2);
        };
        if (!print) {
          // Apart now, the pair is only close: with a clearance it is listed, and its row shows the gap as a dimension.
          w.m_checks->setClearance(100);
          w.runCheck();
          bench2d::pollUntil(&w, settled, 30000, [=, &w](bool) {
            const auto& rows = w.m_checks->findings();
            const bool tooClose = rows.size() == 1 && rows.front().value("kind", "") == "clearance";
            require(tooClose, QString("with a 100 mm clearance the pair is listed as too close: %1 rows").arg(rows.size()));
            if (!tooClose) return QCoreApplication::exit(2);
            w.m_checks->activate(0);
            bench2d::pollUntil(&w, [=] { return settled() && overlays().value("gap", false); }, 20000, [=, &w](bool gap) {
              require(gap && !overlays().value("overlap", true), "a too-close row shows the pair's gap as a dimension, no overlap");
              try {
                w.m_doc->run("transform", {{"target", body}, {"matrix", opad::Mat4::translation(400, 0, 0).to_json()}});
              } catch (const std::exception&) {
              }
              require(!overlays().value("gap", true) && w.m_checkJob, "moved again: the gap goes at once with the findings it belonged to");
              bench2d::pollUntil(&w, settled, 30000, [=, &w](bool) {
                require(w.m_checks->findingCount() == 0 && !overlays().value("gap", true), "checked again: nothing close any more, no dimension left on the model");
                close();
              });
            });
          });
          return;
        }
        // Print: an exploded view takes the colours with the body; a look hiding it takes them off; both ended: back as they were.
        const int tinted = overhangs + thin;
        auto looks = [=, &w] {
          const opad::json at = overlays();
          LookDelta lifted;
          lifted.offset = {0, 0, 30};
          look(LookSource::Explode, {{body, lifted}}, [=, &w](bool) {
            const opad::json now = overlays();
            require(coloured() == tinted && offsetBy(at.value(boxName, opad::json()), now.value(boxName, opad::json()), 30) && !w.m_checkJob,
                    QString("an exploded view lifts the body 30 mm: its colours go with it, no new check: %1 -> %2")
                        .arg(QString::fromStdString(at.value(boxName, opad::json()).dump()), QString::fromStdString(now.value(boxName, opad::json()).dump())));
            LookDelta hidden;
            hidden.visible = false;
            look(LookSource::Activation, {{body, hidden}}, [=, &w](bool) {
              require(coloured() == 0 && !w.m_checkJob, QString("a look hides the body: its colours go too (%1 triangles left)").arg(coloured()));
              v->clearLookLayer(LookSource::Activation);
              look(LookSource::Explode, {}, [=, &w](bool) {
                const opad::json back = overlays();
                require(coloured() == tinted && offsetBy(at.value(boxName, opad::json()), back.value(boxName, opad::json()), 0),
                        QString("shown and assembled again: the colours are back where they were (%1 triangles)").arg(coloured()));
                close();
              });
            });
          });
        };
        std::string other;
        for (const auto& id : w.m_doc->scene.all_bodies()) if (id != body) other = id;
        if (other.empty()) return looks();
        v->isolate({other});
        bench2d::pollUntil(&w, settled, 20000, [=, &w](bool) {
          const opad::json hidden = overlays();
          require(!w.m_checkJob && hidden.value("overhang_triangles", 0) + hidden.value("thin_triangles", 0) == 0,
                  "another body isolated: the hidden body's colours go, the check is not run again " + QString::fromStdString(hidden.dump()));
          v->isolate({});
          bench2d::pollUntil(&w, [=] { return settled() && coloured() > 0; }, 20000, [=, &w](bool back) {
            require(back && !w.m_checkJob && coloured() == tinted, QString("isolation ended: its colours are back (%1 triangles)").arg(coloured()));
            looks();
          });
        });
      });
    };
    if (findings > 0) w.m_checks->activate(0);
    bench2d::pollUntil(&w, shown, 20000, [=, &w](bool ok) {
      const opad::json now = overlays();
      if (print)
        require(ok && now.value("overhang_triangles", 0) + now.value("thin_triangles", 0) < overhangs + thin,
                QString("a clicked finding is selected (%1 picks) and leaves the colours to the others: %2 of %3 triangles coloured")
                    .arg(v->selection().size()).arg(now.value("overhang_triangles", 0) + now.value("thin_triangles", 0)).arg(overhangs + thin));
      else
        require(ok && v->selection().size() == 2 && sameColour(now.value("overlap_colour", opad::json()), theme::current().error) &&
                    now.value("overlap_layer", 0) == int(Graphic3d_ZLayerId_TopOSD),
                "a clicked pair is selected and their overlap shown in the error red over them: " + QString::fromStdString(now.dump()));
      if (!shot.isEmpty()) {
        v->grabImage().save(shot + ".finding.png");
        w.m_toolPanel->grab().save(shot + ".check.png");
      }
      // Minimised: the panel is hidden spontaneously (isVisible() stays true), and nothing it shows goes.
      QHideEvent minimised;
      QCoreApplication::sendEvent(w.m_toolPanel, &minimised);
      const opad::json kept = overlays();
      require(w.m_toolPanel->isVisible() && w.m_checks->findingCount() == findings && kept.value("overlap", false) == now.value("overlap", false) &&
                  kept.value("overhang_triangles", 0) + kept.value("thin_triangles", 0) == now.value("overhang_triangles", 0) + now.value("thin_triangles", 0),
              "minimising the window leaves the panel, its findings and what they show on the model: " + QString::fromStdString(kept.dump()));
      if (print) return (*move)();
      // A row clicked and Check pressed at once: that row's overlap, still being computed, never shows up later.
      w.m_checks->activate(-1);  // a click on the row that is current already changes nothing
      w.m_checks->activate(0);
      const bool computing = w.m_overlapJob != nullptr;
      w.runCheck();
      bench2d::pollUntil(&w, settled, 30000, [=, &w](bool) {
        require(computing && w.m_checks->findingCount() == findings && !overlays().value("overlap", true),
                "a row clicked, then Check at once: the check runs again and the row's overlap is not drawn over the new list");
        w.m_checks->activate(0);
        bench2d::pollUntil(&w, shown, 20000, [=, &w](bool again) {
          require(again, "the row clicked again shows its overlap");
          const std::string a = first.value("a", "");
          LookDelta apart;
          apart.offset = {0, 0, 50};
          look(LookSource::Explode, {{body, apart}}, [=, &w](bool) {
            require(!overlays().value("overlap", true), "exploded, one of the pair lifted 50 mm: the overlap is not drawn between them");
            look(LookSource::Explode, {{a, apart}, {body, apart}}, [=, &w](bool) {
              const opad::json both = overlays();
              const opad::json at = both.value("overlap_offset", opad::json());
              require(both.value("overlap", false) && at.is_array() && at.size() == 3 && std::abs(at[2].get<double>() - 50) < 1e-6,
                      "both lifted 50 mm: the overlap is drawn where the pair is " + QString::fromStdString(both.dump()));
              look(LookSource::Explode, {}, [=, &w](bool) {
                const opad::json home = overlays().value("overlap_offset", opad::json());
                require(overlays().value("overlap", false) && home.is_array() && home.size() == 3 && std::abs(home[2].get<double>()) < 1e-6,
                        "assembled again: the overlap is back in place");
                (*move)();
              });
            });
          });
        });
      });
    });
  });
  return true;
}
