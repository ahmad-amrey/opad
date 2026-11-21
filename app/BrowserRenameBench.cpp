// OPAD_BENCH_BROWSER_RENAME=1 (tools/bench_cases/core.py, cases browser-rename and browser-rename-ar) on a document with a
// body: renaming a row in the browser shows the editor's text alone. The delegate no longer paints the row's name under
// the open editor (its first letter showed at the editor's rounded edge), the editor sits on the name's place (from where
// the name is painted to the row's end, the full row height, opaque, in the row's font), and the name is painted again once
// the editor closes. In English and in Arabic (the row's columns stay left to right).
#include <QApplication>
#include <QCoreApplication>
#include <QFontInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTimer>

#include "BenchRegistry.hpp"
#include "BrowserDelegate.hpp"
#include "BrowserPanel.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"

OPAD_BENCH(OPAD_BENCH_BROWSER_RENAME, browserRename) {
  const auto bodies = w.m_doc->scene.all_bodies();
  if (bodies.empty()) {
    trace::log("bench: browser rename: no body FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  const std::string body = bodies.front();
  QTimer::singleShot(500, &w, [&w, body] {
    bool ok = true;
    auto check = [&ok](bool pass, const QString& what) {
      trace::log(QString("bench: browser rename: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
      ok = ok && pass;
    };
    BrowserTree* tree = w.m_browser->tree();
    auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
    w.m_browser->startRename(body);
    auto* editor = qobject_cast<QLineEdit*>(w.m_browser->renameEditor());
    check(delegate && editor, "Rename opens the row's editor");
    if (delegate && editor) {
      const QModelIndex index = delegate->editing();
      const QRect row = tree->visualRect(index);
      const int before = delegate->namesSkipped();
      tree->viewport()->grab();  // paints the rows (also in a hidden bench window, where repaint() does not)
      check(delegate->namesSkipped() > before, "the row's name is not painted under the open editor");
      const QRect e = editor->geometry();
      check(e.left() <= row.left() + browser::kNameX && e.right() >= row.right() - 1 && e.top() <= row.top() && e.bottom() >= row.bottom(),
            QString("the editor covers the name's place, the full row height: editor (%1, %2, %3 x %4), row (%5, %6, %7 x %8)")
                .arg(e.left()).arg(e.top()).arg(e.width()).arg(e.height()).arg(row.left()).arg(row.top()).arg(row.width()).arg(row.height()));
      editor->ensurePolished();
      check(!editor->hasFrame() && editor->styleSheet().contains("background: #") && editor->styleSheet().contains("border: none") &&
                QFontInfo(editor->font()).pixelSize() == QFontInfo(theme::ui(13)).pixelSize(),
            QString("opaque, flat, square and in the row's own font (%1 px; the row's %2 px; frame %3, fill %4)").arg(QFontInfo(editor->font()).pixelSize()).arg(QFontInfo(theme::ui(13)).pixelSize()).arg(editor->hasFrame()).arg(editor->autoFillBackground()));
      QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QApplication::sendEvent(editor, &esc);
      QCoreApplication::processEvents();
      const int closed = delegate->namesSkipped();
      tree->viewport()->grab();
      check(!w.m_browser->renameEditor() && delegate->namesSkipped() == closed, "the editor closed: the name is painted again");
    }
    trace::log(QString("bench: browser rename: only the editor's text shows %1").arg(ok ? "PASS" : "FAIL"));
    QCoreApplication::exit(ok ? 0 : 2);
  });
  return true;
}
