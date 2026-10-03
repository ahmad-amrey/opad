#pragma once
// What the 2D drawing area's benches share (Drawing2DBench.cpp, Drawing2DToolsBench.cpp): polling, a script of steps that
// each wait for their outcome, PASS/FAIL checks, a layer by name.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "opad/scene.hpp"

namespace bench2d {
// Polls `done` every 50 ms until it holds or `ms` have passed, then calls `then` with the outcome.
inline void pollUntil(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
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
  timer->start(50);
}

using Check = std::function<void(bool, const QString&)>;

// The layer component of this name (the drawing's layers are components holding its bodies), "" if none.
inline std::string layerNamed(const opad::Scene& scene, const std::string& name) {
  for (const auto& [id, n] : scene.nodes)
    if (n.kind == opad::Node::Kind::Component && n.name == name) return id;
  return {};
}

// Steps run one after another: each acts, then waits (at most 10 s) until its condition holds before the next one.
struct Script {
  struct Step {
    QString name;
    std::function<void()> act;
    std::function<bool()> settled;
  };
  std::vector<Step> steps;
  void add(const QString& name, std::function<void()> act, std::function<bool()> settled = [] { return true; }) {
    steps.push_back({name, std::move(act), std::move(settled)});
  }
  static void run(QObject* context, std::shared_ptr<Script> script, size_t i, Check require, std::function<void()> finish) {
    if (i == script->steps.size()) return finish();
    script->steps[i].act();
    pollUntil(context, script->steps[i].settled, 10000, [context, script, i, require, finish](bool ok) {
      if (!ok) require(false, script->steps[i].name + ": timed out");
      run(context, script, i + 1, require, finish);
    });
  }
};
}  // namespace bench2d
