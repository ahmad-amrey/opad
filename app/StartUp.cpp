#include "StartUp.hpp"

#include <QElapsedTimer>
#include <QEvent>
#include <QTimer>
#include <QThread>
#include <QWindow>

#include <Font_FontMgr.hxx>

#include <atomic>

#include <utility>

#include "Jobs.hpp"
#include "MainWindow.hpp"

namespace startup {
namespace {
QElapsedTimer& clock() {
  static QElapsedTimer c;
  return c;
}
Marks& state() {
  static Marks m;
  return m;
}
qint64 now() { return clock().isValid() ? clock().elapsed() : 0; }
std::atomic<bool> fontsRead{true};  // false while prepare()'s worker reads the font list

class Sequence : public QObject {
 public:
  Sequence(MainWindow* window, QString file, bool readOnly) : QObject(window), m_window(window), m_file(std::move(file)), m_readOnly(readOnly) {
    if (QWindow* handle = window->windowHandle()) handle->installEventFilter(this);
    QTimer::singleShot(kExposeWaitMs, this, [this] { go(false); });
  }
  bool eventFilter(QObject* watched, QEvent* event) override {
    // Seen before the window handles it: the shell is painted in that handling, and what is posted here runs after it.
    if (event->type() == QEvent::Expose && static_cast<QWindow*>(watched)->isExposed() && state().exposed < 0) {
      state().exposed = now();
      QTimer::singleShot(0, this, [this] { go(true); });
    }
    return false;
  }

 private:
  void go(bool exposed) {
    if (m_started) return;
    if (!fontsRead) {  // hardly ever: the list is read long before the window is built
      QTimer::singleShot(10, this, [this, exposed] { go(exposed); });
      return;
    }
    m_started = true;
    if (QWindow* handle = m_window->windowHandle()) handle->removeEventFilter(this);
    Marks& m = state();
    m.byExpose = exposed;
    m.viewer = now();
    m_window->warmUpViewport();
    m.viewerDone = now();
    QTimer::singleShot(0, this, [this] {
      Marks& m = state();
      m.frame = now();
      m_window->drawFirstViewportFrame();
      m.frameDone = now();
      trace::log(QStringLiteral("startup: first interactive frame (%1 ms after main: viewer %2 ms, first frame %3 ms, %4)")
                     .arg(m.frameDone).arg(m.viewerDone - m.viewer).arg(m.frameDone - m.frame)
                     .arg(m.byExpose ? QStringLiteral("after the first expose") : QStringLiteral("never exposed")));
      QTimer::singleShot(0, this, [this] {
        state().opened = now();
        if (!m_file.isEmpty()) m_window->openPath(m_file, m_readOnly);
        deleteLater();
      });
    });
  }
  MainWindow* m_window;
  QString m_file;
  bool m_readOnly = false;
  bool m_started = false;
};
}  // namespace

void begin() { clock().start(); }

void prepare() {
  fontsRead = false;
  QThread* worker = QThread::create([] {
    Font_FontMgr::GetInstance();  // its constructor reads the system's font list
    fontsRead = true;
  });
  QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();
}

void run(MainWindow* window, const QString& file, bool readOnly) {
  state().shown = now();
  new Sequence(window, file, readOnly);
}

const Marks& marks() { return state(); }
}  // namespace startup
