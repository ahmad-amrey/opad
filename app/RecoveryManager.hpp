#pragma once
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <functional>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include "opad/core.hpp"

class AppDocument;
class DesignController;
class JobRunner;
class QDialog;
class QWidget;

// Crash snapshots are separate from user files. Each running instance owns a
// locked session directory; other instances only offer abandoned sessions.
class RecoveryManager : public QObject {
  Q_OBJECT
 public:
  RecoveryManager(AppDocument*,DesignController*,JobRunner*,QWidget* window);
  void settings();
  void offerRecovery();
  void saveNow(std::function<void(bool,const QString&)> done = {});
  void finishSession(std::function<void()> done);
  void bench(const QString& mode);
  // Compare (UI-58): the snapshots of the document whose header uuid this is, in every session folder (this one's too),
  // newest first, from their metadata; and the document a snapshot holds, checksums checked. Both read files: workers.
  struct Snapshot { QString file, title, time; };
  static QString recoveryRoot();  // where the session folders are (from the settings)
  static std::vector<Snapshot> snapshotsOf(const QString& root, const std::string& uuid);
  static opad::Document snapshotDocument(const QString& file);  // index mode: a version to compare, not to edit
  // Each snapshot (UI-59) also records its base, the file as the session last read or wrote it ("disk": file, the count of
  // the snapshot's ops it held, its SHA-256 while unchanged), and its .meta a summary of what the snapshot has over that
  // base (summary, counts, changes; editing: the sketch or feature open then), so the offer lists what each one holds.
  // The offer of abandoned snapshots, made but not shown (offerRecovery runs it; benches press its buttons): the list,
  // and for the chosen one the semantic diff against its file as it is now (read on a worker) with the base-hash check;
  // its result (QDialog::done) is an Answer, 0 Later.
  struct Entry {QString file,title,time,source,summary;int changes=-1;};
  enum Answer {RestoreCopy=1,Discard=2,Compare=3,RestoreFile=4,MergeCurrent=5};
  QDialog* offerDialog(const std::vector<Entry>& entries);
  // RestoreFile: the document is the file's again (its path kept), the snapshot's changes unsaved, after the file's own
  // newer ones when it has some; RestoreCopy: an unsaved copy, no path; MergeCurrent: the snapshot's changes added to the
  // open document (the same one) as one undo step; Compare: compareRequested; Discard: the snapshot deleted.
  void answerOffer(int result,const Entry& entry);
 signals:
  void status(const QString& text);
  void answered(bool ok,const QString& text);  // what an answer did, or why it could not (also shown)
  // Compare… in the offer: the snapshot (B) against the file it was taken from (A), opened first when it is not open.
  void compareRequested(const QString& source,const QString& snapshot,const QString& time);
 private:
  struct Session;
  void scan(std::function<void(std::vector<Entry>,QString)> done);
  void restore(const Entry&,bool keepPath,std::function<void(bool,QString)> done);
  void mergeInto(const Entry&,std::function<void(bool,QString)> done);
  bool sameDocument(const Entry&) const;  // the snapshot is of the open document
  void afterCapture(std::function<void()> fn,int tries=100);  // once no snapshot or save is copying the document (10 s at most)
  void configureTimer();
  void discardCurrent();
  void requestCheckpoint();
  AppDocument* m_doc;
  DesignController* m_design;
  JobRunner* m_jobs;
  QWidget* m_window;
  QTimer m_timer;
  std::shared_ptr<Session> m_session;
  bool m_running=false,m_closing=false;
  quint64 m_checkpoint=1,m_savedCheckpoint=0;
  QStringList m_recoveredFiles;
  std::shared_ptr<std::atomic<bool>> m_epoch=std::make_shared<std::atomic<bool>>(true);
};
