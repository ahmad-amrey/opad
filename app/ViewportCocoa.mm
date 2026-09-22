// macOS: wrap the NSView behind the Qt widget for OCCT's Cocoa_Window.
#import <AppKit/AppKit.h>
#include <Cocoa_Window.hxx>
#include <cmath>

// Cocoa_Window reports its NSView size in points and inherits a pixel ratio of 1.
// The native OpenGL surface uses backing pixels, so expose that size and ratio
// to OCCT to keep scene picking and the view cube in the same coordinate space.
class OpadCocoaWindow : public Cocoa_Window {
public:
  explicit OpadCocoaWindow(NSView* view) : Cocoa_Window(view) {}

  Standard_Real DevicePixelRatio() const override {
    return [HView() convertSizeToBacking:NSMakeSize(1.0, 1.0)].width;
  }

  void Size(Standard_Integer& width, Standard_Integer& height) const override {
    Cocoa_Window::Size(width, height);
    const Standard_Real scale = DevicePixelRatio();
    width = Standard_Integer(std::lround(width * scale));
    height = Standard_Integer(std::lround(height * scale));
  }
};

Handle(Aspect_Window) opad_make_cocoa_window(void* nsview) {
  return new OpadCocoaWindow(reinterpret_cast<NSView*>(nsview));
}
