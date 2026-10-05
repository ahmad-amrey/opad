// OPAD_BENCH_WHEEL=1 (a document with a box; case in tools/bench_cases/viewer.py): a scroll zooms as a wheel or pans as a
// trackpad (ScrollInput.hpp). The Ubuntu report: "scroll to zoom acts as panning, like it is scrolling a page" - under X11
// and XWayland Qt hands the mouse wheel over as a TouchPad. Synthetic wheel events from such a device, told apart as on xcb:
// whole notches zoom (in, out, two notches more than one) with the view's direction kept and nothing panned, fractions pan,
// a notch amid a finger scroll pans on, Ctrl with fingers zooms; as on Windows the same device pans as before and a mouse
// zooms. Then the setting, chosen in Preferences: Trackpad pans pans the same notch (and a mouse's), Mouse wheel zooms zooms
// a touchpad's notch and a finger gesture; Automatic again.
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointingDevice>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>

#include <Graphic3d_Camera.hxx>

#include <cmath>
#include <functional>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Preferences.hpp"

bool Viewport::benchWheel(const std::function<bool(int)>& choose) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: wheel: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  if (!m_initialised) return false;
  const scrollinput::Mode saved = m_scrollInput;
  m_scrollInput = scrollinput::Mode::Automatic;
  fitAll();
  m_view->Redraw();
  const Handle(Graphic3d_Camera) start = new Graphic3d_Camera(*m_view->Camera());
  const QPointF at(width() * 0.5, height() * 0.5);
  const opad::Vec3 centre{start->Center().X(), start->Center().Y(), start->Center().Z()};
  const QPoint centreShown = widgetPoint(centre);
  // XWayland's one pointer as Qt's xcb plugin describes it, and a pointer that says Mouse.
  QPointingDevice touchpad("xwayland-pointer:13", 21, QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Generic,
                           QInputDevice::Capability::Position | QInputDevice::Capability::Scroll, 1, 3);
  QPointingDevice mouse("bench mouse", 22, QInputDevice::DeviceType::Mouse, QPointingDevice::PointerType::Generic,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Scroll, 1, 3);
  auto reset = [&] {
    finishTrackpadScroll();
    m_view->SetCamera(new Graphic3d_Camera(*start));
    m_view->Redraw();
  };
  auto send = [&](const QPointingDevice& device, QPoint pixels, QPoint angle, Qt::ScrollPhase phase = Qt::NoScrollPhase,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event(at, mapToGlobal(at), pixels, angle, Qt::NoButton, modifiers, phase, false, Qt::MouseEventNotSynthesized, &device);
    QCoreApplication::sendEvent(this, &event);
    paintEvent(nullptr);  // a hidden window never paints: the frame's flush applies the queued zoom or drag
  };
  // What the steps since the last reset did: the scale's ratio, whether the view turned, how far the centre moved on screen.
  auto ratio = [&] { return m_view->Camera()->Scale() / start->Scale(); };
  auto turned = [&] { return !start->Direction().IsEqual(m_view->Camera()->Direction(), 1e-6); };
  auto shift = [&] { return widgetPoint(centre) - centreShown; };
  auto zoomed = [&](bool in) { return (in ? ratio() < 0.99 : ratio() > 1.01) && !turned() && std::abs(shift().x()) <= 2 && std::abs(shift().y()) <= 2; };
  auto panned = [&](double dy) { return std::abs(ratio() - 1) < 1e-6 && !turned() && std::abs(shift().x()) <= 2 && std::abs(shift().y() - dy) <= 3; };
  auto state = [&] { return QString("(scale x%1, centre moved %2,%3 px%4)").arg(ratio(), 0, 'f', 3).arg(shift().x()).arg(shift().y()).arg(turned() ? ", turned" : ""); };

  benchScrollPlatform("xcb");
  send(touchpad, {}, {0, 120});
  require(zoomed(true), "xcb: a wheel notch from a device that says TouchPad zooms in, the view's direction kept and nothing panned " + state());
  const double oneNotch = ratio();
  reset();
  send(touchpad, {}, {0, -120});
  require(zoomed(false), "xcb: a notch back zooms out " + state());
  reset();
  send(touchpad, {}, {0, 240});
  require(zoomed(true) && ratio() < oneNotch - 0.01, QString("xcb: two notches zoom in more than one (x%1)").arg(oneNotch, 0, 'f', 3) + ' ' + state());
  reset();
  for (int i = 0; i < 3; ++i) send(touchpad, {}, {0, 37});
  require(panned(3 * 37 / 8.0), "xcb: fractions of a notch (two fingers) pan by an eighth of their angle " + state());
  send(touchpad, {}, {0, 120});
  require(panned(3 * 37 / 8.0 + 15), "xcb: a notch amid that finger scroll pans on instead of jumping the zoom " + state());
  reset();
  send(touchpad, {}, {0, 37}, Qt::NoScrollPhase, Qt::ControlModifier);
  require(zoomed(true), "xcb: Ctrl with two fingers zooms " + state());
  reset();
  send(mouse, {}, {0, 120});
  require(zoomed(true), "xcb: a notch from a device that says Mouse zooms " + state());
  reset();

  benchScrollPlatform("windows");
  send(touchpad, {}, {0, 120});
  require(panned(15), "windows: a device that says TouchPad pans, as before " + state());
  reset();
  send(mouse, {}, {0, 120});
  require(zoomed(true), "windows: a mouse wheel zooms " + state());
  reset();

  // The setting, through its row in Preferences.
  benchScrollPlatform("xcb");
  require(choose(2) && m_scrollInput == scrollinput::Mode::Trackpad && QSettings().value("view/scrollInput").toInt() == 2,
          "Preferences: Trackpad pans is saved and reaches the view");
  send(touchpad, {}, {0, 120});
  require(panned(15), "Trackpad pans: the same notch pans " + state());
  reset();
  send(mouse, {}, {0, 120});
  require(panned(15), "Trackpad pans: a mouse's notch pans too " + state());
  reset();
  send(mouse, {}, {0, 120}, Qt::NoScrollPhase, Qt::ControlModifier);
  require(zoomed(true), "Trackpad pans: Ctrl with it zooms " + state());
  reset();
  benchScrollPlatform("windows");
  require(choose(1) && m_scrollInput == scrollinput::Mode::Wheel && QSettings().value("view/scrollInput").toInt() == 1,
          "Preferences: Mouse wheel zooms is saved and reaches the view");
  send(touchpad, {}, {0, 120});
  require(zoomed(true), "Mouse wheel zooms: a notch from a device that says TouchPad zooms " + state());
  reset();
  send(touchpad, {}, {}, Qt::ScrollBegin);
  send(touchpad, {0, 20}, {0, 80}, Qt::ScrollUpdate);
  send(touchpad, {}, {}, Qt::ScrollEnd);
  require(zoomed(true), "Mouse wheel zooms: a finger gesture (pixels and phases) zooms " + state());
  reset();
  require(choose(0) && m_scrollInput == scrollinput::Mode::Automatic && QSettings().value("view/scrollInput").toInt() == 0,
          "Preferences: Automatic again");
  send(touchpad, {}, {}, Qt::ScrollBegin);
  send(touchpad, {0, 20}, {0, 80}, Qt::ScrollUpdate);
  send(touchpad, {}, {}, Qt::ScrollEnd);
  require(panned(20), "Automatic: the finger gesture pans again " + state());
  reset();
  benchScrollPlatform({});
  if (m_scrollInput != saved) setScrollInput(int(saved));
  require(selection().empty(), "scrolling selected nothing");
  return all;
}

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
}  // namespace

OPAD_BENCH(OPAD_BENCH_WHEEL, wheel) {
  Viewport* v = w.m_viewport;
  const bool shown = until([&w, v] { return !w.m_loadJob && v->displayedCount() > 0 && v->remainingBodies() == 0; }, 120000);
  if (!shown) trace::log("bench: wheel: the box is displayed FAIL");
  // Preferences > Keyboard and mouse > Scroll wheel / trackpad, as a user sets it (the window opens off screen in benches).
  auto choose = [&w](int index) {
    PreferencesDialog* dialog = PreferencesDialog::open(&w, "keyboard", "view/scrollInput");
    QWidget* page = dialog ? dialog->pageWidget("keyboard") : nullptr;
    auto* box = page ? page->findChild<QComboBox*>("view/scrollInput") : nullptr;
    if (!box || box->count() != 3) return false;
    box->setCurrentIndex(index);
    dialog->close();
    return box->currentIndex() == index;
  };
  const bool ok = shown && v->benchWheel(choose);
  trace::log(QString("bench: wheel: %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
