// Updates as an area of the window (AreaController.hpp): Help > Check for updates… and Help > Check for updates
// automatically (setting updates/autoCheck, on by default; the check runs about 20 s after start, at most once a day).
// A newer release is offered in a notice (a toast over the view; a message box when the user asked from the start page)
// that says in a line or two what it brings (the notes' opening paragraph or first entries, updates::summarize):
// What's new shows its notes (the release's CHANGELOG.md section), Skip this version stays
// quiet about that version (the automatic check only). Download hands this computer's installer (or the release page)
// to the browser; the user installs it. Help > Offer development builds (setting updates/channel) offers the builds of the
// dev branch too (UpdateCheck.hpp): "OPAD development build N", or "OPAD X.Y.Z release candidate (build N)" while X.Y.Z is
// not released; Skip this version remembers such a build by its tag.
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
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
#include "opad/util.hpp"

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
    m_checker->setBuild(opad::build_number());
    m_checker->setDevelopment(QSettings().value(updates::kChannelKey, "stable").toString() == "development");
    // Only a check that got an answer counts for the daily one: offline, it is tried again at the next start or hour.
    QObject::connect(m_checker, &updates::Checker::available, this, [this](const updates::Release& r, bool asked) {
      QSettings().setValue(updates::kLastCheckKey, QDateTime::currentDateTimeUtc());
      offer(r, asked);
    });
    QObject::connect(m_checker, &updates::Checker::upToDate, this, [this](const QString& current, bool asked) {
      QSettings().setValue(updates::kLastCheckKey, QDateTime::currentDateTimeUtc());
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
    CommandInfo dev{"help.devBuilds", QObject::tr("Offer development builds"), QString()};
    dev.checkable = true;
    dev.keywords = {QObject::tr("update"), QObject::tr("development"), QObject::tr("release candidate"), QObject::tr("dev branch"), QObject::tr("channel")};
    m_dev = services().addCommand(dev, [this] {
      QSettings().setValue(updates::kChannelKey, m_dev->isChecked() ? "development" : "stable");
      m_checker->setDevelopment(m_dev->isChecked());
    });
    m_dev->setChecked(m_checker->development());
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* help = menus.value("help");
    if (!help) return;
    QAction* before = services().action("help.about");  // above About, in a group of their own
    help->insertAction(before, m_check);
    help->insertAction(before, m_auto);
    help->insertAction(before, m_dev);
    help->insertSeparator(before);
  }

  void ready() override {
    if (!automaticCheckAllowed()) return;
    QTimer::singleShot(20000, this, [this] { checkIfDue(); });  // not in the way of the start
    auto* hourly = new QTimer(this);  // a window left open for days still hears of a release
    QObject::connect(hourly, &QTimer::timeout, this, [this] { checkIfDue(); });
    hourly->start(3600 * 1000);
  }
  void checkIfDue() {
    if (!m_auto->isChecked() || m_checker->running()) return;
    if (updates::dueForCheck(QSettings().value(updates::kLastCheckKey).toDateTime(), QDateTime::currentDateTimeUtc())) m_checker->check(false);
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
    // A development build is skipped by its tag (dev-N), a release by its version.
    const QString skipAs = r.development ? r.tag : r.version;
    if (!asked && QSettings().value(updates::kSkipKey).toString() == skipAs) return;
    trace::log("updates: " + updates::displayVersion(r.version, r.build) + " is available (this is " + updates::displayVersion(m_checker->current(), m_checker->build()) + ")");
    const QString text = !r.development ? QObject::tr("OPAD %1 is available").arg(r.version)
                         : r.candidate  ? QObject::tr("OPAD %1 release candidate (build %2) is available").arg(r.version).arg(r.build)
                                        : QObject::tr("OPAD development build %1 is available").arg(r.build);
    // What it brings, then what one has: enough to decide without opening What's new.
    const QString have = m_checker->build() > 0 ? QObject::tr("You have %1 (build %2).").arg(m_checker->current()).arg(m_checker->build())
                                                : QObject::tr("You have %1.").arg(m_checker->current());
    const QString detail = r.summary.isEmpty() ? have : r.summary + QChar(10) + have;
    // Without notes (only the release page answered) What's new is that page. Download is this computer's installer when
    // the release has one, else its page; either opens only GitHub's release addresses of OPAD (safeReleaseUrl).
    auto notes = [this, r] {
      if (r.notes.isEmpty()) QDesktopServices::openUrl(updates::safeReleaseUrl(r.page));
      else showNotes(r);
    };
    auto download = [r] { QDesktopServices::openUrl(updates::safeReleaseUrl(r.download.isEmpty() ? r.page : r.download)); };
    auto skip = [skipAs] { QSettings().setValue(updates::kSkipKey, skipAs); };
    if (hasView()) {
      // One notice at a time: the daily check while the window stays open, or the user asking again, replaces it.
      if (m_notice) m_notice->dismiss();
      m_notice = services().toasts()->ask(
          text, detail, {{QObject::tr("What's new"), notes}, {QObject::tr("Download"), download}, {QObject::tr("Skip this version"), skip}}, 0);
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
    const QUrl url = updates::safeReleaseUrl(r.download.isEmpty() ? r.page : r.download);
    QObject::connect(get, &QPushButton::clicked, dialog, [dialog, url] { QDesktopServices::openUrl(url); dialog->accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->resize(560, 480);
    dialog->show();
  }

  updates::Checker* m_checker = nullptr;
  QAction* m_check = nullptr;
  QAction* m_auto = nullptr;
  QAction* m_dev = nullptr;  // Offer development builds
  std::optional<updates::Release> m_pending;
  QPointer<Toast> m_notice;  // the notice offering a release, while it shows
};

}  // namespace

OPAD_AREA(Updates)
