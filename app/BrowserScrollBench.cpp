// OPAD_BENCH_BROWSER_SCROLL=1 on an empty document: the browser keeps its place when a row's eye is clicked. A tree long
// enough to scroll (30 components with two each, a box in one), two of them closed, Part 02 selected far above, the view
// scrolled to the middle; the eye of the row in the middle of the view clicked as the mouse does (a press on the eye's
// column of the row): the node hidden in one step, the rows updated in place (the same row objects), the scroll position,
// the row at the top, the open and closed rows, the selection and the current row as they were; the same after the undo.
// Then every row made again (BrowserPanel::rebuild, as an area's folder asks): the same place, open rows and current row.
// With the History list on (a step's row is added above the nodes on each change) the row at the top stays the row at the
// top through an eye click and its undo. Case in tools/bench_cases/assembly.py.
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTreeWidgetItemIterator>

#include <functional>
#include <map>
#include <memory>
#include <set>

#include "BenchRegistry.hpp"
#include "BrowserOverlay.hpp"
#include "BrowserPanel.hpp"
#include "MainWindow.hpp"

namespace {
bool settle(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  QCoreApplication::processEvents();
  return done();
}

// What the browser's view shows: where it is scrolled, the row at its top, which rows are open, the current and selected rows.
struct Seen {
  int vertical = 0, horizontal = 0;
  QString top, current;
  std::map<QString, bool> open;
  std::set<QString> selected;
  bool operator==(const Seen& o) const {
    return vertical == o.vertical && horizontal == o.horizontal && top == o.top && current == o.current && open == o.open && selected == o.selected;
  }
};

QString key(const QTreeWidgetItem* it) {  // a row by what it shows: folders have no id
  if (!it) return {};
  const QString id = it->data(0, browser::kIdRole).toString();
  return id.isEmpty() ? it->data(0, Qt::UserRole).toString() + ":" + it->text(0) + ":" + key(it->parent()) : id;
}

Seen seen(BrowserTree* tree) {
  Seen s;
  s.vertical = tree->verticalScrollBar()->value();
  s.horizontal = tree->horizontalScrollBar()->value();
  s.top = key(tree->itemAt(QPoint(tree->viewport()->width() / 2, 0)));
  s.current = key(tree->currentItem());
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->childCount()) s.open[key(*it)] = (*it)->isExpanded();
  for (const QTreeWidgetItem* it : tree->selectedItems()) s.selected.insert(key(it));
  return s;
}

QString describe(const Seen& s) {
  int open = 0;
  for (const auto& [row, on] : s.open) open += on;
  return QString("scrolled %1/%2, top row %3, %4 of %5 rows with children open, current %6, %7 selected")
      .arg(s.vertical).arg(s.horizontal).arg(s.top.left(8)).arg(open).arg(s.open.size()).arg(s.current.left(8)).arg(s.selected.size());
}

QTreeWidgetItem* row(BrowserTree* tree, const std::string& id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, browser::kIdRole).toString().toStdString() == id) return *it;
  return nullptr;
}

