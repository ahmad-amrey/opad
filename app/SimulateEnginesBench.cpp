// Bench: installing a solver pack from inside OPAD (EngineInstall.hpp), against a fake release served by the bench itself
// on 127.0.0.1 (OPAD_ENGINE_PACKS names it; the pinned table never comes into it). Case in tools/bench_cases/simulate.py:
// APPDATA, LOCALAPPDATA and OPAD_ENGINES_DIR point at the case's own folders, so no OpenFOAM of this computer is found.
//
// OPAD_BENCH_ENGINE_INSTALL=<prefix> on an empty document: the airflow check on a reducer is greyed and its Run step offers
// "Install OpenFOAM (<size>)"; the solver window offers Install with the size beside the official page; a download that is
// not the pinned file (one byte changed) is refused with the reason in a toast and leaves nothing behind; the right one
// downloads in a "Downloading" job (the button says Installing meanwhile), is checked and unpacked by an "Installing"
// job, and the check can run: the solvers are read again, flow is ready, engines() names the pack as installed.
// Shots: <prefix>.engine-run.png (before), .engine-window.png, .engine-installed.png.
// OPAD_BENCH_ENGINE_ZIP=<a real pack's zip> with OPAD_BENCH_ENGINE_VERSION=<its version> serves that pack instead (the same
// steps, the real file through the button); OPAD_BENCH_ENGINE_KEEP=1 leaves the installed pack for a solve afterwards.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "EngineInstall.hpp"
#include "GuidedSteps.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "SimChecks.hpp"
#include "SimulateArea.hpp"
#include "SimulateCheck.hpp"
#include "SimulateSolver.hpp"
#include "Toast.hpp"
#include "opad/sim/engine_packs.hpp"

namespace {
bool until(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return done();
}

quint32 crc32(const QByteArray& data) {
  quint32 c = 0xFFFFFFFFu;
  for (char ch : data) {
    c ^= quint8(ch);
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// A zip of stored members: the fake pack.
QByteArray zipOf(const QList<QPair<QString, QByteArray>>& files) {
  QByteArray out, dir;
  auto u16 = [](QByteArray& s, quint32 v) { s.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF)); };
  auto u32 = [&](QByteArray& s, quint32 v) { u16(s, v & 0xFFFF), u16(s, v >> 16); };
  for (const auto& [name, data] : files) {
    const QByteArray n = name.toUtf8();
    const quint32 at = quint32(out.size()), crc = crc32(data);
    auto header = [&](QByteArray& s, bool central) {
      u32(s, central ? 0x02014b50 : 0x04034b50);
      if (central) u16(s, 20);
      u16(s, 20), u16(s, 0), u16(s, 0), u16(s, 0), u16(s, 0), u32(s, crc), u32(s, quint32(data.size())), u32(s, quint32(data.size()));
      u16(s, quint32(n.size())), u16(s, 0);
      if (central) u16(s, 0), u16(s, 0), u16(s, 0), u32(s, 0), u32(s, at);
      s += n;
    };
    header(out, false);
    out += data;
    header(dir, true);
  }
  const quint32 dirAt = quint32(out.size());
  out += dir;
  u32(out, 0x06054b50), u16(out, 0), u16(out, 0), u16(out, quint32(files.size())), u16(out, quint32(files.size()));
  u32(out, quint32(dir.size())), u32(out, dirAt), u16(out, 0);
  return out;
}

// The fake release: GET /<name> answers that file, a slice every 20 ms (the download is seen to move).
class FakeRelease : public QObject {
 public:
  explicit FakeRelease(QObject* parent) : QObject(parent), m_server(new QTcpServer(this)) {
    m_server->listen(QHostAddress::LocalHost, 0);
    connect(m_server, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket* s = m_server->nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, s, [this, s] {
          const QByteArray head = s->peek(4096);
          if (!head.contains("\r\n\r\n")) return;
          s->readAll();
          const QString path = QString::fromLatin1(head.split(' ').value(1)).mid(1);
          const QByteArray body = m_files.value(path);
          ++m_requests;
          if (body.isEmpty()) {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
          }
          s->write("HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
          auto* timer = new QTimer(s);
          auto at = std::make_shared<qsizetype>(0);
          connect(timer, &QTimer::timeout, s, [s, body, at, timer] {
            const qsizetype n = std::min<qsizetype>(256 * 1024, body.size() - *at);
            s->write(body.mid(*at, n));
            *at += n;
            if (*at >= body.size()) timer->stop(), s->disconnectFromHost();
          });
          timer->start(20);
        });
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
      }
    });
  }
  QString url(const QString& name) const { return QString("http://127.0.0.1:%1/%2").arg(m_server->serverPort()).arg(name); }
  QHash<QString, QByteArray> m_files;
  int m_requests = 0;

 private:
  QTcpServer* m_server;
};
}  // namespace

