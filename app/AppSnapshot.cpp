#include "AppDocument.hpp"
#include "Jobs.hpp"
#include "opad/geometry.hpp"
#include <QPointer>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>
#include <algorithm>
#include <set>

Job* AppDocument::saveAsync(JobRunner* jobs,const QString& requested,bool overwrite,
                           std::function<void(bool,const QString&)> done,int testDelayMs,bool overwriteDisk) {
  if(!hasDocument || loading || designBusy || m_capturing)throw opad::Error("Document is busy or not open; retry after the current operation.");
  if(browse)throw opad::Error("Viewer-mode geometry cannot be saved directly; import it into an OPAD document first.");
  const QString destination=requested.isEmpty()?path():requested;
  if(destination.isEmpty())throw opad::Error("This document has no file path. Supply an absolute .opad path for the first save.");
  if(!QDir::isAbsolutePath(destination) || QFileInfo(destination).suffix().compare("opad",Qt::CaseInsensitive)!=0)
    throw opad::Error("Save requires an absolute path ending in .opad.");
  if(readOnly && QFileInfo(destination)==QFileInfo(path()))throw opad::Error("This document is open read-only: save a copy to edit it.");
  followAssetPaths(destination);
  struct Save {
    std::atomic<bool> finished{false};bool written=false,blocked=false;QString error;
    std::vector<std::string> ids;size_t bodies=0;
    DiskStat stat;std::shared_ptr<const opad::Manifest> manifest;
  };
  auto result=std::make_shared<Save>();const auto source=m_storage;const auto current=path();
  const auto diskFile=m_diskFile;const auto diskStat=m_diskStat;  // the save guard (UI-56)
  const auto identity=generation;const auto savedRevision=revision;
  m_capturing=true;designBusy=true;emit undoChanged();
  auto* job=jobs->async(tr("Saving document"),[source,result,destination,current,overwrite,testDelayMs,diskFile,diskStat,overwriteDisk](Progress progress){
    try {
      const QFileInfo target(destination),original(current);
      const bool same=!current.isEmpty() && (QDir::cleanPath(destination)==QDir::cleanPath(current) ||
          (!original.canonicalFilePath().isEmpty() && original.canonicalFilePath()==target.canonicalFilePath()));
      const bool replace=overwrite || same;
      if(target.exists() && !replace)throw opad::Error("Destination already exists. Choose a new path or explicitly set overwrite=true.");
      if(!overwriteDisk && !diskFile.isEmpty() && target==QFileInfo(diskFile)){
        const auto now=statFile(destination);
        if(now.exists && now!=diskStat){result->blocked=true;throw opad::Error("changed_on_disk: the file changed on disk since this session read or wrote it (a git pull, another OPAD or opad-cli); merge or reload it, or save to another path");}
      }
      if(progress.cancelled())throw opad::Error("cancelled");
      const auto text=source->serialize();
      result->ids.reserve(source->ops.size());for(const auto& op:source->ops)result->ids.push_back(op.id);result->bodies=source->body_count();
      QSaveFile atomic(destination);atomic.setDirectWriteFallback(false);
      QTemporaryFile fresh(destination+".XXXXXX");QFileDevice* file=replace?static_cast<QFileDevice*>(&atomic):static_cast<QFileDevice*>(&fresh);
      if(!(replace?atomic.open(QIODevice::WriteOnly):fresh.open()))throw opad::Error(file->errorString().toStdString());
      constexpr size_t chunk=4*1024*1024;
      for(size_t offset=0;offset<text.size();){
        if(progress.cancelled())throw opad::Error("cancelled");
        const auto size=std::min(chunk,text.size()-offset);
        if(file->write(text.data()+offset,qint64(size))!=qint64(size))throw opad::Error(file->errorString().toStdString());
        offset+=size;progress.setOverall(int(offset*100/std::max(size_t(1),text.size())));
      }
      // Isolated acceptance harness: emulate slow I/O before the atomic publish.
      if(testDelayMs>0)QThread::msleep(static_cast<unsigned long>(testDelayMs));
      if(progress.cancelled())throw opad::Error("cancelled");
      if(replace){if(!atomic.commit())throw opad::Error(atomic.errorString().toStdString());}
      else {
        if(!fresh.flush())throw opad::Error(fresh.errorString().toStdString());fresh.close();
        // QFile::rename refuses to replace a file created since the initial check.
        if(!fresh.rename(destination))throw opad::Error(fresh.errorString().toStdString());fresh.setAutoRemove(false);
      }
      result->written=true;
      result->stat=statFile(destination);result->manifest=std::make_shared<opad::Manifest>(opad::Manifest::of(*source));
    }catch(const std::exception& e){result->error=QString::fromUtf8(e.what());}
    catch(...){result->error=QStringLiteral("Unexpected error while saving the document.");}
    result->finished.store(true,std::memory_order_release);
  });
  // Cancel finishes a Job before its worker exits. Keep the write guard and report
  // the actual disk outcome, even if cancellation arrives just after atomic publish.
  auto* timer=new QTimer(this);timer->setInterval(10);
  connect(timer,&QTimer::timeout,this,[this,timer,result,done,destination,identity,savedRevision]{
    if(!result->finished.load(std::memory_order_acquire))return;
    timer->stop();timer->deleteLater();m_capturing=false;designBusy=false;
    // The file holds what was written; it is this document's file when the path moved there or was already it.
    if(result->written && generation==identity && (revision==savedRevision || QFileInfo(destination)==QFileInfo(m_diskFile)))
      setDisk(QFileInfo(destination).absoluteFilePath(),result->stat,result->manifest);
    if(result->blocked)emit saveBlocked(false);
    if(result->written && generation==identity && revision==savedRevision){
      doc.path=std::filesystem::path(destination.toStdU16String());doc.header.format=opad::kFormatVersion;readOnly=false;
      m_savedIds=std::move(result->ids);m_savedBodies=result->bodies;doc.dirty=false;
      emit pathChanged();emit saved();emit message(tr("Saved %1").arg(destination));
    }
    emit undoChanged();done(result->written,result->error);
  });timer->start();return job;
}

