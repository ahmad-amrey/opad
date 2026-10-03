#pragma once
// Linked files in the window (UI-68): files imported by reference (opad/assets.hpp) are marked in the browser (a link icon,
// the name in italics, a badge for the file's state: in sync, changed with a Sync button, missing, not read, syncing (it
// turns), stored in git LFS; their parts read-only), in the view (LookSource::Asset: parts shown from a file that is not the
// version synced tinted the stale colour, a file being synced faded), described in Properties (Linked file section, a KiCad
// board's 3D models) and handled from the context menu and the commands: Link as asset (drawings placed first), Sync, Sync
// all, Sync changed files automatically, Locate, Replace, Reveal, Copy path, Embed as editable, Pack into project (Use
// project copy when the file is read from it), Download KiCad models. AssetMonitor watches the files; a change shows a toast
// that offers to sync. A sync, an embed and a pack are planned on a worker against a copy of the document (nothing changes
// it meanwhile: designBusy) and committed as one undo step.
#include <QPointer>
#include <QString>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "BrowserDelegate.hpp"
#include "PropertiesPanel.hpp"
#include "Toast.hpp"
#include "opad/design/feature.hpp"

class AssetMonitor;
class AppDocument;
class Job;
class JobRunner;
class Progress;
class QWidget;

namespace assets {
// How Import… brings a file in. Linking is suggested for a file over 20 MB and for STEP, IGES and KiCad boards (models
// one designs around), so only then is the user asked; their remembered answer (setting assets/import: link or copy) is
// taken without asking. Cancel: nothing is imported.
enum class Mode { Copy, Link, Cancel };
constexpr qint64 kSuggestBytes = 20 * 1024 * 1024;
bool suggestLink(const QString& path);
Mode askImport(QWidget* parent, const QString& path);
// The question before reading linked files outside the document's project (read them once, trust their folders for good:
// setting assets/trusted, or not now), for `imports` or every untrusted one; false when there was nothing to ask about.
bool askTrust(QWidget* parent, AppDocument* doc, JobRunner* jobs, std::function<void(const QString&)> failed, const std::vector<std::string>& imports = {});
// The question before models of KiCad's library are downloaded (`count`; their licence; setting kicad/download: ask, always or
// never, which the answer may set): true to download. Not asked when the setting says always; refused when it says never,
// unless the user asked for the download himself (`requested`).
bool askModelDownload(QWidget* parent, int count, bool requested);
// The program and arguments that show `file` selected in the system's file manager.
std::pair<QString, QStringList> revealCommand(const QString& file);
}  // namespace assets

class AssetsArea : public AreaController {
  Q_OBJECT
 public:
  explicit AssetsArea(AreaServices& services);
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void documentChanged(bool replaced) override;

  AssetMonitor* monitor() const { return m_monitor; }
  std::vector<std::string> imports(const SelectionContext& selection) const;  // the linked files the selection is part of
  // Each on one linked import; the commands, badges, context menu and Properties run these. Sync reads the file again
  // (from `file` when given: Locate, Replace), one undo step per file; several are synced one after the other.
  void sync(std::vector<std::string> imports, const QString& file = {});
  void syncAll();  // every file that changed since its last sync; also as the monitor finds them while assets.autoSync is on
  void locate(const std::string& import);   // a file dialog, then sync from the file chosen
  void replace(const std::string& import);
  void embed(const std::string& import);
  void pack(const std::string& import);
  void reveal(const std::string& import);
  void copyPath(const std::string& import);
  void trust(const std::string& import);
  // A linked KiCad board's missing models of KiCad's library (AssetMonitor's models_downloadable): downloaded on a worker
  // into OPAD's cache once the user agrees, then the board is looked at again and its sync offered (the models changed).
  // `requested`: from the user's click (asked even when the setting says never).
  void downloadModels(const std::string& import, bool requested = true);
  void modelFolders();  // the KiCad settings (model folders), then the boards looked at again
  void link();                                // Link as asset…: a file dialog, then the file imported linked
  bool busy() const { return m_busy; }
  void decorate(const browser::Row& row, browser::Decoration& d);
  void section(const PropertySubject& subject, const opad::json& props, QList<PropertySection>& out);

 signals:
  // A sync, an embed or a pack is over: committed (ok) or not; `report` is what the core said (plan.report, pack's).
  void done(const QString& what, const std::string& import, bool ok, const QString& error, const opad::json& report);

 private:
  // Plans on a worker against a copy of the document (its own: pack writes to it) and commits the plan as one undo step
  // (`label`); waits while another change of the design is being planned or the document is being copied.
  void planned(const QString& title, const QString& label, const std::string& import,
               std::function<opad::design::Plan(opad::Document&, const Progress&)> plan,
               std::function<void(bool, const QString&, const opad::json&)> then, int waited = 0);
  void nextSync();
  void filesChanged(const std::vector<std::string>& imports);
  void notify(const QString& text, bool undo = false, int ms = 6000);  // a toast (with Undo), replacing the last one
  void updateLooks();
  void offerModels();  // once per board and session: its downloadable models, as a toast (or at once: setting always)
  bool downloadable(const std::string& import) const;
  QString name(const std::string& import) const;
  QString stateText(const std::string& import) const;
  // Read from the project's copy (assets/ beside the document) while the link names another place, where the file is gone:
  // Use project copy (pack, which then copies nothing) points the link there.
  bool fromProjectCopy(const std::string& import) const;
  AssetMonitor* m_monitor = nullptr;
  std::vector<std::pair<std::string, QString>> m_queue;  // syncs waiting their turn, each with where it reads from (Locate, Replace)
  int m_synced = 0, m_syncFailed = 0;  // of the queue so far (its toast)
  bool m_busy = false;
  QPointer<Toast> m_toast;
  QStringList m_offered;  // boards whose models were offered for download this session
  QPointer<Job> m_download;
};
