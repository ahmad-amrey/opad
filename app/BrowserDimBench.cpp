// OPAD_BENCH_BROWSER_DIM=<prefix> on tools/bench_cases/views.py's browser-dim.opad (a box and an empty component "Holder"):
// the browser, collapsed (auto-hide, the default), shows a picture of its rows over the view, which is what it shows until
// it is hovered. Opened, that picture is the browser as it is once everything that decorates its rows had its turn: the
// component the document was last worked in comes back as it opens again (the rows outside it dimmed) and the picture
// shows that; a later change of the rows' look alone (back to the root) shows at once too, not only after a hover. Collapsed,
// its first rows are drawn at full strength over the view (it fades out below them), not greyed as if disabled.
// <prefix>.collapsed.png, <prefix>.browser.png, <prefix>.overlay.png.
#include <QCoreApplication>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "BrowserDelegate.hpp"
#include "BrowserOverlay.hpp"
#include "BrowserPanel.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"

OPAD_BENCH(OPAD_BENCH_BROWSER_DIM, browserDim) {
  static int round = 0;  // runBench runs again after each load the bench starts
  const QString prefix = value;
  auto fail = [](const QString& why) {
    trace::log("bench: browser-dim: FAIL " + why);
    QCoreApplication::exit(2);
    return true;
  };
  AppDocument* doc = w.m_doc;
  BrowserOverlay* overlay = w.m_browserOverlay;
  if (!overlay || !w.m_browser || !doc->hasDocument) return fail("a document, its browser and the overlay");
  std::string holder, box;
  for (const auto& [id, n] : doc->scene.nodes) {
    if (n.kind == opad::Node::Kind::Component && n.name == "Holder") holder = id;
    if (n.kind == opad::Node::Kind::Body) box = id;
  }
  if (holder.empty() || box.empty()) return fail("the box and the component Holder");
  // The collapsed browser's picture is the browser as it is now.
  auto same = [&w, overlay, prefix](const QString& when) {
    const QImage shown = overlay->preview().toImage(), now = w.m_browser->grab().toImage();
    shown.save(prefix + ".collapsed.png");
    now.save(prefix + ".browser.png");
    return !overlay->expanded() && !shown.isNull() && shown == now ? QString() : "the collapsed browser shows its rows as they were, not as they are, " + when;
  };
  BrowserTree* tree = w.m_browser->tree();
  auto dimmed = [tree](const std::string& id) {  // what the decorators say of the row now
    QTreeWidgetItem* row = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
      if ((*it)->data(0, browser::kIdRole).toString().toStdString() == id) row = *it;
    auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
    const QModelIndex index = row ? tree->indexFromItem(row) : QModelIndex();
    return delegate && index.isValid() && delegate->decoration(index).dim;
  };
  // How strongly a row is drawn in the collapsed overlay against the browser itself: the strongest alpha of its pixels.
  auto strength = [&w, tree, overlay, prefix](const std::string& id) {
    QTreeWidgetItem* row = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
      if ((id.empty() && (*it)->data(0, Qt::UserRole).toString() == "document") || (!id.empty() && (*it)->data(0, browser::kIdRole).toString().toStdString() == id)) row = *it;
    if (!row) return 0.0;
    const QRect r(tree->viewport()->mapTo(w.m_browser, tree->visualItemRect(row).topLeft()), tree->visualItemRect(row).size());
    const QImage shown = overlay->grab().toImage(), own = w.m_browser->grab().toImage();
    shown.save(prefix + ".overlay.png");
    auto alpha = [&r](const QImage& img) {
      const qreal d = img.devicePixelRatio();
      int best = 0;
      for (int y = int(r.top() * d); y < int(r.bottom() * d) && y < img.height(); ++y)
        for (int x = int(r.left() * d); x < int(r.right() * d) && x < img.width(); ++x) best = std::max(best, qAlpha(img.pixel(x, y)));
      return best;
    };
    const int base = alpha(own);
    return base ? double(alpha(shown)) / base : 0.0;
  };
  if (round == 0) {
    if (const QString why = same("after the open"); !why.isEmpty()) return fail(why);
    const double top = strength({}), next = strength(box);
    if (overlay->expanded() || top < 0.95 || next < 0.75)
      return fail(QStringLiteral("collapsed, the browser's first rows drawn faint over the view (document row at %1%, the box's at %2% of their strength)").arg(int(top * 100)).arg(int(next * 100)));
    trace::log(QStringLiteral("bench: browser-dim: collapsed, the first rows at full strength (document row %1%, the box's %2%) PASS").arg(int(top * 100)).arg(int(next * 100)));
    doc->setActiveComponent(holder, true);  // worked in Holder: it comes back when the document is opened again
    ++round;
    const QString file = doc->path();
    QTimer::singleShot(0, &w, [&w, file] { w.openPath(file); });
    return true;
  }
  if (doc->activeComponent() != holder || !dimmed(box)) return fail("opened again in Holder: the box's row dimmed (outside it)");
  if (const QString why = same("after opening it again in Holder (the box's row dimmed)"); !why.isEmpty()) return fail(why);
  trace::log("bench: browser-dim: opened again: the collapsed browser shows its rows as the areas left them (the box dimmed outside Holder) PASS");
  doc->setActiveComponent({});  // back to the root: only the rows' look changes
  QTimer::singleShot(100, &w, [same, dimmed, box] {
    if (dimmed(box)) return (void)(trace::log("bench: browser-dim: FAIL the root active: the box's row not dimmed"), QCoreApplication::exit(2));
    if (const QString why = same("after the rows' look changed (the root active again)"); !why.isEmpty())
      return (void)(trace::log("bench: browser-dim: FAIL " + why), QCoreApplication::exit(2));
    trace::log("bench: browser-dim: a change of the rows' look alone shows in the collapsed browser at once PASS");
    QCoreApplication::exit(0);
  });
  return true;
}
