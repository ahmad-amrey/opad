// Preferences (UI-110), offscreen: the page registry (order, replacing a page), the search's word matching, rows bound to
// their setting, the window's filtering and marking, and open() at a page and a control.
#include "Preferences.hpp"
#include "check.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
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

// A setting with several faces (two pages, a panel's check box): a change on one shows on the others, one written without
// a word reaches them with changed({}), and a box being typed in is not set back to what it said.
TEST(faces_of_a_setting_follow_each_other) {
  QSettings().clear();
  QWidget* first = sample("face");
  QWidget* second = sample("face");
  QCheckBox panel("Snap to face (panel)");
  preferences::bind(&panel, "test/face/on", true);
  auto* on = first->findChild<QCheckBox*>("test/face/on");
  CHECK(on->isChecked() && panel.isChecked());
  on->setChecked(false);
  CHECK(!second->findChild<QCheckBox*>("test/face/on")->isChecked() && !panel.isChecked());
  panel.setChecked(true);
  CHECK(on->isChecked() && second->findChild<QCheckBox*>("test/face/on")->isChecked() && QSettings().value("test/face/on").toBool());
  QSettings().setValue("test/face/count", 9);  // written by someone who says nothing
  CHECK(first->findChild<QSpinBox*>("test/face/count")->value() == 3);
  preferences::changed({});
  CHECK(first->findChild<QSpinBox*>("test/face/count")->value() == 9 && second->findChild<QComboBox*>("test/face/mode")->currentIndex() == 0);
  QDoubleSpinBox typed;
  typed.setDecimals(2);
  typed.setRange(0, 100);
  preferences::bind(&typed, "test/face/step", 15.0);
  QDoubleSpinBox other;
  other.setDecimals(2);
  other.setRange(0, 100);
  preferences::bind(&other, "test/face/step", 15.0);
  typed.findChild<QLineEdit*>()->setText("2.5");  // as typed, on the way to 2.55: not set back to "2.50"
  CHECK(typed.findChild<QLineEdit*>()->text() == "2.5" && other.value() == 2.5 && QSettings().value("test/face/step").toDouble() == 2.5);
  int signals_ = 0;  // following another face is quiet: no row applies a change it did not make
  QObject::connect(second->findChild<QSpinBox*>("test/face/count"), &QSpinBox::valueChanged, [&signals_] { ++signals_; });
  first->findChild<QSpinBox*>("test/face/count")->setValue(4);
  CHECK(second->findChild<QSpinBox*>("test/face/count")->value() == 4 && signals_ == 0);
  delete first;
  delete second;
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
