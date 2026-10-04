#include "RecoveryManager.hpp"
#include "AppDocument.hpp"
#include "AgentBridge.hpp"
#include "ComparePanel.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "opad/diff.hpp"
#include "opad/geometry.hpp"
#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QStandardPaths>
#include <QTableWidget>
#include <QThread>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <mutex>
#include <optional>
#include <set>

struct RecoveryManager::Session {
  QString root,directory;
  std::unique_ptr<QLockFile> lock;
  std::mutex mutex;
  bool closed=false;
  QString baseFile,baseHash;
  opad::json baseHeader;
  std::vector<std::string> baseOps;
  std::set<std::string> baseBodies;
  std::string lastSignature;
  quint64 checkpoint=0;
  QString hashedFile;AppDocument::DiskStat hashedStat;std::string fileHash;  // the document's file, hashed once per state
};
namespace {
void writeAtomic(const QString& file,const QByteArray& bytes) {
  QSaveFile out(file);
  if(!out.open(QIODevice::WriteOnly) || out.write(bytes)!=bytes.size() || !out.commit())
    throw opad::Error("Cannot write recovery snapshot: "+out.errorString().toStdString());
}
std::string digest(const std::string& content) {
  return QCryptographicHash::hash(QByteArrayView(content.data(),qsizetype(content.size())),QCryptographicHash::Sha256).toHex().toStdString();
}
std::string fileDigest(const QString& path) {  // streamed: a 334 MB assembly is never held for this
  QFile file(path);QCryptographicHash hash(QCryptographicHash::Sha256);
  return file.open(QIODevice::ReadOnly) && hash.addData(&file)?hash.result().toHex().toStdString():std::string();
}
std::filesystem::path fsPath(const QString& path){return std::filesystem::path(path.toStdU16String());}
opad::json readJson(const QString& path) {
  QFile file(path);if(!file.open(QIODevice::ReadOnly))throw opad::Error(file.errorString().toStdString());
  return opad::json::parse(file.readAll().toStdString());
}
// A snapshot's record (without its document) and its document, checksums checked. `index`: the base's bodies read in place
// (a preview, Compare); else verified and copied (a restore).
std::pair<opad::json,opad::Document> readSnapshot(const QString& path,bool index) {
  auto record=readJson(path);
  const int format=record.value("format",0);
  const auto payload=format==1?record.at("document").get<std::string>()+record.at("edit").dump():record.at("delta").dump()+record.at("edit").dump();
  if(digest(payload)!=record.at("sha256").get<std::string>())throw opad::Error("Recovery snapshot checksum does not match; choose an earlier snapshot");
  if(format==1){
    auto text=record.at("document").get<std::string>();record.erase("document");
    return {std::move(record),index?opad::Document::parse_index(std::move(text)):opad::Document::parse(text)};
  }
  if(format!=2)throw opad::Error("Unsupported recovery snapshot format");
  const auto& delta=record.at("delta");const auto base=QString::fromStdString(delta.at("base").get<std::string>());
  if(base!=QFileInfo(base).fileName() || !base.endsWith(".opad-base") || base.contains('\\') || base.contains('/'))throw opad::Error("Invalid recovery base path");
  QFile file(QFileInfo(path).absolutePath()+"/"+base);if(!file.open(QIODevice::ReadOnly))throw opad::Error("Recovery base is missing");
  auto content=file.readAll().toStdString();
  if(digest(content)!=delta.at("base_sha256").get<std::string>())throw opad::Error("Recovery base checksum does not match");
  auto document=index?opad::Document::parse_index(std::move(content)):opad::Document::parse(content);const auto count=delta.at("keep_ops").get<size_t>();
  if(count>document.ops.size())throw opad::Error("Invalid recovery operation boundary");
  document.truncate_ops(count);
  for(const auto& body:delta.at("bodies"))if(document.add_body(body.at("brep").get<std::string>(),body.at("meta"))!=body.at("key").get<std::string>())throw opad::Error("Recovery body checksum does not match");
  for(const auto& op:delta.at("ops"))document.append(op);
  record.erase("delta");
  return {std::move(record),std::move(document)};
}
opad::json readRecord(const QString& path) {auto [record,document]=readSnapshot(path,false);record["document"]=document.serialize();return record;}
// How many of a snapshot's ops its file held (its "disk" record), npos when not known.
size_t savedOps(const opad::json& record) {
  const auto disk=record.value("disk",opad::json::object());
  return disk.contains("ops") && disk["ops"].is_number_unsigned()?disk["ops"].get<size_t>():std::string::npos;
}
// What an offer's detail pane shows for one snapshot (UI-59), read on a worker: the snapshot against its file as it is now.
struct Preview {
  QString onto;  // unsaved (no file), missing, unreadable, same, extends, rewritten, other
  QString hash;  // the base-hash check: same, changed, unknown (not recorded, or no file)
  QString error,editing;
  opad::json diff;
};
Preview preview(const RecoveryManager::Entry& entry,const Progress& progress) {
  Preview out;
  auto [record,snapshot]=readSnapshot(entry.file,true);
  if(progress.cancelled())throw opad::Error("cancelled");
  const auto edit=record.value("edit",opad::json());
  if(edit.is_object() && edit.value("type","")=="sketch")out.editing=RecoveryManager::tr("The sketch being edited then, %1, comes back as a step of its own.").arg(QString::fromStdString(edit.value("name","")));
  else if(edit.is_object() && edit.value("type","")=="feature")out.editing=RecoveryManager::tr("The feature being edited then was not finished and is not restored.");
  std::optional<opad::Document> file;
  if(entry.source.isEmpty())out.onto="unsaved";
  else if(!QFileInfo::exists(entry.source))out.onto="missing";
  else {
    std::string text=opad::read_text_file(fsPath(entry.source));
    const auto disk=record.value("disk",opad::json::object());
    out.hash=!disk.contains("sha256")?"unknown":digest(text)==disk["sha256"].get<std::string>()?"same":"changed";
    if(progress.cancelled())throw opad::Error("cancelled");
    try {file=opad::Document::parse_index(std::move(text),fsPath(entry.source));}
    catch(const std::exception& e){out.onto="unreadable";out.error=QString::fromUtf8(e.what());}
  }
  const auto plan=opad::plan_snapshot(snapshot,savedOps(record),file?&*file:nullptr);
  out.diff=opad::snapshot_diff(snapshot,plan,file?&*file:nullptr);
  if(out.onto.isEmpty())out.onto=QString::fromLatin1(opad::snapshot_onto_name(plan.onto));
  if(out.hash.isEmpty())out.hash="unknown";
  return out;
}
// Big documents are let go on a thread of their own: freeing them is work that scales with the model. Reset there: the
// callable itself is destroyed with the QThread, on the UI thread.
void dispose(std::shared_ptr<void> value) {
  if(!value)return;
  auto* thread=QThread::create([value=std::move(value)]()mutable{value.reset();});
  QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start(QThread::LowPriority);
}
opad::json readMetadata(const QString& path) {
  if(QFileInfo::exists(path+".meta"))return readJson(path+".meta");
  // Legacy records put their small metadata header before the full document.
  // Listing them must not parse/hash every old assembly; restore validates it.
  QFile file(path);if(!file.open(QIODevice::ReadOnly))throw opad::Error(file.errorString().toStdString());
  const auto prefix=file.read(65536);const auto boundary=prefix.indexOf(",\"document\":");
  if(boundary>=0) {
    const auto header=opad::json::parse((prefix.left(boundary)+"}").toStdString());
    if(header.value("format",0)==1 && header.contains("title") && header.contains("time"))return header;
  }
  return readRecord(path);
}
// Remove only generated artifacts no retained recovery record refers to.
void prune(const QString& directory,const QString& prefix) {
  const auto records=QDir(directory).entryList({prefix+"_*.opad-recovery"},QDir::Files,QDir::Name);
  std::set<QString> bases;
  for(int i=0;i<records.size();++i){const auto path=directory+"/"+records[i];
    if(i<records.size()-3){QFile::remove(path);QFile::remove(path+".meta");continue;}
    try{const auto meta=readJson(path+".meta");bases.insert(QString::fromStdString(meta.value("base","")));}catch(...) {return;}
  }
  for(const auto& file:QDir(directory).entryList({prefix+"_*.opad-base"},QDir::Files))if(!bases.count(file))QFile::remove(directory+"/"+file);
}
}

