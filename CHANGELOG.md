# Changelog

What changed in each release of OPAD, newest first. The section of a version is published as that version's release
notes when the version reaches `main` (docs/releasing.md). Changes merged into `dev` between releases go under
`## [Unreleased]`, which each development build shows as its notes. Entries are written for the people using OPAD, not
as a commit log.

## [0.10.1] - 2026-11-22

Sketching gets a round of fixes from everyday use: double-click selects connected curves, offsets go to one side,
dimensions between lines draw properly, and Sketch fillet says why a corner can't be rounded. Version control keeps each
design in a folder of its own, with linked files saved by relative paths, and OPAD can offer you development builds
before they ship.
Working with an AI assistant is calm and readable now: OPAD tells you in plain words what Claude, Codex or another
AI app looked at and changed, marks its steps in the timeline, and asks you only when it needs you.

### Added
- **See what the AI is working on at a glance.** An assistant can name the stage it is in, in a few words ("Opening the
  drawing", "Designing the mounting bracket"): the AI chip shows it beside the assistant's name, and the AI activity panel
  starts a heading with it, so the entries under it read as that stage's work.
- **Stop AI work** (Tools, the chip's menu and the panel) stops what the assistant is doing and removes its preview;
  changes it already made stay. **Pause changes** lets it look but not change anything until you resume.
- **Development builds, if you want them.** *Help > Offer development builds* makes OPAD offer the builds of the
  development branch and the release candidates too, not only releases, so you can try fixes and features before they
  ship. *About* now shows the build number.
- **Ellipse dimensions.** An ellipse in a sketch takes a major and a minor radius dimension, with expressions like any
  other, so a parametric ellipse no longer needs an equation spline. The Dimension tool picks the radius nearer where
  you click the ellipse.
- **Mirror names its copies.** *Swap in names* (e.g. `R=L, Right=Left`) names each mirrored copy after its source with
  those words swapped ("Bracket R" becomes "Bracket L") and puts it in the component beside its source's whose name swaps
  the same way ("Right side" to "Left side"), so a mirrored assembly needs no renaming or moving afterwards.
- **Sharper edge and face rules.** A rule can take what reaches into a box (`bounds_mode` "intersects") or stays out of it
  ("outside"), leave a side of the box open, pick edges and faces by length or area, and tell a ring's inner rim from its
  outer one (`loop` "inner" / "outer").
- **Motion studies check clearance along the way.** Turn on *clearance* in a motion study's settings and it measures
  how close the moving parts come at every frame: the first collision (when, which two parts, and whether they cross or
  only touch), the closest approach, the pairs closer than a gap you set, and a minimum-distance curve over time. Parts
  that already touch at the start, like a pin in its bore, are listed apart. No more posing the worst frames one by one.

### Changed
- **AI activity in plain words.** The panel now reads like a log of what was done, with your part and step names and
  your units: "Looked at the model from the front", "Added a 40 × 20 × 10 mm box", "Filleted 4 edges of Bracket, 2 mm".
  Lookups fold into one line, a picture the assistant took shows as a thumbnail, and a task it does in several steps is
  one entry with its steps under it. Hover an entry to light up its part, Show frames it, and Undo takes it back while it
  is the latest step, or Back returns the view to where it was until you move it. The panel never opens by itself.
- **An AI preview shows in the view.** While the assistant shows a preview or works on a task, a chip at the top left
  of the view says "Claude's preview · not applied", lists its steps when you point at it, and its *Discard* drops it.
- **The AI chip** in the status bar carries a new AI mark and says what is going on in a word or two: *Connect AI*,
  *AI ready*, *Claude*, *Claude · working*, *Claude · preview*, *Claude needs you*, with a count of changes made while the
  panel was closed. Its menu stops the assistant, sets what it may do, pauses its changes for now, or disconnects it. It
  sits at the right end of the status bar, leaving the room beside the document's name to the name.
- **When the assistant needs you**, one entry says so in amber with one button: for example, if it may only look and
  tries to change something, *Allow changes* lets it. Errors it fixes by itself no longer fill the panel.
- **Steps an AI made** carry its mark in the timeline, their tooltip and Properties name the assistant and the time
  ("Claude (AI) · 14:02"), and Undo reads "Undo Claude: Added …". The assistant's name is saved with the step. A step's
  right-click menu has *Show in AI activity*, and the Annotations panel can show only the AI's notes (author *AI*).
- **Settings > AI assistants** (was AI integration) sets up Claude Code, Claude Desktop or Codex with one click, shows
  whether each is ready, and gives a sentence to try. Setup for other apps moved to *Other AI apps*. It finds Claude
  Desktop and the Codex app as installed from the Microsoft Store, and the Claude Code that comes with Claude Desktop; a
  box for an app's program appears only when OPAD can't find it, and says so.
- **Version control in a folder of its own.** Setting up version control for a document that shares its folder with
  other files now offers to move it into a folder of its own first, so the repository holds just your design. AI agents
  setting up version control ask you first too, then move the design or use the folder as it is.
- **Linked files travel with your design.** They are now saved with paths relative to the design, so a repository no
  longer carries your user name or folder layout. OPAD tells you when linked files sit outside the repository and can
  copy them in, and moving a design into its own folder offers to copy its linked files along.
- **Double-click selects the chain:** in a sketch, double-click a curve to select every curve connected to it end to
  end (Shift or Ctrl+double-click adds the chain to the selection).
- **Offsetting an open curve or chain** in a sketch makes a single parallel copy on the side you point to, instead of a
  closed loop around it. A closed profile still offsets inward or outward. Select several chains (or curves that branch)
  and each is offset on its own by the same distance, on the side you point to, instead of "the path must be one
  connected chain"; a negative distance turns them all to the other side. In Offset, clicking a curve takes its chain:
  on through corners, and where curves branch only into the one that runs straight on; click a picked curve to leave it
  out. The copy goes to the side you point to until you drag the arrow or type a distance, then stays there, and the
  arrow stays put as you reach for it.
- **Renaming is no longer a timeline step:** a body keeps the name you give it through its whole history, also with
  the timeline rolled back, while an earlier feature is edited and after a regenerate. Renaming while rolled back
  leaves the timeline where it is. Version comparisons and merges still see each rename.
- **Search commands puts the tools of where you work first:** in a sketch, *Sketch fillet* comes before *Fillet* (and
  so on for Mirror, Offset, Move and the patterns; "measure" finds Measure in a sketch, Distance outside it).
  Everything is still listed.
- **Rules follow parameters.** The lengths in an edge or face rule (a plane's height, the box, the radius and length
  limits) may be expressions such as `height` or `w / 2`, and the rule matches again when a parameter changes.

### Fixed
- **Sketch fillet says why a corner can't be rounded:** two ends that only touch (common in DXF drawings: merge the
  points), a tangent join, more than two curves meeting, or a radius too big, with the largest that fits. Any sketch
  change OPAD refuses now shows its reason in a notice, not only on the status line.
- Pointing at a constraint's badge or a dimension's value in Trim, Extend and Split also says what it is, while a
  click there still trims or extends the curve.
- A dimension between two lines (and the other two-pick dimensions) draws its lines and arrows while you place it and
  after, not just the number.
- Renaming an item in the browser no longer shows the old name peeking out behind the text box.
- A body keeps its name, colour and visibility when the feature that made it breaks and is fixed again (a sketch change
  loses the profile it was made from, and you pick the profile again). It used to come back as a new body named after
  the feature.
- A sketch tool's preview (Offset, Mirror, Pattern and the others with Apply) shows only what the tool makes or
  changes. In a sketch with freedom left, or one saved a hair off its constraints, it drew every curve of the sketch
  dashed, as if everything were picked.
- Zoomed in close, curved faces stay smooth in two more places: the faces a timeline step made, highlighted while you
  point at it, and Compare's coloured bodies (the other version's, and the changed faces of a modified body). Both
  showed the coarse steps of the whole-model mesh instead of the smooth surface around them.
