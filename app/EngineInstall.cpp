#include "EngineInstall.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <filesystem>
#include <memory>

#include "Jobs.hpp"
#include "opad/util.hpp"

namespace engineinstall {

namespace {
QString qpath(const std::filesystem::path& p) { return QString::fromStdString(opad::path_to_utf8(p)); }
std::filesystem::path fspath(const QString& p) { return std::filesystem::path(p.toStdU16String()); }
}  // namespace

std::optional<opad::sim::EnginePack> pack(const QString& solver) {
  try {
    return opad::sim::engine_pack(solver.toStdString());
  } catch (const std::exception&) {  // an unreadable OPAD_ENGINE_PACKS
    return std::nullopt;
  }
}

QString sizeText(quint64 bytes) {
  const double mb = double(bytes) / (1024.0 * 1024.0);
  return mb < 10 ? QCoreApplication::translate("EngineInstall", "%1 MB").arg(mb, 0, 'f', 1)
                 : QCoreApplication::translate("EngineInstall", "%1 MB").arg(qRound(mb));
}

Installer* Installer::instance() {
  static QPointer<Installer> one;
  if (!one) one = new Installer(qApp);
  return one;
}

Installer::Installer(QObject* parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

void Installer::install(const QString& solver, JobRunner* jobs) {
  if (!jobs || m_running.contains(solver)) return;
  const auto p = pack(solver);
  if (!p) return;
  const QString name = QString::fromStdString(p->name);
  if (!opad::sim::engine_pack_url_allowed(p->url)) {  // never anything but OPAD's own release (or a test's local one)
    emit finished(solver, false, tr("%1 is not downloaded from %2: OPAD only downloads solvers from its own releases on GitHub.")
                                     .arg(name, QString::fromStdString(p->url)));
    return;
  }
  const std::filesystem::path root = opad::sim::engines_dir();
  if (root.empty()) {
    emit finished(solver, false, tr("OPAD has no folder to install %1 into.").arg(name));
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  const QString file = qpath(root / (p->folder() + ".download"));
  auto out = std::make_shared<QFile>(file);
  if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    emit finished(solver, false, tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(file), out->errorString()));
    return;
  }
  m_running.insert(solver);
  emit started(solver);

  Job* job = jobs->begin(tr("Downloading %1").arg(name));
  job->setPhase(tr("Downloading %1 (%2) from OPAD's releases on GitHub").arg(name, sizeText(p->size)), 0);
  QNetworkRequest request{QUrl(QString::fromStdString(p->url))};
  request.setHeader(QNetworkRequest::UserAgentHeader, "OPAD/" + QString::fromStdString(opad::version_string()));
  // GitHub answers a release asset with a redirect to its storage (https to https); the pinned hash checks what arrives.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(60000);  // a minute without a byte
  QNetworkReply* reply = m_net->get(request);
  auto written = std::make_shared<bool>(true);
  connect(reply, &QNetworkReply::readyRead, this, [reply, out, written] {
    const QByteArray chunk = reply->readAll();
    if (out->write(chunk) != chunk.size()) *written = false;
  });
  connect(reply, &QNetworkReply::downloadProgress, job, [job, name, total = p->size](qint64 got, qint64) {
    const int pct = total > 0 ? int(std::min<qint64>(100, got * 100 / qint64(total))) : -1;
    job->setPhase(tr("Downloading %1: %2 of %3").arg(name, sizeText(quint64(got)), sizeText(total)), pct);
  });
  connect(job, &Job::cancelRequested, reply, [reply] { reply->abort(); });
  connect(reply, &QNetworkReply::finished, this, [this, reply, out, written, job = QPointer<Job>(job), solver, pack = *p, file, name,
                                                  jobs = QPointer<JobRunner>(jobs)] {
    reply->deleteLater();
    if (reply->bytesAvailable() > 0) {
      const QByteArray rest = reply->readAll();
      if (out->write(rest) != rest.size()) *written = false;
    }
    out->close();
    const bool cancelled = job && job->cancelled();
    QString error;
    if (cancelled) error = "cancelled";
    else if (reply->error() != QNetworkReply::NoError)
      error = tr("Could not download %1: %2. Check the connection and try again.").arg(name, reply->errorString());
    else if (!*written)
      error = tr("Could not save the download of %1 (%2): is the disk full?").arg(name, QDir::toNativeSeparators(file));
    if (job && job->active()) job->finish(error.isEmpty(), error == "cancelled" ? QString() : error);
    if (!error.isEmpty()) {
      QFile::remove(file);
      return done(solver, false, error);
    }
    unpack(solver, pack, file, jobs);
  });
}

void Installer::unpack(const QString& solver, const opad::sim::EnginePack& pack, const QString& file, QPointer<JobRunner> jobs) {
  if (!jobs) {
    QFile::remove(file);
    return done(solver, false, "cancelled");
  }
  const QString name = QString::fromStdString(pack.name);
  auto where = std::make_shared<QString>();
  Job* job = jobs->async(
      tr("Installing %1").arg(name),
      [pack, file, where](Progress progress) {
        const auto dir = opad::sim::install_pack(pack, fspath(file), [&progress](double f, const std::string& phase) {
          progress.setPhase(QString::fromStdString(phase), int(f * 100));
          return !progress.cancelled();
        });
        *where = qpath(dir);
      },
      [this, solver, file](bool ok, const QString& error) {
        QFile::remove(file);
        done(solver, ok, ok ? QString() : error.isEmpty() ? QStringLiteral("cancelled") : error);
      });
  job->setDoneText(tr("%1 installed").arg(name));
}

void Installer::done(const QString& solver, bool ok, const QString& error) {
  m_running.remove(solver);
  emit finished(solver, ok, error);
}

}  // namespace engineinstall