RecoveryManager::RecoveryManager(AppDocument* doc,DesignController* design,JobRunner* jobs,QWidget* window)
  :QObject(window),m_doc(doc),m_design(design),m_jobs(jobs),m_window(window),m_session(std::make_shared<Session>()) {
  m_session->root=recoveryRoot();
  m_session->directory=m_session->root+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
  connect(&m_timer,&QTimer::timeout,this,[this]{saveNow();});configureTimer();
  connect(doc,&AppDocument::aboutToReplace,this,&RecoveryManager::discardCurrent);
  connect(doc,&AppDocument::saved,this,[this]{if(m_doc->hasDocument && !m_doc->loading && !m_doc->isDirty() && !m_doc->path().isEmpty() && !m_design->sketchActive() && !m_design->featureActive()){requestCheckpoint();}});
  QTimer::singleShot(1500,this,[this]{
    if(qEnvironmentVariableIsSet("OPAD_BENCH_SETTINGS"))return;
    if(m_doc->loading){
      auto connection=std::make_shared<QMetaObject::Connection>();
      *connection=connect(m_doc,&AppDocument::loadFinished,this,[this,connection]{disconnect(*connection);offerRecovery();});return;
    }
    offerRecovery();
  });
}
QString RecoveryManager::recoveryRoot() {
  const QSettings settings;
  const QString data=settings.format()==QSettings::IniFormat?QFileInfo(settings.fileName()).absolutePath():QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  return data+"/recovery";
}
std::vector<RecoveryManager::Snapshot> RecoveryManager::snapshotsOf(const QString& root,const std::string& uuid) {
  std::vector<Snapshot> out;const auto prefix=QString::fromStdString(digest(uuid));
  for(const auto& folder:QDir(root).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
    for(const auto& file:QDir(root+"/"+folder).entryList({prefix+"_*.opad-recovery"},QDir::Files)) {
      const auto path=root+"/"+folder+"/"+file;
      try{const auto meta=readMetadata(path);out.push_back({path,QString::fromStdString(meta.value("title","")),QString::fromStdString(meta.value("time",""))});}
      catch(const std::exception& e){trace::log(QString("recovery: skipped %1: %2").arg(path,e.what()));}
    }
  std::sort(out.begin(),out.end(),[](const Snapshot& a,const Snapshot& b){return a.time>b.time;});
  return out;
}
opad::Document RecoveryManager::snapshotDocument(const QString& file){return readSnapshot(file,true).second;}
void RecoveryManager::requestCheckpoint() {
  ++m_checkpoint;if(!QSettings().value("recovery/enabled",true).toBool())return;
  const auto generation=m_doc->generation;auto* retry=new QTimer(this);retry->setInterval(250);
  connect(retry,&QTimer::timeout,this,[this,retry,generation]{
    if(m_closing || generation!=m_doc->generation || m_savedCheckpoint==m_checkpoint){retry->stop();retry->deleteLater();return;}
    if(!m_running && !m_doc->loading && !m_doc->designBusy)saveNow();
  });retry->start();
}
void RecoveryManager::discardCurrent() {
  ++m_checkpoint;m_savedCheckpoint=0;
  m_epoch->store(false);m_epoch=std::make_shared<std::atomic<bool>>(true);
  if(!m_doc->hasDocument)return;
  const auto session=m_session;
  const auto prefix=QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(m_doc->doc.header.uuid),QCryptographicHash::Sha256).toHex());
  m_jobs->backgroundNext();
  m_jobs->async(tr("Clearing saved recovery snapshots"),[session,prefix](Progress){
    std::lock_guard guard(session->mutex);
    for(const auto& file:QDir(session->directory).entryList({prefix+"_*.opad-recovery",prefix+"_*.opad-recovery.meta",prefix+"_*.opad-base"},QDir::Files))QFile::remove(session->directory+"/"+file);
    session->baseFile.clear();session->lastSignature.clear();
  });
}
void RecoveryManager::configureTimer() {
  QSettings settings;m_timer.stop();
  if(settings.value("recovery/enabled",true).toBool())m_timer.start(std::clamp(settings.value("recovery/minutes",2).toInt(),1,30)*60000);
}
void RecoveryManager::settings() {
  QDialog dialog(m_window);dialog.setWindowTitle(tr("Autosave and recovery"));auto* layout=new QVBoxLayout(&dialog);
  auto* enabled=new QCheckBox(tr("Keep automatic recovery snapshots"));enabled->setChecked(QSettings().value("recovery/enabled",true).toBool());layout->addWidget(enabled);
  auto* minutes=new QSpinBox;minutes->setRange(1,30);minutes->setValue(QSettings().value("recovery/minutes",2).toInt());minutes->setSuffix(tr(" minutes"));layout->addWidget(minutes);
  auto* explanation=new QLabel(tr("Recovery keeps a full base at each save and stores changed operations and new geometry between saves. Unchanged documents are skipped. Recovery preserves sketch drafts and opens with no active tool; unfinished features are cancelled. Your file is never overwritten."));explanation->setWordWrap(true);layout->addWidget(explanation);
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);layout->addWidget(buttons);
  connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  if(dialog.exec()==QDialog::Accepted){QSettings().setValue("recovery/enabled",enabled->isChecked());QSettings().setValue("recovery/minutes",minutes->value());configureTimer();}
}
void RecoveryManager::saveNow(std::function<void(bool,const QString&)> done) {
  auto fail=[done](const QString& error){if(done)done(false,error);};
  const auto* agent=m_window->findChild<AgentBridge*>();
  if((agent && agent->busy()) || m_closing || m_running || !m_doc->hasDocument || m_doc->browse || m_doc->loading || m_doc->designBusy)return fail(tr("Document is busy; recovery will retry."));
  const bool editing=m_design->sketchActive() || m_design->featureActive();
  if(m_doc->readOnly || (!m_doc->isDirty() && !editing && m_checkpoint==m_savedCheckpoint)){if(done)done(true,{});return;}  // read-only: view changes only
  m_running=true;
  const auto generation=m_doc->generation,revision=m_doc->revision;
  const auto source=m_doc->path(),title=m_doc->title();
  const auto diskFile=m_doc->diskFile();const auto diskStat=m_doc->diskStat();const auto diskBase=m_doc->diskBase();  // the snapshot's base (UI-59)
  const auto epoch=m_epoch;const auto checkpoint=m_checkpoint;
  QPointer<RecoveryManager> self(this);
  auto captured=[=,this](opad::json edit,const QString& error){
    if(!self)return;
    if(!error.isEmpty() || m_closing || m_doc->generation!=generation || m_doc->revision!=revision){m_running=false;fail(error.isEmpty()?tr("Document changed during capture; retrying later."):error);return;}
    const auto session=m_session;
    const bool started=m_doc->captureSnapshot(m_jobs,[=,this,edit=std::move(edit)](std::shared_ptr<opad::Document> document,const QString& snapshotError){
      if(!self)return;
      if(!document || m_closing){m_running=false;fail(snapshotError);return;}
      auto written=std::make_shared<std::atomic<bool>>(false);
      m_jobs->backgroundNext();
      m_jobs->async(tr("Saving recovery snapshot"),[session,document,edit,source,title,epoch,written,checkpoint,diskFile,diskStat,diskBase](Progress progress){
        std::lock_guard guard(session->mutex);if(session->closed || progress.cancelled() || !epoch->load())return;
        if(!session->lock){
          if(!QDir().mkpath(session->directory))throw opad::Error("Cannot create recovery folder");
          session->lock=std::make_unique<QLockFile>(session->directory+"/owner.lock");session->lock->setStaleLockTime(0);
          if(!session->lock->tryLock(0))throw opad::Error("Cannot lock recovery session");
        }
        progress.setPhase(tr("Serializing recovery snapshot"));
        const auto prefix=QString::fromStdString(digest(document->header.uuid));
        const auto stamp=QString::number(QDateTime::currentMSecsSinceEpoch())+"_"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        bool newBase=session->baseFile.isEmpty() || session->checkpoint!=checkpoint || session->baseHeader!=document->header.to_json();
        for(const auto& key:session->baseBodies)if(!document->has_body(key)){newBase=true;break;}
        if(newBase){
          const auto content=document->serialize();const auto name=prefix+"_"+stamp+".opad-base";
          if(progress.cancelled() || !epoch->load())return;
          writeAtomic(session->directory+"/"+name,QByteArray::fromStdString(content));
          session->baseFile=name;session->baseHash=QString::fromStdString(digest(content));session->baseHeader=document->header.to_json();
          session->baseOps.clear();for(const auto& op:document->ops)session->baseOps.push_back(op.id);
          session->baseBodies.clear();for(const auto& body:document->bodies())session->baseBodies.insert(body.key);
          session->checkpoint=checkpoint;session->lastSignature.clear();
        }
        size_t keep=0;while(keep<session->baseOps.size() && keep<document->ops.size() && session->baseOps[keep]==document->ops[keep].id)++keep;
        opad::json delta={{"base",session->baseFile.toStdString()},{"base_sha256",session->baseHash.toStdString()},{"keep_ops",keep},{"ops",opad::json::array()},{"bodies",opad::json::array()}};
        for(size_t i=keep;i<document->ops.size();++i)delta["ops"].push_back(document->ops[i].data);
        for(const auto& body:document->bodies())if(!body.external && !session->baseBodies.count(body.key))delta["bodies"].push_back({{"key",body.key},{"meta",body.meta},{"brep",std::string(body.text())}});
        auto draft=edit;
        std::optional<opad::Scene> scene;
        if(draft.is_object() && draft.value("type","")=="sketch" && draft.contains("geometry")){
          scene=opad::resolve(*document);const auto* sketch=scene->sketch(draft.value("id",std::string()));
          if(sketch){draft["geometry_delta"]=opad::design::sketch_delta(sketch->geometry,draft.at("geometry"));draft.erase("geometry");}
        }
        const auto checksum=digest(delta.dump()+draft.dump());
        if(checksum==session->lastSignature){*written=true;return;}
        // Its base (UI-59): the file as the session last read or wrote it, how many of the snapshot's ops it holds, its hash
        // while it is still so (once per state of the file); and what the snapshot has over it, for the offer's list.
        opad::json disk=opad::json::object(),meta=opad::json::object();size_t saved=std::string::npos;
        if(!diskFile.isEmpty()){
          disk["file"]=diskFile.toStdString();
          bool continues=diskBase && diskBase->uuid==document->header.uuid && diskBase->ops.size()<=document->ops.size();
          for(size_t i=0;continues && i<diskBase->ops.size();++i)continues=document->ops[i].id==diskBase->ops[i].first;
          if(continues)disk["ops"]=saved=diskBase->ops.size();
          if(diskStat.exists && AppDocument::statFile(diskFile)==diskStat){
            if(session->hashedFile!=diskFile || session->hashedStat!=diskStat){
              progress.setPhase(tr("Hashing %1").arg(QFileInfo(diskFile).fileName()));trace::Scope timing("recovery: base hash");
              session->fileHash=fileDigest(diskFile);session->hashedFile=diskFile;session->hashedStat=diskStat;
            }
            if(!session->fileHash.empty()){disk["sha256"]=session->fileHash;disk["size"]=diskStat.size;disk["mtime"]=diskStat.mtime;}
          }
        }
        if(progress.cancelled() || !epoch->load())return;
        try {
          progress.setPhase(tr("Listing the snapshot's changes"));trace::Scope timing("recovery: change summary");
          if(!scene)scene=opad::resolve(*document);
          opad::json d;
          if(saved==std::string::npos && QFileInfo::exists(diskFile)){  // undone past a save: against the file itself
            const auto file=opad::Document::load_index(fsPath(diskFile),[](const std::string&){return true;});
            d=opad::snapshot_diff(*document,opad::plan_snapshot(*document,saved,&file),&file);
          } else {
            const auto base=opad::ops_prefix(*document,saved==std::string::npos?0:saved);
            d=opad::semantic_diff(base,opad::resolve(base),*document,*scene);
          }
          const auto changes=d.value("changes",opad::json::array());  // "text": the summary in the UI's language
          meta={{"summary",d["summary"]},{"text",ComparePanel::summaryOf(changes,d.value("relation","")).toStdString()},{"counts",d["counts"]},{"changes",changes.size()}};
        }catch(const std::exception& e){trace::log(QString("recovery: no summary: %1").arg(e.what()));}
        if(draft.is_object() && draft.contains("type"))meta["editing"]={{"type",draft["type"]},{"name",draft.value("name",draft.value("kind",""))}};
        opad::json record={{"format",2},{"title",title.toStdString()},{"source",source.toStdString()},{"time",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()},
          {"delta",std::move(delta)},{"edit",draft},{"sha256",checksum},{"disk",disk}};
        if(progress.cancelled() || !epoch->load())return;
        const auto file=session->directory+"/"+prefix+"_"+stamp+".opad-recovery";
        writeAtomic(file,QByteArray::fromStdString(record.dump()));
        meta.update(opad::json{{"title",record["title"]},{"time",record["time"]},{"source",record["source"]},{"base",session->baseFile.toStdString()},{"disk",disk}});
        writeAtomic(file+".meta",QByteArray::fromStdString(meta.dump()));
        if(!epoch->load()){QFile::remove(file);QFile::remove(file+".meta");return;}
        session->lastSignature=checksum;*written=true;prune(session->directory,prefix);
      },[self,done,written,checkpoint](bool ok,const QString& error){
        if(!self)return;self->m_running=false;
        const bool saved=ok && *written;
        if(saved)self->m_savedCheckpoint=checkpoint;
        const auto reason=!error.isEmpty()?error:tr("Document changed during capture; retrying later.");
        emit self->status(saved?tr("Recovery snapshot saved"):tr("Recovery snapshot failed: %1").arg(reason));
        if(done)done(saved,saved?QString():reason);
      });
    });
    if(!started){m_running=false;fail(tr("Document is busy; recovery will retry."));}
  };
  if(m_design->sketchActive())m_design->sketch()->captureRecovery(captured);
  else captured(m_design->recoveryState(),{});
}

