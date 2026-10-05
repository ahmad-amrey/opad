// A scroll in the 3D view: a mouse wheel that zooms or a trackpad's fingers that pan (app/ScrollInput.hpp). On X11 and
// XWayland Qt says TouchPad for the mouse wheel too: there whole notches zoom; elsewhere the classification is as before.
#include "check.hpp"
#include "ScrollInput.hpp"
using namespace scrollinput;

namespace {
Scroll wheel(std::string_view platform, bool touchpad, int angleY, int angleX = 0) {
  Scroll s;
  s.platform = platform;
  s.touchpadDevice = touchpad;
  s.angleX = angleX;
  s.angleY = angleY;
  return s;
}
Scroll fingers(std::string_view platform, bool touchpad, Phase phase, int pixelX, int pixelY) {
  Scroll s;
  s.platform = platform;
  s.touchpadDevice = touchpad;
  s.phase = phase;
  s.pixelX = pixelX;
  s.pixelY = pixelY;
  s.angleX = pixelX * 4;  // what Qt derives from pixels on most platforms
  s.angleY = pixelY * 4;
  return s;
}
}  // namespace

TEST(xcb_wheel_notches_from_a_touchpad_device_zoom) {
  // XWayland's pointer (and X11 mice through libinput) arrive as a TouchPad, no phase, no pixels, 120 a notch.
  for (int notch : {120, -120, 240, -360}) {
    CHECK(!isTrackpad(wheel("xcb", true, notch)));
    CHECK(!isTrackpad(wheel("xcb", true, 0, notch)));  // a tilted wheel
  }
  CHECK(!isTrackpad(wheel("xcb", false, 120)));
}

TEST(xcb_fractions_pan) {
  // Two fingers move the valuators by fractions of an increment.
  CHECK(isTrackpad(wheel("xcb", true, 37)));
  CHECK(isTrackpad(wheel("xcb", true, -37)));
  CHECK(isTrackpad(wheel("xcb", true, 120, 37)));  // one axis whole, the other a fraction
  CHECK(isTrackpad(wheel("xcb", true, 180)));
  // A driver with a large increment sends pixels too (no phase on xcb).
  Scroll s = wheel("xcb", true, 120);
  s.pixelY = 120;
  CHECK(isTrackpad(s));
  // A notch amid a finger scroll continues it (no zoom jump in the middle of a pan).
  s = wheel("xcb", true, 120);
  s.continuing = true;
  CHECK(isTrackpad(s));
  // A mouse that says Mouse is a wheel whatever its steps.
  CHECK(!isTrackpad(wheel("xcb", false, 37)));
}

TEST(xcb_no_step_is_no_notch) {
  // A device that says TouchPad sending no step at all (a phase's begin or end elsewhere): the trackpad's path, which
  // moves nothing and ends a scroll under way.
  CHECK(isTrackpad(wheel("xcb", true, 0)));
  CHECK(!wholeNotches(0, 0));
  CHECK(wholeNotches(0, 120) && wholeNotches(-240, 0) && wholeNotches(120, -120));
  CHECK(!wholeNotches(0, 119) && !wholeNotches(15, 0) && !wholeNotches(120, 60));
}

TEST(wayland_tells_them_by_pixels_and_phase) {
  CHECK(!isTrackpad(wheel("wayland", false, 120)));  // a wheel: no pixels, no phase
  CHECK(!isTrackpad(wheel("wayland", false, -120)));
  CHECK(isTrackpad(fingers("wayland", false, Phase::Update, 0, 7)));  // fingers: pixels and phases
  CHECK(isTrackpad(fingers("wayland", false, Phase::Begin, 3, 0)));
  CHECK(isTrackpad(fingers("wayland", false, Phase::Momentum, 0, -2)));
  // Pixels without a phase (a high-resolution wheel on some compositors) are a wheel.
  Scroll s = wheel("wayland", false, 120);
  s.pixelY = 15;
  CHECK(!isTrackpad(s));
}

TEST(cocoa_and_windows_classify_as_before) {
  CHECK(isTrackpad(fingers("cocoa", false, Phase::Update, 4, 9)));  // a MacBook trackpad
  CHECK(isTrackpad(fingers("cocoa", true, Phase::Update, 4, 9)));
  CHECK(!isTrackpad(wheel("cocoa", false, 120)));                   // a mouse wheel
  CHECK(!isTrackpad(wheel("windows", false, 120)));                 // a plain wheel
  CHECK(!isTrackpad(wheel("windows", false, 37)));                  // a precision touchpad arrives as wheel fractions
  // A device that says TouchPad is taken at its word outside xcb, notch or not (unchanged).
  CHECK(isTrackpad(wheel("windows", true, 120)));
  CHECK(isTrackpad(wheel("cocoa", true, 120)));
  CHECK(isTrackpad(wheel("wayland", true, 120)));
  CHECK(isTrackpad(fingers("windows", false, Phase::Update, 0, 15)));  // pixels with a phase: a gesture anywhere
  CHECK(!isTrackpad(wheel("offscreen", false, 120)));
}

TEST(the_setting_overrides_detection) {
  const Scroll cases[] = {wheel("xcb", true, 120), wheel("xcb", true, 37), wheel("windows", false, 120), wheel("windows", true, 120),
                          fingers("cocoa", false, Phase::Update, 4, 9), fingers("wayland", false, Phase::Update, 0, 7)};
  for (const Scroll& s : cases) {
    CHECK(!isTrackpad(s, Mode::Wheel));    // Mouse wheel zooms: every scroll zooms
    CHECK(isTrackpad(s, Mode::Trackpad));  // Trackpad pans: every scroll pans (Shift orbits; a pinch or Ctrl zooms)
    CHECK(isTrackpad(s, Mode::Automatic) == isTrackpad(s));
  }
  // The saved index; anything else is Automatic.
  CHECK(mode(0) == Mode::Automatic);
  CHECK(mode(1) == Mode::Wheel);
  CHECK(mode(2) == Mode::Trackpad);
  CHECK(mode(-1) == Mode::Automatic && mode(7) == Mode::Automatic);
}

CHECK_MAIN()
