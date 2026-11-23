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
#include "AgentActivity.hpp"
#include "AgentSentences.hpp"
class QLocalSocket;
class AreaServices;
class Toast;
class QMenu;
class DesignController;
class Viewport;
class ToolPanel;
class AgentBridge : public QObject {
  Q_OBJECT
 public:
  AgentBridge(AppDocument*,DesignController*,Viewport*,JobRunner*,QWidget*);
  ~AgentBridge() override;
  // Settings > AI assistants. page: 0 Connect, 1 Access, 2 Other AI apps; -1: Connect while no assistant is connected,
  // else Access (AgentSettings.cpp).
  void settings(int page = -1);
  bool clientConnected() const;  // a client is bound to this window now
  void showActivity();  // the AI activity panel (it never opens by itself)
  void toggleActivity();
  void stop();
  void userStop();  // Stop from the panel, the chip or tools.agentStop: the same, said in the activity and a toast
  // The window's services (toasts, panels, the timeline, the selection); set once, right after construction.
  void setServices(AreaServices* services);
  AgentChip* chip();  // the status bar's AI chip (made on first call; the window places it)
  void chipMenu(const QPoint& global);
  bool paused() const {return m_paused;}
  void setPaused(bool on);  // Pause changes: the assistants can look but not change anything, for this session only
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
  bool benchActivity(const QString& prefix);  // OPAD_BENCH_AGENT_ACTIVITY (AgentActivityBench.cpp)
  QStringList activityLines() const;  // the technical log's lines, as they always read (benches)
  QStringList feedLines() const;      // the AI activity panel's rows as the user reads them (benches)
  ActivityPanel* activityPanel() const {return m_feed;}  // null until shown once (benches)
 signals:
  void statusChanged();
  // An agent's git tool changed the repository (AgentGit.cpp); `files`: the work tree may have changed too (a switch,
  // a merge, a pull, a resolve), so the open document follows the file. The Version control area refreshes on it.
  void gitChanged(bool files);
  // An agent's open_document / new_document (AgentOpen.cpp): the window opens `path` as File > Open does.
  void openRequested(const QString& path);
  // An agent's git command that may rewrite the document's file is about to run (gitChanged follows, always): the file's
  // states meanwhile are the agent's own change, taken in without a question (DiskSync::expect).
  void gitStarting();
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
  struct Prepared {std::shared_ptr<Snapshot> snapshot;std::string id;QString label;QPointer<QLocalSocket> owner;bool transaction=false;json result,changes;std::vector<std::string> receipts;
    agentsay::Sentence said;QStringList steps;quint64 event=0;};
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
  void clearPrepared(bool cancelReceipts=true,bool dropRow=true);
  void publish();
  void activity(const QString&);  // the technical log (activityLines): today's exact lines
  // The AI activity panel's rows (AgentActivity.hpp): post adds a row or updates the one with its id; the panel is made
  // on the first row whether shown or not, so a later Show finds the history.
  quint64 post(ActivityEvent event);
  ActivityEvent* row(quint64 id);
  void addRead(const QString& text,const QString& who);  // to the open "Looked over the model" row, or a new one
  void divider(const QString& text);
  void needsYou(const QString& headline,const QString& second,const QString& action,const QString& actionText,const QString& details);
  void failed(const std::string& code,const QString& message);  // the row (or none) a failure makes, fail()'s UI half
  void refreshUi();         // chip, strip, empty state (cheap: on every status change)
  void refreshRows();       // rows' Undo, Show and Undone tags (on undo and document changes)
  void stripMenu(const QPoint& global);
  void accessMenu(QMenu& menu);
  void ensurePanel();
  void rowAction(quint64 id,const QString& action);
  void hoverRow(quint64 id);
  void allowChanges();
  AgentChip::State chipState() const;
  struct Names {QString full,brief;};
  Names who(const std::shared_ptr<Session>&) const;
  Names connectedName() const;  // the bound assistant shown (the working one first); empty when none
  int boundCount() const;
  bool canEdit() const {return m_edit && !m_paused;}
  QString fileLabel() const;  // the document's file name, as the windows list names it
  QString target() const;
  QString stateText() const;
  bool editorBusy() const;
  json editingState() const;
  // Linked files outside the project left unread for want of the user's trust (AppDocument::untrustedFiles), said plainly with
  // what to do (ask the user: agents cannot grant it); null when there are none. In live_state, live_diagnostics and context.
  json linkedFiles() const;
  void waitForIdle(const std::shared_ptr<Session>&,int,unsigned long long,std::shared_ptr<QElapsedTimer>);
  void gitTool(const std::shared_ptr<Session>&,const std::string& name,json args,const std::string& receipt,const std::string& hash);  // AgentGit.cpp
  void openDocument(const std::shared_ptr<Session>&,const std::string& name,const json& args,const std::string& receipt,const std::string& hash);  // AgentOpen.cpp
  bool unsaved(const std::shared_ptr<Session>&);
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
  ActivityPanel* m_feed=nullptr;QPointer<AgentChip> m_chip;AreaServices* m_services=nullptr;
  QStringList m_raw;                    // the technical log (activityLines), capped
  quint64 m_eventSeq=0,m_running=0,m_fold=0,m_group=0,m_staged=0,m_needs=0,m_hoverId=0;
  bool m_hoverTint=false,m_paused=false,m_everConnected=false,m_announce=true,m_announced=false,m_listenFailed=false;
  struct Call {std::string name;json args;QString running;} m_call;  // the last request dispatched (needs-you rows name it)
  QString m_dropReason;                 // why a staged preview goes, for its row (set just before clearPrepared)
  int m_unseen=0;                       // changes made while the panel was hidden
  QElapsedTimer m_working;              // since the running call started: "working" shows after 400 ms
  QElapsedTimer m_waiting;              // since an edit was refused because the user is editing: "waiting for you"
  QDateTime m_leftAt;QString m_leftName;  // who left and when: "Claude left" for 60 s
  std::map<std::string,std::vector<qint64>> m_retries;  // a retryable code's times (three in 20 s: "Retrying…")
  QPointer<Toast> m_needsToast;
};
