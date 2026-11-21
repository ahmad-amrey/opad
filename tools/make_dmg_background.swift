// Draws the macOS dmg window's background: an arrow from the app's place to the Applications link's place, no text (any
// language). Writes app/res/dmg-background.tiff (1x + 2x in one TIFF, so Retina screens take the sharp one).
// Run on a Mac from the repository root:  swift tools/make_dmg_background.swift
// The places and the window size must match cmake/dmg.cmake (icons at x 170 and 470, y 190; window 640 x 400).
import AppKit

let width = 640.0, height = 400.0
let from = 170.0 + 90, to = 470.0 - 90, y = height - 190  // icon centres, 128 px icons + room for the label; flipped y

func draw(scale: Double, to path: String) {
  let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(width * scale), pixelsHigh: Int(height * scale),
                             bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                             colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
  rep.size = NSSize(width: width, height: height)  // 72 dpi at 1x, 144 dpi at 2x
  NSGraphicsContext.saveGraphicsState()
  NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
  // A mid-light grey: Finder draws the labels black in light mode and white in dark mode, both readable on it.
  NSColor(srgbRed: 0.80, green: 0.82, blue: 0.85, alpha: 1).setFill()
  NSRect(x: 0, y: 0, width: width, height: height).fill()
  let arrow = NSBezierPath()
  arrow.lineWidth = 6
  arrow.lineCapStyle = .round
  arrow.lineJoinStyle = .round
  arrow.move(to: NSPoint(x: from, y: y))
  arrow.line(to: NSPoint(x: to, y: y))
  arrow.move(to: NSPoint(x: to - 22, y: y + 20))
  arrow.line(to: NSPoint(x: to, y: y))
  arrow.line(to: NSPoint(x: to - 22, y: y - 20))
  NSColor(srgbRed: 0.33, green: 0.36, blue: 0.40, alpha: 1).setStroke()
  arrow.stroke()
  NSGraphicsContext.restoreGraphicsState()
  try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
}

let tmp = NSTemporaryDirectory()
draw(scale: 1, to: tmp + "/dmg-bg.png")
draw(scale: 2, to: tmp + "/dmg-bg@2x.png")
let tiff = Process()
tiff.executableURL = URL(fileURLWithPath: "/usr/bin/tiffutil")
tiff.arguments = ["-cathidpicheck", tmp + "/dmg-bg.png", tmp + "/dmg-bg@2x.png", "-out", "app/res/dmg-background.tiff"]
try! tiff.run()
tiff.waitUntilExit()
exit(tiff.terminationStatus)
