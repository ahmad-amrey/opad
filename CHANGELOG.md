# Changelog

What changed in each release of OPAD, newest first. The section of a version is published as that version's release
notes when the version reaches `main` (docs/releasing.md). Changes merged into `dev` between releases go under
`## [Unreleased]`, which each development build shows as its notes. Entries are written for the people using OPAD, not
as a commit log.

## [Unreleased]

### Added
- **AI assistants can try a mechanism's pose before keeping it.** A trial joint position is now held as a preview
  that the assistant can picture and then apply or drop, so you see the pose it is weighing in the 3D view too.
- **Hard stops are contacts, not collisions.** A motion study's clearance check takes a contact tolerance: faces that
  touch, or overlap by no more than it, at a designed stop are listed as contacts (and marked as expected when a
  joint sits at its limit), instead of a collision of a hair's depth that is only meshing noise.
- **A face's outline in one pick.** Assistants can ask for the outer or inner edges of a face found by its direction and
  height, arcs included, and get short lists back, so filleting or chamfering an outline takes fewer steps.

### Changed
- When an assistant's edit is refused because a later feature breaks or a body is not a valid solid, the message now
  names the feature and says what is wrong with the solid, applied directly as well as previewed.
- An assistant's batch of steps can pass through a broken state on the way (a cut moved off the part and back): only
  the end result is checked, and it is still one step to undo.
- Sketch shapes an assistant draws take parameter expressions for their points and sizes, as dimensions already do.

## [0.11.0] - 2026-11-27

OPAD now works with 3D scans: open the raw point cloud, line a scan up with the axes from fitted planes and axes, and
see how far it is from your CAD on a colour map. Solvers install from inside OPAD, and downloads carry their version.
Sketching gets a round of fixes from everyday use: double-click selects connected curves, offsets go to one side,
dimensions between lines draw properly, and Sketch fillet says why a corner can't be rounded. Version control keeps each
design in a folder of its own, with linked files saved by relative paths, and OPAD can offer you development builds
before they ship.
Working with an AI assistant is calm and readable now: OPAD tells you in plain words what Claude, Codex or another
AI app looked at and changed, marks its steps in the timeline, and asks you only when it needs you.
Simulate is now for everyone: pick the question that matches your case, from a picture, and OPAD walks you through it,
including a new airflow check for ducts and fittings.