void RecoveryManager::scan(std::function<void(std::vector<Entry>,QString)> done) {
  auto entries=std::make_shared<std::vector<Entry>>();const auto root=m_session->root;QPointer<RecoveryManager> self(this);
  m_jobs->async(tr("Finding recoverable documents"),[root,entries](Progress progress){
    for(const auto& folder:QDir(root).entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) {
      if(progress.cancelled())return;
      const auto directory=root+"/"+folder;QLockFile lock(directory+"/owner.lock");lock.setStaleLockTime(0);if(!lock.tryLock(0))continue;
      for(const auto& file:QDir(directory).entryList({"*.opad-recovery"},QDir::Files,QDir::Name|QDir::Reversed)) {
        const auto path=directory+"/"+file;
        try {
          const auto meta=readMetadata(path);
          entries->push_back({path,QString::fromStdString(meta.value("title","Untitled")),QString::fromStdString(meta.value("time","")),QString::fromStdString(meta.value("source","")),
                              QString::fromStdString(meta.value("text",meta.value("summary",""))),meta.value("changes",-1)});
        }
        catch(const std::exception& e){trace::log(QString("recovery: skipped %1: %2").arg(path,e.what()));}
      }
    }
  },[self,entries,done](bool ok,const QString& error){if(self)done(ok?std::move(*entries):std::vector<Entry>{},error);});
}
bool RecoveryManager::sameDocument(const Entry& entry) const {
  return m_doc->hasDocument && !m_doc->browse && QFileInfo(entry.file).fileName().section('_',0,0)==QString::fromStdString(digest(m_doc->doc.header.uuid));
}
void RecoveryManager::restore(const Entry& entry,bool keepPath,std::function<void(bool,QString)> done) {
  if(m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive()){done(false,tr("Finish the current operation before recovery."));return;}
  struct Result {opad::Document document;opad::Scene scene;std::optional<opad::design::Plan> draft;AppDocument::Recovered into;QString onto;size_t incoming=0,conflicts=0;bool design=false;};
  auto result=std::make_shared<Result>();const auto generation=m_doc->generation,revision=m_doc->revision;QPointer<RecoveryManager> self(this);
  const QString source=keepPath?entry.source:QString();
  m_jobs->async(tr("Recovering document"),[result,entry,source](Progress progress){
    auto [record,document]=readSnapshot(entry.file,false);
    auto& r=*result;r.document=std::move(document);
    if(!source.isEmpty()) {  // into its file (UI-59): what the file holds now decides what the snapshot's changes come after
      progress.setPhase(tr("Reading %1").arg(QFileInfo(source).fileName()));
      auto known=std::make_shared<opad::Manifest>(opad::Manifest::of(r.document));  // only bodies the snapshot lacks are read
      auto read=AppDocument::readDisk(source,known,r.document.shape_cache);
      const auto plan=opad::plan_snapshot(r.document,savedOps(record),read.doc.get());
      r.onto=!read.stat.exists?"missing":!read.doc?"unreadable":QString::fromLatin1(opad::snapshot_onto_name(plan.onto));
      if(r.onto=="same")r.into={source,plan.base,read.stat,read.manifest};
      else if(r.onto=="extends" && plan.merge.error.empty()) {  // the file's newer ops first, then the snapshot's
        r.incoming=plan.merge.incoming;r.conflicts=plan.merge.conflicts.size();r.design=plan.merge.design;
        opad::apply_merge(r.document,*read.doc,plan.merge,read.bodies);
        r.into={source,r.document.ops.size()-plan.merge.mine.size(),read.stat,read.manifest};
      } else if(r.onto=="missing")r.into={source,0,{},{}};  // Save writes it again
      else {  // rewritten, another document, unreadable: DiskSync reads it again and asks before anything replaces it
        if(r.onto=="extends")r.onto="rewritten";
        r.into={source,plan.base,AppDocument::DiskStat{true,-1,-1},plan.manifest};
      }
    }
    if(opad::has_assets(r.document)){  // linked files, read again from where the document was (the user's own snapshot)
      auto assets=AppDocument::assetOptions();assets.trust_all=true;assets.progress=[progress](double,const std::string&){return !progress.cancelled();};
      r.document.path=std::filesystem::path(entry.source.toStdU16String());opad::load_assets(r.document,assets);
    }
    progress.setPhase(tr("Preparing recovered geometry"));opad::warm_shape_cache(r.document,[progress](size_t,size_t){return !progress.cancelled();});
    r.scene=opad::resolve(r.document);
    auto edit=record.at("edit");
    if(edit.is_object() && edit.contains("geometry_delta")){
      const auto* sketch=r.scene.sketch(edit.at("id").get<std::string>());if(!sketch)throw opad::Error("Recovery sketch baseline is missing");
      edit["geometry"]=opad::design::apply_sketch_delta(sketch->geometry,edit.at("geometry_delta"));
    }
    if(edit.is_object() && edit.value("type","")=="sketch"){
      const auto id=edit.value("id",std::string());
      auto op=id.empty()?opad::design::make_sketch_op(edit.value("name","Recovered sketch"),edit.at("plane"),edit.at("geometry")):
        opad::json{{"op","edit"},{"target",id},{"set",{{"plane",[&]{const auto* sketch=r.scene.sketch(id);const auto& plane=edit.at("plane");return sketch&&plane!=sketch->plane?opad::design::plane_as_made(*sketch,plane):plane;}()},{"geometry",edit.at("geometry")}}}};
      if(const auto* c=r.scene.node(edit.value("component",std::string()));id.empty() && c && c->kind==opad::Node::Kind::Component)op["component"]=c->id;  // drawn in a component (UI-33)
      r.draft=opad::design::plan_ops(r.document,{op});
    }
  },[=,this](bool ok,const QString& error){
    if(!self)return;
    if(!ok){done(false,error);return;}
    if(m_doc->generation!=generation || m_doc->revision!=revision || m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive()){done(false,tr("Document changed during recovery. Retry when ready."));return;}
    try {
      m_doc->recover(std::move(result->document),std::move(result->scene),result->into);
      if(result->draft)m_doc->commitPlan(std::move(*result->draft),tr("Recovered sketch"));
      m_recoveredFiles<<entry.file;
      const QString name=QFileInfo(source).fileName(),&onto=result->onto;
      QString text=tr("Document recovered. Use Save As to keep it.");
      if(onto=="same")text=tr("Restored into %1: Save writes the recovered changes there.").arg(name);
      else if(onto=="extends"){
        text=tr("Restored into %1 after the changes saved there since (%n): Save writes the result there.",nullptr,int(result->incoming)).arg(name);
        if(result->conflicts)text+=' '+tr("Changes of both to the same things: %n (the snapshot's come last).",nullptr,int(result->conflicts));
        if(result->design)text+=' '+tr("Both changed the design: regenerate it.");
      } else if(onto=="missing")text=tr("Restored as %1, which is no longer on disk: Save writes it again.").arg(name);
      else if(!onto.isEmpty())text=tr("Restored into %1, which changed on disk since: see the bar over the view before saving.").arg(name);
      done(true,text);
    }catch(const std::exception& e){done(false,QString::fromUtf8(e.what()));}
  });
}
// Merge into current (UI-59): the open document (the same one, perhaps with changes of its own) keeps everything it has; the
// snapshot's unsaved ops come after, one undo step.
void RecoveryManager::mergeInto(const Entry& entry,std::function<void(bool,QString)> done) {
  if(!sameDocument(entry)){done(false,tr("Open the document this snapshot was taken of to merge it there."));return;}
  if(m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive()){done(false,tr("Finish the current operation before recovery."));return;}
  const auto generation=m_doc->generation,revision=m_doc->revision;QPointer<RecoveryManager> self(this);
  const bool started=m_doc->captureSnapshot(m_jobs,[=,this](std::shared_ptr<opad::Document> current,const QString& error){
    if(!self)return;
    if(!current){done(false,error.isEmpty()?tr("Document changed during recovery. Retry when ready."):error);return;}
    struct Result {opad::Document document;opad::Scene scene;size_t mine=0,conflicts=0;bool design=false;};
    auto result=std::make_shared<Result>();
    m_jobs->async(tr("Merging recovered changes"),[result,current,entry](Progress progress) mutable {
      auto [record,document]=readSnapshot(entry.file,false);
      const auto plan=opad::plan_snapshot(document,savedOps(record),current.get());
      if(plan.onto==opad::SnapshotPlan::Onto::other)throw opad::Error(tr("The open document is another one.").toStdString());
      if(plan.onto==opad::SnapshotPlan::Onto::rewritten || !plan.merge.error.empty())
        throw opad::Error(tr("The open document no longer continues what this snapshot was based on: restore it as a copy, or compare the two.").toStdString());
      if(plan.merge.mine.empty())throw opad::Error(tr("The open document has every change of this snapshot already.").toStdString());
      result->mine=plan.merge.mine.size();result->conflicts=plan.merge.conflicts.size();result->design=plan.merge.design;
      document.shape_cache=current->shape_cache;  // the open document's shapes: only the snapshot's new bodies are parsed
      opad::apply_merge(document,*current,plan.merge,current->body_keys());
      current.reset();  // the copy goes here, not on the UI thread
      progress.setPhase(tr("Preparing recovered geometry"));opad::warm_shape_cache(document,[progress](size_t,size_t){return !progress.cancelled();});
      result->scene=opad::resolve(document);result->document=std::move(document);
    },[=,this](bool ok,const QString& error){
      if(!self)return;
      if(!ok){done(false,error);return;}
      if(m_doc->generation!=generation || m_doc->revision!=revision){done(false,tr("Document changed during recovery. Retry when ready."));return;}
      try {
        m_doc->commitSnapshot(result->document,result->scene,revision,tr("merge recovered changes"));
        dispose(std::make_shared<std::pair<opad::Document,opad::Scene>>(std::move(result->document),std::move(result->scene)));  // what it replaced
        m_recoveredFiles<<entry.file;
        QString text=tr("Recovered changes merged into the open document: %n, one step to undo.",nullptr,int(result->mine));
        if(result->conflicts)text+=' '+tr("Changes of both to the same things: %n (the snapshot's come last).",nullptr,int(result->conflicts));
        if(result->design)text+=' '+tr("Both changed the design: regenerate it.");
        done(true,text);
      }catch(const std::exception& e){done(false,QString::fromUtf8(e.what()));}
    });
  });
  if(!started)done(false,tr("Finish the current operation before recovery."));
}
void RecoveryManager::offerRecovery() {
  scan([this](std::vector<Entry> entries,QString error){
    if(!error.isEmpty()){QMessageBox::warning(m_window,tr("Recovery"),error);return;}
    if(entries.empty()){emit status(tr("No recoverable documents found."));return;}
    std::unique_ptr<QDialog> dialog(offerDialog(entries));
    const int result=dialog->exec(),index=dialog->findChild<QListWidget*>("recoveryList")->currentRow();
    if(index>=0)answerOffer(result,entries.at(size_t(index)));
  });
}
// The snapshots on the left (what each holds, from its .meta); on the right the chosen one against its file as it is now
// (read on a worker): the base-hash check, the changes as Compare lists them and what one changed. Restore into file /
// Merge into current / Restore as copy / Compare… (UI-58) / Discard snapshot / Later. Not shown here: offerRecovery runs
// it, a bench presses its buttons hidden.
QDialog* RecoveryManager::offerDialog(const std::vector<Entry>& entries) {
  auto* dialog=new QDialog(m_window);dialog->setWindowTitle(tr("Recover documents"));dialog->resize(940,580);auto* layout=new QVBoxLayout(dialog);
  auto* note=new QLabel(tr("These snapshots were left by a previous session. Each one is shown against its file as it is now. Later keeps them for another time."));note->setWordWrap(true);layout->addWidget(note);
  auto* split=new QHBoxLayout;split->setSpacing(12);layout->addLayout(split,1);
  auto* list=new QListWidget;list->setObjectName("recoveryList");list->setFixedWidth(300);list->setTextElideMode(Qt::ElideRight);
  list->setStyleSheet(QStringLiteral("QListWidget::item { height: %1px; }").arg(2*list->fontMetrics().lineSpacing()+14));  // two lines a snapshot
  for(const auto& e:entries){  // the file (the title it had when never saved), when; what it holds
    const QString name=e.source.isEmpty()?e.title.section(" - ",0,0).remove('*'):QFileInfo(e.source).fileName();
    auto* item=new QListWidgetItem(tr("%1 · %2").arg(name,i18n::localTime(e.time.toStdString()))+"\n"+(e.summary.isEmpty()?(e.source.isEmpty()?tr("Never saved"):QString()):e.summary));
    item->setToolTip(e.source.isEmpty()?tr("Never saved"):QDir::toNativeSeparators(e.source)+(e.summary.isEmpty()?QString():"\n"+e.summary));list->addItem(item);
  }
  split->addWidget(list);
  auto* pane=new QWidget;pane->setObjectName("recoveryDetail");auto* pv=new QVBoxLayout(pane);pv->setContentsMargins(0,0,0,0);pv->setSpacing(6);
  auto* heading=new QLabel;heading->setObjectName("recoveryHeading");heading->setFont(theme::ui(15,QFont::DemiBold));
  auto* where=new QLabel;where->setObjectName("secondary");where->setWordWrap(true);where->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto* base=new QLabel;base->setObjectName("recoveryBase");base->setWordWrap(true);
  auto* summary=new QLabel;summary->setObjectName("recoverySummary");summary->setWordWrap(true);summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto* changes=ComparePanel::makeList(pane);changes->setObjectName("recoveryChanges");
  auto* details=ComparePanel::makeDetails(pane);details->setObjectName("recoveryDetails");details->setHorizontalHeaderLabels({tr("What"),tr("File"),tr("Snapshot")});
  for(QWidget* w:std::initializer_list<QWidget*>{heading,where,base,summary})pv->addWidget(w);
  pv->addWidget(changes,1);pv->addWidget(details);split->addWidget(pane,1);
  // Discard on its own at the start; the ways back after it, the one that keeps the file's path last before Later.
  auto* buttons=new QHBoxLayout;layout->addLayout(buttons);
  auto* discard=new QPushButton(tr("Discard snapshot"));auto* compare=new QPushButton(tr("Compare…"));auto* copy=new QPushButton(tr("Restore as copy"));
  auto* merge=new QPushButton(tr("Merge into current"));auto* intoFile=new QPushButton(tr("Restore into file"));auto* later=new QPushButton(tr("Later"));
  buttons->addWidget(discard);buttons->addStretch(1);
  for(QPushButton* b:{compare,copy,merge,intoFile,later})buttons->addWidget(b);
  const std::pair<QPushButton*,Answer> answers[]={{intoFile,RestoreFile},{merge,MergeCurrent},{copy,RestoreCopy},{compare,Compare},{discard,Discard}};
  for(const auto& [button,answer]:answers){
    button->setProperty("answer",int(answer));button->setAutoDefault(false);
    connect(button,&QPushButton::clicked,dialog,[dialog,answer=answer]{dialog->done(answer);});
  }
  compare->setObjectName("recoveryCompare");later->setAutoDefault(false);connect(later,&QPushButton::clicked,dialog,&QDialog::reject);
  auto primary=[](QPushButton* b,bool on){b->setObjectName(on?"primary":"");b->setDefault(on);b->style()->unpolish(b);b->style()->polish(b);};  // the default, styled
  struct State {std::vector<Entry> entries;QPointer<Job> job;unsigned serial=0;opad::json changes;std::vector<QTreeWidgetItem*> rows;std::vector<int> order;};
  auto state=std::make_shared<State>();state->entries=entries;
  connect(changes,&QTreeWidget::currentItemChanged,details,[state,details](QTreeWidgetItem* item){
    const int change=item?item->data(0,Qt::UserRole).toInt():-1;
    if(change<0 || size_t(change)>=state->changes.size()){details->hide();return;}
    ComparePanel::fillDetails(details,state->changes[size_t(change)]);
  });
  const QPointer<QDialog> alive(dialog);
  connect(list,&QListWidget::currentRowChanged,dialog,[=,this](int row){
    if(state->job)state->job->cancel();
    const unsigned serial=++state->serial;
    state->changes=opad::json::array();changes->clear();details->hide();
    for(QPushButton* b:{intoFile,merge,copy,compare,discard})b->setEnabled(false);
    if(row<0 || size_t(row)>=state->entries.size())return;
    const Entry e=state->entries[size_t(row)];
    const QString file=QFileInfo(e.source).fileName();
    heading->setText(e.source.isEmpty()?e.title:file);
    where->setText(tr("Snapshot of %1 · %2").arg(e.source.isEmpty()?tr("an unsaved document"):QDir::toNativeSeparators(e.source),i18n::localTime(e.time.toStdString())));
    summary->setText(e.summary);
    base->setStyleSheet({});base->setProperty("state",QString());base->setProperty("hash",QString());
    base->setText(tr("Reading the snapshot and comparing it with its file…"));
    const bool saved=!e.source.isEmpty(),same=sameDocument(e),exists=saved && QFileInfo::exists(e.source);
    intoFile->setEnabled(saved);copy->setEnabled(true);merge->setEnabled(same);compare->setEnabled(exists);discard->setEnabled(true);
    intoFile->setToolTip(saved?tr("Open the snapshot as %1 again, its changes unsaved: Save writes them there").arg(file):tr("Never saved: there is no file to restore it into."));
    merge->setToolTip(same?tr("Add the snapshot's changes to the open document as one step you can undo"):tr("Open the document this snapshot was taken of to merge it there."));
    copy->setToolTip(tr("Open the snapshot as a new unsaved document; its file stays as it is"));
    compare->setToolTip(exists?tr("Show what the snapshot has over %1").arg(file):tr("Its file is not on disk: there is nothing to compare it with."));
    primary(intoFile,saved);primary(copy,!saved);
    auto out=std::make_shared<Preview>();
    state->job=m_jobs->quiet(tr("Reading recovery snapshot"),[out,e](Progress p){*out=preview(e,p);},[=](bool ok,const QString& error){
      if(!alive || serial!=state->serial)return;
      state->job=nullptr;
      const Tokens& t=theme::current();
      if(!ok){
        base->setProperty("state","error");base->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(t.error)));
        base->setText(tr("This snapshot cannot be read: %1").arg(error));
        for(QPushButton* b:{intoFile,merge,copy,compare})b->setEnabled(false);
        return;
      }
      const opad::json& d=out->diff;const QString& onto=out->onto;
      const int incoming=d.value("incoming",0),conflicts=int(d.value("conflicts",opad::json::array()).size());
      QString text;QColor colour=t.warning;
      if(onto=="unsaved"){text=tr("Never saved: Restore as copy opens it, Save as keeps it.");colour=t.fg2;}
      else if(onto=="missing")text=tr("%1 is no longer on disk: Restore into file writes it there again when you save.").arg(file);
      else if(onto=="unreadable"){text=tr("%1 cannot be read now (%2): Restore into file asks before replacing it.").arg(file,out->error);colour=t.error;}
      else if(onto=="same"){
        text=out->hash=="same"?tr("%1 is as it was when this snapshot was taken (its hash matches). Restore into file brings back the changes below; Save writes them.").arg(file)
                              :tr("%1 holds the same history as when this snapshot was taken. Restore into file brings back the changes below; Save writes them.").arg(file);
        colour=t.green;
      } else if(onto=="extends" && !d.contains("merge_error")){
        text=tr("%1 has changes saved after this snapshot (%n). Restore into file keeps them and adds the changes below after them.",nullptr,incoming).arg(file);
        if(conflicts)text+=' '+tr("Changes of both to the same things: %n (the snapshot's come last).",nullptr,conflicts);
      } else if(onto=="other")text=tr("%1 is another document now. Restore into file asks before replacing it; Restore as copy keeps both.").arg(file);
      else text=tr("%1 was rewritten after this snapshot (another branch, a reset or an older copy). Restore into file asks before replacing it; Restore as copy keeps both.").arg(file);
      base->setText(text);base->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(colour)));
      base->setProperty("state",onto);base->setProperty("hash",out->hash);
      state->changes=d.value("changes",opad::json::array());
      ComparePanel::listChanges(changes,state->changes,state->rows,state->order);
      QString line=state->changes.empty()?tr("Nothing to restore: the file has every change of this snapshot."):
                   tr("Changes: %n. %1.",nullptr,int(state->changes.size())).arg(ComparePanel::summaryOf(state->changes,d.value("relation","")));
      if(!out->editing.isEmpty())line+=' '+out->editing;
      summary->setText(line);
    });
  });
  list->setCurrentRow(0);
  return dialog;
}
void RecoveryManager::answerOffer(int result,const Entry& entry) {
  auto report=[this](bool ok,const QString& text){
    emit answered(ok,text);
    if(ok)emit status(text);
    else if(qEnvironmentVariableIsSet("OPAD_BENCH_SETTINGS"))trace::log("recovery: "+text);  // hidden benches: never a dialog on the desktop
    else QMessageBox::warning(m_window,tr("Recovery"),text);
  };
  if(result==Discard){
    const auto file=entry.file;
    m_jobs->async(tr("Discarding recovery snapshot"),[file](Progress){if(!QFile::remove(file))throw opad::Error("Cannot remove recovery snapshot");QFile::remove(file+".meta");},
                  [report](bool ok,const QString& e){report(ok,ok?tr("Recovery snapshot discarded."):e);});
  } else if(result==Compare)emit compareRequested(entry.source,entry.file,entry.time);
  else if(result==MergeCurrent)afterCapture([this,entry,report]{mergeInto(entry,report);});
  else if(result==RestoreCopy || result==RestoreFile){
    if(m_doc->isDirty())return report(false,sameDocument(entry)?tr("The open document has unsaved changes: merge the snapshot into it, or save or close it first."):
                                                               tr("Save or close the current document before recovery."));
    afterCapture([this,entry,report,keep=result==RestoreFile]{restore(entry,keep,report);});
  }
}
// An autosave or a save copying the document holds it for a moment: an answer waits for that rather than fail.
void RecoveryManager::afterCapture(std::function<void()> fn,int tries) {
  if(!m_doc->snapshotBusy() || tries<=0)return fn();
  QPointer<RecoveryManager> self(this);
  QTimer::singleShot(100,this,[self,fn=std::move(fn),tries]{if(self)self->afterCapture(fn,tries-1);});
}
void RecoveryManager::finishSession(std::function<void()> done) {
  m_closing=true;m_timer.stop();const auto session=m_session;const auto recovered=m_recoveredFiles;
  auto finished=std::make_shared<std::atomic<bool>>(false);
  m_jobs->backgroundNext();
  m_jobs->async(tr("Closing recovery session"),[session,recovered,finished](Progress){
    std::lock_guard guard(session->mutex);session->closed=true;
    for(const auto& file:QDir(session->directory).entryList({"*.opad-recovery","*.opad-recovery.meta","*.opad-base"},QDir::Files))QFile::remove(session->directory+"/"+file);
    session->lock.reset();QDir().rmdir(session->directory);
    for(const auto& file:recovered){
      const auto directory=QFileInfo(file).absolutePath();QLockFile lock(directory+"/owner.lock");lock.setStaleLockTime(0);if(!lock.tryLock(0))continue;
      const auto prefix=QFileInfo(file).fileName().section('_',0,0);
      for(const auto& name:QDir(directory).entryList({prefix+"_*.opad-recovery",prefix+"_*.opad-recovery.meta",prefix+"_*.opad-base"},QDir::Files))QFile::remove(directory+"/"+name);
    }
    finished->store(true,std::memory_order_release);
  });
  // Closing must wait for actual cleanup, even if Cancel was pressed in the strip.
  auto* timer=new QTimer(this);timer->setInterval(10);
  connect(timer,&QTimer::timeout,this,[timer,finished,done]{if(finished->load(std::memory_order_acquire)){timer->stop();timer->deleteLater();done();}});timer->start();
}

