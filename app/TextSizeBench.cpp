// Bench of the text size and high contrast (UI-124): theme::textScale and highContrastTokens as the window shows them.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "BrowserOverlay.hpp"
#include "Ribbon.hpp"

#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

// OPAD_BENCH_TEXTSIZE=<prefix> (a box and the sketch "Plate", started with ui/textScale=200): the window's text is twice
// its size and the boxes that hold it grew with it: the ribbon's tabs, the browser's rows, the timeline (its title and
// count, the ‹ › buttons), the status bar and the menus; <prefix>.window.png, .browser.png, .timeline.png. Then High
// contrast on: one background, the text and every line at least 7:1 against it, the selection's text 4.5:1 on it, the
// view's background the same (<prefix>.contrast.png); off again, and back at 100 % every box is its usual size.
OPAD_BENCH(OPAD_BENCH_TEXTSIZE, textsize) {
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: text size: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  w.m_browserOverlay->setAutoHide(false);
  w.m_browserOverlay->reveal();
  QTimer::singleShot(800, &w, [=, &w] {
    const QFontMetrics body(theme::ui(13));
    check(std::abs(theme::textScale() - 2.0) < 0.01 && QApplication::font().pixelSize() == 26 && body.height() >= 30,
          QString("text at 200 %: the window's font %1 px, a line %2 px").arg(QApplication::font().pixelSize()).arg(body.height()));
    QTabBar* tabs = w.m_ribbon->tabBar();
    const int tabText = QFontMetrics(tabs->font()).height();
    check(tabs->height() == 56 && tabs->count() > 0 && tabs->tabRect(0).height() >= tabText + 8 && tabs->tabRect(0).width() > QFontMetrics(tabs->font()).horizontalAdvance(tabs->tabText(0)),
          QString("the ribbon's tabs grew: %1 px high for %2 px of text").arg(tabs->height()).arg(tabText));
    QTreeWidget* tree = w.m_browser->tree();
    const int row = tree->visualRect(tree->model()->index(0, 0)).height();
    check(row == 56 && row >= body.height() + 8, QString("the browser's rows grew: %1 px for %2 px of text").arg(row).arg(body.height()));
    const QFontMetrics title(theme::ui(13, QFont::Medium)), count(theme::mono(11));
    check(w.m_timeline->height() == 96 && w.m_timeline->height() >= title.height() + count.height() + 8 && theme::px(16) >= title.height() - 4 &&
              w.m_timeline->markerArea().left() > 12 + title.horizontalAdvance(QCoreApplication::translate("TimelineWidget", "Timeline")),
          QString("the timeline grew: %1 px high, markers from %2 px").arg(w.m_timeline->height()).arg(w.m_timeline->markerArea().left()));
    const int status = w.statusBar()->height(), statusText = QFontMetrics(theme::ui(12)).height();
    check(status >= statusText + 4, QString("the status bar grew: %1 px for %2 px of text").arg(status).arg(statusText));
    QMenu* file = w.menuBar()->actions().first()->menu();
    file->ensurePolished();
    file->adjustSize();
    QAction* first = nullptr;
    for (QAction* a : file->actions())
      if (!a->isSeparator() && !first) first = a;
    const int item = first ? file->actionGeometry(first).height() : 0;
    check(item >= QFontMetrics(file->font()).height() + 4, QString("menu items grew: %1 px").arg(item));
    w.grab().save(prefix + ".window.png");
    w.m_browserOverlay->grab().save(prefix + ".browser.png");  // the overlay paints the background
    w.m_timeline->grab().save(prefix + ".timeline.png");
    // High contrast.
    QSettings().setValue("ui/contrast", 1);
    theme::refresh();
    QTimer::singleShot(300, &w, [=, &w] {
      const Tokens& t = theme::current();
      check(t.highContrast && t.bg == t.bg2 && t.bg == t.vp && theme::contrast(t.fg, t.bg) >= 7 && theme::contrast(t.line, t.bg) >= 7 && theme::contrast(t.fg3, t.bg) >= 4.5 &&
                theme::contrast(t.onsel, t.sel) >= 4.5 && qApp->styleSheet().contains("item:selected { color: " + t.onsel.name()),
            QString("high contrast: text %1:1, lines %2:1, the selection's text %3:1").arg(theme::contrast(t.fg, t.bg), 0, 'f', 1).arg(theme::contrast(t.line, t.bg), 0, 'f', 1)
                .arg(theme::contrast(t.onsel, t.sel), 0, 'f', 1));
      check(QApplication::palette().color(QPalette::Window) == t.bg && QApplication::palette().color(QPalette::Highlight) == t.sel, "the palette takes them");
      w.m_browser->setSelectedIds({w.m_doc->scene.all_bodies().front()});
      w.grab().save(prefix + ".contrast.png");
      w.m_browserOverlay->grab().save(prefix + ".contrast-browser.png");
      QSettings().setValue("ui/contrast", 2);
      QSettings().setValue("ui/textScale", 100);
      theme::refresh();
      QTimer::singleShot(300, &w, [=, &w] {
        QTreeWidget* tree = w.m_browser->tree();
        check(!theme::current().highContrast && theme::current().bg == theme::tokens(theme::current().dark).bg, "high contrast off: the theme's own colours");
        check(w.m_ribbon->tabBar()->height() == 28 && tree->visualRect(tree->model()->index(0, 0)).height() == 28 && w.m_timeline->height() == 48 && QApplication::font().pixelSize() == 13,
              QString("at 100 %: tabs %1, rows %2, timeline %3 px").arg(w.m_ribbon->tabBar()->height()).arg(tree->visualRect(tree->model()->index(0, 0)).height()).arg(w.m_timeline->height()));
        QSettings().remove("ui/contrast");
        QSettings().remove("ui/textScale");
        trace::log(QString("bench: text size: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
        QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
      });
    });
  });
  return true;
}
