#pragma once
#include <QObject>
#include <QPointer>
#include <QLocalServer>
#include <QLockFile>
#include <QLabel>
#include <QListWidget>
#include <QCheckBox>
#include <deque>
#include <map>
#include <mutex>
#include "AppDocument.hpp"
#include "Jobs.hpp"
class QLocalSocket;
class DesignController;
class Viewport;
class ToolPanel;
class AgentBridge : public QObject {
  Q_OBJECT
 public:
  AgentBridge(AppDocument*,DesignController*,Viewport*,JobRunner*,QWidget*);
  ~AgentBridge() override;
  void settings();
  void showActivity();
  void stop();
  void disconnectClients();
  void setAccess(bool enabled,bool edit);
  QString discoveryPath() const {return m_directory;}
  bool busy() const {return m_busy;}
  // Agent access is on, or a session is bound (UI-06): only then is the GUI selection published (selection.json).
  bool publishesSelection() const;
  QString statusSummary() const {return stateText();}
  opad::json descriptor() const;
  opad::json liveState() const;  // what live_state answers (benches read it too)
  // Used only by the isolated application acceptance harness.
  void bench();
  QStringList activityLines() const;  // Agent activity's lines (benches)
 signals:
  void statusChanged();
  // An agent's git tool changed the repository (AgentGit.cpp); `files`: the work tree may have changed too (a switch,
  // a merge, a pull, a resolve), so the open document follows the file. The Version control area refreshes on it.
  void gitChanged(bool files);
 public:
  // The sub-shape references a connection was given (entity_details, query_entities, selection, change lists), by
  // normalised ref, with the body key and placement they were given for (TODO 10 B6). Transport only.
  struct Known {std::string geometry,placement;};
  using KnownRefs=std::map<std::string,Known>;
  using BatchSteps=std::map<std::string,opad::json>;  // step id -> its result, from this connection's earlier batches
 private:
  using json=opad::json;
  struct Snapshot {std::shared_ptr<opad::Document> doc;opad::Scene scene;unsigned long long revision=0;};
  struct Session {
    QPointer<QLocalSocket> socket;QByteArray input;QString target,agent;std::string clientId;
    QElapsedTimer requestTimer;bool bound=false,receiving=false;
    bool reloaded=false;  // the document was reloaded from its file since the last reply: the next one says so (notice)
    std::shared_ptr<const KnownRefs> known;  // replaced, never changed in place: running jobs keep their copy
    std::shared_ptr<const BatchSteps> steps;  // earlier batches' step results, for @{step#/...} (gap log #14); likewise
  };
  struct Receipt {std::string hash,state="pending";QByteArray response;unsigned long long revision=0;};
  struct Prepared {std::shared_ptr<Snapshot> snapshot;std::string id;QString label;QPointer<QLocalSocket> owner;bool transaction=false;json result,changes;std::vector<std::string> receipts;};
  void accept();
  void read(const std::shared_ptr<Session>&);
  void dispatch(const std::shared_ptr<Session>&,json,std::string hash);
  void reply(const std::shared_ptr<Session>&,json,const std::string& receipt={});
  void replyReceipt(const std::shared_ptr<Session>&,const Receipt&);
  void fail(const std::shared_ptr<Session>&,const std::string&,const QString&,const std::string& receipt={});
  void snapshot(std::function<void(std::shared_ptr<Snapshot>,QString)>,int waited=0);
  void execute(const std::shared_ptr<Session>&,std::string,json,const std::string& receipt);
  void save(const std::shared_ptr<Session>&,const json&,const std::string& receipt);
  void commit(const std::shared_ptr<Session>&,const std::string&,const std::string&,unsigned long long);
  void clearPrepared(bool cancelReceipts=true);
  void publish();
  void activity(const QString&);
  QString target() const;
  QString stateText() const;
  bool editorBusy() const;
  json editingState() const;
  // Linked files outside the project left unread for want of the user's trust (AppDocument::untrustedFiles), said plainly with
  // what to do (ask the user: agents cannot grant it); null when there are none. In live_state, live_diagnostics and context.
  json linkedFiles() const;
  void waitForIdle(const std::shared_ptr<Session>&,int,unsigned long long,std::shared_ptr<QElapsedTimer>);
  void gitTool(const std::shared_ptr<Session>&,const std::string& name,json args,const std::string& receipt,const std::string& hash);  // AgentGit.cpp
  AppDocument* m_doc;DesignController* m_design;Viewport* m_viewport;JobRunner* m_jobs;QWidget* m_window;
  QLocalServer m_server;QString m_instance,m_endpoint,m_directory,m_file;
  std::unique_ptr<QLockFile> m_lock;
  struct Publication {std::mutex mutex;std::atomic<unsigned long long> sequence{0};std::atomic<bool> closed{false};};
  std::shared_ptr<Publication> m_publication=std::make_shared<Publication>();
  std::vector<std::shared_ptr<Session>> m_sessions;
  bool m_enabled=false,m_edit=false,m_follow=false,m_busy=false,m_committing=false,m_seenClient=false;
  int m_benchDelay=0;
  unsigned long long m_epoch=0;
  // The target is the document's identity, not its generation (UI-06): a reload of the same document (same uuid, same file:
  // a git switch, merge or pull, a change on disk, Reload) keeps it, so a bound connection follows it with a notice; any other
  // document (a new one, another file, a copy with a new uuid) is a new target that needs an explicit live_bind.
  unsigned long long m_identity=0;
  struct Replacing {std::string uuid;QString path;bool open=false,pending=false;} m_replacing;
  void followReplace();  // after aboutToReplace: the same document again, or another one
  QPointer<Job> m_job;
  std::shared_ptr<Snapshot> m_cache;
  std::shared_ptr<Prepared> m_prepared;
  QPointer<QLocalSocket> m_owner;
  std::string m_activeOperation;
  std::map<std::string,Receipt> m_receipts;
  json m_changes=json::array();
  QPointer<ToolPanel> m_panel;
  QLabel* m_stateLabel=nullptr;QListWidget* m_activity=nullptr;
};
