// The shared UI contracts of UI-120 that every track builds on: the command registry, the ribbon's titled groups and
// adaptive collapse, the panel footer, toasts and the semantic colour tokens. Offscreen; the in-app side is the
// gui_benches cases ribbon, ribbon-rtl, toast and toast-rtl (app/ContractsBench.cpp).
#include <QApplication>

#include <algorithm>
#include <utility>

#include "Theme.hpp"
#include "check.hpp"

TEST(semantic_tokens) {
  for (const bool dark : {true, false}) {
    const Tokens t = theme::tokens(dark);
    // Tokens that stand for colours the app already draws keep their values.
    CHECK(t.hover == QColor("#ffffff") && t.candidate == t.amber && t.warning == t.amber && t.error == t.red);
    CHECK(t.selected3d.isValid() && t.selected3d != t.hover && t.ghost.alpha() < 128 && t.locked.isValid());
    // Each family stays apart for normal vision and under deuteranopia and protanopia (CIE76 >= 20 after simulation).
    const QList<QList<QColor>> families = {{t.diffAdded, t.diffRemoved, t.diffModified, t.diffMoved}, {t.assetLinked, t.assetStale, t.assetMissing}};
    for (const auto& family : families)
      for (const theme::Vision v : {theme::Vision::Normal, theme::Vision::Deuteranopia, theme::Vision::Protanopia})
        for (int i = 0; i < family.size(); ++i)
          for (int j = i + 1; j < family.size(); ++j) CHECK(theme::deltaE(theme::simulate(family[i], v), theme::simulate(family[j], v)) >= 20);
    // The check has teeth: the plain green / red / amber set the app uses elsewhere does not pass it.
    double plain = 1e9;
    for (const auto& [a, b] : {std::pair{t.green, t.red}, std::pair{t.green, t.amber}, std::pair{t.red, t.amber}})
      plain = std::min(plain, theme::deltaE(theme::simulate(a, theme::Vision::Deuteranopia), theme::simulate(b, theme::Vision::Deuteranopia)));
    CHECK(plain < 20);
  }
  // Simulation: grey stays grey, pure red and green come together for a deuteranope.
  CHECK(theme::deltaE(theme::simulate(QColor(128, 128, 128), theme::Vision::Deuteranopia), QColor(128, 128, 128)) < 1.5);
  CHECK(theme::deltaE(theme::simulate(QColor("#ff0000"), theme::Vision::Deuteranopia), theme::simulate(QColor("#00a000"), theme::Vision::Deuteranopia)) <
        theme::deltaE(QColor("#ff0000"), QColor("#00a000")) / 3);
  // Every state has its own mark and label: colour is never the only cue.
  QStringList marks, labels;
  for (const theme::Cue& c : theme::cues()) {
    CHECK(theme::cue(c.state) == &c && QString::fromUtf8(c.mark).size() == 1 && c.label[0]);
    marks << QString::fromUtf8(c.mark);
    labels << c.label;
  }
  marks.removeDuplicates();
  labels.removeDuplicates();
  CHECK(theme::cues().size() == 10 && marks.size() == 10 && labels.size() == 10 && !theme::cue("hover"));
  const Tokens dark = theme::tokens(true);
  CHECK(dark.*(theme::cue("diffAdded")->colour) == dark.diffAdded && dark.*(theme::cue("assetMissing")->colour) == dark.assetMissing);
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return check::run_all(argc, argv);
}