- **AI assistants' changes no longer fail because you looked back in the timeline.** Rolling the timeline back and
  forward, opening a feature or sketch to edit it and leaving it, or switching between light and dark no longer count as
  changes to the design, so an assistant's next change is not refused as "the document changed". The assistant can also
  see what changed the design last.
- AI assistants can read the list of feature kinds and their inputs while OPAD shows its start page, before a design is
  open.
- AI assistants read OPAD's agent guide one section at a time instead of all 50 KB at once, which some of them could not
  take in; the whole guide is still there for those that want it.
- **An AI assistant's multi-step change checks every step.** A step that makes a broken solid (a sweep along a path
  too tight for its profile, say) now stops the whole change at that step and says which one, as a single step already
  did. Before, a later step could swallow the broken body and the change went through with part of the model missing.
  A component chosen for the whole change no longer adds a "not used" warning to every fillet, cut or join in it.
- **Sweep paths join ends that nearly meet**, such as curves whose end points were typed to a few decimals. A path
  that really is open now names the curves whose ends don't meet and how far apart they are.
- **Shell after a pocket near a fillet.** A body whose pocket comes close to a rim fillet, closer than the shell's
  thickness, now shells, where it used to say it "cannot be shelled by that thickness". When it still cannot, the
  message names the two faces that come too close and how far apart they are.
- Changing part of a study's settings (an AI assistant adding traces, say) keeps the rest: the drivers are no longer
  lost. The settings given are merged in, and an empty value removes one.
- A parameter used only in a sketch's equation curve (an ellipse's half axes, for example) is now listed as used by
  that sketch, can't be deleted while the sketch needs it, and renaming it updates the equation.

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
