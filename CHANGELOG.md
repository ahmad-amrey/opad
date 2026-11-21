# Changelog

What changed in each release of OPAD, newest first. The section of a version is published as that version's release
notes when the version reaches `main` (docs/releasing.md). Changes merged into `dev` between releases go under
`## [Unreleased]`, which each development build shows as its notes. Entries are written for the people using OPAD, not
as a commit log.

## [Unreleased]

### Added
- **Development builds, if you want them.** *Help > Offer development builds* makes OPAD offer the builds of the
  development branch and the release candidates too, not only releases, so you can try fixes and features before they
  ship. *About* now shows the build number.

### Fixed
- Pointing at a constraint's badge or a dimension's value in Trim, Extend and Split also says what it is, while a
  click there still trims or extends the curve.

## [0.10.0] - 2026-11-21

Sketching gets steadier and quicker: adding a constraint moves only what it names, offsets stay tied to their curves,
D measures without adding anything, and tools used once get out of the way. Connecting Claude or Codex is now a few
clicks, step by step.

### Added

- **Offsets that follow their curves.** An offset copy made in a sketch is now held to the curves it came from as one
  rigid piece, so moving or turning the original takes the copy along at the same distance. Its distance label shows
  next to it; double-click it to make the offset again at another distance. Set the Offset tool's *Relationship* to
  *None (plain copy)* for a copy that is not held, as before.
- **Measure in a sketch.** D now starts the Measure tool in a sketch: pick a point or a curve for its position or sizes,
  or two for the distance or angle between them. Nothing is added to the sketch. Outside a sketch, D is still Distance.
- **Hover a constraint or a dimension to see what it is.** The status bar names it at once ("Horizontal constraint",
  "Distance dimension: 32 mm"), and a tooltip adds what it holds, what it is on, and what a click or double-click does.
- **Connect an AI client, step by step.** With no AI client connected, the AI chip in the status bar opens *Connect an
  AI client*: one click registers OPAD in Claude Code, Claude Desktop or Codex. Each client shows whether it is
  connected, not found, registered for another or older OPAD, or pointing at a program that is gone, and *Point it at
  this OPAD* fixes the last two. Then restart the client, allow access for your document, and ask the AI with the
  ready-made prompt.

### Changed

- **Adding a constraint or changing a dimension no longer shifts the whole sketch.** What you constrain moves (the
  second pick first), plus whatever must follow through shared points; everything else stays where it was. A change
  that cannot be satisfied is still refused.
- **Tools you use once close when they are done.** The sketch's Apply tools (Offset, Chamfer, Move, Copy, Rotate,
  Mirror, the patterns and the like), every feature panel, Mesh to solid, Drawing to sketch, placing a drawing or a Canvas picture, KiCad Sync, Note
  and the section's Pick face end after Apply, OK or Enter. Hold Shift (Shift+Enter, or Shift-click the button) to keep
  the tool open for another use. Panels that belong to the document, such as Annotations or a pinned Properties panel,
  stay until you close them.
- **Sketch Dimension moves to Shift+D**, to make room for Measure. A D saved for Dimension in your settings is reset.
- **Design > Assemble fits the window** at 1600 px with every label: *Play explode* and *Collapse exploded view* are in
  the *Exploded view* button's menu, and stay on the Explode tab that shows while an explode is on.
- **The macOS download opens as the usual install window:** drag OPAD onto the Applications folder beside it.
- **Export keeps the 3D formats** when the document or the selection also holds drawings or sketches. STEP stays the
  default, the file holds the 3D bodies, and the summary says how many drawings or sketches were left out. The 2D
  formats are still there for a view.

## [0.9.0] - 2026-11-21

First public release: an open-source parametric CAD program whose design files git can diff and merge.

### Highlights

- **Designs that version cleanly in git.** An `.opad` file is plain text: a log of operations plus each body stored once
  by its content. A diff shows what you did, `opad-cli diff` explains it in design terms (parameters, features, bodies),
  and a merge driver joins independent changes from two branches, stopping only on real conflicts. Version control is
  built into the program: commit, branches, pull and merge, and side-by-side comparison of any two versions.
- **Opens what you already have.** STEP, IGES, Parasolid X_T/X_B, BREP, STL, 3MF, OBJ, PLY, glTF/GLB, VRML, DXF, DWG,
  SVG and KiCad boards open in a fast viewer mode; *Save to edit* turns them into an OPAD design. Meshes can be rebuilt
  as solids.
- **From a 2D sketch to a solid.** Sketches with constraints and dimensions, extrude, revolve, fillet, shell, patterns
  and more, all in an editable history.
- **Sketch Align and Rigid groups.** *Align* turns a shape as one piece so that one of its lines (or a nearly straight
  arc, as DXF profiles often contain) stands exactly vertical, horizontal, or along another line, keeping every size
  and angle. The *Rigid* constraint holds curves together so they move and turn as a whole.
- **Technical drawings, notes and hand drawing** on the model, and **AI agents** working in the same window through
  MCP, with every command also available from `opad-cli` and Python.
- **English and Arabic** interface.
- **Tells you about new versions.** OPAD checks GitHub at most once a day (or when you choose *Help > Check for
  updates*) and, when a newer version is out, says what it brings and offers its download. It can be switched off
  under *Help*; nothing about you or your files is sent.

### Downloads

- Windows: an installer (per user, no administrator rights; updates keep your settings) and a single-file exe that runs
  without installing.
- macOS 15 or later (Apple silicon): a dmg.
- Linux: a Debian package for Ubuntu and Debian (24.04 or newer), and an AppImage for other distributions.
- Your settings, templates and recovery snapshots live in your own user folders, never beside the program: installing,
  updating and uninstalling leave them as they are, and each new version keeps a copy of your settings before it
  changes any. On Windows, for a portable copy that keeps everything beside it, put an empty file named
  `opad.portable` next to the exe.

### Known limitations

- The packages are not code-signed yet: Windows and macOS warn on the first start (see *First start* below).
- Dynamic, static and modal simulation studies need the Chrono and Netgen engines, which the Windows and Linux
  packages leave out; joints and motion studies work there.
- The Python module and the plugin interface are not in the packages, and the command-line tool `opad-cli` comes
  with the Windows downloads and the Debian package only (not the dmg or the AppImage); build
  from source for them.
- macOS: Apple silicon (M1 or newer) with macOS 15 or later; the app bundles libraries built for macOS 15.
