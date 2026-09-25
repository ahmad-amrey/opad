#include "AppDocument.hpp"
#include "Jobs.hpp"
#include <QPointer>

bool AppDocument::captureSnapshot(JobRunner* jobs, SnapshotCallback done) {
  if (!hasDocument || loading || designBusy || m_capturing) return false;
  struct Copy {
    std::shared_ptr<opad::Document> document;
    QString error;
    std::atomic<bool> finished{false};
  };
  auto copy=std::make_shared<Copy>();
  m_capturing=true;designBusy=true;emit undoChanged();
  auto source=m_storage;
  QPointer<Job> job=jobs->async(tr("Capturing document"),[source,copy](Progress p){
    try { if(!p.cancelled())copy->document=std::make_shared<opad::Document>(*source); }
    catch(const std::exception& e){copy->error=QString::fromUtf8(e.what());}
    copy->finished.store(true,std::memory_order_release);
  });
  const auto progress=job->progress();
  // Job cancellation reports immediately, but the copy may still be reading. Release
  // the write guard only after the worker has actually stopped using the source.
  auto* timer=new QTimer(this);timer->setInterval(10);
  connect(timer,&QTimer::timeout,this,[this,copy,progress,timer,done=std::move(done)]{
    if(!copy->finished.load(std::memory_order_acquire))return;
    timer->stop();timer->deleteLater();m_capturing=false;designBusy=false;emit undoChanged();
    if(progress.cancelled())done({},tr("Snapshot cancelled"));
    else done(std::move(copy->document),copy->error);
  });timer->start();return true;
}

