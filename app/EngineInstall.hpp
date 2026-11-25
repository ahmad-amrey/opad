#pragma once
// Installs a solver pack (opad/sim/engine_packs.hpp) from inside OPAD: Simulate's "Install OpenFOAM (37 MB)" on a check and
// in the solver window. The zip comes only from the URL compiled into OPAD (a release of github.com/ahmad-amrey/opad; a
// local fake release only through OPAD_ENGINE_PACKS, for tests), downloads in a "Downloading <solver>" job (the status
// bar's strip, its Cancel aborts it), then a worker checks its size and SHA-256 against the pinned ones and unpacks it into
// the user's engines folder ("Installing <solver>" job). No administrator rights, nothing outside that folder; the
// download is deleted afterwards, whatever happened.
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <optional>

#include "opad/sim/engine_packs.hpp"

class JobRunner;
class QNetworkAccessManager;

namespace engineinstall {

// The pack this OPAD can install for a solver ("openfoam", "calculix"), or none here.
std::optional<opad::sim::EnginePack> pack(const QString& solver);
// "37 MB" (one decimal below 10 MB).
QString sizeText(quint64 bytes);

class Installer : public QObject {
  Q_OBJECT
 public:
  static Installer* instance();  // one for the app (owned by qApp)
  bool running(const QString& solver) const { return m_running.contains(solver); }
  // Starts it (nothing when it already runs, or when this OPAD has no pack for it): finished follows, also on failure or
  // cancel (error "cancelled").
  void install(const QString& solver, JobRunner* jobs);

 signals:
  void started(const QString& solver);
  void finished(const QString& solver, bool ok, const QString& error);

 private:
  explicit Installer(QObject* parent);
  void unpack(const QString& solver, const opad::sim::EnginePack& pack, const QString& file, QPointer<JobRunner> jobs);
  void done(const QString& solver, bool ok, const QString& error);
  QNetworkAccessManager* m_net;
  QSet<QString> m_running;
};

}  // namespace engineinstall