bool AppDocument::captureSnapshot(JobRunner* jobs, SnapshotCallback done, bool background) {
  if (!hasDocument || loading || designBusy || m_capturing) return false;
  struct Copy {
    std::shared_ptr<opad::Document> document;
    QString error;
    std::atomic<bool> finished{false};
  };
  auto copy=std::make_shared<Copy>();
  m_capturing=true;designBusy=true;emit undoChanged();
  auto source=m_storage;
  if(background)jobs->backgroundNext();
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

Job* AppDocument::readAsync(JobRunner* jobs, const QString& title, std::function<void(const opad::Document&, const opad::Scene&, Progress)> work,
                            std::function<void(bool, const QString&)> done) {
  if (!hasDocument || loading || designBusy || m_capturing || m_converting) return nullptr;
  struct Read {
    std::atomic<bool> finished{false};
    bool ok = false;
    QString error;
  };
  auto read = std::make_shared<Read>();
  auto source = m_storage;
  auto resolved = std::make_shared<opad::Scene>(scene);
  designBusy = true;  // nothing changes the document while the worker reads it
  emit undoChanged();
  QPointer<Job> job = jobs->async(title, [source, resolved, read, work = std::move(work)](Progress p) {
    try {
      work(*source, *resolved, p);
      read->ok = !p.cancelled();
      if (!read->ok) read->error = QStringLiteral("cancelled");
    } catch (const std::exception& e) {
      read->error = QString::fromUtf8(e.what());
    }
    read->finished.store(true, std::memory_order_release);
  });
  // The job reports a cancel at once while its worker runs on: the document is released when the worker has stopped.
  auto* timer = new QTimer(this);
  timer->setInterval(10);
  connect(timer, &QTimer::timeout, this, [this, timer, read, done = std::move(done)] {
    if (!read->finished.load(std::memory_order_acquire)) return;
    timer->stop();
    timer->deleteLater();
    designBusy = false;
    emit undoChanged();
    if (done) done(read->ok, read->error);
  });
  timer->start();
  return job;
}

void AppDocument::afterCapture(std::function<void()> fn) {
  if (!m_capturing) return fn();
  const auto identity = generation;
  QTimer::singleShot(10, this, [this, identity, fn = std::move(fn)] {
    if (generation == identity) afterCapture(fn);
  });
}

// Viewer mode -> editable (kept beside the other worker-backed document jobs; AppDocument.cpp stays free of JobRunner).
void AppDocument::startEditable(JobRunner* jobs, std::function<void(bool, const QString&)> done) {
  if (!browse || loading || designBusy || m_converting || m_capturing) {
    if (done) done(false, tr("The document is busy; try again in a moment."));
    return;
  }
  m_converting = true;
  designBusy = true;  // nothing changes the document while the worker reads it
  emit undoChanged();
  struct Out {
    opad::Document doc;
    opad::EditableKeys keys;
  };
  auto out = std::make_shared<Out>();
  auto source = m_storage;
  const auto identity = generation;
  jobs->async(tr("Preparing the document for editing"), [source, out](Progress p) {
    p.setPhase(tr("Preparing bodies for editing"), 0);
    out->doc = opad::make_editable(*source, &out->keys, [p](double f) {
      p.setPhase(tr("Preparing bodies for editing"), static_cast<int>(f * 100));
      return !p.cancelled();
    });
    opad::warm_shape_cache(out->doc);
  }, [this, out, identity, done](bool ok, const QString& error) {
    m_converting = false;
    designBusy = false;
    emit undoChanged();
    if (!ok || generation != identity || !browse) {
      if (done) done(false, ok ? tr("The document changed meanwhile; try again.") : error);
      return;
    }
    emit bodyKeysRenamed(out->keys.renamed);
    doc = std::move(out->doc);
    browse = false;
    clearHistory();  // what was changed while viewing is part of the document now
    m_savedIds.clear();
    m_savedBodies = 0;
    refresh();
    emit pathChanged();
    if (done) done(true, {});
  });
}

void AppDocument::loadAssets(JobRunner* jobs, bool trustAll, std::function<void(bool, const QString&)> done, const std::vector<std::string>& only) {
  if (!hasDocument || browse || loading || designBusy) {
    if (done) done(false, tr("The document is busy; try again in a moment."));
    return;
  }
  // The linked imports whose bodies are not loaded, with their edits, in a document of their own (the ops only, never the
  // body store); read on a worker into the shared shape cache, their body entries join this document afterwards.
  auto probe = std::make_shared<opad::Document>(opad::Document::create());
  probe->path = doc.path;
  probe->shape_cache = doc.shape_cache;
  std::set<std::string> wanted;
  for (const auto& e : opad::effective_ops(doc)) {
    if (e.op->type != "import" || !e.data().contains("asset")) continue;
    if (!only.empty() && std::find(only.begin(), only.end(), e.op->id) == only.end()) continue;
    bool loaded = true;
    std::function<void(const opad::json&)> walk = [&](const opad::json& nodes) {
      for (const auto& n : nodes) {
        if (n.value("type", "") == "body") loaded = loaded && doc.has_body(n.value("key", ""));
        if (n.contains("children")) walk(n["children"]);
      }
    };
    walk(e.data().value("nodes", opad::json::array()));
    if (!loaded) wanted.insert(e.op->id);
  }
  try {
    for (const auto& o : doc.ops)
      if (wanted.count(o.id) || ((o.type == "edit" || o.type == "delete") && wanted.count(o.data.value("target", "")))) probe->append(o.data);
  } catch (const std::exception& e) {
    if (done) done(false, QString::fromUtf8(e.what()));
    return;
  }
  opad::AssetOptions options = assetOptions();
  options.trust_all = trustAll;
  auto states = std::make_shared<opad::json>(opad::json::array());
  const auto identity = generation;
  jobs->async(tr("Reading linked files"), [probe, options, states](Progress p) mutable {
    options.progress = [p](double, const std::string&) { return !p.cancelled(); };
    for (const auto& s : opad::load_assets(*probe, options)) states->push_back(s.to_json());
    opad::warm_shape_cache(*probe, [p](size_t, size_t) { return !p.cancelled(); });
  }, [this, probe, states, identity, done](bool ok, const QString& error) {
    if (!ok || generation != identity) {
      if (done) done(false, ok ? tr("The document changed meanwhile; try again.") : error);
      return;
    }
    for (const auto& b : probe->bodies())
      if (b.external) doc.add_external_body(b.key, b.meta);
    for (const auto& s : *states)  // the states of the files read again
      for (auto& known : assetStates)
        if (known.value("import", "") == s.value("import", "")) known = s;
    refresh();
    if (const QString linked = assetSummary(assetStates); !linked.isEmpty()) emit message(linked);
    if (done) done(true, {});
  });
}

void AppDocument::storeViewerCache(JobRunner* jobs) {
  if (!browse || m_cacheSource.isEmpty()) return;
  const QString source = std::exchange(m_cacheSource, QString());
  auto copy = std::make_shared<opad::Document>(doc);  // the import op and body entries; the shapes stay shared
  opad::ImportOptions options;
  options.center_drawing = m_cacheCenter;
  const double readMs = m_cacheReadMs;
  const std::filesystem::path file(source.toStdU16String());
  jobs->backgroundNext();
  jobs->async(tr("Remembering %1 for faster opening").arg(QFileInfo(source).fileName()), [copy, file, options, readMs](Progress p) {
    const opad::json kept = opad::viewer_cache_store(*copy, file, options, readMs, [p] { return p.cancelled(); });
    if (trace::enabled()) trace::log(QString("viewer cache: %1").arg(QString::fromStdString(kept.dump())));
  });
}
