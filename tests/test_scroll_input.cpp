// A scroll in the 3D view: a mouse wheel that zooms or a trackpad's fingers that pan (app/ScrollInput.hpp). On xcb Qt says
// TouchPad for XWayland's wheel too: there steps in eighths of a notch zoom; elsewhere the classification is as before.
// Nothing chosen, Windows and Linux assume a wheel and the first scroll asks once; macOS keeps that classification (Automatic).
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
  // XWayland's relative pointer (and evdev-driver mice) arrive as a TouchPad, no phase, no pixels, 120 a notch.
  for (int notch : {120, -120, 240, -360}) {
    CHECK(!isTrackpad(wheel("xcb", true, notch)));
    CHECK(!isTrackpad(wheel("xcb", true, 0, notch)));  // a tilted wheel
  }
  CHECK(!isTrackpad(wheel("xcb", false, 120)));  // a mouse through xf86-input-libinput on Xorg says Mouse
}

TEST(xcb_high_resolution_wheel_steps_zoom) {
  // A high-resolution wheel through XWayland (mutter's v120, an eighth of a notch a step): 15 a step, more when it spins.
  for (int step : {15, -15, 30, -45, 60, 105, 180})
    CHECK(!isTrackpad(wheel("xcb", true, step)));
  CHECK(!isTrackpad(wheel("xcb", true, 0, 15)));  // its tilt
  // A burst of them zooms throughout: no step makes a scroll under way (only a trackpad step does).
  int zoomed = 0;
  for (int i = 0; i < 8; ++i) zoomed += isTrackpad(wheel("xcb", true, 15)) ? 0 : 1;
  CHECK(zoomed == 8);
  CHECK(wheelSteps(0, 15) && wheelSteps(-30, 0) && wheelSteps(15, 120));
}

TEST(xcb_fractions_pan) {
  // Two fingers move the valuators by fractions of a step: trunc(12 x px) through XWayland, 8 x px with pixels on Xorg.
  CHECK(isTrackpad(wheel("xcb", true, 37)));
  CHECK(isTrackpad(wheel("xcb", true, -37)));
  CHECK(isTrackpad(wheel("xcb", true, 12)));   // a finger's 1 px through XWayland
  CHECK(isTrackpad(wheel("xcb", true, -24)));  // 2 px
  CHECK(isTrackpad(wheel("xcb", true, 100)));
  CHECK(isTrackpad(wheel("xcb", true, 120, 37)));  // one axis whole, the other a fraction
  CHECK(isTrackpad(wheel("xcb", true, 15, 7)));
  // A finger's step that lands on a multiple of 15 amid its scroll pans on.
  for (int step : {15, -45, 120}) {
    Scroll s = wheel("xcb", true, step);
    s.continuing = true;
    CHECK(isTrackpad(s));
  }
  // An Xorg touchpad with gestures (xf86-input-libinput 1.2+, increment 120) sends pixels too, whatever its angle (no phase
  // on xcb).
  Scroll s = wheel("xcb", true, 120);
  s.pixelY = 120;
  CHECK(isTrackpad(s));
  // A mouse that says Mouse is a wheel whatever its steps.
  CHECK(!isTrackpad(wheel("xcb", false, 37)));
}

TEST(xcb_no_step_is_no_notch) {
  // A device that says TouchPad sending no step at all (a phase's begin or end elsewhere): the trackpad's path, which
  // moves nothing and ends a scroll under way.
  CHECK(isTrackpad(wheel("xcb", true, 0)));
  CHECK(!wheelSteps(0, 0));
  CHECK(wheelSteps(0, 120) && wheelSteps(-240, 0) && wheelSteps(120, -120));
  CHECK(!wheelSteps(0, 119) && !wheelSteps(14, 0) && !wheelSteps(120, 61) && !wheelSteps(0, 7));
}

TEST(pan_step) {
  // xcb: an eighth of the angle. An Xorg touchpad (xf86-input-libinput 1.2+, increment 120) sends 8 x the finger's px as
  // both pixel and angle delta: 20 px of finger pan 20, not 160.
  Scroll s = wheel("xcb", true, 160);
  s.pixelY = 160;
  CHECK(panStep(s).y == 20.0 && panStep(s).x == 0.0);
  CHECK(panStep(wheel("xcb", true, 37)).y == 37 / 8.0);    // XWayland: no pixels
  CHECK(panStep(wheel("xcb", true, 0, -24)).x == -3.0);    // sideways, the sign kept
  // Elsewhere the pixel delta when there is one, else an eighth of the angle (as before).
  CHECK(panStep(fingers("cocoa", false, Phase::Update, 4, 9)).x == 4.0 && panStep(fingers("cocoa", false, Phase::Update, 4, 9)).y == 9.0);
  CHECK(panStep(fingers("wayland", false, Phase::Update, 0, 7)).y == 7.0);
  CHECK(panStep(wheel("windows", true, 120)).y == 15.0);
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

TEST(nothing_chosen_is_the_platforms_assumption) {
  // Windows and Linux (xcb, wayland, and anything else) take every scroll for a mouse wheel until the user chooses; macOS
  // keeps Automatic.
  for (std::string_view platform : {"windows", "xcb", "wayland", "offscreen"}) {
    CHECK(defaultMode(platform) == Mode::Wheel);
    CHECK(modeFor(platform, unset) == Mode::Wheel);
  }
  CHECK(defaultMode("cocoa") == Mode::Automatic);
  CHECK(modeFor("cocoa", unset) == Mode::Automatic);
  // A saved choice is kept on every platform, Automatic included.
  for (std::string_view platform : {"windows", "xcb", "wayland", "cocoa"}) {
    CHECK(modeFor(platform, 0) == Mode::Automatic);
    CHECK(modeFor(platform, 1) == Mode::Wheel);
    CHECK(modeFor(platform, 2) == Mode::Trackpad);
    CHECK(modeFor(platform, 9) == Mode::Automatic);  // an unknown index saved
  }
  // The assumed wheel zooms a touchpad's fractions and a finger gesture as Mouse wheel zooms does.
  CHECK(!isTrackpad(wheel("xcb", true, 37), modeFor("xcb", unset)));
  CHECK(!isTrackpad(fingers("windows", true, Phase::Update, 0, 15), modeFor("windows", unset)));
  CHECK(isTrackpad(fingers("cocoa", false, Phase::Update, 4, 9), modeFor("cocoa", unset)));
}

TEST(the_first_scroll_asks_once) {
  // Asked while nothing is chosen and it was never asked, where the wheel is an assumption.
  CHECK(asks("windows", unset, false));
  CHECK(asks("xcb", unset, false));
  CHECK(asks("wayland", unset, false));
  CHECK(!asks("windows", unset, true));  // answered or dismissed before
  CHECK(!asks("xcb", unset, true));
  for (int saved : {0, 1, 2}) {  // chosen (in Preferences or on the card)
    CHECK(!asks("windows", saved, false));
    CHECK(!asks("xcb", saved, false));
  }
  CHECK(!asks("cocoa", unset, false));  // macOS tells them apart: nothing to ask
  CHECK(!asks("cocoa", 2, false));
}

CHECK_MAIN()