OPAD_BENCH(OPAD_BENCH_ENGINE_INSTALL, engineinstall) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: engine install: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  auto shot = [prefix = value](QWidget* widget, const QString& name) {
    if (widget && !prefix.isEmpty() && prefix != "1") widget->grab().save(prefix + "." + name + ".png");
  };
  auto toastWith = [&w](const QString& part) -> Toast* {
    for (Toast* t : w.m_toasts->toasts())
      if (t->text().contains(part)) return t;
    return nullptr;
  };
  AppDocument* doc = w.m_doc;
  Simulate* area = w.findChild<Simulate*>();
  const QString engines = qEnvironmentVariable("OPAD_ENGINES_DIR");
  if (!area || !doc->hasDocument || engines.isEmpty()) {
    require(false, "an empty document, the Simulate area and the case's own OPAD_ENGINES_DIR");
    return QCoreApplication::exit(2), true;
  }

  // The fake release and the table naming it (in place of the pinned one).
  auto* release = new FakeRelease(&w);
  QByteArray filler(3 * 1024 * 1024, '\0');  // a pack of some size, so the download runs a moment
  for (int i = 0; i < filler.size(); ++i) filler[i] = char((i * 7919) >> 3);
  QByteArray good = zipOf({{"bin/simpleFoam.exe", "not a program: the bench's stand-in"},
                           {"bin/libfiller.dll", filler},
                           {"etc/controlDict", "// the bench's stand-in"},
                           {"NOTICE.txt", "OpenFOAM vBench for OPAD (the bench's fake pack)"},
                           {"opad-pack.json", R"({"id": "openfoam", "version": "vbench-1"})"}});
  QString version = "vbench-1";
  if (const QString real = qEnvironmentVariable("OPAD_BENCH_ENGINE_ZIP"); !real.isEmpty()) {
    QFile f(real);
    if (!f.open(QIODevice::ReadOnly) || qEnvironmentVariableIsEmpty("OPAD_BENCH_ENGINE_VERSION")) {
      require(false, "OPAD_BENCH_ENGINE_ZIP is a readable pack and OPAD_BENCH_ENGINE_VERSION its version");
      return QCoreApplication::exit(2), true;
    }
    good = f.readAll();
    version = qEnvironmentVariable("OPAD_BENCH_ENGINE_VERSION");
  }
  QByteArray bad = good;
  bad[bad.size() / 2] = char(bad[bad.size() / 2] ^ 0x55);  // the same size, one byte changed
  release->m_files["good.zip"] = good, release->m_files["bad.zip"] = bad;
  QDir().mkpath(engines);
  const QString table = QDir::cleanPath(QDir(engines).filePath("../engine-packs.json"));
  auto publish = [&](const QString& file) {
    QSaveFile f(table);  // whole or not at all: the solvers may be read meanwhile
    if (f.open(QIODevice::WriteOnly))
      f.write(QString(R"([{"id": "openfoam", "name": "OpenFOAM vBench", "version": "%4", "platform": "windows-x64", "url": "%1", "size": %2, "sha256": "%3"}])")
                  .arg(release->url(file))
                  .arg(good.size())
                  .arg(QString::fromLatin1(QCryptographicHash::hash(good, QCryptographicHash::Sha256).toHex()))
                  .arg(version)
                  .toUtf8());
    f.commit();
  };
  publish("bad.zip");
  qputenv("OPAD_ENGINE_PACKS", QDir::toNativeSeparators(table).toUtf8());
  qunsetenv("OPAD_OPENFOAM");

  w.setWorkspace("simulate");
  area->refreshEngines();
  // The read begun at the start knew no table: the one asked now follows it.
  require(until([area] { return area->engines().value("packs", opad::json::object()).contains("openfoam"); }, 20000), "the solvers are looked for again on a worker");
  if (area->engines().value("flow", false)) {
    require(false, "OpenFOAM is not found before the install (the case's folders hide this computer's)");
    return QCoreApplication::exit(2), true;
  }
  require(area->engines().contains("packs") && area->engines()["packs"].contains("openfoam"),
          "engines() names the pack this OPAD can install");

  // The airflow check on a reducer, its Run step.
  const std::string reducer = [&] {
    until([doc] { return !doc->designBusy && !doc->snapshotBusy(); }, 10000);
    return doc->run("feature", {{"kind", "cone"}, {"name", "Reducer"},
                                {"inputs", {{"plane", {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}}}, {"diameter", 200}, {"top_diameter", 150}, {"height", 120}}}})["body_ids"][0]
        .get<std::string>();
  }();
  opad::Ref part;
  part.body = reducer;
  area->services().select({part});
  area->openCheck("fitting");
  auto* check = w.findChild<GuidedCheck*>();
  if (!check || check->check() != "fitting") {
    require(false, "the airflow check opens");
    return QCoreApplication::exit(2), true;
  }
  check->frame()->setCurrent(check->stepNames().indexOf("run"));
  auto* button = check->findChild<QPushButton*>("checkSolver");
  const QString size = engineinstall::sizeText(quint64(good.size()));
  require(!check->runButton()->isEnabled(), "Run is greyed without OpenFOAM");
  require(button && button->isVisibleTo(check) && button->text().contains("Install OpenFOAM") && button->text().contains(size),
          "the Run step offers to install OpenFOAM with its size: " + (button ? button->text() : QString()));
  shot(area->checkPanel(), "engine-run");

  // The solver window: Install with the size, the official page beside it.
  {
    bool asked = false;
    SolverWindow window("openfoam", {}, &w, [&asked] { asked = true; });
    window.show();
    QCoreApplication::processEvents();
    auto* install = window.findChild<QPushButton*>("solverInstall");
    require(install && install->text().contains(size) && window.findChild<QPushButton*>("solverGet") && window.findChild<QPushButton*>("solverFind"),
            "the solver window offers Install (" + size + "), the official page and I already have it");
    shot(&window, "engine-window");
    if (install) install->click();
    require(asked && !window.isVisible(), "its Install hands over and closes the window");
  }

  // A download that is not the pinned file: refused, nothing left behind.
  const int before = release->m_requests;
  button->click();
  require(until([&] { return toastWith("was not installed"); }, 60000), "a download that is not the pinned file is refused");
  if (Toast* t = toastWith("was not installed")) require(t->text().contains("SHA-256"), "the toast says why: " + t->text());
  require(release->m_requests == before + 1, "it was asked of the fake release, once");
  require(QDir(engines).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(), "nothing is left in the engines folder (no download, no half-unpacked pack)");
  require(!area->engines().value("flow", false) && button->isEnabled(), "the check still waits for OpenFOAM and offers Install again");

  // The right one: downloading (the button says so), checked, unpacked; the check can run.
  publish("good.zip");
  button->click();
  require(until([&] { return w.m_jobs->titles().join("|").contains("Downloading OpenFOAM vBench"); }, 5000), "the download runs as a job (status bar, Cancel)");
  require(!button->isEnabled() && button->text().contains("Installing"), "the button says it is installing: " + button->text());
  require(until([&] { return toastWith("is installed"); }, 60000), "the pack is installed");
  require(until([area] { return area->engines().value("flow", false); }, 20000), "the solvers are read again: OpenFOAM is found");
  const QString dir = QDir(engines).filePath("openfoam-" + version);
  require(QFile::exists(dir + "/bin/simpleFoam.exe") && QFile::exists(dir + "/etc/controlDict"), "it is unpacked into the engines folder");
  require(QDir(engines).entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == QStringList{"openfoam-" + version}, "the download is gone, only the pack stays");
  require(area->engines()["packs"]["openfoam"].value("installed", opad::json()).is_string(), "engines() names it as installed");
  require(!button->isVisibleTo(check), "the check no longer asks for the solver");
  shot(area->checkPanel(), "engine-installed");

  if (qEnvironmentVariableIsEmpty("OPAD_BENCH_ENGINE_KEEP")) opad::sim::remove_pack("openfoam");
  QFile::remove(table);
  trace::log(QString("bench: engine install: done %1").arg(all ? "PASS" : "FAIL"));
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
