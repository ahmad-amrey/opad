// Updates as an area of the window (AreaController.hpp): Help > Check for updates… and Help > Check for updates
// automatically (setting updates/autoCheck, on by default; the check runs about 20 s after start, at most once a day).
// A newer release is offered in a notice (a toast over the view; a message box when the user asked from the start page):
// What's new shows its notes (the release's CHANGELOG.md section), Download opens its page, Skip this version stays
// quiet about that version (the automatic check only). Step 1 of updates: nothing is downloaded or installed here.
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <optional>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "Commands.hpp"
#include "Jobs.hpp"
#include "Toast.hpp"
#include "UpdateCheck.hpp"

namespace {

// Benches drive the app by themselves (the updates bench through the Help command): no automatic check in them, and
// none when OPAD_NO_UPDATE_CHECK is set.
bool automaticCheckAllowed() {
  if (qEnvironmentVariableIsSet("OPAD_NO_UPDATE_CHECK")) return false;
  for (const QString& key : QProcessEnvironment::systemEnvironment().keys())
    if (key.startsWith("OPAD_BENCH")) return false;
  return true;
}

class Updates : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    m_checker = new updates::Checker(QApplication::applicationVersion(), this);
    QObject::connect(m_checker, &updates::Checker::available, this, [this](const updates::Release& r, bool asked) { offer(r, asked); });
    QObject::connect(m_checker, &updates::Checker::upToDate, this, [this](const QString& current, bool asked) {
      if (asked) tell(QObject::tr("OPAD %1 is the newest version.").arg(current));
    });
    QObject::connect(m_checker, &updates::Checker::failed, this, [this](const QString& why, bool asked) {
      trace::log("updates: check failed: " + why);
      if (asked) tell(QObject::tr("Could not check for updates: %1").arg(why));
    });
    CommandInfo check{"help.checkUpdates", QObject::tr("Check for &updates…"), "download"};
    check.keywords = {QObject::tr("update"), QObject::tr("new version"), QObject::tr("upgrade"), QObject::tr("release")};
    m_check = services().addCommand(check, [this] { m_checker->check(true); });
    CommandInfo automatic{"help.autoUpdates", QObject::tr("Check for updates automatically"), QString()};
    automatic.checkable = true;
    automatic.keywords = {QObject::tr("update"), QObject::tr("new version"), QObject::tr("notify")};
    m_auto = services().addCommand(automatic, [this] { QSettings().setValue(updates::kAutoCheckKey, m_auto->isChecked()); });
    m_auto->setChecked(QSettings().value(updates::kAutoCheckKey, true).toBool());
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* help = menus.value("help");
    if (!help) return;
    QAction* before = services().action("help.about");  // above About, in a group of their own
    help->insertAction(before, m_check);
    help->insertAction(before, m_auto);
    help->insertSeparator(before);
  }

  void ready() override {
    QSettings settings;
    if (!m_auto->isChecked() || !automaticCheckAllowed()) return;
    if (!updates::dueForCheck(settings.value(updates::kLastCheckKey).toDateTime(), QDateTime::currentDateTimeUtc())) return;
    QTimer::singleShot(20000, this, [this] {  // not in the way of the start
      QSettings().setValue(updates::kLastCheckKey, QDateTime::currentDateTimeUtc());
      m_checker->check(false);
    });
  }

  void documentChanged(bool) override {  // the start page had no view for the notice: now there is one
    if (m_pending && hasView()) {
      const updates::Release r = *std::exchange(m_pending, std::nullopt);
      offer(r, false);
    }
  }

  updates::Checker* checker() const { return m_checker; }

 private:
  bool hasView() const {
    const AppDocument* d = services().document();
    return d && d->hasDocument && services().toasts();
  }
  void tell(const QString& text) {
    if (hasView()) services().toast(text, QString(), {}, 6000);
    else QMessageBox::information(services().window(), QObject::tr("Updates"), text);
  }
  void offer(const updates::Release& r, bool asked) {
    if (!asked && QSettings().value(updates::kSkipKey).toString() == r.version) return;
    trace::log("updates: " + r.version + " is available (this is " + m_checker->current() + ")");
    const QString text = QObject::tr("OPAD %1 is available").arg(r.version);
    const QString detail = QObject::tr("You have %1.").arg(m_checker->current());
    auto notes = [this, r] { showNotes(r); };
    auto download = [r] { QDesktopServices::openUrl(QUrl(r.page)); };
    auto skip = [r] { QSettings().setValue(updates::kSkipKey, r.version); };
    if (hasView()) {
      services().toasts()->ask(text, detail,
                               {{QObject::tr("What's new"), notes}, {QObject::tr("Download"), download}, {QObject::tr("Skip this version"), skip}}, 0);
      return;
    }
    if (!asked) {  // the start page: shown when a document opens
      m_pending = r;
      return;
    }
    QMessageBox box(QMessageBox::Information, QObject::tr("Updates"), text, QMessageBox::NoButton, services().window());
    box.setInformativeText(detail);
    QPushButton* whatsNew = box.addButton(QObject::tr("What's new"), QMessageBox::ActionRole);
    QPushButton* get = box.addButton(QObject::tr("Download"), QMessageBox::AcceptRole);
    QPushButton* skipIt = box.addButton(QObject::tr("Skip this version"), QMessageBox::DestructiveRole);
    box.addButton(QObject::tr("Later"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == whatsNew) notes();
    else if (box.clickedButton() == get) download();
    else if (box.clickedButton() == skipIt) skip();
  }
  void showNotes(const updates::Release& r) {
    auto* dialog = new QDialog(services().window());
    dialog->setObjectName("updatesNotes");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QObject::tr("What's new in %1").arg(r.name));
    auto* layout = new QVBoxLayout(dialog);
    auto* text = new QTextBrowser(dialog);
    text->setObjectName("updatesNotesText");
    text->setOpenExternalLinks(true);
    text->setMarkdown(r.notes.isEmpty() ? QObject::tr("No notes were published with this release.") : r.notes);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(dialog);
    QPushButton* get = buttons->addButton(QObject::tr("Download"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    QObject::connect(get, &QPushButton::clicked, dialog, [dialog, page = r.page] { QDesktopServices::openUrl(QUrl(page)); dialog->accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->resize(560, 480);
    dialog->show();
  }

  updates::Checker* m_checker = nullptr;
  QAction* m_check = nullptr;
  QAction* m_auto = nullptr;
  std::optional<updates::Release> m_pending;
};

}  // namespace

OPAD_AREA(Updates)
