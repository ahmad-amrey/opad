#pragma once
// A scroll in the 3D view: a mouse wheel (zoom at the cursor) or two fingers on a trackpad (pan, Shift orbits, a pinch or
// Ctrl zooms), decided without widgets from what the event carries. Viewport::wheelEvent asks; tests/test_scroll_input.cpp.
// No Qt here.
//
// What the platforms send (Qt 6.4 to 6.10):
//  - xcb (X11, and XWayland on a Wayland desktop: main() picks xcb on Linux). Qt's XI2 code (populateTouchDevices) calls a
//    pointer a TouchPad when it has relative X/Y and an axis labelled "Rel Vert/Horiz Wheel", or an XI 2.4 gesture class.
//    That is XWayland's "xwayland-relative-pointer:N" (the device every scroll comes from when the compositor offers relative
//    pointers, as mutter and KWin do), an evdev-driver mouse, and an Xorg touchpad with gestures; a mouse through
//    xf86-input-libinput labels its axes "Rel Vert Scroll" and is a Mouse, which zooms whatever it sends. No scroll phase
//    on xcb; the angle delta is valuator delta / increment * 120 and the pixel delta the raw valuator delta, only when the
//    increment exceeds 15.
//      XWayland (increment 1): a wheel sends its v120 value, 120 a notch and 15 a step of a high-resolution wheel (an
//      eighth of a notch, Logitech MX and most hidpp mice; faster spins 30, 45, ...); fingers send trunc(12 x px), which
//      lands on a multiple of 15 about once in ten steps.
//      Xorg touchpad, xf86-input-libinput 1.2 or later (increment 120): pixel and angle deltas are both 8 x the finger's px.
//    So a step in multiples of 15 zooms and anything else pans; a scroll under way (`continuing`) keeps panning, so only
//    the first step of a finger scroll can be taken for a wheel (one zoom step of an eighth of a notch or so).
//  - wayland: a wheel has no pixel delta and no phase; fingers have both.
//  - cocoa: a trackpad has a pixel delta and phases (momentum too); a mouse wheel neither.
//  - windows: precision touchpads arrive as plain wheel events (fractions of 120) and zoom.
// Outside xcb a device that says TouchPad is taken at its word, as before. All of this is Automatic, which is the default
// on macOS only (defaultMode below).
#include <string_view>

namespace scrollinput {
// The setting view/scrollInput (Preferences > Keyboard and mouse > Scroll wheel / trackpad), by its index.
enum class Mode { Automatic = 0, Wheel = 1, Trackpad = 2 };
inline Mode mode(int setting) { return setting == 1 ? Mode::Wheel : setting == 2 ? Mode::Trackpad : Mode::Automatic; }

// While the user has not chosen (nothing saved): an assumption, and one question at the first scroll. Windows and Linux
// assume a mouse wheel, every scroll zooms: there a trackpad's scroll and a wheel's look alike too often (a precision
// touchpad sends fractions of a notch, XWayland calls the wheel a touchpad), so a guess per scroll would pan some wheels
// and zoom some trackpads. macOS keeps Automatic, which tells them apart reliably (a trackpad's pixels come with scroll
// phases), and asks nothing. Viewport::readScrollInput; the question is PreferencesArea's card.
constexpr int unset = -1;  // no view/scrollInput saved
inline Mode defaultMode(std::string_view platform) { return platform == "cocoa" ? Mode::Automatic : Mode::Wheel; }
inline Mode modeFor(std::string_view platform, int saved) { return saved == unset ? defaultMode(platform) : mode(saved); }
// The first scroll asks "Using a trackpad?" once: nothing chosen, never asked (view/scrollAsked), and the default is the
// wheel's assumption.
inline bool asks(std::string_view platform, int saved, bool asked) {
  return saved == unset && !asked && defaultMode(platform) == Mode::Wheel;
}

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

// The steps a wheel takes through XWayland: v120 in eighths of a notch (15), whole notches included; no step is none.
constexpr int wheelStep = 15;
inline bool wheelSteps(int angleX, int angleY) {
  return (angleX != 0 || angleY != 0) && angleX % wheelStep == 0 && angleY % wheelStep == 0;
}

// True: pan or orbit as a trackpad; false: zoom as a wheel. The setting decides when it is not Automatic.
inline bool isTrackpad(const Scroll& s, Mode m = Mode::Automatic) {
  if (m == Mode::Wheel) return false;
  if (m == Mode::Trackpad) return true;
  const bool pixels = s.pixelX != 0 || s.pixelY != 0;
  if (pixels && s.phase != Phase::None) return true;  // fingers on a surface: the system says where a gesture starts and ends
  if (!s.touchpadDevice) return false;
  if (s.platform != "xcb") return true;
  // xcb calls the wheel a TouchPad too: steps of a wheel zoom, unless they come amid a finger scroll (a finger step that
  // happened to land on a multiple of 15 there must not jump the zoom). Pixels are an Xorg touchpad's (increment 120).
  return pixels || !wheelSteps(s.angleX, s.angleY) || s.continuing;
}

// How far a trackpad's scroll moves the view, in pixels. An eighth of the angle on xcb: there the pixel delta is the
// driver's raw valuator delta, 8 x the finger's px on Xorg with xf86-input-libinput 1.2 or later (ScrollPixelDistance 15
// scaled into its increment of 120), while the angle's eighth is 15 px per increment, about the finger's own px on Xorg
// and 1.5 x on XWayland. Elsewhere the pixel delta when there is one.
struct Pan {
  double x = 0, y = 0;
};
inline Pan panStep(const Scroll& s) {
  if (s.platform == "xcb" || (s.pixelX == 0 && s.pixelY == 0)) return {s.angleX / 8.0, s.angleY / 8.0};
  return {double(s.pixelX), double(s.pixelY)};
}
}  // namespace scrollinput
