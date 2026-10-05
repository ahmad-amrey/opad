// OPAD_BENCH_COMPARE=<prefix> (CompareMode, UI-58) on tools/bench_cases/vcs.py's compare/model.opad: four parts, also in
// git's HEAD and in first.opad beside it. The bench makes Box1 40 mm long and adds Sphere1 and saves, moves Box2 and
// deletes Box3 without saving, then compares: the chips' counts, the colours of the session's bodies
// and of A's ghosts, the arrow, the change rows and the details table, ] and [, the emphasis at both ends, an eye, the
// timeline's marks, side by side (the halves, which view shows what, the cameras together, navigation over A's view, back,
// Esc, remembered), another A (the saved file, a recovery snapshot, another file), an edit while comparing, Esc; then `opad --compare
// first.opad model.opad` in a hidden child of its own (the value <prefix>.cli runs that side), which then presses Compare… in
// the Recovery offer of a snapshot of model.opad said to come from first.opad: first.opad opens and is compared with it.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "CompareMode.hpp"

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QMainWindow>
#include <QProcess>
#include <QSlider>
#include <QMouseEvent>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <cmath>
#include <functional>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "GitWatch.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "RecoveryManager.hpp"
#include "TimelineWidget.hpp"
#include "ToolPanel.hpp"

