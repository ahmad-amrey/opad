#pragma once
// Preferences (UI-110, Ctrl+,): one window for the settings that were spread over the gear menu, one-off dialogs and the
// sketch panel. Pages in a list on the leading side, a search over every page on top: typing keeps the pages that have
// a match (in their title, keywords or any label, check box, button or choice) and marks the matching rows of the page
// shown. Changes apply at once and are saved (QSettings), as the controls they replace did; the window is not modal.
// A feature area adds its own page from ready() (version control, assets, drawings), its texts translated:
//   preferences::addPage({"assets", title, "link", 75, {"lfs", "linked"}, [this] {
//     auto* page = new QWidget;
//     preferences::Form form(page);
//     form.section(linkedFiles);
//     form.check("assets/watch", watchLabel, true, [this](bool on) { m_monitor->setWatching(on); });
//     form.finish();
//     return page;
//   }});
// The window builds every page once, the first time it opens; open() shows it at a page and a control (its objectName
// is the setting's key), which is how the gear menu's old entries and the status bar's toggles lead there.
#include <QDialog>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QVBoxLayout;

namespace preferences {
struct Page {
  QString id;            // "sketch"
  QString title;         // translated
  QString icon;          // icons:: name
  int order = 100;       // place in the list: the built-in pages are 10, 20, ... 110
  QStringList keywords;  // more words the search finds it by (translated and English)
  std::function<QWidget*()> build;  // its controls; the window takes the widget
};
void addPage(const Page& page);  // a page with the same id replaces the earlier one
QList<Page> pages();              // by order
// Every word of the query is in one of the texts (case and accents aside); an empty query matches everything.
bool matches(const QStringList& texts, const QString& query);

// A setting a page shows may have other faces (a status-bar toggle and its menu, the sketch panel's snaps): whoever writes
// it says so with changed(key) and every face follows. An empty key is every setting: the window says that when it is
// activated, for writers that say nothing.
class Notifier : public QObject {
  Q_OBJECT
 signals:
  void changed(const QString& key);
};
Notifier* notifier();
void changed(const QString& key);
// A control that is one face of a setting: it starts with the saved value (fallback when none), saves and says each
// change, and follows the others. The page rows below are bound this way; so are the sketch panel's snaps.
void bind(QCheckBox* box, const QString& key, bool fallback);
void bind(QSpinBox* box, const QString& key, int fallback);
void bind(QDoubleSpinBox* box, const QString& key, double fallback);
void bind(QComboBox* box, const QString& key, int fallback);
void bind(QLineEdit* edit, const QString& key, const QString& fallback);

// Rows of a page: titled sections of check boxes, numbers and choices bound to a QSettings key (also their objectName),
// each calling `apply` with the new value after saving it.
class Form {
 public:
  explicit Form(QWidget* page);
  void section(const QString& title, const QString& note = QString());
  QLabel* note(const QString& text);
  QCheckBox* check(const QString& key, const QString& label, bool fallback, std::function<void(bool)> apply = {});
  // A check box that is a checkable command's other face (Grid, Tracking): either toggles both; the command saves.
  QCheckBox* option(QAction* command, const QString& label);
  QSpinBox* integer(const QString& key, const QString& label, int fallback, int min, int max, const QString& suffix = QString(), std::function<void(int)> apply = {});
  QDoubleSpinBox* number(const QString& key, const QString& label, double fallback, double min, double max, int decimals, const QString& suffix = QString(),
                         std::function<void(double)> apply = {});
  QComboBox* choice(const QString& key, const QString& label, const QStringList& items, int fallback, std::function<void(int)> apply = {});
  QLineEdit* text(const QString& key, const QString& label, const QString& fallback, std::function<void(const QString&)> apply = {});
  QPushButton* button(const QString& label, std::function<void()> fn, const QString& name = QString());
  void row(const QString& label, QWidget* field);  // a field of the page's own
  void finish();  // the rest of the page stays empty
 private:
  QWidget* m_page;
  QVBoxLayout* m_layout;
  QFormLayout* m_form = nullptr;
  QFormLayout* form();
};
}  // namespace preferences

class PreferencesDialog : public QDialog {
  Q_OBJECT
 public:
  explicit PreferencesDialog(QWidget* window);
  // The window's one Preferences window (made the first time), shown and raised at `page` (empty: where it was), with
  // the control named `focus` on it focused and marked.
  static PreferencesDialog* open(QWidget* window, const QString& page = QString(), const QString& focus = QString());
  void setPage(const QString& id);
  QString page() const;
  QStringList pageIds() const;      // in list order
  QStringList visiblePages() const;  // the ones the search leaves
  QWidget* pageWidget(const QString& id) const;
  void setSearch(const QString& text);
  QStringList marked() const;  // the texts of the rows marked on the page shown
 protected:
  bool event(QEvent* e) override;  // activated: every row reads its setting again (preferences::changed({}))
 private:
  void filter();
  void mark();
  QLineEdit* m_search;
  QListWidget* m_list;
  QStackedWidget* m_stack;
  QLabel* m_none;
  QStringList m_ids;
  QList<QStringList> m_texts;  // per page: title, keywords and every text on it
};
