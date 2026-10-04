// OPAD_BENCH_PANELS=<prefix> (TODO 11 wave 3, help audit P9): the Annotations panel and Reset layout do what their guides
// say. On a box with four notes (pinned to the body, a face, an edge and a point): a click on a card lights up exactly what
// its note is pinned to (the face's outline lies inside the body's, an edge, a point ringed), rings the card as the current
// one and leaves the selection as it was; another selection, a click in the view, Esc closing the panel and a note being
// written put it out; a note on a hidden body says so. Reset layout brings the browser and the timeline back with their
// commands ticked. <prefix>.face.png: the view with the face lit and the panel.
#include <QApplication>
#include <QDockWidget>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QStatusBar>

#include <memory>
#include <string>
#include <vector>

#include "AnnotationEditor.hpp"
#include "BenchRegistry.hpp"
#include "BrowserOverlay.hpp"
#include "BrowserPanel.hpp"
#include "Drawing2DBench.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Notes.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"

OPAD_BENCH(OPAD_BENCH_PANELS, panels) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: panels: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  auto script = std::make_shared<bench2d::Script>();
  struct State {
    std::string body, onBody, onFace, onEdge, onPoint;
    QRect bodyRect, faceRect;
  };
  auto st = std::make_shared<State>();
  auto card = [&w](const std::string& id) -> NoteCard* {
    for (NoteCard* c : w.m_annotations->findChildren<NoteCard*>())
      if (c->note().id == id) return c;
    return nullptr;
  };
  auto press = [card](const std::string& id) {  // a click on the card's free area (its text), as the mouse makes it
    NoteCard* c = card(id);
    if (!c) return false;
    const QPointF at(c->width() / 2.0, 4);
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
      QMouseEvent e(type, at, c->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(c, &e);
    }
    return true;
  };
  auto current = [&w, card](const std::string& id) {  // this card alone is ringed as the current one, and Resolve takes it
    for (NoteCard* c : w.m_annotations->findChildren<NoteCard*>())
      if (c->property("current").toBool() != (c->note().id == id)) return false;
    return card(id) && w.m_annotations->currentOpId() == id;
  };
  auto lit = [v] { return v->annotationTargetRect(); };
  auto idle = [&w, v] { return !w.m_jobs->busy() && !v->pumpJob(); };

  script->add("the box is displayed", [] {}, [v, &w, st] {
    if (v->pumpJob() || v->remainingBodies() > 0 || v->displayedCount() < 1) return false;
    const auto bodies = w.m_doc->scene.all_bodies();
    if (bodies.empty()) return false;
    st->body = bodies.front();
    return true;
  });
  script->add("four notes and the panel", [&w, v, st] {
    v->setCameraJson({{"eye", {110, -90, 90}}, {"target", {15, 10, 5}}, {"up", {0, 0, 1}}, {"scale", 90}, {"projection", "orthographic"}, {"absolute", true}});
    auto note = [&w](const std::string& anchor, const char* text) { return w.m_doc->run("annotate", {{"anchor", anchor}, {"text", text}}).value("id", ""); };
    st->onBody = note(st->body, "The whole part");
    st->onFace = note(st->body + "/face/0", "Check this face");
    st->onEdge = note(st->body + "/edge/0", "Round this edge");
    st->onPoint = note("point/45,10,5", "Leave room here");
    w.action("panel.annotations")->trigger();
  }, [&w, st, card, idle] {
    return idle() && w.m_annotationsPanel->isVisible() && card(st->onBody) && card(st->onFace) && card(st->onEdge) && card(st->onPoint);
  });
  script->add("the body's card", [press, st] { press(st->onBody); }, [&w] { return w.m_cardTarget; });
  script->add("the face's card", [=, &w] {
    st->bodyRect = lit();
    require(!st->bodyRect.isNull() && current(st->onBody) && v->selection().empty(), "a click on a body note's card lights the body up, rings the card, selects nothing");
    press(st->onFace);
  }, [&w] { return w.m_cardTarget; });
  script->add("the edge's card", [=, &w] {
    st->faceRect = lit();
    const QRect body = st->bodyRect.adjusted(-3, -3, 3, 3);
    require(!st->faceRect.isNull() && body.contains(st->faceRect) && st->faceRect.width() * st->faceRect.height() < st->bodyRect.width() * st->bodyRect.height() &&
                current(st->onFace) && v->selection().empty(),
            QString("a face note's card lights up that face, not the whole body: outline %1x%2 inside the body's %3x%4")
                .arg(st->faceRect.width()).arg(st->faceRect.height()).arg(st->bodyRect.width()).arg(st->bodyRect.height()));
    const QString shot = value;
    if (!shot.isEmpty()) {  // the view as drawn, the panel where it floats
      QImage image = v->grabImage();
      image.setDevicePixelRatio(v->devicePixelRatioF());
      QPainter painter(&image);
      painter.drawPixmap(v->mapFromGlobal(w.m_annotationsPanel->pos()), w.m_annotationsPanel->grab());
      painter.end();
      image.save(shot + ".face.png");
    }
    press(st->onEdge);
  }, [&w, lit, st] { return w.m_cardTarget && lit() != st->faceRect; });
  script->add("the point's card", [=] {
    require(!lit().isNull() && current(st->onEdge), "an edge note's card lights up its edge");
    press(st->onPoint);
  }, [&w, lit, st] { return w.m_cardTarget && lit().width() <= 1; });
  script->add("another selection", [=, &w] {
    const QPoint at = v->widgetPoint({45, 10, 5});
    require(lit().width() <= 1 && (lit().center() - at).manhattanLength() <= 2 && current(st->onPoint), "a point note's card rings its point");
    w.m_browser->setSelectedIds({st->body});
    w.onBrowserSelection({st->body});
  }, [=, &w] { return !w.m_cardTarget && lit().isNull() && v->selection().size() == 1 && idle(); });
  script->add("the face's card over a selection", [=] {
    require(true, "a selection made in the browser puts it out");
    press(st->onFace);
  }, [&w] { return w.m_cardTarget; });
  script->add("a click in the view", [=] {
    require(!lit().isNull() && v->selection().size() == 1 && v->selection().front().body == st->body, "the card lights its face and the selection stays as it was");
    const double scale = v->devicePixelRatioF();
    v->benchClickAt(int(12 * scale), int((v->height() - 12) * scale));  // empty space
  }, [=, &w] { return !w.m_cardTarget && lit().isNull() && v->selection().empty() && idle(); });
  script->add("a hidden body", [=, &w] {
    require(true, "a click in the view puts it out");
    w.m_doc->run("appearance", {{"target", st->body}, {"visible", false}});
  }, [=, &w] { return idle() && card(st->onFace) && !w.m_doc->scene.effectively_visible(st->body); });
  script->add("its face's card", [=] { press(st->onFace); });
  script->add("shown again", [=, &w] {
    const QString message = w.statusBar()->currentMessage();
    require(!w.m_cardTarget && lit().isNull() && message == MainWindow::tr("What this note is pinned to is not shown in the view (hidden or isolated away)."),
            "a note on a hidden body lights nothing and says why: " + message);
    w.m_doc->undo();
  }, [=, &w] { return idle() && card(st->onFace) && w.m_doc->scene.effectively_visible(st->body) && v->remainingBodies() == 0; });
  script->add("Esc", [=, &w] {
    press(st->onFace);
    require(w.m_cardTarget && !lit().isNull(), "shown again, the face lights up again");
    w.action("inspect.clear")->trigger();
  }, [=, &w] { return !w.m_annotationsPanel->isVisible() && !w.m_cardTarget && lit().isNull(); });
  script->add("a note being written", [=, &w] {
    require(true, "Esc closes the panel and puts it out");
    w.action("panel.annotations")->trigger();
    press(st->onFace);
    w.action("annotate.add")->trigger();
  }, [=, &w] { return w.m_annotationEditor && !w.m_cardTarget && lit().isNull(); });
  script->add("a card while a note is written", [=, &w] {
    require(true, "a note being written takes its place (it asks for its own target)");
    press(st->onFace);
  }, [=, &w] { return !w.m_cardTarget && lit().isNull(); });
  script->add("Reset layout", [=, &w] {
    require(true, "a card clicked while a note is written leaves the note's target to it");
    w.m_annotationEditor->cancel();
    w.action("panel.browser")->trigger();
    w.action("panel.timeline")->trigger();
    require(!w.action("panel.browser")->isChecked() && !w.m_browserOverlay->isVisible() && !w.action("panel.timeline")->isChecked() && !w.m_timelineDock->isVisible(),
            "Browser and Timeline hidden by their commands");
    w.action("panel.reset")->trigger();
  }, [&w] { return w.m_timelineDock->isVisible(); });
  script->add("done", [=, &w] {
    require(w.action("panel.browser")->isChecked() && w.m_browserOverlay->isVisible() == w.isVisible() && w.action("panel.timeline")->isChecked(),
            "Reset layout brings the browser and the timeline back with their commands ticked");
  });
  bench2d::Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