bool CompareMode::bench(const QString& prefix) {
  using Cat = ComparePanel::Category;
  using Kind = CompareVersion::Kind;
  struct State {
    size_t step = 0;
    int wait = 0, exit = -1;
    bool git = false, child = false, childDone = false;
    std::map<std::string, std::string> body;  // by name
    std::string box1Feature, box3Feature, sphereFeature;
    QString file, first, childLog;
    QProcess* process = nullptr;
  };
  auto st = std::make_shared<State>();
  const bool cli = prefix.endsWith(".cli");  // the child: opened with --compare
  Viewport* vp = m_services.viewport();
  AppDocument* doc = m_services.document();
  auto require = [](bool ok, const QString& why) { if (!ok) throw opad::Error(why.toStdString()); };
  auto pass = [](const QString& what) { trace::log("bench: compare: " + what + " PASS"); };
  auto sameColour = [](const opad::json& rgb, const QColor& c) {
    return rgb.is_array() && rgb.size() == 3 && std::abs(rgb[0].get<double>() - c.redF()) < 0.02 && std::abs(rgb[1].get<double>() - c.greenF()) < 0.02 &&
           std::abs(rgb[2].get<double>() - c.blueF()) < 0.02;
  };
  auto idle = [this, vp] { return settled() && !vp->looksPending(); };
  auto sameCamera = [](const opad::json& a, const opad::json& b) {
    for (const char* key : {"eye", "center", "up"})
      for (size_t i = 0; i < 3; ++i)
        if (std::abs(a[key][i].get<double>() - b[key][i].get<double>()) > 1e-6 * (1 + std::abs(a[key][i].get<double>()))) return false;
    return std::abs(a["scale"].get<double>() - b["scale"].get<double>()) <= 1e-6 * a["scale"].get<double>();
  };
  auto counts = [this] {
    QStringList n;
    for (int c = 0; c < ComparePanel::Categories; ++c) n << QString::number(m_panel->chip(Cat(c))->count());
    return n.join(' ');
  };
  auto partOf = [vp](const std::string& id) {
    const opad::json state = vp->benchCompareState();
    for (const auto& p : state["parts"])
      if (p.value("id", "") == id) return p;
    return opad::json::object();
  };
  auto change = [this](const std::string& kind, const std::string& what, const std::string& name) {
    const opad::json changes = this->changes();
    for (size_t i = 0; i < changes.size(); ++i)
      if (changes[i].value("kind", "") == kind && changes[i].value("change", "") == what && changes[i].value("name", "") == name) return int(i);
    return -1;
  };
  auto choose = [this](int side, Kind kind) {  // as a person picks it in the list
    QComboBox* box = m_panel->picker(side);
    for (int i = 0; i < box->count(); ++i) {
      const int v = box->itemData(i).toInt();
      if (v >= 0 && m_versions[size_t(v)].kind == kind) {
        box->setCurrentIndex(i);
        emit box->activated(i);
        return true;
      }
    }
    return false;
  };
  auto shot = [this, vp, prefix](const QString& name) {
    m_tool->grab().save(prefix + "." + name + ".panel.png");
    vp->benchDesignShot(prefix + "." + name + ".scene.png");
  };
  std::vector<std::function<bool()>> steps;
  if (prefix.startsWith("perf:")) {  // OPAD_BENCH_COMPARE=perf:<prefix> on any saved document (the Engine): timings, no fixture
    auto clock = std::make_shared<QElapsedTimer>();
    steps = {
        [=, this] {  // everything shown (in memory only: never saved), as Unhide all does
          if (doc->loading || doc->designBusy) return false;
          for (const auto& root : doc->scene.roots)
            if (!doc->scene.node(root)->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
          return true;
        },
        [=, this] {
          if (m_services.jobs()->busy() || vp->displayedCount() == 0) return false;
          const auto bodies = doc->scene.all_bodies();
          doc->run("transform", {{"target", bodies.front()}, {"matrix", {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}});
          trace::log(QStringLiteral("bench: compare: perf: %1 bodies shown").arg(vp->displayedCount()));
          return true;
        },
        [=, this] {  // the move drawn: Compare alone from here
          if (m_services.jobs()->busy()) return false;
          clock->start();
          m_services.action("vcs.compare")->trigger();
          return true;
        },
        [=, this] {
          if (relation().empty()) return false;
          trace::log(QStringLiteral("bench: compare: perf: the change list in %1 ms (%2 changes, %3 parts)").arg(clock->elapsed()).arg(changes().size()).arg(m_parts.size()));
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          trace::log(QStringLiteral("bench: compare: perf: drawn in %1 ms: %2 PASS").arg(clock->elapsed()).arg(counts()));
          clock->start();
          setSideBySide(true);
          const qint64 on = clock->restart();
          vp->benchSideState();  // a frame of both views
          const qint64 frame = clock->restart();
          setSideBySide(false);
          const qint64 off = clock->restart();
          vp->benchSideState();
          trace::log(QStringLiteral("bench: compare: perf: side by side on %1 ms, first frame of both %2 ms, off %3 ms, a frame of one %4 ms PASS")
                         .arg(on).arg(frame).arg(off).arg(clock->elapsed()));
          clock->start();
          m_services.action("inspect.clear")->trigger();
          return true;
        },
        [=, this] {
          if (m_active || vp->looksPending()) return false;
          trace::log(QStringLiteral("bench: compare: perf: closed in %1 ms PASS").arg(clock->elapsed()));
          return true;
        },
    };
  } else if (cli) {
    steps = {
        [=, this] {
          if (!idle()) return false;
          require(m_a >= 0 && m_versions[size_t(m_a)].kind == Kind::File && m_versions[size_t(m_a)].label == "first.opad", "A is first.opad, from --compare");
          require(m_versions[size_t(m_b)].kind == Kind::Session, "B is the session");
          require(counts() == "1 0 1 0 3", "counts " + counts() + " (Sphere1 added, Box1 modified)");
          pass("opad --compare first.opad model.opad: " + counts());
          // The Recovery offer of a snapshot of model.opad (the parent's) as if it came from first.opad: Compare… opens
          // first.opad and compares the snapshot with it; a snapshot whose file is gone cannot be compared.
          auto* recovery = m_services.window()->findChild<RecoveryManager*>();
          const QString snapshot = qEnvironmentVariable("OPAD_BENCH_COMPARE_SNAPSHOT"), dir = QFileInfo(doc->path()).absolutePath();
          require(recovery && QFileInfo::exists(snapshot), "a recovery snapshot from the parent: " + snapshot);
          st->first = dir + "/first.opad";
          const std::vector<RecoveryManager::Entry> entries = {{snapshot, "model", qEnvironmentVariable("OPAD_BENCH_COMPARE_SNAPSHOT_TIME"), st->first},
                                                               {snapshot, "gone", QString(), dir + "/gone.opad"}};
          std::unique_ptr<QDialog> offer(recovery->offerDialog(entries));
          auto* list = offer->findChild<QListWidget*>("recoveryList");
          auto* button = offer->findChild<QPushButton*>("recoveryCompare");
          require(list && button && button->isEnabled(), "Compare… for a snapshot whose file is there");
          list->setCurrentRow(1);
          require(!button->isEnabled() && !button->toolTip().isEmpty(), "no Compare… for a snapshot whose file is gone");
          list->setCurrentRow(0);
          offer->grab().save(prefix + ".recovery.png");
          button->click();
          require(offer->result() == 3, "Compare… ends the offer with its own answer");
          recovery->answerOffer(offer->result(), entries[0]);
          return true;
        },
        [=, this] {  // first.opad opened, then compared with the snapshot
          if (doc->loading || !idle() || QFileInfo(doc->path()) != QFileInfo(st->first)) return false;
          const CompareVersion &a = m_versions[size_t(m_a)], &b = m_versions[size_t(m_b)];
          require(a.kind == Kind::Saved && QFileInfo(a.ref) == QFileInfo(st->first) && b.kind == Kind::Recovery && b.ref == qEnvironmentVariable("OPAD_BENCH_COMPARE_SNAPSHOT"),
                  "A the file, B the snapshot: " + a.label + " / " + b.label);
          require(!relation().empty() && relation() != "unrelated" && (counts() == "1 0 1 0 3" || counts() == "1 1 1 1 1") && !vp->benchCompareState()["parts"].empty(),
                  QStringLiteral("the snapshot's changes drawn: %1 (%2)").arg(counts(), QString::fromStdString(relation())));
          pass("the Recovery offer's Compare… opens the file and compares the snapshot with it: " + counts());
          return true;
        },
    };
  } else {
    steps = {
        [=, this] {  // the session's own changes: Box2 moved, Box3 deleted
          if (doc->loading || doc->designBusy || vp->displayedCount() < 4) return false;
          st->file = doc->path();
          st->first = QFileInfo(st->file).absolutePath() + "/first.opad";
          for (const auto& id : doc->scene.all_bodies()) st->body[doc->scene.node(id)->name] = id;
          for (const auto& f : doc->scene.features) {
            if (f.name == "Box1") st->box1Feature = f.id;
            else if (f.name == "Box3") st->box3Feature = f.id;
          }
          for (const char* name : {"Box1", "Box2", "Box3", "Cylinder1"}) require(st->body.count(name), QString("a body named %1").arg(name));
          require(!st->box1Feature.empty() && !st->box3Feature.empty(), "the features Box1 and Box3");
          st->git = qEnvironmentVariable("OPAD_BENCH_COMPARE_GIT") == "1";
          doc->run("feature_edit", {{"target", st->box1Feature}, {"inputs", {{"length", "40 mm"}}}});  // saved: in the file, not in HEAD
          const opad::json sphere = doc->run("feature", {{"kind", "sphere"}, {"inputs", {{"y", "-50 mm"}, {"diameter", "10 mm"}}}});
          st->sphereFeature = sphere.value("feature_id", "");
          require(sphere.value("body_ids", opad::json::array()).size() == 1, "Sphere1 made");
          st->body["Sphere1"] = sphere["body_ids"][0].get<std::string>();
          m_services.action("file.save")->trigger();
          return true;
        },
        [=, this] {  // saved, its recovery checkpoint written, and git has looked at the file (the chip knows it is tracked)
          if (doc->isDirty() || doc->designBusy) return false;
          if (st->git && !(m_git && m_git->repo().state == git::Repo::State::Ready && m_git->repo().doc() == git::Repo::Doc::Modified)) return false;
          if (RecoveryManager::snapshotsOf(RecoveryManager::recoveryRoot(), doc->doc.header.uuid).empty()) return false;
          doc->run("transform", {{"target", st->body["Box2"]}, {"matrix", {1, 0, 0, 0, 0, 1, 0, 20, 0, 0, 1, 0, 0, 0, 0, 1}}});
          doc->run("delete", {{"target", st->box3Feature}});
          require(doc->isDirty(), "unsaved changes");
          QAction* compare = m_services.action("vcs.compare");
          require(compare && compare->isEnabled(), "the Compare versions… command");
          compare->trigger();
          require(m_active && m_tool->isVisible(), "Compare opens its panel");
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          const CompareVersion& a = m_versions[size_t(m_a)];
          require(a.kind == (st->git ? Kind::Git : Kind::Saved), "A: " + a.label);
          require(m_versions[size_t(m_b)].kind == Kind::Session, "B: the session");
          if (!st->git) {  // without git: the saved file first, then the other file holds what HEAD would
            require(counts() == "0 1 0 1 3", "saved vs session counts " + counts());
            compare(parseVersion(st->first), m_versions[size_t(m_b)]);
          }
          return true;
        },
        [=, this] {  // HEAD (or first.opad) against the session: one of each
          if (!idle()) return false;
          require(counts() == "1 1 1 1 1", "counts " + counts() + ", want 1 added, 1 removed, 1 modified, 1 moved, 1 unchanged");
          const Tokens& t = theme::current();
          const opad::json box1 = vp->benchLookState(st->body["Box1"]), box2 = vp->benchLookState(st->body["Box2"]),
                           sphere = vp->benchLookState(st->body["Sphere1"]), cylinder = vp->benchLookState(st->body["Cylinder1"]);
          require(sameColour(box1["color"], t.diffModified) && box1["transparency"].get<double>() < 0.05, "Box1 in the modified colour, opaque: " + QString::fromStdString(box1.dump()));
          require(sameColour(box2["color"], t.diffMoved) && sameColour(sphere["color"], t.diffAdded), "Box2 moved, Sphere1 added colours");
          require(cylinder["transparency"].get<double>() > 0.5 && cylinder["activated"].get<int>() == 0, "Cylinder1 unchanged: a ghost, not pickable");
          const opad::json parts = vp->benchCompareState();
          require(parts["parts"].size() == 3 && parts["arrows"].get<int>() == 1, "three ghosts of A and an arrow: " + QString::fromStdString(parts.dump()));
          const opad::json removed = partOf(st->body["Box3"]), moved = partOf(st->body["Box2"]), modified = partOf(st->body["Box1"]);
          require(removed.value("displayed", false) && sameColour(removed["color"], t.diffRemoved), "Box3's ghost in the removed colour");
          require(moved.value("displayed", false) && sameColour(moved["color"], t.diffMoved), "Box2's old place in the moved colour");
          require(modified.value("displayed", false) && sameColour(modified["color"], t.diffModified) && std::abs(modified["transparency"].get<double>() - 0.55) < 0.02,
                  "Box1's old geometry, translucent");
          require(m_chip && m_chip->isVisible(), "the Compare chip over the view");
          pass("A " + m_versions[size_t(m_a)].label + " vs this session: counts " + counts() + ", tints, 3 ghosts, 1 arrow");
          // The rows: Box1's edited feature with what changed.
          const int edited = change("feature", "edited", "Box1");
          require(edited >= 0 && m_panel->order().size() >= 5, "a row for Box1's edited feature");
          QTreeWidgetItem* row = nullptr;
          for (int i = 0; i < m_panel->list()->topLevelItemCount(); ++i)
            if (m_panel->list()->topLevelItem(i)->data(0, Qt::UserRole).toInt() == edited) row = m_panel->list()->topLevelItem(i);
          require(row && row->text(1).contains("Box1") && row->text(1).contains(i18n::t("Length")), "Box1's row names the input");
          m_panel->list()->setCurrentItem(row);  // a click
          require(m_panel->current() == edited, "the clicked row is current");
          QTableWidget* t2 = m_panel->details();
          require(t2->isVisibleTo(m_panel) && t2->rowCount() >= 1 && t2->item(0, 0)->text() == i18n::t("Length") && t2->item(0, 1)->text() == "30 mm" && t2->item(0, 2)->text() == "40 mm",
                  "details: Length 30 mm -> 40 mm");
          pass("rows and details (Length 30 mm -> 40 mm)");
          // The summary line in the UI's language: the core's English one in English, words of the translation otherwise.
          const QString line = m_panel->summaryLabel()->text(), english = QString::fromStdString(summary());
          const bool translated = qEnvironmentVariable("OPAD_LANG") == "ar";
          require(!english.isEmpty() && line.contains("Box1") && line.contains(english) != translated && (!translated || line.contains(ComparePanel::tr("add %1").arg("Sphere1"))),
                  "the summary: " + line + " / " + english);
          pass("the summary in the UI's language: " + line);
          return true;
        },
        [=, this] {  // the clicked row selected Box1 and the timeline shows the feature
          const auto sel = vp->selection();
          if (sel.empty() || vp->looksPending()) return false;
          require(sel.size() == 1 && sel.front().body == st->body["Box1"], "the row selects Box1");
          require(m_services.timeline()->currentOp() == st->box1Feature, "the timeline's current op is Box1's feature");
          const auto& marks = m_services.timeline()->markedOps();
          auto markOf = [&](const std::string& id) { const auto it = marks.find(id); return it == marks.end() ? nullptr : it->second; };
          require(markOf(st->sphereFeature) == &Tokens::diffAdded && markOf(st->box1Feature) == &Tokens::diffModified && markOf(st->box3Feature) == &Tokens::diffModified,
                  "timeline marks: Sphere1 added, Box1 edited, Box3 deleted");
          pass("a row selects and fits its body; timeline marks");
          // ] and [: the commands with those keys, enabled while comparing.
          QAction *next = m_services.action("vcs.nextChange"), *previous = m_services.action("vcs.previousChange");
          require(next && previous && next->shortcut() == QKeySequence("]") && previous->shortcut() == QKeySequence("[") && next->isEnabled(), "] and [ commands");
          const auto order = m_panel->order();
          const auto at = std::find(order.begin(), order.end(), m_panel->current());
          const size_t k = size_t(at - order.begin());
          next->trigger();
          require(m_panel->current() == order[(k + 1) % order.size()], "] goes to the next change");
          previous->trigger();
          previous->trigger();
          require(m_panel->current() == order[(k + order.size() - 1) % order.size()], "[ goes back");
          pass("] and [");
          shot("overlay");
          m_panel->slider()->setValue(0);  // A alone
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          require(!vp->benchLookState(st->body["Box1"]).value("displayed", true) && !vp->benchLookState(st->body["Sphere1"]).value("displayed", true),
                  "emphasis A: B's changed bodies gone");
          require(std::abs(partOf(st->body["Box3"])["transparency"].get<double>()) < 0.02, "emphasis A: A's ghosts opaque");
          shot("emphasis-a");
          m_panel->slider()->setValue(100);  // B alone
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          require(!partOf(st->body["Box3"]).value("displayed", true) && vp->benchLookState(st->body["Box1"]).value("displayed", false), "emphasis B: A's ghosts gone, B shown");
          pass("emphasis A <-> B");
          m_panel->slider()->setValue(50);
          m_panel->chip(Cat::Moved)->click();  // its eye
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          require(!m_panel->shown(Cat::Moved) && !vp->benchLookState(st->body["Box2"]).value("displayed", true) && !partOf(st->body["Box2"]).value("displayed", true) &&
                      vp->benchCompareState()["arrows"].get<int>() == 0,
                  "the Moved eye hides Box2, its old place and the arrow");
          m_panel->chip(Cat::Moved)->click();
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          require(vp->benchLookState(st->body["Box2"]).value("displayed", false) && vp->benchCompareState()["arrows"].get<int>() == 1, "shown again");
          pass("legend eye");
          m_panel->layoutButton(true)->click();  // Side by side
          return true;
        },
        [=, this] {  // A's view left of B's: each shows its version whole, the cameras together
          if (!idle()) return false;
          require(m_sideBySide && vp->sideBySide() && vp->sideWidget(), "Side by side makes A's view");
          const opad::json s = vp->benchSideState();
          const int sideW = s["sideRect"][2], mainW = s["rect"][2], hostW = s["host"][0];
          require(s["sideRect"][0].get<int>() == 0 && std::abs(sideW - mainW) <= 1 && s["rect"][0].get<int>() == sideW + 2 && sideW + 2 + mainW == hostW,
                  "the halves: " + QString::fromStdString(s.dump()).left(300));
          require(s["caption"].get<std::string>() == tr("A · %1").arg(m_versions[size_t(m_a)].label).toStdString(), "A's view is named A");
          require(m_chip->text() == tr("B · %1").arg(m_versions[size_t(m_b)].label), "the chip names B: " + m_chip->text());
          require(sameCamera(s["main"], s["sideCamera"]), "the same camera");
          auto partIn = [&](const std::string& id) {
            for (const auto& p : s["parts"])
              if (p.value("id", "") == id) return std::pair<bool, bool>{p.value("main", true), p.value("side", true)};
            return std::pair<bool, bool>{true, true};
          };
          for (const char* name : {"Box1", "Box2", "Box3"}) require(partIn(st->body[name]) == std::pair<bool, bool>{false, true}, QString("A's %1 only in A's view").arg(name));
          const opad::json& bodies = s["bodies"];
          for (const char* name : {"Box1", "Box2", "Sphere1"})
            require(bodies[st->body[name]].value("main", false) && !bodies[st->body[name]].value("side", true), QString("B's %1 only in B's view").arg(name));
          require(bodies[st->body["Cylinder1"]].value("main", false) && bodies[st->body["Cylinder1"]].value("side", false), "Cylinder1 (unchanged) in both");
          require(std::abs(partOf(st->body["Box3"])["transparency"].get<double>()) < 0.02 && !m_panel->slider()->isVisibleTo(m_panel), "A whole (opaque), no emphasis");
          m_tool->grab().save(prefix + ".side.panel.png");
          vp->grabSide().save(prefix + ".side-a.png");
          vp->grabImage().save(prefix + ".side-b.png");
          m_services.window()->grab().save(prefix + ".side.window.png");  // the widgets: A's caption, B's chip (the views blank)
          pass("side by side: halves, captions, A's ghosts in A's view, B's changes in B's, the same camera");
          // Navigation over A's view drives both: the wheel zooms, a middle drag pans; a left click selects nothing.
          QWidget* side = vp->sideWidget();
          const QPointF at(side->width() / 2.0, side->height() / 2.0);
          const auto selected = vp->selection().size();
          const double scale = s["main"]["scale"];
          QWheelEvent wheel(at, side->mapToGlobal(at), QPoint(), QPoint(0, 240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
          QCoreApplication::sendEvent(side, &wheel);
          const opad::json zoomed = vp->benchSideState();
          require(std::abs(zoomed["main"]["scale"].get<double>() - scale) > scale * 0.05 && sameCamera(zoomed["main"], zoomed["sideCamera"]),
                  QStringLiteral("the wheel over A zooms both: %1 -> %2").arg(scale).arg(zoomed["main"]["scale"].get<double>()));
          QMouseEvent down(QEvent::MouseButtonPress, at, side->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
          QMouseEvent up(QEvent::MouseButtonRelease, at, side->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
          QCoreApplication::sendEvent(side, &down);
          QCoreApplication::sendEvent(side, &up);
          require(vp->benchSideState().is_object() && vp->selection().size() == selected, "a left click in A's view selects nothing");
          const opad::json before = vp->benchSideState();
          const QPointF to = at + QPointF(60, 0);
          QMouseEvent press(QEvent::MouseButtonPress, at, side->mapToGlobal(at), Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
          QMouseEvent move(QEvent::MouseMove, to, side->mapToGlobal(to), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
          QCoreApplication::sendEvent(side, &press);
          QCoreApplication::sendEvent(side, &move);
          const opad::json panned = vp->benchSideState();
          QMouseEvent release(QEvent::MouseButtonRelease, to, side->mapToGlobal(to), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
          QCoreApplication::sendEvent(side, &release);
          require(panned["main"]["center"] != before["main"]["center"] && sameCamera(panned["main"], panned["sideCamera"]), "a middle drag over A pans both");
          pass("navigation over A's view drives both views");
          m_panel->layoutButton(false)->click();  // Overlay again
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          const opad::json s = vp->benchSideState();
          require(!m_sideBySide && !vp->sideBySide() && s["rect"][2] == s["host"][0] && s["rect"][0] == 0, "Overlay: one view, the whole width");
          for (const auto& p : vp->benchCompareState()["parts"]) require(p.value("displayed", false), "A's ghosts in the one view again");
          require(m_panel->slider()->isVisibleTo(m_panel) && m_chip->text() == tr("Compare: %1 → %2").arg(m_versions[size_t(m_a)].label, m_versions[size_t(m_b)].label),
                  "the emphasis and the chip back");
          pass("back to overlay");
          require(choose(0, Kind::Saved), "the saved file in A's list");
          return true;
        },
        [=, this] {  // A = the saved file: only the session's own changes
          if (!idle()) return false;
          require(m_versions[size_t(m_a)].kind == Kind::Saved && counts() == "0 1 0 1 3", "saved file vs session: " + counts());
          pass("A = the saved file: " + counts());
          if (st->git) require(std::any_of(m_versions.begin(), m_versions.end(), [](const CompareVersion& v) { return v.kind == Kind::Git && v.ref == "HEAD"; }), "HEAD listed");
          // Save took a recovery snapshot (the checkpoint); a later one may hold the session as it is.
          require(choose(0, Kind::Recovery), "a recovery snapshot in A's list");
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          require(m_versions[size_t(m_a)].kind == Kind::Recovery && relation() != "unrelated" && !relation().empty() && (counts() == "0 1 0 1 3" || counts() == "0 0 0 0 4"),
                  "recovery snapshot vs session: " + counts());
          pass("A = a recovery snapshot: " + counts());
          doc->run("rename", {{"target", st->body["Cylinder1"]}, {"name", "Drum"}});  // an edit while comparing: compared again
          return true;
        },
        [=, this] {
          if (!idle() || change("body", "renamed", "Drum") < 0) return false;
          pass("an edit while comparing is compared again");
          m_panel->layoutButton(true)->click();  // Esc while side by side
          return true;
        },
        [=, this] {
          if (!idle() || !vp->sideBySide()) return false;
          m_services.action("inspect.clear")->trigger();  // Esc
          return true;
        },
        [=, this] {
          if (m_active || vp->looksPending()) return false;
          const Tokens& t = theme::current();
          require(!m_tool->isVisible() && !(m_chip && m_chip->isVisible()) && vp->benchCompareState()["parts"].empty() && m_services.timeline()->markedOps().empty(),
                  "Esc closes the panel, the ghosts, the chip and the marks");
          const opad::json box2 = vp->benchLookState(st->body["Box2"]);
          require(!sameColour(box2["color"], t.diffMoved) && box2["transparency"].get<double>() < 0.05, "Box2 back in its own colour");
          require(!m_services.action("vcs.nextChange")->isEnabled(), "] is off again");
          const opad::json s = vp->benchSideState();
          require(!vp->sideBySide() && s["rect"][2] == s["host"][0], "Esc ends side by side: one view, the whole width");
          for (const auto& [id, body] : s["bodies"].items()) require(body.value("main", false), "every body in the view");
          pass("Esc ends Compare");
          m_services.action("vcs.compare")->trigger();  // opens side by side, as it was left
          return true;
        },
        [=, this] {
          if (!idle()) return false;
          require(vp->sideBySide() && m_panel->sideBySide(), "Compare opens side by side again: remembered");
          pass("side by side remembered");
          m_panel->layoutButton(false)->click();
          m_services.action("inspect.clear")->trigger();
          return true;
        },
        [=, this] {
          if (m_active || vp->looksPending()) return false;
          require(!vp->sideBySide(), "closed");
          // opad --compare first.opad model.opad, in a hidden child with settings of its own.
          st->childLog = prefix + ".cli.log";
          QFile::remove(st->childLog);
          st->process = new QProcess(this);
          QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
          for (const QString& key : env.keys())
            if (key.startsWith("OPAD_BENCH_")) env.remove(key);
          env.insert("OPAD_BENCH_COMPARE", prefix + ".cli");
          const auto snapshots = RecoveryManager::snapshotsOf(RecoveryManager::recoveryRoot(), doc->doc.header.uuid);
          require(!snapshots.empty(), "a recovery snapshot for the child's Recovery offer");
          env.insert("OPAD_BENCH_COMPARE_SNAPSHOT", snapshots.front().file);
          env.insert("OPAD_BENCH_COMPARE_SNAPSHOT_TIME", snapshots.front().time);
          env.insert("OPAD_BENCH_SETTINGS", prefix + ".cli-settings");
          env.insert("OPAD_TRACE", st->childLog);
          st->process->setProcessEnvironment(env);
#ifdef _WIN32
          st->process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            a->startupInfo->wShowWindow = SW_HIDE;
          });
#endif
          connect(st->process, &QProcess::finished, this, [st](int code) { st->exit = code; st->childDone = true; });
          connect(st->process, &QProcess::errorOccurred, this, [st](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) st->childDone = true; });
          st->process->start(QCoreApplication::applicationFilePath(), {"--compare", st->first, st->file, "--bench-select"});
          return true;
        },
        [=, this] {
          if (!st->childDone) return false;
          QFile log(st->childLog);
          const QString text = log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
          require(st->exit == 0 && text.contains("bench: compare: opad --compare") && text.contains("PASS") && !text.contains("FAIL"),
                  QStringLiteral("the child exited %1: %2").arg(st->exit).arg(text.right(400)));
          pass("opad --compare a b (child)");
          return true;
        },
    };
  }
  auto* timer = new QTimer(this);
  timer->setInterval(100);
  connect(timer, &QTimer::timeout, this, [st, steps, timer, cli] {
    try {
      if (st->step >= steps.size()) {
        timer->stop();
        QCoreApplication::exit(0);
        return;
      }
      if (steps[st->step]()) {
        ++st->step;
        st->wait = 0;
      } else if (++st->wait > 6000) {  // ten minutes (a child opens a document of its own; perf: the Engine shows its bodies)
        throw opad::Error("timed out in step " + std::to_string(st->step));
      }
    } catch (const std::exception& e) {
      timer->stop();
      if (st->process && st->process->state() != QProcess::NotRunning) st->process->kill();  // only the child this bench started
      trace::log(QStringLiteral("bench: compare%1: FAIL %2").arg(cli ? " (child)" : "").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
