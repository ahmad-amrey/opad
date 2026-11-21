// OPAD_BENCH_FEATURE_ONESHOT=1 on a document with a body (tools/bench_cases/sketch.py, case feature-oneshot): a feature is
// normally made once, so OK commits it and closes its panel; Shift held with OK or Enter commits it and starts the same
// feature again (DesignController::runPreview). Move by 5 mm: OK closes it, and the footer says Shift keeps it open;
// Shift+click on OK, Shift+Enter in the view and Shift+Enter while the panel has the keyboard each move the body and open
// Move again, empty (not the window's Repeat last command, which would start it over with its inputs lost); a plain Enter
// in the view moves it and closes Move. Keys are sent where the keyboard is, as typed ones arrive.
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "KeyText.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "ToolPanel.hpp"

namespace {
struct OneShotChecks {
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: feature one-shot: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
};

void waitUntil(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(20);
}

void pressKey(QWidget* to, int key, Qt::KeyboardModifiers modifiers) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers);  // not spontaneous: Qt offers it to the shortcuts first
  QApplication::sendEvent(to, &event);
}

void clickButton(QPushButton* b, Qt::KeyboardModifiers modifiers) {
  const QPointF local = QRectF(b->rect()).center(), global = b->mapToGlobal(local);
  QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, modifiers);
  QApplication::sendEvent(b, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, modifiers);
  QApplication::sendEvent(b, &release);
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_FEATURE_ONESHOT, featureOneShot) {
  auto check = std::make_shared<OneShotChecks>();
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  AppDocument* doc = w.m_doc;
  MainWindow* window = &w;
  ToolPanel* panel = w.m_featurePanel;
  Viewport* view = w.m_viewport;
  const std::string body = doc->scene.all_bodies().empty() ? std::string() : doc->scene.all_bodies().front();
  check->operator()(!body.empty() && !doc->browse && panel, "an editable document with a body");
  if (body.empty() || !panel) {
    QCoreApplication::exit(2);
    return true;
  }
  w.setWorkspace("design");
  const opad::json refs = opad::json::array({{{"body", body}, {"kind", "body"}}});
  const std::vector<std::pair<QString, opad::json>> inputs{{"bodies", refs}, {"dx", "5 mm"}};
  auto movesDone = std::make_shared<size_t>(doc->scene.features.size());
  // After a commit: the next move is in the document, and Move is open again (kept) or closed.
  auto committed = [=](bool kept, const QString& what, std::function<void()> then) {
    const size_t want = *movesDone + 1;
    waitUntil(window, [=] {
      if (doc->designBusy || doc->scene.features.size() < want) return false;
      return kept ? design->featureActive() && form->spec()->kind == "move" : !design->featureActive();
    }, 30000, [=](bool ok) {
      *movesDone = doc->scene.features.size();
      (*check)(ok && doc->scene.features.back().kind == "move", what);
      (*check)(panel->isVisible() == kept, kept ? "the panel stays open, Move empty for the next" : "the panel closes");
      then();
    });
  };
  auto leave = [=] {
    for (int i = 0; i < 4 && design->featureActive(); ++i) {
      QApplication::setActiveWindow(window);
      view->setFocus();
      pressKey(view, Qt::Key_Escape, Qt::NoModifier);
    }
  };
  auto finish = [=] {
    leave();
    trace::log(QString("bench: feature one-shot: OK and Enter close a feature, Shift keeps it %1").arg(check->all ? "PASS" : "FAIL"));
    QCoreApplication::exit(check->all ? 0 : 2);
  };

  design->startFeature("move", inputs);
  QLabel* hint = form->footer()->hint();
  (*check)(design->featureActive() && hint->isVisible() && hint->text().contains(keys::fixedText("shift")), "the footer of a new feature says Shift keeps it open: " + hint->text());
  (*check)(form->footer()->primary()->toolTip().contains(keys::fixedText("shiftEnter")), "OK's tooltip names Shift+Enter");
  clickButton(form->footer()->primary(), Qt::NoModifier);
  committed(false, "OK moves the body and closes Move", [=] {
    design->startFeature("move", inputs);
    clickButton(form->footer()->primary(), Qt::ShiftModifier);
    committed(true, "Shift+click on OK moves it and opens Move again", [=] {
      form->setValues(inputs);
      QApplication::setActiveWindow(window);
      view->setFocus();
      pressKey(view, Qt::Key_Return, Qt::ShiftModifier);
      committed(true, "Shift+Enter in the view moves it and opens Move again", [=] {
        form->setValues(inputs);
        QPushButton* button = nullptr;
        for (auto* b : form->findChildren<QPushButton*>())
          if (b->isVisible() && b->focusPolicy() != Qt::NoFocus && b != form->footer()->primary()) button = b;
        if (!button) button = form->footer()->cancel();
        QApplication::setActiveWindow(panel);
        button->setFocus(Qt::MouseFocusReason);
        pressKey(QApplication::focusWidget() ? QApplication::focusWidget() : button, Qt::Key_Return, Qt::ShiftModifier);
        committed(true, "Shift+Enter while the panel has the keyboard moves it and opens Move again", [=] {
          form->setValues(inputs);
          QApplication::setActiveWindow(window);
          view->setFocus();
          pressKey(view, Qt::Key_Return, Qt::NoModifier);
          committed(false, "a plain Enter in the view moves it and closes Move", finish);
        });
      });
    });
  });
  return true;
}