// A left press and release on the row's eye, where BrowserTree::mousePressEvent takes it (the delegate's eye column).
void clickEye(BrowserTree* tree, QTreeWidgetItem* item) {
  const QRect r = tree->visualItemRect(item);
  const QPointF at(r.left() + browser::kEyeX + 8, r.center().y());
  const QPointF global = tree->viewport()->mapToGlobal(at);
  QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(tree->viewport(), &press);
  QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(tree->viewport(), &release);
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_BROWSER_SCROLL, browserScroll) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: browser scroll: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] { QCoreApplication::exit(all ? 0 : 2); return true; };
  AppDocument* doc = w.m_doc;
  BrowserPanel* browser = w.m_browser;
  BrowserTree* tree = browser->tree();
  std::vector<std::string> parts;
  try {
    for (int i = 1; i <= 30; ++i) {
      const QString name = QString("Part %1").arg(i, 2, 10, QChar('0'));
      parts.push_back(doc->run("component", {{"name", name.toStdString()}}).value("id", ""));
      for (const char* sub : {"a", "b"}) doc->run("component", {{"name", (name + "." + sub).toStdString()}, {"parent", parts.back()}});
    }
    doc->run("feature", {{"kind", "box"}, {"component", parts[14]}, {"inputs", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}}});
  } catch (const std::exception& e) {
    require(false, QString("the tree made: %1").arg(QString::fromUtf8(e.what())));
    return finish();
  }
  settle([doc] { return !doc->designBusy; }, 60000);
  w.m_browserOverlay->reveal(true);
  QApplication::setActiveWindow(&w);
  settle([] { return false; }, 300);
  browser->expandAll();
  QTreeWidgetItem* closed[] = {row(tree, parts[4]), row(tree, parts[19])};
  for (QTreeWidgetItem* it : closed)
    if (it) it->setExpanded(false);
  browser->selectIds({parts[1]});  // far above the middle: the view must not go back to it
  settle([] { return false; }, 100);
  QScrollBar* bar = tree->verticalScrollBar();
  auto resets = std::make_shared<int>(0);  // the rows made again (QTreeWidget::clear resets its model)
  QObject::connect(tree->model(), &QAbstractItemModel::modelReset, &w, [resets] { ++*resets; });
  if (!require(closed[0] && closed[1] && bar->maximum() > 8 && doc->scene.all_bodies().size() == 1,
               QString("90 components and a box in the browser, two closed, it scrolls (0 to %1)").arg(bar->maximum())))
    return finish();
  // The row in the middle of the view: a component or the box.
  auto middleRow = [tree] {
    bool any = false;
    QTreeWidgetItem* it = tree->itemAt(QPoint(tree->viewport()->width() / 2, tree->viewport()->height() / 2));
    for (int step = 0; it && step < 6 && !any; ++step) {
      const QString kind = it->data(0, Qt::UserRole).toString();
      any = kind == "body" || kind == "component";
      if (!any) it = tree->itemBelow(it);
    }
    return any ? it : nullptr;
  };
  // An eye click on the middle row and its undo: the node's visibility flips and comes back, each change one step, the rows
  // the same objects and the view as it was.
  // rowsAbove: a row comes above the view with each change (the History list): the scroll bar's value moves by it, the row
  // at the top stays the same.
  auto same = [](Seen a, const Seen& b, bool rowsAbove) {
    if (rowsAbove) a.vertical = b.vertical;
    return a == b;
  };
  auto eyeAndUndo = [&](const QString& phase, bool rowsAbove) {
    bar->setValue(bar->maximum() / 2);
    settle([] { return false; }, 50);
    QTreeWidgetItem* target = middleRow();
    const std::string id = target ? target->data(0, browser::kIdRole).toString().toStdString() : std::string();
    const opad::Node* node = id.empty() ? nullptr : doc->node(id);
    if (!require(node && node->visible && bar->value() > 0, phase + ": a shown row in the middle of the view: " + QString::fromStdString(node ? node->name : "none")))
      return;
    const Seen before = seen(tree);
    const int resetsBefore = *resets;
    const QString name = QString::fromStdString(node->name);
    clickEye(tree, target);
    settle([] { return false; }, 100);
    node = doc->node(id);
    const Seen hidden = seen(tree);
    require(node && !node->visible && doc->undoLabel() == AppDocument::tr("hide"), phase + ": its eye click hid " + name + " in one step");
    require(row(tree, id) == target && *resets == resetsBefore, phase + ": the rows were updated in place (the same row objects, no model reset)");
    require(same(hidden, before, rowsAbove), phase + ": the view kept its place: " + describe(hidden) + " (before: " + describe(before) + ")");
    doc->undo();
    settle([] { return false; }, 100);
    node = doc->node(id);
    const Seen back = seen(tree);
    require(node && node->visible && row(tree, id) == target && *resets == resetsBefore, phase + ": undone, " + name + " shown again, its row in place");
    require(same(back, before, rowsAbove), phase + ": after the undo the view kept its place: " + describe(back));
  };
  eyeAndUndo("assembly", false);
  // Every row made again, as an area's folder asks (BrowserPanel::rebuild): the place, the open rows and the current row stay.
  {
    bar->setValue(bar->maximum() / 2 + 3);
    settle([] { return false; }, 50);
    QTreeWidgetItem* current = middleRow();
    if (current) tree->setCurrentItem(current, 0, QItemSelectionModel::NoUpdate);
    const Seen before = seen(tree);
    const int resetsBefore = *resets;
    browser->rebuild();
    settle([] { return false; }, 100);
    const Seen after = seen(tree);
    require(current && *resets > resetsBefore, "a rebuild made the rows again (the model reset)");
    require(after == before, "a rebuild kept the view: " + describe(after) + " (before: " + describe(before) + ")");
  }
  // The History list in the browser: each change adds a step's row above the nodes; the row at the top stays on top.
  if (QAction* list = w.action("timeline.historyList")) {
    if (!list->isChecked()) list->trigger();
    settle([] { return false; }, 100);
    QTreeWidgetItem* history = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
      if ((*it)->data(0, browser::kFolderRole).toString() == "history" && (*it)->data(0, Qt::UserRole).toString() == "folder") history = *it;
    const int steps = history ? history->childCount() : 0;
    require(history && steps > 0 && history->isExpanded(), QString("the History list shows %1 steps above the nodes").arg(steps));
    eyeAndUndo("with the History list", true);
  } else {
    require(false, "the History list command exists");
  }
  return finish();
}
