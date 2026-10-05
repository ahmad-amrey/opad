// OPAD_BENCH_WHEEL=<prefix> or 1 (a document with a box; case in tools/bench_cases/viewer.py, with OPAD_BENCH_SCROLLASK so that the
// question is asked in a bench): a scroll zooms as a wheel or pans as a trackpad (ScrollInput.hpp). The Ubuntu report:
// "scroll to zoom acts as panning, like it is scrolling a page". First nothing is chosen: as on xcb and Windows a mouse
// wheel is assumed (Automatic on macOS, nothing asked there), so a touchpad's fraction of a notch zooms and the first
// scroll asks once ("Using a trackpad?" card, not again on the next scroll); Trackpad pans on it is saved and the next
// fraction pans; Keep zoom saves the wheel; closing it keeps the assumption unsaved and asks no more. Then Automatic - on
// a Wayland desktop OPAD runs through XWayland, whose relative pointer Qt's xcb plugin calls a TouchPad. Synthetic wheel events
// from such a device, told apart as on xcb: whole notches zoom (in, out, two notches more than one) with the view's direction
// kept and nothing panned, a high-resolution wheel's eighths of a notch zoom (eight of them about as much as a notch, out by
// 45), fractions pan by an eighth of their angle, a notch amid a finger scroll pans on, an Xorg touchpad's pixels (8 x the
// finger) pan by the finger's own px, Ctrl with fingers zooms; as on Windows the same device pans as before and a mouse
// zooms. Then the setting, chosen in Preferences: Trackpad pans pans the same notch (and a mouse's), Mouse wheel zooms zooms
// a touchpad's notch and a finger gesture; Automatic again. OPAD_BENCH_WHEEL=<prefix> saves <prefix>.card.png.
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
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
#include "Toast.hpp"

