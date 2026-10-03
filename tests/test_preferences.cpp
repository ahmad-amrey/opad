// Preferences (UI-110), offscreen: the page registry (order, replacing a page), the search's word matching, rows bound to
// their setting, the window's filtering and marking, and open() at a page and a control.
#include "Preferences.hpp"
#include "check.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QWidget>

namespace {
QWidget* sample(const QString& name) {
  auto* page = new QWidget;
  preferences::Form form(page);
  form.section(name + " section", "A note about " + name);
  form.check("test/" + name + "/on", "Snap to " + name, true);
  form.integer("test/" + name + "/count", "Count of " + name, 3, 0, 10);
  form.choice("test/" + name + "/mode", "Mode", {"Fast", "Exact"}, 0);
  form.finish();
  return page;
}
}  // namespace

TEST(word_matching) {
  CHECK(preferences::matches({"Grid snapping"}, ""));
  CHECK(preferences::matches({"Grid snapping"}, "snap"));
  CHECK(preferences::matches({"Grid", "snapping"}, "grid SNAP"));
  CHECK(!preferences::matches({"Grid snapping"}, "grid tolerance"));
  CHECK(preferences::matches({"الالتقاط إلى الشبكة"}, "الشبكة"));
}

TEST(registry_order_and_replace) {
  preferences::addPage({"b", "Bee", "grid", 20, {}, [] { return sample("bee"); }});
  preferences::addPage({"a", "Ay", "grid", 10, {"alpha"}, [] { return sample("ay"); }});
  preferences::addPage({"b", "Bee again", "grid", 30, {}, [] { return sample("bee"); }});
  const auto pages = preferences::pages();
  CHECK(pages.size() == 2 && pages[0].id == "a" && pages[1].id == "b" && pages[1].title == "Bee again");
}

TEST(rows_save_their_setting) {
  QSettings().clear();
  QWidget* page = sample("row");
  auto* on = page->findChild<QCheckBox*>("test/row/on");
  auto* count = page->findChild<QSpinBox*>("test/row/count");
  auto* mode = page->findChild<QComboBox*>("test/row/mode");
  CHECK(on && on->isChecked() && count && count->value() == 3 && mode && mode->currentIndex() == 0);
  on->setChecked(false);
  count->setValue(7);
  mode->setCurrentIndex(1);
  QSettings s;
  CHECK(!s.value("test/row/on").toBool() && s.value("test/row/count").toInt() == 7 && s.value("test/row/mode").toInt() == 1);
  QWidget* again = sample("row");  // a page built later starts from the saved values
  CHECK(!again->findChild<QCheckBox*>("test/row/on")->isChecked() && again->findChild<QSpinBox*>("test/row/count")->value() == 7);
  QAction command;
  command.setObjectName("view.grid");
  command.setCheckable(true);
  preferences::Form form(again);
  QCheckBox* option = form.option(&command, "Show the grid");
  option->click();
  CHECK(command.isChecked());
  command.setChecked(false);
  CHECK(!option->isChecked());
  delete page;
  delete again;
}

TEST(search_filters_pages_and_marks_rows) {
  QWidget window;
  PreferencesDialog* dialog = PreferencesDialog::open(&window, "b");
  CHECK(dialog->isVisible() && dialog->page() == "b" && dialog->pageIds() == QStringList({"a", "b"}));
  dialog->setSearch("alpha");  // a page's keyword
  CHECK(dialog->visiblePages() == QStringList{"a"} && dialog->page() == "a" && dialog->marked().isEmpty());
  dialog->setSearch("snap bee");
  CHECK(dialog->visiblePages() == QStringList{"b"} && dialog->page() == "b");
  CHECK(dialog->marked() == QStringList{"Snap to bee"});
  dialog->setSearch("count");
  CHECK(dialog->visiblePages() == QStringList({"a", "b"}));
  CHECK(dialog->marked().contains("Count of bee"));  // the label, and its field with it
  CHECK(dialog->pageWidget("b")->findChild<QSpinBox*>("test/bee/count")->property("prefMatch").toBool());
  dialog->setSearch("nothing like it");
  CHECK(dialog->visiblePages().isEmpty() && dialog->findChild<QStackedWidget*>()->isHidden());
  // open() at a control: the search goes, the page shows, the control is focused and marked.
  CHECK(PreferencesDialog::open(&window, "a", "test/ay/count") == dialog);
  QWidget* count = dialog->pageWidget("a")->findChild<QWidget*>("test/ay/count");
  CHECK(dialog->visiblePages().size() == 2 && dialog->page() == "a" && count->property("prefMatch").toBool());
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir dir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
  QCoreApplication::setOrganizationName("opad-test");
  QCoreApplication::setApplicationName("preferences");
  return check::run_all(argc, argv);
}
