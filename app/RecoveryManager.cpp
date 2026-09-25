#include "RecoveryManager.hpp"
#include "AppDocument.hpp"
#include "AgentBridge.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
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
#include <QLabel>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUuid>
#include <QVBoxLayout>
#include <mutex>
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
};
namespace {
void writeAtomic(const QString& file,const QByteArray& bytes) {
  QSaveFile out(file);
  if(!out.open(QIODevice::WriteOnly) || out.write(bytes)!=bytes.size() || !out.commit())
    throw opad::Error("Cannot write recovery snapshot: "+out.errorString().toStdString());
}
std::string digest(const std::string& content) {
  return QCryptographicHash::hash(QByteArray::fromStdString(content),QCryptographicHash::Sha256).toHex().toStdString();
}
opad::json readJson(const QString& path) {
  QFile file(path);if(!file.open(QIODevice::ReadOnly))throw opad::Error(file.errorString().toStdString());
  return opad::json::parse(file.readAll().toStdString());
}
opad::json readRecord(const QString& path) {
  auto record=readJson(path);
  const int format=record.value("format",0);
  const auto payload=format==1?record.at("document").get<std::string>()+record.at("edit").dump():record.at("delta").dump()+record.at("edit").dump();
  if(digest(payload)!=record.at("sha256").get<std::string>())throw opad::Error("Recovery snapshot checksum does not match; choose an earlier snapshot");
  if(format==1)return record;
  if(format!=2)throw opad::Error("Unsupported recovery snapshot format");
  const auto& delta=record.at("delta");const auto base=QString::fromStdString(delta.at("base").get<std::string>());
  if(base!=QFileInfo(base).fileName() || !base.endsWith(".opad-base") || base.contains('\\') || base.contains('/'))throw opad::Error("Invalid recovery base path");
  QFile file(QFileInfo(path).absolutePath()+"/"+base);if(!file.open(QIODevice::ReadOnly))throw opad::Error("Recovery base is missing");
  const auto content=file.readAll().toStdString();
  if(digest(content)!=delta.at("base_sha256").get<std::string>())throw opad::Error("Recovery base checksum does not match");
  auto document=opad::Document::parse(content);const auto count=delta.at("keep_ops").get<size_t>();
  if(count>document.ops.size())throw opad::Error("Invalid recovery operation boundary");
  document.truncate_ops(count);
  for(const auto& body:delta.at("bodies"))if(document.add_body(body.at("brep").get<std::string>(),body.at("meta"))!=body.at("key").get<std::string>())throw opad::Error("Recovery body checksum does not match");
  for(const auto& op:delta.at("ops"))document.append(op);
  record["document"]=document.serialize();return record;
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
  const QSettings settings;
  const QString data=settings.format()==QSettings::IniFormat?QFileInfo(settings.fileName()).absolutePath():QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  m_session->root=data+"/recovery";
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
  if(!m_doc->isDirty() && !editing && m_checkpoint==m_savedCheckpoint){if(done)done(true,{});return;}
  m_running=true;
  const auto generation=m_doc->generation,revision=m_doc->revision;
  const auto source=m_doc->path(),title=m_doc->title();
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
      m_jobs->async(tr("Saving recovery snapshot"),[session,document,edit,source,title,epoch,written,checkpoint](Progress progress){
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
        for(const auto& body:document->bodies())if(!session->baseBodies.count(body.key))delta["bodies"].push_back({{"key",body.key},{"meta",body.meta},{"brep",body.brep}});
        auto draft=edit;
        if(draft.is_object() && draft.value("type","")=="sketch" && draft.contains("geometry")){
          const auto scene=opad::resolve(*document);const auto* sketch=scene.sketch(draft.value("id",std::string()));
          if(sketch){draft["geometry_delta"]=opad::design::sketch_delta(sketch->geometry,draft.at("geometry"));draft.erase("geometry");}
        }
        const auto checksum=digest(delta.dump()+draft.dump());
        if(checksum==session->lastSignature){*written=true;return;}
        opad::json record={{"format",2},{"title",title.toStdString()},{"source",source.toStdString()},{"time",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()},
          {"delta",std::move(delta)},{"edit",draft},{"sha256",checksum}};
        if(progress.cancelled() || !epoch->load())return;
        const auto file=session->directory+"/"+prefix+"_"+stamp+".opad-recovery";
        writeAtomic(file,QByteArray::fromStdString(record.dump()));
        opad::json meta={{"title",record["title"]},{"time",record["time"]},{"source",record["source"]},{"base",session->baseFile.toStdString()}};
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
        try {const auto record=readMetadata(path);entries->push_back({path,QString::fromStdString(record.value("title","Untitled")),QString::fromStdString(record.value("time","")),QString::fromStdString(record.value("source",""))});}
        catch(const std::exception& e){trace::log(QString("recovery: skipped %1: %2").arg(path,e.what()));}
      }
    }
  },[self,entries,done](bool ok,const QString& error){if(self)done(ok?std::move(*entries):std::vector<Entry>{},error);});
}
void RecoveryManager::restore(const Entry& entry,std::function<void(bool,QString)> done) {
  if(m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive()){done(false,tr("Finish the current operation before recovery."));return;}
  struct Result {opad::Document document;opad::Scene scene;std::optional<opad::design::Plan> draft;};
  auto result=std::make_shared<Result>();const auto generation=m_doc->generation,revision=m_doc->revision;QPointer<RecoveryManager> self(this);
  m_jobs->async(tr("Recovering document"),[result,entry](Progress progress){
    const auto record=readRecord(entry.file);result->document=opad::Document::parse(record.at("document").get<std::string>());
    progress.setPhase(tr("Preparing recovered geometry"));opad::warm_shape_cache(result->document,[progress](size_t,size_t){return !progress.cancelled();});
    result->scene=opad::resolve(result->document);
    auto edit=record.at("edit");
    if(edit.is_object() && edit.contains("geometry_delta")){
      const auto* sketch=result->scene.sketch(edit.at("id").get<std::string>());if(!sketch)throw opad::Error("Recovery sketch baseline is missing");
      edit["geometry"]=opad::design::apply_sketch_delta(sketch->geometry,edit.at("geometry_delta"));
    }
    if(edit.is_object() && edit.value("type","")=="sketch"){
      const auto id=edit.value("id",std::string());
      const auto op=id.empty()?opad::design::make_sketch_op(edit.value("name","Recovered sketch"),edit.at("plane"),edit.at("geometry")):
        opad::json{{"op","edit"},{"target",id},{"set",{{"plane",edit.at("plane")},{"geometry",edit.at("geometry")}}}};
      result->draft=opad::design::plan_ops(result->document,{op});
    }
  },[=,this](bool ok,const QString& error){
    if(!self)return;
    if(!ok){done(false,error);return;}
    if(m_doc->generation!=generation || m_doc->revision!=revision || m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive()){done(false,tr("Document changed during recovery. Retry when ready."));return;}
    try {m_doc->recover(std::move(result->document),std::move(result->scene));if(result->draft)m_doc->commitPlan(std::move(*result->draft),tr("Recovered sketch"));m_recoveredFiles<<entry.file;done(true,{});}
    catch(const std::exception& e){done(false,QString::fromUtf8(e.what()));}
  });
}
void RecoveryManager::offerRecovery() {
  scan([this](std::vector<Entry> entries,QString error){
    if(!error.isEmpty()){QMessageBox::warning(m_window,tr("Recovery"),error);return;}
    if(entries.empty()){emit status(tr("No recoverable documents found."));return;}
    QDialog dialog(m_window);dialog.setWindowTitle(tr("Recover documents"));dialog.resize(540,340);auto* layout=new QVBoxLayout(&dialog);
    auto* note=new QLabel(tr("These snapshots were left by a previous session. Recover opens an unsaved copy. Later keeps them for another time."));note->setWordWrap(true);layout->addWidget(note);
    auto* list=new QListWidget;for(const auto& entry:entries)list->addItem(entry.title+" — "+entry.time+"\n"+entry.source);list->setCurrentRow(0);layout->addWidget(list);
    auto* buttons=new QDialogButtonBox;auto* recover=buttons->addButton(tr("Recover"),QDialogButtonBox::AcceptRole);auto* discard=buttons->addButton(tr("Discard snapshot"),QDialogButtonBox::DestructiveRole);auto* later=buttons->addButton(tr("Later"),QDialogButtonBox::RejectRole);layout->addWidget(buttons);
    connect(recover,&QPushButton::clicked,&dialog,&QDialog::accept);connect(later,&QPushButton::clicked,&dialog,&QDialog::reject);
    connect(discard,&QPushButton::clicked,&dialog,[&]{dialog.done(2);});
    const int result=dialog.exec(),index=list->currentRow();if(index<0)return;
    if(result==2){const auto file=entries.at(size_t(index)).file;m_jobs->async(tr("Discarding recovery snapshot"),[file](Progress){if(!QFile::remove(file))throw opad::Error("Cannot remove recovery snapshot");});}
    else if(result==QDialog::Accepted){
      if(m_doc->isDirty()){QMessageBox::information(m_window,tr("Recovery"),tr("Save or close the current document before recovery."));return;}
      restore(entries.at(size_t(index)),[this](bool ok,const QString& e){if(!ok)QMessageBox::warning(m_window,tr("Recovery"),e);else emit status(tr("Document recovered. Use Save As to keep it."));});
    }
  });
}
void RecoveryManager::finishSession(std::function<void()> done) {
  m_closing=true;m_timer.stop();const auto session=m_session;const auto recovered=m_recoveredFiles;
  auto finished=std::make_shared<std::atomic<bool>>(false);
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
    const bool feature=mode=="read-feature";
    scan([this,fail,feature](std::vector<Entry> entries,QString error){
      if(!error.isEmpty() || entries.empty())return fail("missing abandoned snapshot "+error);
      restore(entries.front(),[this,fail,feature](bool ok,QString error){try{
        if(!ok)return fail(error);
        if(!m_doc->isDirty() || !m_doc->path().isEmpty() || m_design->featureActive() || m_design->sketchActive())return fail("recovery reopened a tool or lost dirty state");
        if(!m_doc->scene.features.empty())return fail("unfinished feature was committed");
        const auto id=m_doc->scene.sketches.back().id;
        auto sketch=opad::design::Sketch::from_json(m_doc->scene.sketch(id)->geometry);
        if(!feature){
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