void RecoveryManager::bench(const QString& mode) {
  auto fail=[](const QString& e){trace::log("bench: recovery FAIL: "+e);QCoreApplication::exit(2);};
  if(mode.startsWith("read")) {
    const bool feature=mode=="read-feature",component=mode=="read-component";
    scan([this,fail,feature,component](std::vector<Entry> entries,QString error){
      if(!error.isEmpty() || entries.empty())return fail("missing abandoned snapshot "+error);
      restore(entries.front(),false,[this,fail,feature,component](bool ok,QString error){try{
        if(!ok)return fail(error);
        if(!m_doc->isDirty() || !m_doc->path().isEmpty() || m_design->featureActive() || m_design->sketchActive())return fail("recovery reopened a tool or lost dirty state");
        if(!m_doc->scene.features.empty())return fail("unfinished feature was committed");
        const auto id=m_doc->scene.sketches.back().id;
        auto sketch=opad::design::Sketch::from_json(m_doc->scene.sketch(id)->geometry);
        if(component){  // a new sketch drawn in the active component comes back in it (UI-33)
          const auto* lid=m_doc->scene.node(m_doc->scene.sketch(id)->component);
          if(!lid || lid->name!="Lid")return fail("the new sketch drawn in the Lid came back at the root");
        }else if(!feature){
          if(sketch.points.back().x!=32)return fail("sketch draft missing");
          m_doc->undo();if(opad::design::Sketch::from_json(m_doc->scene.sketch(id)->geometry).points.back().x!=20)return fail("draft undo baseline missing");m_doc->redo();
        }
        const auto saved=m_session->root+"/recovered-test.opad";m_doc->saveAs(saved);
        if(opad::resolve(opad::Document::load(saved.toStdString())).sketches.empty())return fail("recovery save/reopen failed");
        finishSession([this,fail]{scan([fail](std::vector<Entry> entries,QString error){if(!entries.empty() || !error.isEmpty())return fail("clean session left recovery snapshots");trace::log("bench: recovery draft, neutral tool state, undo/redo, save/reopen and cleanup PASS");QCoreApplication::exit(0);});});
      }catch(const std::exception& e){fail(e.what());}});
    });return;
  }
  try {
    opad::design::Sketch sk;const auto a=sk.add_point(0,0,true),b=sk.add_point(20,0);sk.add_line(a,b);
    if(mode=="write-feature"){const auto c=sk.add_point(20,10),d=sk.add_point(0,10);sk.add_line(b,c);sk.add_line(c,d);sk.add_line(d,a);}
    if(mode=="write-component"){  // a new sketch being drawn in the active component
      m_doc->setActiveComponent(m_doc->run("component",{{"name","Lid"}}).value("id",""));
      m_design->restoreRecovery({{"type","sketch"},{"id",""},{"name","Lid sketch"},{"plane",{{"base","xy"}}},{"frame",opad::Frame{}.to_json()},{"geometry",sk.to_json()}});
      auto* timer=new QTimer(this);timer->setInterval(30);
      connect(timer,&QTimer::timeout,this,[this,timer,fail]{if(m_design->sketch()->busy())return;timer->stop();timer->deleteLater();
        saveNow([fail](bool ok,const QString& error){if(!ok)return fail(error);trace::log("bench: new sketch in a component recovery snapshot PASS; simulated crash");std::_Exit(0);});});
      timer->start();return;
    }
    auto plan=opad::design::plan_ops(m_doc->doc,{opad::design::make_sketch_op("Recovery sketch",{{"base","xy"}},sk.to_json())});
    m_doc->commitPlan(std::move(plan),tr("sketch"));const auto id=m_doc->scene.sketches.back().id;
    if(mode=="write-feature"){
      m_design->restoreRecovery({{"type","feature"},{"kind","extrude"},{"id",""},{"inputs",{{"profiles",opad::json::array({{{"sketch",id},{"at",{5,5}}}})},{"distance","13 mm"},{"start","offset"},{"start_offset","4 mm"}}}});
      saveNow([fail](bool ok,const QString& error){if(!ok)return fail(error);trace::log("bench: pending feature recovery snapshot PASS; simulated crash");std::_Exit(0);});return;
    }
    sk.points.back().x=32;
    m_design->restoreRecovery({{"type","sketch"},{"id",id},{"name","Recovery sketch"},{"plane",{{"base","xy"}}},{"frame",opad::Frame{}.to_json()},{"geometry",sk.to_json()}});
    auto* timer=new QTimer(this);timer->setInterval(30);
    connect(timer,&QTimer::timeout,this,[this,timer,fail]{if(m_design->sketch()->busy())return;timer->stop();timer->deleteLater();
      if(!m_doc->captureSnapshot(m_jobs,[this,fail](std::shared_ptr<opad::Document> copy,const QString&){
        if(copy || m_doc->snapshotBusy())return fail("cancelled snapshot published or retained write guard");
        saveNow([this,fail](bool ok,const QString& error){if(!ok)return fail(error);
          const auto files=QDir(m_session->directory).entryList({"*.opad-recovery","*.opad-base"},QDir::Files,QDir::Name);
          saveNow([this,fail,files](bool saved,const QString& problem){
            if(!saved)return fail(problem);
            if(files!=QDir(m_session->directory).entryList({"*.opad-recovery","*.opad-base"},QDir::Files,QDir::Name))return fail("unchanged document wrote another snapshot");
            scan([fail](std::vector<Entry> entries,QString error){
          if(!entries.empty() || !error.isEmpty())return fail("offered a live instance for recovery");
          trace::log("bench: recovery snapshot, cancellation guard and incremental unchanged suppression and live-instance exclusion PASS; simulated crash");std::_Exit(0);
        });});});
      }))return fail("snapshot refused");
      m_jobs->current()->cancel();
      try {m_doc->run("param",{{"name","shouldNotWrite"},{"expr","1 mm"}});fail("write allowed while copying document");}catch(const opad::Error&){}
    });timer->start();
  }catch(const std::exception& e){fail(e.what());}
}
