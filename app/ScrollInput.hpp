#pragma once
// A scroll in the 3D view: a mouse wheel (zoom at the cursor) or two fingers on a trackpad (pan, Shift orbits, a pinch or
// Ctrl zooms), decided without widgets from what the event carries. Viewport::wheelEvent asks; tests/test_scroll_input.cpp.
// No Qt here.
//
// What the platforms send (Qt 6.4 to 6.10):
//  - xcb (X11, and XWayland on a Wayland desktop: main() picks xcb on Linux): Qt's XI2 code takes every pointer of unknown
//    type that scrolls with relative valuators for a TouchPad (XWayland's one pointer is such a device), sends no scroll
//    phase, an angle delta of valuator delta / increment * 120 and a pixel delta only when the increment exceeds 15. A
//    wheel notch is a whole 120 there, fingers move by fractions of it: the device's word alone made every wheel pan.
//  - wayland: a wheel has no pixel delta and no phase; fingers have both.
//  - cocoa: a trackpad has a pixel delta and phases (momentum too); a mouse wheel neither.
//  - windows: precision touchpads arrive as plain wheel events (fractions of 120) and zoom.
// Outside xcb a device that says TouchPad is taken at its word, as before.
#include <string_view>

namespace scrollinput {
// The setting view/scrollInput (Preferences > Keyboard and mouse > Scroll wheel / trackpad), by its index.
enum class Mode { Automatic = 0, Wheel = 1, Trackpad = 2 };
inline Mode mode(int setting) { return setting == 1 ? Mode::Wheel : setting == 2 ? Mode::Trackpad : Mode::Automatic; }

// Qt::ScrollPhase's values.
enum class Phase { None = 0, Begin = 1, Update = 2, End = 3, Momentum = 4 };

struct Scroll {
  std::string_view platform;    // QGuiApplication::platformName(): "xcb", "wayland", "cocoa", "windows", ...
  bool touchpadDevice = false;  // the event's device says QInputDevice::DeviceType::TouchPad
  Phase phase = Phase::None;
  int pixelX = 0, pixelY = 0;  // pixelDelta
  int angleX = 0, angleY = 0;  // angleDelta, in eighths of a degree: a wheel notch is 120
  bool continuing = false;     // a trackpad scroll is under way (its last step came moments ago)
};

// A step of whole wheel notches (one or more on an axis, none on the other); no step at all is none.
inline bool wholeNotches(int angleX, int angleY) { return (angleX != 0 || angleY != 0) && angleX % 120 == 0 && angleY % 120 == 0; }

// True: pan or orbit as a trackpad; false: zoom as a wheel. The setting decides when it is not Automatic.
inline bool isTrackpad(const Scroll& s, Mode m = Mode::Automatic) {
  if (m == Mode::Wheel) return false;
  if (m == Mode::Trackpad) return true;
  const bool pixels = s.pixelX != 0 || s.pixelY != 0;
  if (pixels && s.phase != Phase::None) return true;  // fingers on a surface: the system says where a gesture starts and ends
  if (!s.touchpadDevice) return false;
  if (s.platform != "xcb") return true;
  // xcb calls the wheel a TouchPad too: whole notches zoom, unless they come amid a finger scroll (a step that happened to
  // land on 120 there must not jump the zoom).
  return pixels || !wholeNotches(s.angleX, s.angleY) || s.continuing;
}
}  // namespace scrollinput
