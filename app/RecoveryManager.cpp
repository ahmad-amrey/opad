#include "RecoveryManager.hpp"
#include "AppDocument.hpp"
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

struct RecoveryManager::Session {
  QString root,directory;
  std::unique_ptr<QLockFile> lock;
  std::mutex mutex;
  bool closed=false;
};
namespace {
void writeAtomic(const QString& file,const QByteArray& bytes) {
  QSaveFile out(file);
  if(!out.open(QIODevice::WriteOnly) || out.write(bytes)!=bytes.size() || !out.commit())
    throw opad::Error("Cannot write recovery snapshot: "+out.errorString().toStdString());
}
opad::json readRecord(const QString& path) {
  QFile file(path);if(!file.open(QIODevice::ReadOnly))throw opad::Error(file.errorString().toStdString());
  auto record=opad::json::parse(file.readAll().toStdString());
  if(record.value("format",0)!=1)throw opad::Error("Unsupported recovery snapshot format");
  const auto payload=record.at("document").get<std::string>()+record.at("edit").dump();
  const auto hash=QCryptographicHash::hash(QByteArray::fromStdString(payload),QCryptographicHash::Sha256).toHex().toStdString();
  if(hash!=record.at("sha256").get<std::string>())throw opad::Error("Recovery snapshot checksum does not match; choose an earlier snapshot");
  return record;
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
  connect(doc,&AppDocument::pathChanged,this,[this]{if(m_doc->hasDocument && !m_doc->loading && !m_doc->isDirty() && !m_doc->path().isEmpty() && !m_design->sketchActive() && !m_design->featureActive())discardCurrent();});
  QTimer::singleShot(1500,this,[this]{
    if(qEnvironmentVariableIsSet("OPAD_BENCH_SETTINGS"))return;
    if(m_doc->loading){
      auto connection=std::make_shared<QMetaObject::Connection>();
      *connection=connect(m_doc,&AppDocument::loadFinished,this,[this,connection]{disconnect(*connection);offerRecovery();});return;
    }
    offerRecovery();
  });
}
void RecoveryManager::discardCurrent() {
  m_epoch->store(false);m_epoch=std::make_shared<std::atomic<bool>>(true);
  if(!m_doc->hasDocument)return;
  const auto session=m_session;
  const auto prefix=QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(m_doc->doc.header.uuid),QCryptographicHash::Sha256).toHex());
  m_jobs->async(tr("Clearing saved recovery snapshots"),[session,prefix](Progress){
    std::lock_guard guard(session->mutex);
    for(const auto& file:QDir(session->directory).entryList({prefix+"_*.opad-recovery"},QDir::Files))QFile::remove(session->directory+"/"+file);
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
  auto* explanation=new QLabel(tr("Snapshots include unfinished sketch and feature edits. They do not overwrite your file. Recovery opens an unsaved document; use Save As to keep it."));explanation->setWordWrap(true);layout->addWidget(explanation);
  auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);layout->addWidget(buttons);
  connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  if(dialog.exec()==QDialog::Accepted){QSettings().setValue("recovery/enabled",enabled->isChecked());QSettings().setValue("recovery/minutes",minutes->value());configureTimer();}
}
void RecoveryManager::saveNow(std::function<void(bool,const QString&)> done) {
  auto fail=[done](const QString& error){if(done)done(false,error);};
  if(m_closing || m_running || !m_doc->hasDocument || m_doc->browse || m_doc->loading || m_doc->designBusy)return fail(tr("Document is busy; recovery will retry."));
  const bool editing=m_design->sketchActive() || m_design->featureActive();
  if(!m_doc->isDirty() && !editing){if(done)done(true,{});return;}
  m_running=true;
  const auto generation=m_doc->generation,revision=m_doc->revision;
  const auto source=m_doc->path(),title=m_doc->title();
  const auto epoch=m_epoch;
  QPointer<RecoveryManager> self(this);
  auto captured=[=,this](opad::json edit,const QString& error){
    if(!self)return;
    if(!error.isEmpty() || m_closing || m_doc->generation!=generation || m_doc->revision!=revision){m_running=false;fail(error.isEmpty()?tr("Document changed during capture; retrying later."):error);return;}
    const auto session=m_session;
    const bool started=m_doc->captureSnapshot(m_jobs,[=,this,edit=std::move(edit)](std::shared_ptr<opad::Document> document,const QString& snapshotError){
      if(!self)return;
      if(!document || m_closing){m_running=false;fail(snapshotError);return;}
      auto written=std::make_shared<std::atomic<bool>>(false);
      m_jobs->async(tr("Saving recovery snapshot"),[session,document,edit,source,title,epoch,written](Progress progress){
        std::lock_guard guard(session->mutex);if(session->closed || progress.cancelled() || !epoch->load())return;
        if(!session->lock){
          if(!QDir().mkpath(session->directory))throw opad::Error("Cannot create recovery folder");
          session->lock=std::make_unique<QLockFile>(session->directory+"/owner.lock");session->lock->setStaleLockTime(0);
          if(!session->lock->tryLock(0))throw opad::Error("Cannot lock recovery session");
        }
        progress.setPhase(tr("Serializing recovery snapshot"));
        auto content=document->serialize();auto payload=content+edit.dump();
        const auto checksum=QCryptographicHash::hash(QByteArray::fromStdString(payload),QCryptographicHash::Sha256).toHex().toStdString();
        opad::json record={{"format",1},{"title",title.toStdString()},{"source",source.toStdString()},{"time",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()},
          {"document",std::move(content)},{"edit",edit},{"sha256",checksum}};
        if(progress.cancelled() || !epoch->load())return;
        const auto prefix=QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(document->header.uuid),QCryptographicHash::Sha256).toHex());
        const auto file=session->directory+"/"+prefix+"_"+QString::number(QDateTime::currentMSecsSinceEpoch())+".opad-recovery";
        writeAtomic(file,QByteArray::fromStdString(record.dump()));
        if(!epoch->load()){QFile::remove(file);return;}
        *written=true;
        // Only prune this instance's generated records, never user documents.
        const auto records=QDir(session->directory).entryList({prefix+"_*.opad-recovery"},QDir::Files,QDir::Name);
        for(int i=0;i<records.size()-3;++i)QFile::remove(session->directory+"/"+records[i]);
      },[self,done,written](bool ok,const QString& error){
        if(!self)return;self->m_running=false;
        const bool saved=ok && *written;
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
        try {const auto record=readRecord(path);entries->push_back({path,QString::fromStdString(record.value("title","Untitled")),QString::fromStdString(record.value("time","")),QString::fromStdString(record.value("source",""))});}
        catch(const std::exception& e){trace::log(QString("recovery: skipped %1: %2").arg(path,e.what()));}
      }
    }
  },[self,entries,done](bool ok,const QString& error){if(self)done(ok?std::move(*entries):std::vector<Entry>{},error);});
}
void RecoveryManager::restore(const Entry& entry,std::function<void(bool,QString)> done) {
  if(m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive()){done(false,tr("Finish the current operation before recovery."));return;}
  struct Result {opad::Document document;opad::Scene scene;opad::json edit;};
  auto result=std::make_shared<Result>();const auto generation=m_doc->generation,revision=m_doc->revision;QPointer<RecoveryManager> self(this);
  m_jobs->async(tr("Recovering document"),[result,entry](Progress progress){
    const auto record=readRecord(entry.file);result->document=opad::Document::parse(record.at("document").get<std::string>());
    progress.setPhase(tr("Preparing recovered geometry"));opad::warm_shape_cache(result->document,[progress](size_t,size_t){return !progress.cancelled();});
    result->scene=opad::resolve(result->document);result->edit=record.at("edit");
  },[=,this](bool ok,const QString& error){
    if(!self)return;
    if(!ok){done(false,error);return;}
    if(m_doc->generation!=generation || m_doc->revision!=revision || m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive()){done(false,tr("Document changed during recovery. Retry when ready."));return;}
    try {m_doc->recover(std::move(result->document),std::move(result->scene));m_design->restoreRecovery(result->edit);m_recoveredFiles<<entry.file;done(true,{});}
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
    for(const auto& file:QDir(session->directory).entryList({"*.opad-recovery"},QDir::Files))QFile::remove(session->directory+"/"+file);
    session->lock.reset();QDir().rmdir(session->directory);
    for(const auto& file:recovered){
      const auto directory=QFileInfo(file).absolutePath();QLockFile lock(directory+"/owner.lock");lock.setStaleLockTime(0);if(!lock.tryLock(0))continue;
      const auto prefix=QFileInfo(file).fileName().section('_',0,0);
      for(const auto& name:QDir(directory).entryList({prefix+"_*.opad-recovery"},QDir::Files))QFile::remove(directory+"/"+name);
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
      restore(entries.front(),[this,fail,feature](bool ok,QString error){
        if(!ok)return fail(error);
        if(feature) {
          if(!m_doc->isDirty() || !m_doc->path().isEmpty() || !m_design->featureActive() || !m_doc->scene.features.empty())return fail("pending feature was lost or committed during recovery");
          const auto inputs=m_design->featurePanel()->inputs();
          if(inputs.value("distance","")!="13 mm" || inputs.value("start_offset","")!="4 mm" || inputs.at("profiles").size()!=1)return fail("feature inputs/picks were lost");
          QMetaObject::invokeMethod(m_design->featurePanel(),"accepted");
          auto* timer=new QTimer(this);timer->setInterval(50);auto ticks=std::make_shared<int>(0);
          connect(timer,&QTimer::timeout,this,[this,timer,ticks,fail]{
            if(++*ticks>200){timer->stop();return fail("recovered feature commit timed out");}
            if(m_design->featureActive())return;timer->stop();timer->deleteLater();
            if(m_doc->scene.features.size()!=1)return fail("recovered feature did not commit");
            m_doc->undo();if(!m_doc->scene.features.empty())return fail("feature undo failed");m_doc->redo();
            finishSession([]{trace::log("bench: pending feature inputs, preview, commit and undo/redo recovery PASS");QCoreApplication::exit(0);});
          });timer->start();return;
        }
        if(!m_doc->path().isEmpty() || !m_doc->isDirty() || !m_design->sketchActive() || !m_design->sketch()->modified())return fail("recovery state/dirty/editor");
        auto* timer=new QTimer(this);timer->setInterval(30);
        connect(timer,&QTimer::timeout,this,[this,timer,fail]{if(m_design->sketch()->busy())return;timer->stop();timer->deleteLater();
          m_design->finishSketch([this,fail]{try{
            if(m_doc->scene.sketches.empty())throw opad::Error("recovered sketch missing");
            const auto id=m_doc->scene.sketches.back().id;
            auto sketch=opad::design::Sketch::from_json(m_doc->scene.sketch(id)->geometry);
            if(sketch.points.back().x!=32)throw opad::Error("recovered edit was not committed");
            m_doc->undo();sketch=opad::design::Sketch::from_json(m_doc->scene.sketch(id)->geometry);
            if(sketch.points.back().x!=20)throw opad::Error("recovered edit lost its original undo baseline");m_doc->redo();
            const auto saved=m_session->root+"/recovered-test.opad";m_doc->saveAs(saved);const auto reopened=opad::Document::load(saved.toStdString());
            if(opad::design::Sketch::from_json(opad::resolve(reopened).sketch(id)->geometry).points.back().x!=32)throw opad::Error("recovery save/reopen failed");
            finishSession([this,fail]{scan([fail](std::vector<Entry> entries,QString error){if(!entries.empty() || !error.isEmpty())return fail("clean session left recovery snapshots");trace::log("bench: recovery crash, active sketch, undo/redo, save/reopen and cleanup PASS");QCoreApplication::exit(0);});});
          }catch(const std::exception& e){fail(e.what());}});
        });timer->start();
      });
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
        saveNow([this,fail](bool ok,const QString& error){if(!ok)return fail(error);scan([fail](std::vector<Entry> entries,QString error){
          if(!entries.empty() || !error.isEmpty())return fail("offered a live instance for recovery");
          trace::log("bench: recovery snapshot, cancellation guard and live-instance exclusion PASS; simulated crash");std::_Exit(0);
        });});
      }))return fail("snapshot refused");
      m_jobs->current()->cancel();
      try {m_doc->run("param",{{"name","shouldNotWrite"},{"expr","1 mm"}});fail("write allowed while copying document");}catch(const opad::Error&){}
    });timer->start();
  }catch(const std::exception& e){fail(e.what());}
}