bool Viewport::benchWheel(const std::function<bool(int)>& choose, const std::function<QList<Toast*>()>& cards, const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: wheel: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  if (!m_initialised) return false;
  const scrollinput::Mode saved = m_scrollInput;
  fitAll();
  m_view->Redraw();
  const Handle(Graphic3d_Camera) start = new Graphic3d_Camera(*m_view->Camera());
  const QPointF at(width() * 0.5, height() * 0.5);
  const opad::Vec3 centre{start->Center().X(), start->Center().Y(), start->Center().Z()};
  const QPoint centreShown = widgetPoint(centre);
  // XWayland's relative pointer as Qt's xcb plugin describes it, and a pointer that says Mouse.
  QPointingDevice touchpad("xwayland-relative-pointer:13", 21, QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Generic,
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

  // Nothing chosen (the bench's settings are fresh): Windows and Linux assume a mouse wheel and the first scroll asks.
  QSettings settings;
  auto unchosen = [&] {
    settings.remove("view/scrollInput");
    settings.remove("view/scrollAsked");
  };
  require(!settings.contains("view/scrollInput") && !settings.contains("view/scrollAsked") && m_scrollInput == scrollinput::Mode::Wheel && m_scrollAsk,
          "nothing chosen at the start: a mouse wheel is assumed here and the first scroll will ask");
  for (const char* platform : {"xcb", "wayland", "cocoa"}) {
    benchScrollPlatform(platform);
    readScrollInput();
    const bool mac = QByteArray(platform) == "cocoa";
    require(m_scrollInput == (mac ? scrollinput::Mode::Automatic : scrollinput::Mode::Wheel) && m_scrollAsk == !mac,
            QString("%1: nothing chosen: %2").arg(platform, mac ? "Automatic, nothing asked" : "a mouse wheel, asked at the first scroll"));
  }
  auto question = [&]() -> Toast* {
    const QList<Toast*> shown = cards();
    return shown.size() == 1 ? shown.first() : nullptr;
  };
  benchScrollPlatform("xcb");
  readScrollInput();
  send(touchpad, {}, {0, 37});
  require(zoomed(true), "xcb, nothing chosen: a touchpad's fraction of a notch zooms as a wheel's " + state());
  Toast* card = question();
  require(card && card->text() == QObject::tr("Scrolling zooms the view. Using a trackpad?") && !card->detail().isEmpty() && card->actionButton(0) &&
              card->actionButton(1) && !card->actionButton(2) && card->actionButton(0)->text() == QObject::tr("Trackpad pans") &&
              card->actionButton(1)->text() == QObject::tr("Keep zoom") && card->parentWidget() == this && !m_scrollAsk,
          "the first scroll asks once: a card over the view, Trackpad pans and Keep zoom");
  if (card) {
    // Its rows: the question with × at the top, the detail under it, the answers below at the end of their row (mirrored
    // right to left); bottom centre of the view, inside it.
    const bool rtl = card->layoutDirection() == Qt::RightToLeft;
    const QRect text = card->findChild<QLabel*>("toastText")->geometry(), keep = card->actionButton(1)->geometry(),
                pans = card->actionButton(0)->geometry(), close = card->closeButton()->geometry();
    const QRect detail = card->findChild<QLabel*>("toastDetail") ? card->findChild<QLabel*>("toastDetail")->geometry() : QRect();
    const bool rows = text.bottom() < detail.top() && detail.bottom() < pans.top() && pans.top() == keep.top() && close.top() < detail.top();
    const bool ends = rtl ? keep.left() < pans.left() && keep.left() - card->rect().left() <= 16 && close.left() < text.left()
                          : keep.right() > pans.right() && card->rect().right() - keep.right() <= 16 && close.right() > text.right();
    const bool placed = rect().contains(card->geometry()) && std::abs(card->geometry().center().x() - rect().center().x()) <= 1;
    require(rows && ends && placed, QString("the card's rows: question and ×, detail, answers at the end (%1)").arg(rtl ? "rtl" : "ltr"));
    if (!prefix.isEmpty()) {
      const QRect area = card->geometry().adjusted(-24, -24, 24, 24);
      require(window()->grab(QRect(mapTo(window(), area.topLeft()), area.size())).save(prefix + ".card.png"), "screenshot " + prefix + ".card.png");
    }
  }
  reset();
  send(touchpad, {}, {0, 37});
  require(zoomed(true) && cards().size() == 1 && question() == card, "the next scroll zooms too and asks nothing more " + state());
  reset();
  if (card) card->actionButton(0)->click();  // Trackpad pans
  require(m_scrollInput == scrollinput::Mode::Trackpad && settings.value("view/scrollInput").toInt() == 2 && settings.value("view/scrollAsked").toBool() &&
              cards().isEmpty(),
          "Trackpad pans on the card: chosen and saved, asked, the card gone");
  send(touchpad, {}, {0, 37});
  require(panned(37 / 8.0), "after Trackpad pans the next fraction pans " + state());
  reset();
  readScrollInput();
  require(m_scrollInput == scrollinput::Mode::Trackpad && !m_scrollAsk, "the next start: Trackpad pans, nothing asked");
  unchosen();
  benchScrollPlatform("windows");
  readScrollInput();
  send(touchpad, {}, {0, 120});
  require(zoomed(true), "windows, nothing chosen: a notch from a device that says TouchPad zooms " + state());
  reset();
  card = question();
  if (card) card->actionButton(1)->click();  // Keep zoom
  require(card && m_scrollInput == scrollinput::Mode::Wheel && settings.value("view/scrollInput").toInt() == 1 && settings.value("view/scrollAsked").toBool() &&
              cards().isEmpty(),
          "Keep zoom on the card: the wheel saved, asked");
  unchosen();
  readScrollInput();
  send(mouse, {}, {0, 120});
  reset();
  card = question();
  if (card) card->closeButton()->click();
  require(card && cards().isEmpty() && !settings.contains("view/scrollInput") && settings.value("view/scrollAsked").toBool() &&
              m_scrollInput == scrollinput::Mode::Wheel,
          "closed: the assumption stays unsaved, the question remembered");
  send(mouse, {}, {0, 120});
  require(zoomed(true) && cards().isEmpty(), "a scroll after it zooms and asks nothing " + state());
  reset();
  readScrollInput();
  require(m_scrollInput == scrollinput::Mode::Wheel && !m_scrollAsk, "the next start: still the wheel, nothing asked");

  // Automatic: told apart by what the scroll carries.
  m_scrollInput = scrollinput::Mode::Automatic;
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
  for (int i = 0; i < 8; ++i) send(touchpad, {}, {0, 15});
  const double asNotch = std::log(ratio()) / std::log(oneNotch);
  require(zoomed(true) && asNotch > 0.85 && asNotch < 1.15,
          QString("xcb: eight steps of a high-resolution wheel (15 each) zoom in about as much as a notch (%1 of one)").arg(asNotch, 0, 'f', 2) + ' ' + state());
  reset();
  send(touchpad, {}, {0, -45});
  require(zoomed(false), "xcb: a faster step of it back (-45) zooms out " + state());
  reset();
  for (int i = 0; i < 3; ++i) send(touchpad, {}, {0, 37});
  require(panned(3 * 37 / 8.0), "xcb: fractions of a notch (two fingers) pan by an eighth of their angle " + state());
  send(touchpad, {}, {0, 120});
  require(panned(3 * 37 / 8.0 + 15), "xcb: a notch amid that finger scroll pans on instead of jumping the zoom " + state());
  reset();
  send(touchpad, {0, 160}, {0, 160});
  require(panned(20), "xcb: an Xorg touchpad's 20 px of finger (pixel and angle deltas 160) pan 20 px, not 160 " + state());
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
  // The "Using a trackpad?" cards showing (PreferencesArea::askScrollInput).
  auto cards = [&w] {
    QList<Toast*> out;
    for (Toast* t : w.m_toasts->toasts())
      if (t->property("question").toString() == "view/scrollInput") out << t;
    return out;
  };
  const bool ok = shown && v->benchWheel(choose, cards, value == "1" ? QString() : value);
  trace::log(QString("bench: wheel: %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
