// macOS: wrap the NSView behind the Qt widget for OCCT's Cocoa_Window.
#include <Cocoa_Window.hxx>
#import <Cocoa/Cocoa.h>

Handle(Aspect_Window) opad_make_cocoa_window(void* nsview) {
  return new Cocoa_Window(reinterpret_cast<NSView*>(nsview));
}
