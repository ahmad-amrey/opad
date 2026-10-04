#pragma once
// Reduced motion (UI-124): one switch, ui/reduceMotion, that by default follows the system (Windows: "Show animations in
// Windows" off, SPI_GETCLIENTAREAANIMATION). When it is on, the camera jumps instead of turning (view cube, roll, align to
// a plane), the help clips hold their still frame, the rich card and the browser open without growing. Progress (the load
// spinner) still moves: it says the app is working.
namespace motion {
bool system();   // the system asks for animations
bool reduced();  // the setting, else !system()
void setReduced(bool on);  // saved
// A duration in seconds as it should run now: `seconds`, or a moment (a camera still ends where it was going).
double seconds(double seconds);
int milliseconds(int ms);
}  // namespace motion
