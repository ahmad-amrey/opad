# Changelog

What changed in each release of OPAD, newest first. The section of a version is published as that version's release
notes (the Build workflow reads it when the tag `vX.Y.Z` is pushed), so write it before tagging: see
[docs/releasing.md](docs/releasing.md). Entries are written for the people using OPAD, not as a commit log.

## [0.9.0] - unreleased

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
- macOS (Apple Silicon): a dmg.
- Linux: a Debian package for Ubuntu and Debian (24.04 or newer), and an AppImage for other distributions.

### Known limitations

- The packages are not code-signed yet: Windows and macOS warn on the first start (see *First start* below).
- Dynamic, static and modal simulation studies need the Chrono and Netgen engines, which the Windows and Linux
  packages leave out; joints and motion studies work there.
- The Python module, the plugin interface and the command-line tool on macOS and Linux are not in the packages; build
  from source for them.
- macOS: Apple silicon only (M1 or newer).
