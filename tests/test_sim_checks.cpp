// Simulate's checks (app/SimChecks.hpp): every simulation OPAD has is covered by a check, each has a picture, the search
// finds a check from how people describe it, readiness follows the engines; the pictures draw in the light, dark and
// high-contrast themes. OPAD_SIMPIC_OUT=<png>: the pictures, one row per theme, saved to look at.
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QSet>

#include "SimChecks.hpp"
#include "SimPictures.hpp"
#include "Theme.hpp"
#include "check.hpp"
#include "opad/sim/joints.hpp"

using simchecks::Goal;

TEST(checks_cover_every_study_kind) {
  QSet<QString> kinds, ids;
  for (const auto& c : simchecks::all()) {
    CHECK(!c.question.isEmpty() && !c.line.isEmpty() && !c.time.isEmpty() && !c.words.isEmpty());
    CHECK(c.question.endsWith('?'));
    CHECK(!ids.contains(c.id));
    ids << c.id;
    kinds << c.kind;
    CHECK(simpic::has(c.id));
    CHECK(c.command() == "simulate.check." + c.id);
  }
  // Every study kind but the sweep (agent-only: several designs at once) has a check; flow before core has it too.
  for (const auto& k : opad::sim::study_kinds())
    if (k != "sweep") CHECK(kinds.contains(QString::fromStdString(k)));
  CHECK(kinds.contains("flow"));
  for (Goal g : {Goal::Air, Goal::Heat, Goal::Strength, Goal::Movement}) {
    CHECK(simchecks::ofGoal(g).size() >= 2);
    Goal back;
    CHECK(simchecks::goalOf(simchecks::goalTab(g), &back) && back == g);
  }
}

TEST(search_finds_checks_by_description) {
  auto best = [](const QString& q) {
    const simchecks::Check* top = nullptr;
    int score = 0;
    for (const auto& c : simchecks::all())
      if (const int s = simchecks::match(c, q); s > score) score = s, top = &c;
    return top ? top->id : QString();
  };
  CHECK_EQ(best("air through a pipe that gets narrower"), QString("fitting"));
  CHECK_EQ(best("my Raspberry Pi case gets hot"), QString("overheat"));
  CHECK_EQ(best("shelf bracket weight"), QString("bend"));
  CHECK_EQ(best("PLA infill"), QString("print"));
  CHECK_EQ(best("lid hits the hinge"), QString("clearance"));
  CHECK_EQ(best("motor torque"), QString("forces"));
  CHECK_EQ(best("CFM of my fan"), QString("fan"));
  CHECK_EQ(best("how long until it heats up"), QString("warmup"));
  CHECK_EQ(simchecks::match(simchecks::all().front(), ""), 1);  // nothing typed: every card
  CHECK_EQ(simchecks::match(*simchecks::find("bend"), "zebra"), 0);
}

TEST(readiness_follows_the_engines) {
  using R = simchecks::Readiness;
  const opad::json none = {{"motion", true}, {"dynamic", false}, {"static", false}, {"modal", false}, {"thermal", false}, {"cfd", false}};
  const opad::json all = {{"motion", true}, {"dynamic", true}, {"static", true}, {"modal", true}, {"thermal", true}, {"cfd", true}, {"flow", true}};
  CHECK(simchecks::readiness(*simchecks::find("clearance"), none).state == R::Ready);
  CHECK(simchecks::readiness(*simchecks::find("bend"), none).state == R::NeedsSolver);
  CHECK(simchecks::readiness(*simchecks::find("bend"), none).solver == "calculix");
  CHECK(simchecks::readiness(*simchecks::find("forces"), none).solver == "chrono");
  CHECK(simchecks::readiness(*simchecks::find("bend"), all).state == R::Ready);
  const auto& kinds = opad::sim::study_kinds();
  const bool flow = std::find(kinds.begin(), kinds.end(), "flow") != kinds.end();
  CHECK(simchecks::readiness(*simchecks::find("fitting"), all).state == (flow ? R::Ready : R::Coming));
  CHECK(simchecks::readiness(*simchecks::find("fitting"), none).state == (flow ? R::NeedsSolver : R::Coming));
  for (const char* s : {"openfoam", "calculix", "chrono"}) {
    const simchecks::Solver* v = simchecks::solver(s);
    CHECK(v && !v->name.isEmpty() && !v->what.isEmpty() && v->url.startsWith("http"));
  }
}

TEST(pictures_draw_in_every_theme) {
  const QList<simchecks::Check>& checks = simchecks::all();
  const int w = 300, h = 104, pad = 8;
  QImage sheet((w + pad) * int(checks.size()) + pad, (h + pad) * 3 + pad, QImage::Format_ARGB32);
  sheet.fill(Qt::gray);
  QPainter p(&sheet);
  int row = 0;
  for (const Tokens& t : {theme::tokens(false), theme::tokens(true), theme::highContrastTokens()}) {
    for (int i = 0; i < checks.size(); ++i) {
      const QRectF r(pad + i * (w + pad), pad + row * (h + pad), w, h);
      p.fillRect(r, t.bg2);
      // Something is drawn: the picture's pixels differ from the card's.
      QImage one(w, h, QImage::Format_ARGB32);
      one.fill(t.bg2);
      {
        QPainter q(&one);
        simpic::paint(q, QRectF(0, 0, w, h), checks[i].id, t);
      }
      int changed = 0;
      for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) changed += one.pixel(x, y) != t.bg2.rgba();
      if (changed < 150) throw check::Failure(("picture " + checks[i].id + " draws almost nothing").toStdString());
      p.drawImage(r.topLeft(), one);
    }
    ++row;
  }
  p.end();
  if (const QByteArray out = qgetenv("OPAD_SIMPIC_OUT"); !out.isEmpty()) sheet.save(QString::fromLocal8Bit(out));
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  return check::run_all(argc, argv);
}
