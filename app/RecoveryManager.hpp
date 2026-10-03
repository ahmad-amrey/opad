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
  static std::string snapshotText(const QString& file);
 signals:
  void status(const QString& text);
 private:
  struct Session;
  struct Entry {QString file,title,time,source;};
  void scan(std::function<void(std::vector<Entry>,QString)> done);
  void restore(const Entry&,std::function<void(bool,QString)> done);
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
