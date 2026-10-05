#!/usr/bin/env python3
"""Regenerates the README's pictures (docs/media) from real runs of the desktop app.

The app is recorded on Linux, where Xvfb draws its window for real (the 3D view, tool windows, hover, animations and
the pointer), and the GIFs are assembled wherever Pillow is installed:

  record    Linux, Python stdlib only; needs Xvfb and libX11, libXtst, libXcomposite, libXrender (no window manager,
            xdotool, ffmpeg or sudo). Per scene: starts a virtual X screen that mirrors its framebuffer into a file
            (-fbdir), a minimal compositor (so translucent tool windows look as on a desktop), and OPAD with its own
            settings and cache; drives it with XTest (real pointer moves, clicks, drags and keys, paced like a person)
            and samples the framebuffer into build/readme-media/<scene>/ (zlib-compressed raw frames + index.json).
              python3 tools/make_readme_media.py record [--only SCENE ...] [--bin build/linux/bin]
            `drawings` and `arabic` use the part `design` saves (build/readme-media/_documents): record design first.
  assemble  any OS, Pillow: build/readme-media/<scene>/ -> docs/media/<scene>.gif (or .png for stills): crop, scale,
            at most --fps frames a second, one palette per GIF; a scene's "gif:<name>" mark starts another GIF.
              py -3 tools/make_readme_media.py assemble [--only SCENE ...]
  logo      the logo scaled for the README's header, plus a variant for dark pages:
              py -3 tools/make_readme_media.py logo <opad_logo.png>
  serve/ctl a live session for writing a new scene: `serve [file]` keeps Xvfb + OPAD running and executes the Python
            lines `ctl` sends (`d.click(640, 400)`, `d.snap("/tmp/x.png")`) against the same driver the scenes use.

Scenes use only the STEPcode AS1 test assembly from tests/corpus (python3 tests/corpus/fetch.py) and parts made in the
scenes themselves. Window coordinates in the scenes assume the default layout of a fresh settings folder on a
1280 x 800 screen; after a UI change, `serve` and `snap` find the new ones.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = Path(__file__).resolve().parent.parent
FRAMES = ROOT / "build" / "readme-media"
MEDIA = ROOT / "docs" / "media"
CORPUS = ROOT / "tests" / "corpus"
SCREEN = (1280, 800)
FPS = 15


# ---------------------------------------------------------------------------------------------------------------
# The framebuffer: Xvfb -fbdir keeps the screen as an XWD file that it updates in place.

class Framebuffer:
    def __init__(self, path):
        self.file = open(path, "rb")
        self.map = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        header = self.map[:100]
        endian = ">" if struct.unpack(">I", header[:4])[0] < 0x10000 else "<"
        f = struct.unpack(endian + "25I", header)
        header_size, self.width, self.height, self.bpp, self.stride, ncolors = f[0], f[4], f[5], f[11], f[12], f[19]
        self.byte_order, red_mask = f[7], f[14]
        if self.bpp != 32:
            raise RuntimeError(f"framebuffer has {self.bpp} bits per pixel; start Xvfb with depth 24")
        self.offset = header_size + ncolors * 12
        # 32-bit TrueColor, least significant byte first: B, G, R, X in memory
        self.bgrx = self.byte_order == 0 and red_mask == 0xFF0000

    def grab(self):
        """The whole screen as raw BGRX bytes."""
        return self.map[self.offset:self.offset + self.stride * self.height]

    def close(self):
        self.map.close()
        self.file.close()


def bgrx_to_rgb(raw, width, height, stride, box=None):
    """Rows of RGB bytes (for PNG) from BGRX, optionally cropped to box = (x, y, w, h)."""
    x0, y0, w, h = box or (0, 0, width, height)
    rows = []
    for y in range(y0, y0 + h):
        row = raw[y * stride + x0 * 4:y * stride + (x0 + w) * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows.append(bytes(rgb))
    return rows, w, h


def write_png(path, rows, width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    data = zlib.compress(b"".join(b"\0" + r for r in rows), 6)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", data) + chunk(b"IEND", b""))


class Recorder(threading.Thread):
    """Samples the framebuffer at FPS while recording is on; a frame is stored only when the screen changed."""

    def __init__(self, fb, out, fps=FPS):
        super().__init__(daemon=True)
        self.fb, self.out, self.period = fb, Path(out), 1.0 / fps
        self.on, self.stop_flag, self.frames, self.marks, self.last = False, False, [], [], None
        self.lock = threading.Lock()
        self.t0 = None
        self.paused = 0.0

    def run(self):
        while not self.stop_flag:
            start = time.monotonic()
            if self.on:
                self.capture()
            time.sleep(max(0.0, self.period - (time.monotonic() - start)))

    def capture(self, force=False):
        raw = self.fb.grab()
        digest = hashlib.md5(raw).digest()
        with self.lock:
            now = time.monotonic() - self.t0 - self.paused
            if digest == self.last and not force:
                return
            self.last = digest
            name = f"{len(self.frames):05d}.z"
            (self.out / name).write_bytes(zlib.compress(raw, 1))
            self.frames.append({"file": name, "t": round(now, 3)})

    def start_recording(self):
        if self.t0 is None:
            self.t0 = time.monotonic()
        elif not self.on:
            self.paused += time.monotonic() - self.off_at
        self.on = True
        self.capture(force=True)

    def stop_recording(self):
        if self.on:
            self.capture()
            self.on, self.off_at = False, time.monotonic()

    def mark(self, label, **data):
        """A named moment: assemble may hold the frame there or write it as a still."""
        with self.lock:
            now = time.monotonic() - (self.t0 or time.monotonic()) - self.paused
        self.capture(force=True) if self.on else None
        self.marks.append(dict(label=label, t=round(now, 3), frame=len(self.frames) - 1, **data))

    def finish(self, meta):
        self.stop_flag = True
        self.join()
        index = dict(meta, width=self.fb.width, height=self.fb.height, stride=self.fb.stride,
                     end=round(time.monotonic() - (self.t0 or time.monotonic()) - self.paused, 3),
                     frames=self.frames, marks=self.marks)
        (self.out / "index.json").write_text(json.dumps(index, indent=1), encoding="utf-8")


# ---------------------------------------------------------------------------------------------------------------
# Input: XTest through ctypes (what xdotool does, without needing it installed).

KEYSYMS = {"ctrl": "Control_L", "shift": "Shift_L", "alt": "Alt_L", "enter": "Return", "return": "Return", "esc": "Escape",
           "tab": "Tab", "space": "space", "backspace": "BackSpace", "del": "Delete", "delete": "Delete", "up": "Up",
           "down": "Down", "left": "Left", "right": "Right", "home": "Home", "end": "End", "f1": "F1", "f2": "F2",
           "f9": "F9", ".": "period", ",": "comma", "-": "minus", "/": "slash", "=": "equal", "[": "bracketleft",
           "]": "bracketright", "#": "numbersign", "@": "at", "<": "less", " ": "space", "*": "asterisk"}
SHIFTED = set('~!@#$%^&*()_+{}|:"<>?ABCDEFGHIJKLMNOPQRSTUVWXYZ')


class Input:
    def __init__(self, display):
        self.x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        self.xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XStringToKeysym.restype = ctypes.c_ulong
        self.x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
        self.x11.XKeysymToKeycode.restype = ctypes.c_ubyte
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.dpy = self.x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise RuntimeError(f"cannot open display {display}")
        self.pos = (SCREEN[0] // 2, SCREEN[1] // 2)

    def motion(self, x, y):
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(x), int(y), 0)
        self.x11.XFlush(self.dpy)
        self.pos = (x, y)

    def button(self, button, down):
        self.xtst.XTestFakeButtonEvent(self.dpy, button, 1 if down else 0, 0)
        self.x11.XFlush(self.dpy)

    def keycode(self, name):
        sym = KEYSYMS.get(name.lower() if len(name) > 1 else name, name)
        code = self.x11.XKeysymToKeycode(self.dpy, self.x11.XStringToKeysym(sym.encode()))
        if not code:
            raise ValueError(f"no key for {name!r}")
        return code

    def key(self, code, down):
        self.xtst.XTestFakeKeyEvent(self.dpy, code, 1 if down else 0, 0)
        self.x11.XFlush(self.dpy)

    def focus_main(self):
        """Gives the keyboard to the biggest mapped top-level window (OPAD's main window): with no window manager
        nothing does that after a dialog or a tool window closes, and keys would go nowhere."""
        x, ul = self.x11, ctypes.c_ulong
        x.XDefaultRootWindow.restype, x.XDefaultRootWindow.argtypes = ul, [ctypes.c_void_p]
        x.XQueryTree.argtypes = [ctypes.c_void_p, ul, ctypes.POINTER(ul), ctypes.POINTER(ul),
                                 ctypes.POINTER(ctypes.POINTER(ul)), ctypes.POINTER(ctypes.c_uint)]
        x.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ul, ctypes.POINTER(_XWindowAttributes)]
        x.XSetInputFocus.argtypes = [ctypes.c_void_p, ul, ctypes.c_int, ul]
        root = x.XDefaultRootWindow(self.dpy)
        r, p, children, n = ul(), ul(), ctypes.POINTER(ul)(), ctypes.c_uint()
        x.XQueryTree(self.dpy, root, ctypes.byref(r), ctypes.byref(p), ctypes.byref(children), ctypes.byref(n))
        best, area = None, 0
        for i in range(n.value):
            a = _XWindowAttributes()
            if x.XGetWindowAttributes(self.dpy, children[i], ctypes.byref(a)) and a.map_state == 2 and a.width * a.height > area:
                best, area = children[i], a.width * a.height
        if best:
            x.XSetInputFocus(self.dpy, best, 2, 0)  # RevertToParent, CurrentTime
            x.XFlush(self.dpy)


def ease(t):
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------------------------------------------------------
# A minimal compositing manager. Xvfb has none, so the translucent parts of OPAD's floating windows (tool panels'
# rounded corners and shadows, the browser overlay, rich tips) would come out black. This one redirects the top-level
# windows (Composite) and paints them over each other with XRender, alpha included, ~40 times a second.

class _XWindowAttributes(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p),
                ("root", ctypes.c_ulong), ("class_", ctypes.c_int), ("bit_gravity", ctypes.c_int),
                ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int), ("backing_planes", ctypes.c_ulong),
                ("backing_pixel", ctypes.c_ulong), ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong),
                ("map_installed", ctypes.c_int), ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
                ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
                ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]


class _XRenderDirectFormat(ctypes.Structure):
    _fields_ = [(n, ctypes.c_short) for n in ("red", "redMask", "green", "greenMask", "blue", "blueMask", "alpha", "alphaMask")]


class _XRenderPictFormat(ctypes.Structure):
    _fields_ = [("id", ctypes.c_ulong), ("type", ctypes.c_int), ("depth", ctypes.c_int),
                ("direct", _XRenderDirectFormat), ("colormap", ctypes.c_ulong)]


class _XRenderPictureAttributes(ctypes.Structure):
    _fields_ = [("repeat", ctypes.c_int), ("alpha_map", ctypes.c_ulong), ("alpha_x_origin", ctypes.c_int),
                ("alpha_y_origin", ctypes.c_int), ("clip_x_origin", ctypes.c_int), ("clip_y_origin", ctypes.c_int),
                ("clip_mask", ctypes.c_ulong), ("graphics_exposures", ctypes.c_int), ("subwindow_mode", ctypes.c_int),
                ("poly_edge", ctypes.c_int), ("poly_mode", ctypes.c_int), ("dither", ctypes.c_ulong),
                ("component_alpha", ctypes.c_int)]


class _XRenderColor(ctypes.Structure):
    _fields_ = [("red", ctypes.c_ushort), ("green", ctypes.c_ushort), ("blue", ctypes.c_ushort), ("alpha", ctypes.c_ushort)]


class Compositor(threading.Thread):
    def __init__(self, display, period=0.025):
        super().__init__(daemon=True)
        self.period, self.stop_flag = period, False
        lib = lambda name, so: ctypes.CDLL(ctypes.util.find_library(name) or so)  # noqa: E731
        x, xc, xr = lib("X11", "libX11.so.6"), lib("Xcomposite", "libXcomposite.so.1"), lib("Xrender", "libXrender.so.1")
        self.x, self.xc, self.xr = x, xc, xr
        vp, ul = ctypes.c_void_p, ctypes.c_ulong
        x.XOpenDisplay.restype, x.XOpenDisplay.argtypes = vp, [ctypes.c_char_p]
        x.XDefaultRootWindow.restype, x.XDefaultRootWindow.argtypes = ul, [vp]
        x.XDefaultVisual.restype, x.XDefaultVisual.argtypes = vp, [vp, ctypes.c_int]
        x.XQueryTree.argtypes = [vp, ul, ctypes.POINTER(ul), ctypes.POINTER(ul), ctypes.POINTER(ctypes.POINTER(ul)),
                                 ctypes.POINTER(ctypes.c_uint)]
        x.XGetWindowAttributes.argtypes = [vp, ul, ctypes.POINTER(_XWindowAttributes)]
        x.XFree.argtypes = [vp]
        x.XSync.argtypes = [vp, ctypes.c_int]
        x.XInternAtom.restype, x.XInternAtom.argtypes = ul, [vp, ctypes.c_char_p, ctypes.c_int]
        x.XCreateSimpleWindow.restype = ul
        x.XCreateSimpleWindow.argtypes = [vp, ul, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ul, ul]
        x.XSetSelectionOwner.argtypes = [vp, ul, ul, ul]
        x.XCreatePixmap.restype, x.XCreatePixmap.argtypes = ul, [vp, ul, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
        x.XFreePixmap.argtypes = [vp, ul]
        xc.XCompositeRedirectSubwindows.argtypes = [vp, ul, ctypes.c_int]
        xc.XCompositeNameWindowPixmap.restype, xc.XCompositeNameWindowPixmap.argtypes = ul, [vp, ul]
        xr.XRenderFindVisualFormat.restype, xr.XRenderFindVisualFormat.argtypes = ctypes.POINTER(_XRenderPictFormat), [vp, vp]
        xr.XRenderFindStandardFormat.restype, xr.XRenderFindStandardFormat.argtypes = ctypes.POINTER(_XRenderPictFormat), [vp, ctypes.c_int]
        xr.XRenderCreatePicture.restype = ul
        xr.XRenderCreatePicture.argtypes = [vp, ul, ctypes.POINTER(_XRenderPictFormat), ul, ctypes.POINTER(_XRenderPictureAttributes)]
        xr.XRenderFreePicture.argtypes = [vp, ul]
        xr.XRenderComposite.argtypes = [vp, ctypes.c_int, ul, ul, ul] + [ctypes.c_int] * 6 + [ctypes.c_uint, ctypes.c_uint]
        xr.XRenderFillRectangle.argtypes = [vp, ctypes.c_int, ul, ctypes.POINTER(_XRenderColor)] + [ctypes.c_int] * 2 + [ctypes.c_uint] * 2
        # a window that vanishes between XQueryTree and its use is not an error worth dying for
        self._handler = ctypes.CFUNCTYPE(ctypes.c_int, vp, vp)(lambda d, e: 0)
        x.XSetErrorHandler(self._handler)
        self.dpy = x.XOpenDisplay(display.encode())
        if not self.dpy:
            raise RuntimeError(f"compositor: cannot open {display}")
        self.root = x.XDefaultRootWindow(self.dpy)
        owner = x.XCreateSimpleWindow(self.dpy, self.root, -10, -10, 1, 1, 0, 0, 0)
        x.XSetSelectionOwner(self.dpy, x.XInternAtom(self.dpy, b"_NET_WM_CM_S0", 0), owner, 0)
        xc.XCompositeRedirectSubwindows(self.dpy, self.root, 1)  # CompositeRedirectManual
        attrs = _XRenderPictureAttributes(subwindow_mode=1)  # IncludeInferiors
        root_format = xr.XRenderFindVisualFormat(self.dpy, x.XDefaultVisual(self.dpy, 0))
        self.root_pic = xr.XRenderCreatePicture(self.dpy, self.root, root_format, 1 << 8, ctypes.byref(attrs))
        self.back_pix = x.XCreatePixmap(self.dpy, self.root, SCREEN[0], SCREEN[1], 24)
        self.back_pic = xr.XRenderCreatePicture(self.dpy, self.back_pix, root_format, 0, None)
        x.XSync(self.dpy, 0)

    def paint(self):
        x, xc, xr, dpy = self.x, self.xc, self.xr, self.dpy
        black = _XRenderColor(0, 0, 0, 0xFFFF)
        xr.XRenderFillRectangle(dpy, 1, self.back_pic, ctypes.byref(black), 0, 0, SCREEN[0], SCREEN[1])  # PictOpSrc
        root_ret, parent, children, count = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if not x.XQueryTree(dpy, self.root, ctypes.byref(root_ret), ctypes.byref(parent), ctypes.byref(children), ctypes.byref(count)):
            return
        windows = [children[i] for i in range(count.value)]
        if children:
            x.XFree(children)
        for w in windows:  # bottom to top
            a = _XWindowAttributes()
            if not x.XGetWindowAttributes(dpy, w, ctypes.byref(a)) or a.map_state != 2 or a.class_ != 1:
                continue
            fmt = xr.XRenderFindVisualFormat(dpy, a.visual)
            if not fmt:
                continue
            pixmap = xc.XCompositeNameWindowPixmap(dpy, w)
            if not pixmap:
                continue
            pic = xr.XRenderCreatePicture(dpy, pixmap, fmt, 0, None)
            op = 3 if fmt.contents.direct.alphaMask else 1  # PictOpOver for ARGB windows, else PictOpSrc
            b = a.border_width
            xr.XRenderComposite(dpy, op, pic, 0, self.back_pic, 0, 0, 0, 0, a.x, a.y, a.width + 2 * b, a.height + 2 * b)
            xr.XRenderFreePicture(dpy, pic)
            x.XFreePixmap(dpy, pixmap)
        xr.XRenderComposite(dpy, 1, self.back_pic, 0, self.root_pic, 0, 0, 0, 0, 0, 0, SCREEN[0], SCREEN[1])
        x.XSync(dpy, 0)

    def run(self):
        while not self.stop_flag:
            start = time.monotonic()
            self.paint()
            time.sleep(max(0.0, self.period - (time.monotonic() - start)))


class Driver:
    """What a scene does, at a person's pace. Coordinates are screen pixels (the window sits at 0, 0)."""

    def __init__(self, inp, recorder, fb):
        self.inp, self.rec, self.fb = inp, recorder, fb

    # pointer
    def move(self, x, y, dur=0.45):
        x0, y0 = self.inp.pos
        steps = max(1, int(dur * 60))
        for i in range(1, steps + 1):
            t = ease(i / steps)
            self.inp.motion(x0 + (x - x0) * t, y0 + (y - y0) * t)
            time.sleep(dur / steps)

    def click(self, x=None, y=None, button=1, dur=0.45, hold=0.08):
        if x is not None:
            self.move(x, y, dur)
        self.inp.button(button, True)
        time.sleep(hold)
        self.inp.button(button, False)
        time.sleep(0.12)

    def dclick(self, x=None, y=None):
        if x is not None:
            self.move(x, y)
        for _ in range(2):
            self.inp.button(1, True)
            time.sleep(0.04)
            self.inp.button(1, False)
            time.sleep(0.06)

    def drag(self, x0, y0, x1, y1, button=1, dur=0.8, keys=()):
        self.move(x0, y0)
        codes = [self.inp.keycode(k) for k in keys]
        for c in codes:
            self.inp.key(c, True)
        time.sleep(0.15)  # the modifier first, then a small move: the view reads both before the press
        self.move(x0 + 2, y0, 0.05)
        self.inp.button(button, True)
        time.sleep(0.1)
        self.move(x1, y1, dur)
        time.sleep(0.08)
        self.inp.button(button, False)
        for c in reversed(codes):
            self.inp.key(c, False)
        time.sleep(0.1)

    def path(self, points, button=1, dur=1.0):
        """Press, follow the points, release: a freehand stroke."""
        self.move(*points[0])
        self.inp.button(button, True)
        per = dur / max(1, len(points) - 1)
        for p in points[1:]:
            self.move(p[0], p[1], per)
        self.inp.button(button, False)
        time.sleep(0.1)

    def wheel(self, clicks, x=None, y=None):
        if x is not None:
            self.move(x, y)
        b = 4 if clicks > 0 else 5
        for _ in range(abs(clicks)):
            self.inp.button(b, True)
            self.inp.button(b, False)
            time.sleep(0.06)

    # keyboard
    def focus(self):
        self.inp.focus_main()
        time.sleep(0.15)

    def key(self, combo, pause=0.25):
        """'ctrl+2', 'shift+n', 'enter', 'F1'."""
        parts = combo.split("+") if combo != "+" else ["+"]
        codes = [self.inp.keycode(p) for p in parts]
        for c in codes:
            self.inp.key(c, True)
            time.sleep(0.02)
        for c in reversed(codes):
            self.inp.key(c, False)
            time.sleep(0.02)
        time.sleep(pause)

    def type(self, text, per=0.09):
        for ch in text:
            name = {"\n": "enter", "\t": "tab"}.get(ch, ch)
            if ch in SHIFTED:
                self.key("shift+" + (ch.lower() if ch.isalpha() else name), pause=per)
            else:
                self.key(name, pause=per)

    # time and recording
    def wait(self, seconds):
        time.sleep(seconds)

    def settle(self, quiet=0.8, timeout=60):
        """Waits until the screen has not changed for `quiet` seconds (a load, a job, an animation finished)."""
        end, last, since = time.monotonic() + timeout, None, time.monotonic()
        while time.monotonic() < end:
            digest = hashlib.md5(self.fb.grab()).digest()
            if digest != last:
                last, since = digest, time.monotonic()
            elif time.monotonic() - since >= quiet:
                return True
            time.sleep(0.05)
        return False

    def record(self, on=True):
        self.rec.start_recording() if on else self.rec.stop_recording()

    def mark(self, label, **data):
        self.rec.mark(label, **data)

    def snap(self, path, box=None):
        rows, w, h = bgrx_to_rgb(self.fb.grab(), self.fb.width, self.fb.height, self.fb.stride, box)
        write_png(path, rows, w, h)
        return path


# ---------------------------------------------------------------------------------------------------------------
# A session: Xvfb + OPAD with private settings, cache and documents.

class Session:
    def __init__(self, bin_dir, work, display=":77", lang=None, settings=None):
        self.bin, self.work, self.display = Path(bin_dir), Path(work), display
        self.lang, self.settings = lang, settings
        self.xvfb = self.app = None

    def cli(self, *args, check=True):
        out = subprocess.run([str(self.bin / "opad-cli")] + [str(a) for a in args], capture_output=True, text=True,
                             env=self.env())
        if check and out.returncode:
            raise RuntimeError(f"opad-cli {' '.join(map(str, args))}: {out.stderr or out.stdout}")
        return json.loads(out.stdout) if out.stdout.strip().startswith(("{", "[")) else out.stdout

    def env(self):
        env = dict(os.environ, DISPLAY=self.display, XDG_CONFIG_HOME=str(self.work / "config"),
                   XDG_CACHE_HOME=str(self.work / "cache"), XDG_DATA_HOME=str(self.work / "data"),
                   OPAD_CACHE_DIR=str(self.work / "cache" / "opad"), QT_QPA_PLATFORM="xcb", LIBGL_ALWAYS_SOFTWARE="1",
                   OPAD_AUTHOR="Sam")  # notes, title blocks and history name this author, not the recording machine's user
        env.pop("WAYLAND_DISPLAY", None)
        if self.lang:
            env["OPAD_LANG"] = self.lang
        return env

    def start_x(self):
        fbdir = self.work / "fb"
        fbdir.mkdir(parents=True, exist_ok=True)
        lock = Path(f"/tmp/.X{self.display[1:]}-lock")
        if lock.exists():
            raise RuntimeError(f"display {self.display} is taken ({lock}); pass --display")
        self.xvfb = subprocess.Popen(["Xvfb", self.display, "-screen", "0", f"{SCREEN[0]}x{SCREEN[1]}x24", "-fbdir",
                                      str(fbdir), "-nolisten", "tcp", "-dpi", "96"], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        path = fbdir / "Xvfb_screen0"
        for _ in range(100):
            if path.exists() and path.stat().st_size > 100:
                break
            time.sleep(0.05)
        time.sleep(0.3)
        self.compositor = Compositor(self.display)
        self.compositor.start()
        return Framebuffer(path)

    def start_app(self, *args, extra_env=None):
        config = self.work / "config" / "opad"
        config.mkdir(parents=True, exist_ok=True)
        if self.settings is not None and not (config / "OPAD.conf").exists():
            (config / "OPAD.conf").write_text(self.settings, encoding="utf-8")
        env = dict(self.env(), **(extra_env or {}))
        log = open(self.work / "opad.log", "w")
        self.app = subprocess.Popen([str(self.bin / "opad"), "-geometry", f"{SCREEN[0]}x{SCREEN[1]}+0+0"] + [str(a) for a in args],
                                    env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

    def stop(self):
        if getattr(self, "compositor", None):
            self.compositor.stop_flag = True
            self.compositor.join(2)
        for proc in (self.app, self.xvfb):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


# ---------------------------------------------------------------------------------------------------------------
# Scenes: what each picture shows. A scene prepares its files, starts the app, waits for it, turns recording on and
# acts. The coordinates assume the default layout of a fresh settings folder on the SCREEN above.

SCENES = {}

# Settings every scene starts from (config/opad/OPAD.conf): the scroll question answered (a wheel zooms), so no
# first-run card covers the view.
BASE_SETTINGS = """[view]
scrollInput=1
scrollAsked=true
"""


def scene(name, width=1000, crop=None, fps=FPS, still=False, lang=None, settings=""):
    def wrap(fn):
        SCENES[name] = dict(fn=fn, crop=crop, width=width, fps=fps, still=still, lang=lang, settings=BASE_SETTINGS + settings)
        return fn
    return wrap


def corpus(s, name):
    """A tests/corpus model copied into the scene's folder (fetch.py downloads them)."""
    path = CORPUS / name
    if not path.exists():
        raise SystemExit(f"{path} is missing: python3 tests/corpus/fetch.py")
    target = s.work / "models" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(path, target)
    return target


DOCUMENTS = FRAMES / "_documents"  # what one scene leaves for the next (the designed part for the drawing)


def opened(d, s, *files, wait=2.0, env=None):
    """Starts the app on files, waits for the load to finish and gives it the keyboard."""
    s.start_app(*files, extra_env=env)
    time.sleep(3)
    d.settle(quiet=1.5, timeout=30)  # an animated start card never settles: 30 s is plenty for these loads
    time.sleep(wait)
    d.move(1150, 690, 0.1)  # an empty corner of the view
    d.focus()


def orbit(d, x0, y0, x1, y1, dur=1.4):
    """Fusion-style navigation (the default preset): Shift + middle drag orbits."""
    d.drag(x0, y0, x1, y1, button=2, keys=["shift"], dur=dur)


def ctrl_clicks(d, points):
    ctrl = d.inp.keycode("ctrl")
    d.inp.key(ctrl, True)
    for x, y in points:
        d.click(x, y, dur=0.35)
    d.inp.key(ctrl, False)


@scene("hero")
def hero(d, s):
    """Viewer mode on the STEPcode AS1 assembly: hover, orbit, explode and back together."""
    opened(d, s, corpus(s, "stepcode-as1-oc-214.stp"))
    d.record()
    d.wait(0.5)
    for x, y in [(640, 400), (470, 360), (820, 600)]:
        d.move(x, y, 0.6)
        d.wait(0.5)
    orbit(d, 820, 600, 930, 585)  # starting on a part: it pivots on that surface
    d.move(1150, 700, 0.4)
    d.wait(0.3)
    d.key("shift+e")  # Exploded view
    d.wait(2.2)
    d.key("esc")  # its panel closes, the parts stay apart
    d.wait(0.6)
    orbit(d, 230, 700, 330, 688, dur=1.4)
    d.wait(0.8)
    d.key("shift+e")
    d.wait(0.6)
    d.move(62, 85, 0.6)
    d.click()  # Play explode: back together
    d.wait(2.4)
    d.key("esc")
    d.wait(1.0)


@scene("measure")
def measure(d, s):
    """Guided Distance and Radius on AS1: the prompt asks for each pick; results in the view and the tool panel."""
    opened(d, s, corpus(s, "stepcode-as1-oc-214.stp"))
    d.record()
    d.wait(0.5)
    d.key("d")
    d.wait(0.9)
    for x, y in [(450, 300), (800, 560)]:
        d.move(x, y, 0.7)
        d.wait(0.4)
        d.click()
        d.wait(0.6)
    d.wait(2.2)
    d.key("esc")
    d.key("r")
    d.wait(0.9)
    d.move(578, 413, 0.7)
    d.wait(0.4)
    d.click()
    d.wait(2.8)
    for _ in range(3):
        d.key("esc", pause=0.15)
    d.wait(0.6)


@scene("design")
def design(d, s):
    """From nothing to a parametric part: a sketch typed by keyboard, Extrude by its arrow, Fillet on picked edges,
    then the extrude edited on the timeline and the fillet following."""
    doc = s.work / "part.opad"
    s.cli("new", doc)
    opened(d, s, doc)
    d.key("ctrl+2")  # Design workspace
    d.wait(1.2)
    d.record()
    d.wait(0.6)
    d.move(611, 622, 0.7)
    d.click()  # New sketch (the empty document's start card)
    d.wait(1.2)
    d.move(868, 228, 0.6)
    d.wait(0.3)
    d.click()  # the XY plane's tile
    d.wait(1.6)
    d.move(1103, 687, 0.5)
    d.click()  # origin as it is: OK
    d.wait(1.0)
    d.focus()
    d.move(500, 600, 0.5)
    d.key("r")  # Rectangle: first corner and size typed
    d.wait(0.4)
    d.type("-30\t-20")
    d.wait(0.3)
    d.key("enter")
    d.move(760, 360, 0.8)
    d.type("60\t40")
    d.wait(0.5)
    d.key("enter")
    d.wait(0.8)
    d.move(560, 600, 0.5)
    d.key("c")  # Circle: centre and diameter typed
    d.type("0\t0")
    d.key("enter")
    d.move(680, 420, 0.5)
    d.type("20")
    d.wait(0.4)
    d.key("enter")
    d.wait(1.0)
    d.key("ctrl+enter")  # Finish sketch
    d.wait(1.2)
    d.focus()  # the sketch panel had the keyboard
    d.key("shift+h")  # Isometric view
    d.wait(1.6)
    d.wheel(-6, 640, 430)
    d.wait(0.8)
    d.key("e")  # Extrude: pick the profile, drag the arrow, type the distance
    d.wait(0.6)
    d.move(505, 425, 0.6)
    d.click()
    d.wait(1.2)
    d.drag(512, 282, 512, 215, dur=1.2)
    d.wait(0.6)
    d.type("25")
    d.wait(0.7)
    d.key("enter")
    d.wait(1.5)
    d.mark("gif:parametric")  # the rest is its own picture: fillet, then the timeline edit
    d.focus()
    d.key("3")  # Edges filter; Ctrl adds to the selection
    d.move(513, 233, 0.6)
    d.click()
    ctrl_clicks(d, [(725, 258), (555, 356), (767, 381)])
    d.wait(0.5)
    d.move(418, 88, 0.6)
    d.click()  # Fillet takes the picked edges
    d.wait(1.2)
    d.move(700, 600, 0.5)
    d.type("5")
    d.wait(0.9)
    d.key("enter")
    d.wait(1.6)
    d.move(163, 748, 0.7)
    d.dclick()  # the extrude's marker on the timeline: edit it
    d.wait(1.5)
    d.move(700, 620, 0.5)
    d.type("40")
    d.wait(1.0)
    d.key("enter")
    d.wait(2.0)
    d.focus()
    d.key("shift+f")  # Fit all
    d.wait(1.4)
    orbit(d, 1000, 640, 1090, 625)
    d.wait(1.2)
    d.record(False)
    d.key("ctrl+s")
    d.wait(2.0)
    DOCUMENTS.mkdir(parents=True, exist_ok=True)
    shutil.copy(doc, DOCUMENTS / "part.opad")


@scene("notes")
def notes(d, s):
    """Hand drawing on AS1 (strokes on camera-facing planes, orbit between them), then an AI agent note."""
    doc = s.work / "review.opad"
    s.cli("new", doc)
    s.cli("import", doc, "--file", corpus(s, "stepcode-as1-oc-214.stp"))
    opened(d, s, doc)
    d.record()
    d.wait(0.5)
    d.key("shift+n")  # Hand drawing
    d.wait(0.8)
    d.move(445, 330, 0.6)
    d.wait(0.3)
    d.click()  # the target: the left bracket
    d.wait(0.8)
    d.key("]")  # a wider pen
    d.path([(550 + 80 * math.cos(t / 48 * 6.6 + 2.2), 392 + 48 * math.sin(t / 48 * 6.6 + 2.2)) for t in range(49)], dur=1.6)
    d.wait(0.6)
    orbit(d, 200, 620, 290, 600, dur=1.2)  # another view, another stroke plane
    d.wait(0.5)
    d.key("3")  # blue
    d.path([(760, 270), (730, 292), (700, 314), (672, 335)], dur=0.6)
    d.path([(690, 312), (672, 335), (700, 333)], dur=0.4)
    d.wait(0.8)
    d.key("ctrl+enter")  # Save: one annotate op
    d.wait(1.5)
    d.move(1150, 650, 0.5)
    d.focus()
    d.key("n")  # Note
    d.wait(0.8)
    d.move(830, 440, 0.6)
    d.click()  # the rod's end
    d.wait(0.8)
    d.move(1007, 429, 0.5)
    d.click()  # type: AI agent notes (an MCP agent reads these as requests)
    d.click(1100, 530, dur=0.4)
    d.type("Chamfer the rod end before assembly", per=0.05)
    d.wait(0.6)
    d.key("ctrl+enter")
    d.wait(2.8)


@scene("drawings")
def drawings(d, s):
    """A drawing of the designed part: New drawing lays out the views, then dimensions picked on them."""
    source = DOCUMENTS / "part.opad"
    if not source.exists():
        raise SystemExit("record the design scene first: it leaves the part this drawing shows")
    doc = s.work / "part.opad"
    shutil.copy(source, doc)
    opened(d, s, doc)
    d.record()
    d.wait(0.5)
    d.key("ctrl+3")  # Drawings workspace
    d.wait(1.0)
    d.move(639, 443, 0.6)
    d.click()  # New drawing…
    d.wait(1.2)
    d.move(977, 741, 0.6)
    d.click()  # Create drawing
    d.wait(2.0)
    d.settle(quiet=1.0, timeout=60)
    d.move(369, 43, 0.6)
    d.click()  # Annotate
    d.wait(0.6)
    d.move(56, 85, 0.4)
    d.click()  # Dimension
    d.wait(0.8)
    d.wheel(3, 640, 345)  # closer to the views
    d.wait(1.0)
    for pick, place in DRAWING_DIMENSIONS:
        d.move(*pick, 0.6)
        d.wait(0.3)
        d.click()
        d.move(*place, 0.6)
        d.wait(0.4)
        d.click()
        d.wait(0.7)
    d.key("esc")
    d.wait(2.0)


# (edge or circle to pick, where the dimension goes) on the sheet New drawing lays out for the design scene's part
# (picked off the edges' midpoints, which would snap to a point instead)
DRAWING_DIMENSIONS = [((470, 518), (470, 555)), ((610, 228), (655, 228)), ((501, 440), (575, 385))]


@scene("help")
def help_scene(d, s):
    """Help that shows the real tool: a ribbon card with its animated clip and the command search."""
    doc = s.work / "empty.opad"
    s.cli("new", doc)
    opened(d, s, doc)
    d.key("ctrl+2")
    d.wait(1.0)
    d.record()
    d.wait(0.4)
    d.move(160, 70, 0.8)  # Extrude: its card and clip
    d.wait(5.0)
    d.move(330, 330, 0.5)  # off the ribbon, so the next card is Fillet's
    d.wait(0.6)
    d.move(418, 88, 0.6)  # Fillet
    d.wait(4.5)
    d.move(700, 350, 0.5)
    d.focus()
    d.key("s")  # Search commands
    d.wait(0.6)
    d.type("fil", per=0.18)
    d.wait(2.2)
    d.key("down")
    d.wait(1.8)
    d.key("esc")
    d.wait(1.0)  # the cheat sheet (Ctrl+/) is its own picture: the keys scene


@scene("keys", still=True, width=1000, crop=[0, 0, 1000, 680])
def keys(d, s):
    """The shortcuts cheat sheet (Ctrl+/) of a fresh install: every default binding, as a still."""
    doc = s.work / "empty.opad"
    s.cli("new", doc)
    opened(d, s, doc)
    d.key("ctrl+/")
    d.wait(1.5)
    d.move(1240, 760, 0.1)
    d.record()
    d.wait(0.5)
    d.mark("still")


@scene("arabic", still=True, width=1280, lang="ar")
def arabic(d, s):
    """The same window in Arabic, right to left, in the Design workspace with the designed part."""
    source = DOCUMENTS / "part.opad"
    if not source.exists():
        raise SystemExit("record the design scene first")
    doc = s.work / "part.opad"
    shutil.copy(source, doc)
    opened(d, s, doc)
    d.key("ctrl+2")
    d.wait(1.5)
    d.move(1120, 70, 0.6)  # the ribbon's Extrude (the ribbon runs right to left)
    d.wait(3.0)
    d.record()
    d.wait(0.5)
    d.mark("still")


@scene("agent")
def agent(d, s):
    """An AI agent builds a boat through the live MCP bridge while the window shows each step: the acceptance test
    tools/test_agent_benchy.py, run against this window with a pause after each write."""
    sys.path.insert(0, str(ROOT / "tools"))
    import test_live_agent
    import test_agent_benchy
    settings, control = s.work / "settings", s.work / "control"
    control.mkdir(parents=True)
    doc = s.work / "boat.opad"
    s.cli("new", doc)
    # OPAD_BENCH_AGENT turns the live bridge on with editing allowed, as the acceptance tests do
    opened(d, s, doc, env=dict(OPAD_BENCH_SETTINGS=str(settings), OPAD_BENCH_AGENT=str(control), OPAD_LANG="en"))
    d.key("ctrl+2")
    d.wait(1.0)
    pid = s.app.pid

    class Desktop:
        """test_agent_benchy's Desktop, bound to the window this scene started."""

        def __init__(self, app, cli, root):
            root.mkdir(parents=True, exist_ok=True)  # the test writes its picture there
            for _ in range(300):
                for file in settings.rglob("agent/*.json"):
                    try:
                        data = json.loads(file.read_text(encoding="utf-8"))
                    except (OSError, ValueError):
                        continue
                    if data.get("target") and data.get("pid") == pid:
                        self.discovery, self.descriptor = file.parent, data
                        return
                time.sleep(0.1)
            raise RuntimeError("the window never published its live bridge")

        bind = test_live_agent.Desktop.bind

        def close(self):
            pass

    raw = test_live_agent.Client.raw

    def paced(client, tool, **arguments):
        result = raw(client, tool, **arguments)
        if tool == "model_batch":  # let the window show the step, framed
            d.wait(0.6)
            d.focus()
            d.key("shift+f")
            d.wait(2.2)
        return result

    test_live_agent.Client.raw = paced
    test_agent_benchy.Desktop = Desktop
    argv, cwd = sys.argv, os.getcwd()
    sys.argv = ["test_agent_benchy.py", str(s.bin / "opad"), str(s.bin / "opad-cli")]
    os.chdir(s.work)
    d.record()
    d.wait(0.8)
    try:
        test_agent_benchy.main()
    finally:
        sys.argv = argv
        os.chdir(cwd)
        test_live_agent.Client.raw = raw
    orbit(d, 1000, 640, 1100, 620, dur=1.6)
    d.wait(1.5)

def record(args):
    names = args.only or list(SCENES)
    for name in names:
        spec = SCENES[name]
        work = Path(tempfile.mkdtemp(prefix=f"opad-media-{name}-"))
        out = work / "frames"  # local disk while recording (the repository may be a slow mount), copied at the end
        out.mkdir(parents=True)
        session = Session(args.bin, work, args.display, spec["lang"], spec["settings"])
        recorder = None
        print(f"{name}: recording", flush=True)
        try:
            fb = session.start_x()
            recorder = Recorder(fb, out, spec["fps"])
            recorder.start()
            d = Driver(Input(args.display), recorder, fb)
            spec["fn"](d, session)
            d.record(False)
            recorder.finish(dict(scene=name, crop=spec["crop"], target_width=spec["width"], still=spec["still"], fps=spec["fps"]))
            shutil.rmtree(FRAMES / name, ignore_errors=True)
            shutil.copytree(out, FRAMES / name)
            print(f"{name}: {len(recorder.frames)} frames, {recorder.frames[-1]['t'] if recorder.frames else 0} s", flush=True)
        finally:
            if recorder and recorder.is_alive():
                recorder.stop_flag = True
            session.stop()
            if not args.keep:
                shutil.rmtree(work, ignore_errors=True)
            else:
                print(f"{name}: work folder kept: {work}")


# ---------------------------------------------------------------------------------------------------------------
# serve / ctl: a live session for writing scenes.

SOCKET = "/tmp/opad-readme-media.sock"


def serve(args):
    work = Path(tempfile.mkdtemp(prefix="opad-media-live-"))
    session = Session(args.bin, work, args.display, args.lang, BASE_SETTINGS)
    fb = session.start_x()
    recorder = Recorder(fb, work / "frames")
    (work / "frames").mkdir()
    d = Driver(Input(args.display), recorder, fb)
    session.start_app(*args.files)
    namespace = dict(d=d, s=session, time=time, json=json, Path=Path)
    if os.path.exists(SOCKET):
        os.unlink(SOCKET)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET)
    server.listen(1)
    print(f"serving on {SOCKET}, work {work}", flush=True)
    try:
        while True:
            conn, _ = server.accept()
            code = conn.makefile("r", encoding="utf-8").read()
            if code.strip() == "quit":
                conn.sendall(b"bye\n")
                conn.close()
                break
            try:
                try:
                    compiled, single = compile(code, "<ctl>", "eval"), True
                except SyntaxError:
                    compiled, single = compile(code, "<ctl>", "exec"), False
                result = eval(compiled, namespace) if single else exec(compiled, namespace)
                reply = repr(result)
            except Exception as e:  # noqa: BLE001
                reply = f"error: {type(e).__name__}: {e}"
            conn.sendall((reply + "\n").encode())
            conn.close()
    finally:
        server.close()
        os.unlink(SOCKET)
        session.stop()
        shutil.rmtree(work, ignore_errors=True)


def ctl(args):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(SOCKET)
    client.sendall(" ".join(args.code).encode())
    client.shutdown(socket.SHUT_WR)
    print(client.makefile("r", encoding="utf-8").read(), end="")


# ---------------------------------------------------------------------------------------------------------------
# assemble (Pillow)

def assemble(args):
    from PIL import Image
    MEDIA.mkdir(parents=True, exist_ok=True)
    scenes = sorted(p for p in FRAMES.iterdir() if (p / "index.json").exists())
    for folder in scenes:
        if args.only and folder.name not in args.only:
            continue
        index = json.loads((folder / "index.json").read_text(encoding="utf-8"))
        width, height, stride = index["width"], index["height"], index["stride"]
        crop = index.get("crop") or [0, 0, width, height]

        def load(frame):
            raw = zlib.decompress((folder / frame["file"]).read_bytes())
            img = Image.frombuffer("RGBX", (width, height), raw, "raw", "BGRX", stride, 1).convert("RGB")
            img = img.crop((crop[0], crop[1], crop[0] + crop[2], crop[1] + crop[3]))
            target = index.get("target_width") or img.width
            if target < img.width:
                img = img.resize((target, round(img.height * target / img.width)), Image.LANCZOS)
            return img

        if index.get("still"):
            frame = index["frames"][index["marks"][-1]["frame"] if index["marks"] else -1]
            out = MEDIA / f"{folder.name}.png"
            load(frame).quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(out, optimize=True)
            print(f"{out.relative_to(ROOT)}: {out.stat().st_size / 1e6:.2f} MB")
            continue
        frames, times = index["frames"], [f["t"] for f in index["frames"]] + [index["end"]]
        # A mark "gif:<name>" starts another GIF from the same recording (one long run, several pictures).
        cuts = [(0, folder.name)] + [(m["frame"], m["label"][4:]) for m in index["marks"] if m["label"].startswith("gif:")]
        min_ms = 1000 // args.fps
        for (first, name), (stop, _) in zip(cuts, cuts[1:] + [(len(frames), None)]):
            if args.only and folder.name not in args.only and name not in args.only:
                continue
            images, durations, kept_at = [], [], None
            for i in range(first, stop):
                ms = int(round((times[i + 1] - times[i]) * 1000))
                if kept_at is not None and (times[i] - kept_at) * 1000 < min_ms and i != stop - 1:
                    durations[-1] += ms  # at most args.fps frames a second: a frame this close is dropped
                    continue
                images.append(load(frames[i]))
                durations.append(ms)
                kept_at = times[i]
            durations = [min(max(ms, min_ms), args.max_hold) for ms in durations]
            durations[-1] = max(durations[-1], 1500)  # rest on the last frame before the loop starts again
            # one palette for the whole GIF, from a mosaic of evenly spaced frames, so colours never flicker
            sample = images[:: max(1, len(images) // 12)][:12]
            mosaic = Image.new("RGB", (sample[0].width, sample[0].height * len(sample)))
            for k, img in enumerate(sample):
                mosaic.paste(img, (0, k * img.height))
            palette = mosaic.quantize(args.colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
            quantized = [img.quantize(palette=palette, dither=Image.Dither.NONE) for img in images]
            out = MEDIA / f"{name}.gif"
            quantized[0].save(out, save_all=True, append_images=quantized[1:], duration=durations, loop=0,
                              optimize=False, disposal=1)
            print(f"{out.relative_to(ROOT)}: {len(images)} frames, {sum(durations) / 1000:.1f} s, "
                  f"{out.stat().st_size / 1e6:.2f} MB")


def logo(args):
    from PIL import Image
    MEDIA.mkdir(parents=True, exist_ok=True)
    img = Image.open(args.logo).convert("RGBA")
    box = img.getbbox()
    img = img.crop(box)
    target = args.height
    img = img.resize((round(img.width * target / img.height), target), Image.LANCZOS)
    out = MEDIA / "opad-logo.png"
    img.save(out, optimize=True)
    print(f"{out.relative_to(ROOT)}: {img.width} x {img.height}, {out.stat().st_size / 1e3:.0f} kB")
    # for dark pages (GitHub's dark theme): the near-black wordmark and cube face turn light, the blue stays
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a and max(r, g, b) < 90:
                px[x, y] = (232 - r // 4, 234 - g // 4, 238 - b // 4, a)
    out = MEDIA / "opad-logo-dark.png"
    img.save(out, optimize=True)
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size / 1e3:.0f} kB")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("record")
    p.add_argument("--only", action="append", default=[])
    p.add_argument("--bin", type=Path, default=ROOT / "build" / "linux" / "bin")
    p.add_argument("--display", default=":77")
    p.add_argument("--keep", action="store_true", help="keep each scene's scratch folder (settings, log)")
    p.set_defaults(run=record)
    p = sub.add_parser("assemble")
    p.add_argument("--only", action="append", default=[])
    p.add_argument("--colors", type=int, default=256)
    p.add_argument("--max-hold", type=int, default=2500, help="longest a frame is held, in ms")
    p.add_argument("--fps", type=int, default=12, help="most frames a second kept in a GIF")
    p.set_defaults(run=assemble)
    p = sub.add_parser("logo")
    p.add_argument("logo", type=Path)
    p.add_argument("--height", type=int, default=160)
    p.set_defaults(run=logo)
    p = sub.add_parser("serve")
    p.add_argument("files", nargs="*")
    p.add_argument("--bin", type=Path, default=ROOT / "build" / "linux" / "bin")
    p.add_argument("--display", default=":78")
    p.add_argument("--lang")
    p.set_defaults(run=serve)
    p = sub.add_parser("ctl")
    p.add_argument("code", nargs="+")
    p.set_defaults(run=ctl)
    args = parser.parse_args()
    if os.environ.get("OPAD_MEDIA_SCREEN"):  # a different screen size while trying a scene out (serve)
        global SCREEN
        SCREEN = tuple(int(v) for v in os.environ["OPAD_MEDIA_SCREEN"].split("x"))
    args.run(args)


if __name__ == "__main__":
    main()