### Added
- **Line up 3D scans and STL meshes.** A scan has no exact faces or edges, so OPAD now fits them: click a mesh where a
  plane or an axis is asked for (Align, Move, a sketch's plane, construction planes and axes) and it fits the flat around
  the click, or a hole's or boss's axis, and says how good the fit is (how far the mesh strays from it, and over how
  many triangles), so a curved region taken for a plane shows up as a poor fit. A box over the mesh fits the plane
  through the triangles it takes, *Fit to points* the plane through picked vertices, and a construction axis can be
  where two planes meet. **Align scan to axes** stands a scan on XY in one step, its base flat down, a wall along X
  and centred, with *Next base*, *Next X* and *Flip* for the other ways it could stand. AI assistants get the same fits
  (`fit_reference`, `scan_axes`).
- **Put a scan on its CAD by matching points.** Align has a new *Point pairs* way: click a point on the scan, then the
  same point on the CAD body (or on another body, or a sketch's point), and so on in turn. From three pairs on, OPAD
  finds the move that lines them all up best, shows it at once, and lists how far each pair is still apart. A pair
  picked by mistake, far off the others, is left out and named; Esc takes back the last pick and Remove drops a pair.
  It works with meshes, point clouds and solids alike, and AI assistants use it through Align (`from: point_pairs`).
- **Align a scan step by step (3-2-1).** Align's new *3-2-1 steps* way lines a part up the way reverse engineering does:
  a primary plane onto a plane, then a secondary line or plane, then a tertiary point or plane, each step keeping what
  the earlier ones fixed, while the panel says what is still free ("Free: slide along X"). On a scan, rest the pointer
  near an edge and OPAD offers the crease where its two flats meet, drawn along its whole length and far more accurate
  than the ragged mesh edges; a box over a run of edges gives the line through them. Align no longer opens the plane
  picker by itself: the panel shows which plane (From or To) comes next and opens the picker when you click it, and
  *Fit to points* explains itself and says what each click did (a vertex added, one taken back, or nothing under it).
- **Point clouds: work with the raw scan.** A scanning app's mesh rounds edges, fills small holes and smooths detail
  away; the measured points keep it. OPAD now opens and imports point clouds as bodies of their own: PLY files without
  faces (they used to be refused), XYZ, ASC (RealityScan, CloudCompare), PTS, CSV, E57 and LAS, with their colours, normals and intensity, in the right
  units and the right way up. Clouds of tens of millions of points stay smooth to turn and zoom: the overview is drawn
  from a sample and the part you zoom into at full density. The new *Design > Scan* tab estimates normals, removes
  stray points, thins a cloud evenly, selects points with a lasso, a box or a brush to keep or delete them, and slices
  a cloud with a plane: each slice is a flat profile you can sketch over, and *Slice to sketch* turns it into lines to
  fit with arcs and dimensions, slice by slice. Points can be coloured by the scan's colours, their normals or their
  intensity, and clouds export as PLY, XYZ, PTS, CSV or E57. AI assistants read clouds, query regions and slice them
  too (`cloud`, `cloud_sketch`). LAZ files need converting to LAS or E57 first.
- **Edit a point cloud by hand, as you would sculpt it.** Select a scan and choose *Edit point cloud* (or double-click
  it): its own *Edit points* tab opens with round brushes you drag over the points. *Push/Pull* pulls them out of the
  surface or pushes them in (Ctrl or Alt turns it round), *Smooth* settles scanner noise onto the surface, the *Delete*
  brush wipes stray points, and *Add points* fills a hole the scanner left with points on the surface around it. When
  the scanner bridged a hole that should be open, paint over the bridge (or box it, lasso it, or let *Grow region*
  spread over that one surface from a click), check the highlight and press Delete. Undo goes stroke by stroke while
  you edit, Cancel drops everything, and Finish keeps it all as one step in the timeline (only the changes are
  stored), ready for Deviation, slices and modelling. Double-click that step to edit it again.
- **Simulate asks what you want to find out.** The Simulate tabs are now goals (Air and fluids, Heat, Strength,
  Movement), and the start page shows each check as a plain question with a small picture of the case: "How much
  pressure does a fitting lose?", "Will my electronics box overheat?", "Will it bend or break under a load?", "Will my 3D
  print hold?", "Does my mechanism move freely?", "How strong a motor do I need?" and more. Each check walks you through
  a few steps, proposes what it can (the faces a part rests on, a duct's open ends, the parts that get hot), and answers
  in words first ("Safe: holds 3.2 times this load"), then with the colour maps. Compare puts two designs side by side.
  The earlier tools are all still there under *Advanced*.
- **Airflow through ducts and fittings.** A new airflow check (and `flow` study) works out the pressure a fitting loses
  at a given airflow, or the airflow a fan really delivers through it, shows the air speed, pressure and streamlines, and
  marks where the air swirls. You say where the air goes in and out by clicking an edge round each opening (OPAD
  proposes the bores, past any chamfer at the rim), and an arrow through each opening shows the air's way: click it, or
  *Flip the air's way*, to turn it round. Round parts are solved as a thin slice, steps and ledges included, so a run
  takes well under a minute (an 8" to 6" duct adapter with a step: 13 seconds, where it took 12 minutes as a full 3D
  part); other parts are solved whole in about 3 to 7 minutes. A run stops once the pressure drop has settled, and the
  check says beforehand about how long it will take on your computer.
- **Solvers installed from inside OPAD.** The airflow and strength checks need OpenFOAM and CalculiX; when one is
  missing, *Install* downloads it (about 45 MB for OpenFOAM, 13 MB for CalculiX) from OPAD's own releases, checks it
  and sets it up for you on Windows, Linux and macOS, with no admin rights and nothing else to install. A copy you
  installed yourself still works, and the solvers' licences and sources are listed under *Help > Third-party licences*.
  Each release's notes link the solver versions it installs.
- **A .gitignore when you set up a repository.** *Set up repository* offers a recommended .gitignore that keeps exports
  (STEP, STL, 3MF, drawings, renders) and OPAD's temporary files out of the history; *Customise* shows the groups to tick.
  Files your design links stay tracked even when their type is ignored, and an existing .gitignore is only added to.
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
- **Deviation: how close is the scan to the CAD?** *Inspect > Deviation* compares a measured body (a 3D scan, a printed
  part, an older revision) with a reference and colours it by how far each spot lies from it: green within your
  tolerance, yellow to red where there is extra material, cyan to blue where material is missing. The panel gives the
  mean, median, RMS, the largest deviations and the share within tolerance, with a histogram and the worst spots, which
  are pinned on the model; rest the mouse on the map to read a spot, click to pin it. It can also measure both ways (to
  see what a scan does not cover), ignore far-off noise, keep to faces you pick, and fit the scan onto the reference
  first as a preview you accept or discard. A point cloud can be the measured side too, its points coloured
  the same way. Scans of millions of triangles are measured in a second or two without
  freezing the window, and AI assistants get the same numbers from the `deviation` tool.

### Changed
- **Connecting an AI app is one button.** *AI assistants* lists the AI apps found on this computer, each with one
  Connect button, and then says what is left (restart the app, a first question to ask). For any other AI app, *Copy
  setup message* gives you one message to paste into its chat, and its assistant adds OPAD itself. What the assistant may
  do is one choice (Nothing, Look only, Look and edit); program paths and the raw settings moved under *Details*.
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
- **Downloads say their version.** Every file of a release carries its version in its name
  (`OPAD-0.11.0-windows-x64-setup.exe`, a development build `OPAD-0.11.1-dev.80-...`), so a downloaded file tells you
  which version it is. The download links in the README open the latest release.
- **Simulate's start page fills the window.** The object browser stays out of the way while you choose a check, and
  comes back with the 3D view.

### Fixed
- **Updating no longer asks to close OPAD's command-line tool.** Each open session of an AI app connected to OPAD
  (Claude Code, Claude Desktop, Codex) keeps its own `opad-cli` running, so the installer used to list ten or more of
  them to close. It now leaves them running, and those sessions keep their connection; only an open OPAD still has to be
  closed first. The old copy is removed the next time OPAD starts after those sessions end.
- **3D scans and other big meshes no longer freeze OPAD.** Choosing the axis in *Move / copy*, or switching to the
  Face, Edge or Vertex filter, used to make OPAD wait while it prepared every triangle of the mesh for picking: over
  three minutes on a 3-million-triangle scan. It now takes no noticeable time, picking a scan's edges, points and
  triangles works as on any part, committing a move no longer pauses, and scans open faster.
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
  Hiding, showing, colouring or locking parts doesn't count either, also while the assistant is working or has a preview
  waiting: its change goes in after yours and your hidden parts stay hidden. When the design did change meanwhile, the
  assistant is told what changed it.
- AI assistants can read the list of feature kinds and their inputs while OPAD shows its start page, before a design is
  open.
- AI assistants get the design history and the operation list a page at a time (50 short entries: name, kind and
  state), and only the last change when they ask what changed, instead of replies of tens of KB that could fill their
  memory. They can still ask for the full details.
- An AI assistant trying a mechanism position (a preview) no longer gets back an id for a position that was never saved.
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
- **Joints made by an AI assistant or a script:**
  - A ground or rigid joint no longer asks where it is: it holds the part where it stands.
  - A joint can be moved onto another axis (the cylinder face of a pin, say), another part or another base and stays
    the same joint, with its name, limits, drive and poses; the parts stay where they are. Before, it had to be deleted
    and made again.
  - Driving a joint past a limit says which coordinate stopped (rotation or translation), at what value, and what was
    asked: "Slide rotation held at its upper limit 30 deg (asked 45 deg)".
  - The mechanism no longer counts the ground's equations as redundant (a clean mechanism read "6 redundant"), and
    names the joints whose equations repeat others, such as the one closing a loop. Dynamic studies of a grounded
    mechanism no longer warn about redundant equations either.
  - A dynamic study in a build without its dynamics engine (Project Chrono) says so, and what still works, instead of
    a bare "not available"; static, modal and thermal studies say what they are missing the same way.
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
