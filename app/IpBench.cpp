// Benches of the IP area (TODO 11 UI-13/14/54), registered through BenchRegistry; cases in tools/bench_cases/ip.py.
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "Legal.hpp"
#include "MainWindow.hpp"
#include "opad/drawing_io.hpp"

// OPAD_BENCH_IP=<prefix> [OPAD_LANG=ar]: the IP switches through their commands. The view cube's corner under the pointer
// is a corner view with view/cubeEdgesCorners on, the face it lies on with it off (UI-54), and back. The ODA File
// Converter is off by default and turns on only through its terms box (UI-14; <prefix>.oda-terms.png). Help shows the
// third-party notices (UI-13; <prefix>.notices.png), About Qt and an About box that names the licences. The commands are
// registered with their groups and keywords; in Arabic every string of the area comes from app/i18n/ar/ip.json.
OPAD_BENCH(OPAD_BENCH_IP, ip) {
  const QString prefix = value;
  const bool english = i18n::current() == "en";
  bool all = true;
  auto report = [&](const QString& what, bool ok) { all = all && ok; trace::log(QString("bench: ip: %1 %2").arg(what, ok ? "PASS" : "FAIL")); };
  w.m_viewport->standardView("iso");  // the front-right-top corner lies at the cube's centre on screen
  QAction* cube = w.action("view.cubeEdgesCorners");
  const QString on = w.m_viewport->benchCubePart(0, -5), face = w.m_viewport->benchCubePart(0, -36);
  report(QString("cube default full (%1), corner under the pointer: %2, top face: %3").arg(cube->isChecked()).arg(on, face), cube->isChecked() && on == "corner" && face == "side");
  cube->trigger();
  const QString off = w.m_viewport->benchCubePart(0, -5);
  report(QString("cube faces only: corner area picks %1, setting %2").arg(off, w.m_settings.value("view/cubeEdgesCorners").toString()), off == "side" && !w.m_settings.value("view/cubeEdgesCorners").toBool());
  cube->trigger();
  const QString back = w.m_viewport->benchCubePart(0, -5);
  report(QString("cube full again: %1").arg(back), back == "corner" && w.m_settings.value("view/cubeEdgesCorners").toBool());
  w.showDocument(false);  // the start page: the view commands are off, the cube switch (a setting, like the theme) is not
  const bool startPage = cube->isEnabled() && !w.action("view.fit")->isEnabled();
  w.showDocument(true);
  report("cube switch enabled with no document open", startPage && w.action("view.fit")->isEnabled());

  QAction* oda = w.action("files.useOda");
  // Answered on the first turn of the box's loop, before the bench's own dismissal (BenchQuiet: Cancel).
  int cancelDefault = 0;  // boxes whose default button (Enter) is Cancel: the terms take a deliberate click
  auto answer = [&w, prefix, &cancelDefault](bool accept) {
    QTimer::singleShot(0, &w, [&w, accept, prefix, &cancelDefault] {
      auto* box = w.findChild<QMessageBox*>("odaTerms");
      if (!box) return;
      if (box->defaultButton() && box->defaultButton() == box->button(QMessageBox::Cancel)) ++cancelDefault;
      if (accept) box->grab().save(prefix + ".oda-terms.png");
      for (auto* b : box->buttons())
        if (box->buttonRole(b) == (accept ? QMessageBox::AcceptRole : QMessageBox::RejectRole)) return b->click();
    });
  };
  auto odaSetting = [&w] { return w.m_settings.value("files/useOda", false).toBool(); };
  report(QString("ODA converter off by default (action %1, core %2, setting %3)").arg(oda && oda->isChecked()).arg(opad::use_oda()).arg(odaSetting()), oda && !oda->isChecked() && !opad::use_oda() && !odaSetting());
  if (oda) {
    answer(false);
    oda->trigger();
    report("ODA terms box cancelled: stays off", !oda->isChecked() && !opad::use_oda() && !odaSetting());
    answer(true);
    oda->trigger();
    report("ODA terms accepted: on", oda->isChecked() && opad::use_oda() && odaSetting());
    report(QString("ODA terms box: Enter means Cancel (%1 of 2 boxes)").arg(cancelDefault), cancelDefault == 2);
    oda->trigger();
    report("ODA off again without asking", !oda->isChecked() && !opad::use_oda() && !odaSetting());
  }
  // Presets are named after what they mimic, never as the product (trademarks).
  QStringList presets;
  bool styled = true;
  for (const auto& [id, product] : {std::pair{"nav.fusion", "Fusion"}, {"nav.solidworks", "SOLIDWORKS"}, {"nav.onshape", "Onshape"}, {"nav.blender", "Blender"}}) {
    const QString text = w.action(id)->text();
    presets << text;
    styled = styled && text == QCoreApplication::translate("MainWindow", "Navigation: %1-style").arg(QString::fromLatin1(product)) && (!english || text.endsWith("-style"));
  }
  report("navigation presets: " + presets.join(", "), styled && (!english || presets[1] == "Navigation: SOLIDWORKS-style"));

  // The commands are in the registry like the built-in ones: a group, words the palette finds them by, no document edit.
  QStringList records;
  bool registered = true;
  for (const char* id : {"help.licenses", "help.aboutqt", "help.about", "view.cubeEdgesCorners", "files.useOda"}) {
    const CommandInfo* info = w.m_commands.find(id);
    registered = registered && info && !info->group.isEmpty() && !info->keywords.isEmpty() && !w.m_commands.editsDocument(id);
    records << QString("%1 [%2: %3]").arg(id, info ? info->group : "-", info ? info->keywords.join(" ") : "-");
  }
  report("commands registered: " + records.join(", "), registered);

  // Help > Third-party licences (UI-13): the notices compiled in from the link libraries, in a dialog with About Qt.
  QMenu* help = nullptr;
  for (QAction* a : w.menuBar()->actions())
    if (a->menu() && a->menu()->actions().contains(w.action("help.licenses"))) help = a->menu();
  report("Help menu: Third-party licences, About Qt, About", help && help->actions().contains(w.action("help.aboutqt")) && help->actions().contains(w.action("help.about")));
  w.action("help.licenses")->trigger();
  auto* notices = w.findChild<QDialog*>("thirdPartyNotices");
  auto* text = notices ? notices->findChild<QPlainTextEdit*>("noticesText") : nullptr;
  const QString body = text ? text->toPlainText() : QString();
  if (notices) notices->grab().save(prefix + ".notices.png");
  report(QString("third-party licences: %1 characters, left to right %2").arg(body.size()).arg(text && text->layoutDirection() == Qt::LeftToRight),
         text && text->layoutDirection() == Qt::LeftToRight && body.startsWith("OPAD ") && body.contains("MIT licence") && body.contains("Trademarks") &&
             (!body.contains("packages.msys2.org") || (body.contains("* opencascade ") && body.contains("* qt6-") && body.contains("Licence texts"))));
  if (notices) notices->close();
  const QString about = legal::aboutText();
  report("About: licences and trademarks, no 'STEP viewer'", !about.contains("STEP viewer") && about.contains("MIT") && about.contains("SOLIDWORKS") &&
                                                               (!english || (about.contains("MIT licence") && about.contains("Third-party licences") && about.contains("not affiliated"))));
  if (!english) {  // the area's fragment is embedded and every one of its strings is looked up through it
    QFile file(":/i18n/" + i18n::current() + "/ip.json");
    const QJsonObject strings = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    QStringList missing;
    for (auto it = strings.begin(); it != strings.end(); ++it)  // a product name may stay as it is
      if (i18n::t(it.key()) != it.value().toString() || (it.value().toString() == it.key() && !it.key().startsWith("ODA"))) missing << it.key().left(40);
    report(QString("%1 strings from ip.json, untranslated: %2").arg(strings.size()).arg(missing.join(" | ")), !strings.isEmpty() && missing.isEmpty());
  }
  w.action("help.about")->trigger();  // message boxes: dismissed by the bench (BenchQuiet), logged
  w.action("help.aboutqt")->trigger();
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
