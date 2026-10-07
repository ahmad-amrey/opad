# Simulation guide

How to use OPAD's simulation tools, step by step, with a use case for each: mechanisms, structures, 3D-printed parts and
heat. The same use cases are inside the app:
**Simulate ▸ Simulation guide** (on both Simulate ribbon tabs, in the Help menu and as **Step-by-step guides…** at the
top of the Simulation panel). There each step names your own keys, and the tools a use case needs are buttons that start
them.

| You want to know | Use | Solved by |
|---|---|---|
| How a hinge, slider or linkage moves | Joints and the slider | OPAD's kinematic solver |
| Where every part goes as a crank turns, how fast | Motion study | OPAD's kinematic solver |
| How gears, racks and lead screws drive each other | Relations between joints | OPAD's kinematic solver |
| What gravity, masses and motors do, the forces in the joints | Dynamic study | Project Chrono |
| Whether a part holds a load, how far it bends | Static stress study | Netgen mesh + CalculiX |
| What a tightened bolt does to the parts it clamps | Bolt preload + static study | Netgen mesh + CalculiX |
| The frequencies a part rings at | Vibration modes study | Netgen mesh + CalculiX |
| How a 3D-printed part fails: which layer, which way | Printed part + static study | as above, with printed materials |
| How hot a part gets: heatsinks, fans, still air, warm-up | Thermal study | Netgen mesh + CalculiX, the air from correlations |
| Where a fan's air goes, and how hot the parts get in it; a board in a vented box | Thermal study, *with the air solved*, or the Cooling assistant | OpenFOAM (snappyHexMesh, simpleFoam, buoyantBoussinesqSimpleFoam) and CalculiX |
| Which design is best: a vent's place, a fin count, a wall thickness | Design sweep (Cooling assistant's Best vents, or an agent) | any of the above, once per value |

Static and modal studies need CalculiX's `ccx` on the PATH (or beside OPAD, or `OPAD_CCX`); the dynamic study needs a
build with Project Chrono; the air solved needs OpenFOAM (Ubuntu: `apt install openfoam`; or `OPAD_OPENFOAM` set to
its directory). A study that needs an engine this machine lacks says so; `opad-cli mechanism` lists them.

## The workspace in two minutes

- **Simulate workspace**: Ctrl+5, or the workspace switcher. Its ribbon has three tabs: **Mechanism** (joints, motion and
  dynamic studies), **Structure** (loads and supports, static and vibration studies, Printed part) and **Thermal** (heat
  sources, fans, convection, radiation, fixed temperatures, thermal studies).
- **Simulation panel** (Simulation panel on the ribbon): top to bottom,
  - **Step-by-step guides…**: the guide, opened at the use case that fits your document;
  - **MECHANISM**: how many degrees of freedom the joints leave, the joint list and its **slider**, which moves the
    mechanism;
  - **NEW JOINT**: the joint kind and **Add**; for a relation, its ratio, radius or lead;
  - **STUDY**: the studies, **Run**, and a new study's kind with **New study**; then the run's summary, and either the
    frames (Play, the frame slider, the chart and what it shows) or the result map (the field and **Result map**).
- **Selection filters** decide what a click picks: **Bodies** (1), **Faces** (2), **Edges** (3), **Vertices** (4), keys
  as set in the shortcut editor. A tool that needs picks turns on the filter it wants when you start it with nothing
  picked, and says what to click.
- **The browser's Simulation folder** lists the joints, the load cases with their loads, and the studies. Double-click
  a study to run it and show it. Right-click: Run study; on a joint Lock joint and Move with the slider; on a structural
  study Printed part….
- Every joint, load, study and pose is an operation on the timeline: Undo takes it back, and the document keeps it.

## 1. Make a hinged part move

*A lid, door or arm on a hinge, turned with the slider.*

1. Open the Simulate workspace (Ctrl+5) and the Simulation panel.
2. **Hold the frame still.** Switch to the Bodies filter (1), click the frame, choose **Ground** in the panel's NEW
   JOINT list and press **Add**.
3. **Say where the hinge is.** Switch to the Edges filter (3) and click the circular edge of the hinge's hole on the
   part that moves. Then Ctrl-click any edge of the frame it hangs on: the second pick is the part it is fixed to.
4. Choose **Revolute** and press **Add**. The message says the joint was added and how many degrees of freedom the
   mechanism has now: 1 for a hinge.
5. **Drag the slider** under MECHANISM: the lid turns about the hole's axis. Letting go writes that position into the
   document; Undo takes it back.

**What you should see**: one degree of freedom, and the lid turning about the hole's centre line.

**Other joints, the same way**: **Slider** (slides along an axis), **Cylindrical** (turns and slides), **Ball**,
**Planar**, **Pin in slot**, **Screw** (advances as it turns; the pitch goes in the field under the list), **Rigid**
(moves as one). Where the joint is: a circular edge or a round face gives the axis, a flat face its normal, a vertex a
point. With nothing picked second, the part is joined to the world.

**If something goes wrong**

- *"Click where the joint is…"*: nothing was picked; the Edges filter is on now, click the edge and Add again.
- *More degrees of freedom than you expected*: a part has no joint to what holds it, or one part of the mechanism is not
  grounded.
- *A joint shows an error in the browser*: its parts cannot meet its frame where they are now (a mismatched hole).

## 2. A mechanism's travel (motion study)

*A slider-crank, a four-bar linkage or a steering rack: where every part goes as the input turns.*

1. Join the parts (use case 1); ground the frame. The panel should show 1 degree of freedom.
2. In the panel's joint list choose the joint that drives it: the crank.
3. Under STUDY choose **Motion (kinematic)** and press **New study**. The crank turns a full turn (a slider: 50 mm) in
   2 seconds, in 121 frames, and the study runs at once.
4. Press **Play** (Space) or drag the frame slider: the parts move in the view.
5. In the list above the chart choose what it shows: each joint's value, speed and acceleration.

**What you should see**: for a slider-crank of crank r and rod l, the piston travels 2 r; its position follows
x = r cos θ + √(l² − r² sin² θ).

**If something goes wrong**: a mechanism that cannot reach a position (a linkage at its dead point, a rod too short)
stops at the last frame it reached and the summary says which joints could not be met.

## 3. Gears, racks and lead screws

*Two joints coupled at a fixed ratio.*

1. Make each gear a part of its own. **Design ▸ Gear** makes involute gears that mesh: two spur gears mesh with their
   centres the sum of their pitch radii apart (pitch diameter = module × teeth).
2. Join each gear to the frame with a **Revolute** joint on its bore (use case 1), and ground the frame.
3. In the browser's Simulation folder click the first gear's joint, then Ctrl-click the second's.
4. In the panel choose **Gear relation between two joints** and type the ratio: how far the second turns per turn of the
   first. A 20-tooth gear driving a 40-tooth one: **-0.5** (meshing gears turn opposite ways; an internal ring gear
   turns the same way: positive). Press **Add**.
5. Drag the slider on either joint: both turn, the second at half the speed.

**Rack and pinion**: a Revolute joint for the pinion and a Slider for the rack; pick the pinion's joint first, then the
rack's, and type the pinion's pitch radius in mm. **Lead screw**: a Revolute joint for the screw and a Slider for the
nut, the screw's first; the value is the lead in mm per turn.

## 4. Let it swing or fall (dynamic study)

*A pendulum, a lid dropping shut, an arm swinging down: gravity, masses and inertia, and the forces in the joints.*

1. Give the parts their materials: select them and choose **Material** (Inspect); steel is assumed otherwise, and the
   warnings say so.
2. Join them (use case 1). A part in no joint stays where it is.
3. Under STUDY choose **Dynamic** and press **New study**: one second under gravity (down, along −Z), solved by Project
   Chrono.
4. Play it. The chart lists each joint's value and speed, the reaction forces and torques in the joints, and the
   kinetic, potential and total energy.

**What you should see**: a pendulum let go at 90° swings to −90°; with no motor and no friction the total energy stays
flat. A small pendulum's period is 2π √(I / m g d).

**Not on the panel yet** (an agent or `opad-cli` sets them; see *Settings the panel does not show* below): motors and
their speed, torque or position, springs and dampers, friction, contacts between parts, the duration and gravity's
direction.

## 5. Will it hold? (static stress)

*A bracket, a lever, a shelf: its stress, how far it bends, its safety factor.*

1. Give the part its material (**Material**); steel is assumed otherwise. The safety factor is against the material's
   yield strength.
2. Switch to the Faces filter (2), click the faces that are held (bolted, clamped, glued) and press **Fixed support**.
3. Click the faces the load acts on and press **Force**: type x, y, z in N (`0, 0, -500` pushes down with 500 N; the
   force is shared over the faces by area). Or **Pressure** in MPa (positive pushes into the face), or **Gravity load**
   for the parts' own weight.
4. Press **Static stress study**. The parts are meshed (second-order tetrahedra) and solved with CalculiX. The panel
   gives the peak von Mises stress and where, the largest displacement and each body's safety factor.
5. The **Result map** colours the parts; choose **von Mises stress** or **Displacement** in the list beside it. The
   bending is exaggerated so that it can be seen; the legend gives the numbers.

**What you should see**: a cantilever of length L, width b, height h with F at its end bends F L³ / 3 E I
(I = b h³ / 12) and its root's top fibre carries 6 F L / b h².

**Reading it right**

- A sharp inside corner is a stress peak that keeps growing as the mesh gets finer (a singularity of the model): read the
  stress a little away from it, or round the corner with a fillet. The legend stops at the 99.5th percentile when a peak
  stands far above the rest, and says so.
- Bodies that touch along faces are bonded, as if glued.
- Supports, forces and pressures are a **load case** ("Load case 1"); a study takes one case.

## 6. A bolted joint

*A bolt's preload and what it does to the plates it clamps.*

1. Model the bolt as one body (head, shank and nut) through the holes in the plates.
2. Fix the plate that is held: Faces filter, **Fixed support**.
3. Switch to the Bodies filter (1), click the bolt and press **Bolt preload**; type the preload in N (an M10 8.8 bolt
   is tightened to about 25 kN).
4. Add the service load if there is one (**Force** on its faces), then press **Static stress study**.
5. The panel gives the bolt's shank stress beside its nominal stress (the preload over the shank's area), and every
   part's safety factor.

The shank is cut half way and pulled together by the preload through a reference node, as a tightened bolt is.

## 7. Natural frequencies (vibration modes)

*The frequencies a part rings at: keep them away from a motor's speed or another shaking.*

1. Fix the faces that are held (Faces filter, **Fixed support**). Forces do not matter here.
2. Press **Vibration modes study**: the panel lists the natural frequencies in Hz.
3. In the list beside **Result map** choose **Mode 1**, **Mode 2**… to see each shape on the parts.

**What you should see**: a cantilever's first frequency is 1.875² / 2π · √(E I / ρ A L⁴).

A frequency near a running speed (rpm ÷ 60) or its multiples means resonance: stiffen the part or change its mass to move
the frequency.

## 8. A 3D-printed part

*A part simulated as your printer makes it: walls, top and bottom skins and infill, weaker between layers.*

1. Set it up as in *Will it hold?* (supports and loads) and run the **Static stress study** once.
2. Press **Printed part** (Structure tab, or right-click the study in the browser) and tick **The bodies are 3D printed
   (FFF / FDM)**.
3. Press **Import slicer profile…** and choose your slicer's settings:
   - PrusaSlicer: File ▸ Export ▸ Export Config (`.ini`), or the project `.3mf`;
   - OrcaSlicer / Bambu Studio: the process or filament preset (`.json`) or the project `.3mf`;
   - Cura: a profile (`.cfg` / `.inst.cfg`);
   - or any G-code file the slicer made (the settings are at its end).

   Or set **Material**, **Layer height**, **Line width**, **Walls**, **Top layers**, **Bottom layers**, **Infill**,
   **Infill pattern**, **Infill angle** and **Flow** yourself. The line under the form says what the study will use:
   wall and skin thicknesses, the infill model and how full each road is.
4. Choose the **Build direction**: which way is up on the bed. *Along +Z (as modelled)* is the part as it sits in the
   model.
5. Press **OK**: the study runs again. The panel gives the plastic it takes, how it fails first (*along the roads*,
   *across the roads*, *between layers*, *in compression*, *in shear*; in the walls, the skins or the infill) and on
   which layer.
6. In the result list choose **Failure index, printed (1 fails)**: where it fails first. The safety factor is 1 / √index.
7. Open **Printed part** again, change the build direction or the infill, press OK, and compare.

**What you should see**: a part standing up, bent so that its layers are pulled apart, splits between its layers at a
safety factor well under the same part lying flat. An L-bracket printed standing on its foot is weakest at the
upright's root.

**Choosing an infill** (20 %, same part and load):

- squeezed through its thickness: patterns whose walls stand straight up through the layers, **grid**, **triangles** and
  **honeycomb**, are several times stronger than **gyroid** or **cubic**, and **rectilinear** (its layers cross at right
  angles and touch only where they cross) and **lightning** (it only props up top surfaces) crush first;
- bent: the walls and skins carry most of it, and the pattern changes the stiffness by tens of percent.

**Reading it right**: the filament values are typical printed-test-bar data, and printers differ, most between layers.
Your own measured values (stiffness along a road, strengths along, across and between layers) can be set by an agent
or `opad-cli` (below). Very thin parts, where walls meet, print solid: the study says so.

## 9. Keep a part cool (thermal study)

*How hot a chip, a motor or an LED gets on its heatsink: in still air, with a fan, or while it warms up.*

1. Give each part its material (**Material**): aluminium conducts heat about three times better than steel, and steel
   is assumed when none is set (the warnings say so).
2. On the **Thermal** tab, pick the part that makes the heat (Bodies filter, 1) and press **Heat source**; type its
   power in W. It warms through its whole volume; pick faces instead for heat that comes in through a surface.
3. Say how the heat leaves:
   - **Fan**: pick the heatsink, press **Fan**, choose the fan (typical 40 to 140 mm fans, or type its free flow and
     shut-off pressure from its datasheet) and the way the air goes along the fins (`1, 0, 0` for +X).
   - **Convection**, *Natural*: still air. Each face gets its own film coefficient from its size, its tilt and the gap to
     the face it looks at (fins close together choke each other's air).
   - **Convection**, *Forced*: air moving along the faces at a speed you give; or a film coefficient you know.
   - **Radiation** (emissivity: bare aluminium 0.1, anodised or painted 0.85) and **Fixed temperature** (a face on a
     cold plate) work the same way.
4. Press **Thermal study** and choose **Steady** (where the temperatures settle) or **Over time** (how fast it warms up,
   from the room's 25 °C). With a fan or forced convection, and OpenFOAM installed, **Steady, with the air solved**
   solves the air itself (see below).
5. The panel gives the hottest temperature of each part, the heat that reached the air and, for a fan, its air flow, the
   pressure it works against, how much the air warms and the heatsink's resistance in °C/W. The **Result map** shows the
   temperatures; over time, **Play** shows them rising.

**What you should see**: a 60 × 60 mm heatsink with ten 30 mm fins holds about 4 °C/W in still air and about 1 °C/W
with a 60 mm fan; the air warms by the heat over its mass flow times its specific heat (30 W in 17 m³/h: 5 °C).

**How the air is worked out**: the fins are found in the heatsink's shape (straight plate fins along the air). The fan's
curve meets the pressure the fins take to push air through them, which sets the flow; the heat transfer in the channels
between the fins follows from it, and the air warms along the fins as it takes their heat. Still air uses the textbook
correlations for plates and for fins facing each other. Each is solved again with the temperatures until they settle.
The fan's air is taken to go through the fins, as in a duct or under a shroud; air that goes round them cools less.

**With the air solved (CFD)**: the parts sit in a duct of air a little longer than them; OpenFOAM meshes the air and the
parts together, solves the fan's flow through and around the fins (the fan finds its own operating point against
them), then the heat: conduction in the parts and the air carrying it away, all at once. It takes minutes rather than
seconds. The **Result map** draws the air's streamlines over the parts' temperatures; choose **Air speed** to colour
them by how fast the air moves. It shows what the correlations cannot: air slipping round the fins instead of through
them, dead corners, a part downstream in another's warm air. A study that has not settled says so; finer cells
(`cfd.cell_size` mm) cost time but sharpen thin fins. Only one fan (or one stream), heat sources in whole parts, and
the parts' heat leaves only by the moving air.

## 10. Cool a board in a vented box (air solved)

*How hot a single-board computer gets in its case: with a fan blowing in, or with vents only and warm air rising.*
Needs OpenFOAM.

1. Model the box as one solid: its walls and the openings cut through them. Model the board, its chips and the
   heatsink inside it as bodies of their own, and a block where each fan sits, as big as the fan. (On an empty
   document the assistant's first page builds an example: a board, a 4 W chip under a finned heatsink and a 0.5 W power
   chip in a 110 × 80 × 40 mm ABS box with a 30 mm fan and three exhaust slots.)
2. Press **Cooling assistant** on the **Thermal** tab. Its first page shows the box it found (the smallest body that holds
   the parts making heat) and what is inside it.
3. **Heat**: tick each part that makes heat and type its power. Tick **Board** for the circuit board and give its copper
   layers: copper spreads heat along a board about eighty times better than through it. A chip with no material is
   taken as silicon.
4. **Air**: for each fan, the block that stands for it (or the heatsink it blows on), the fan (typical fans by size, or
   **Custom** with its datasheet's free flow and shut-off pressure) and the way it blows. Untick **Fans move the air**
   for a box with vents only: the air then moves by rising where it is warm, so put vents low and high.
5. **Run**: the room's temperature and the quality: **Quick** to compare designs, **Normal** for the answer, **Fine** to
   check it. It takes minutes; the status bar shows how far it is and you can keep working.
6. The page lists each part's temperature (hottest first), each fan's flow and pressure, the air through the box and
   how warm it leaves, and how much went out as radiation. **See inside the box** hides the box; **Temperatures** and
   **Air speed** colour the map and the streamlines.

**How it is worked out**: the air in the box and a margin of the room around it are meshed around every part in it
(fine enough for each part's walls, fins and vents). Each fan is a disk across which its curve sets the pressure jump,
so it finds its own operating point against the box's resistance. The heat goes back and forth: the parts in CalculiX
as one bonded mesh (touching parts conduct as one, the board along and across its layers, radiation between the parts
and to the room by view factors), the air's temperature in OpenFOAM with the parts' surfaces as its walls; the heat each
face gives the air is its film for the next CalculiX solve, until the temperatures settle. With fans the flow is solved
once (laminar); with vents only, the flow and the air's temperature are solved together under gravity at each pass
(Boussinesq), the walls stepped to the cells. The assistant writes ordinary loads (case *Cooling*) and a study, which
the Simulation panel and an agent can change afterwards.

## 11. Find the best place for the vents (design sweep)

*Which vent height, slot count or fan position keeps the hottest part coolest: each candidate run and compared.*

1. Make what you want to vary a parameter: edit the feature that cuts the vent (double-click it in the timeline) and
   type a parameter's name, such as `vent_z`, instead of its position. **Parameters** on the Design ribbon lists them.
   (The example of use case 10 has `vent_z`, the exhaust slots' height.)
2. Set the case up in the **Cooling assistant** and run it once.
3. On its **Best vents** page choose the parameter, the range (**From**, **To**) and how many **Values**; **Then closer
   in** adds runs between the best value's neighbours (golden section). **Keep cool**: the hottest part, or one part.
4. Keep **Compare at Quick quality** ticked: every value runs on coarse cells, then the best runs again at full
   quality.
5. Press **Find the best**. The table fills with each value's hottest temperature and marks the best; **Use the best**
   sets the parameter to it (one undo step). Run the case again for the best design's full result.

**What you should see**: the hottest temperature for each value, the best marked; the Simulation panel shows the best
design's map. A value that breaks the model (a slot cut through nothing) is listed as failed and the rest go on. Each value is a whole run: five values and
two refinements at Quick take about as long as two Normal runs. The sweep works on copies of the document.

An agent sweeps any study the same way: a `sweep` study with `study`, `params` (`values`, or `from`, `to`, `steps`),
`objective` (`of`, `bodies`, `goal`), `refine`, `screening` and `confirm` (see the agent guide).

## Settings the panel does not show

Everything above is stored in each study's `settings`. Some settings have no control in the panel yet; an AI agent (MCP)
or `opad-cli` sets them on a study (`--id`; the settings given replace the study's) and runs it again. For example:

```sh
opad-cli study model.opad --id <study id> --settings '{"duration": 5, "frames": 301, "drivers": [{"joint": "<joint id>", "speed": 360}]}'
```

or ask an agent: *"Run the motion study for 5 seconds with the crank turning at 60 rpm."*

| Study | Settings |
|---|---|
| Motion | `duration` s, `frames`, `drivers: [{joint, to \| speed \| expr \| table, profile}]`, `traces: [{part, point, name}]` |
| Dynamic | `duration`, `frames`, `step`, `gravity` (true, false or [x, y, z] mm/s²), `contacts`, `friction`, `restitution`, `free`; on joints: `drive` (position, speed, torque, force), `spring`, `friction`, `limits` |
| Static, modal | `case`, `bodies`, `mesh_size` mm, `modes`, `materials: {"all" \| body: {E, nu, density, yield}}` |
| Thermal | `ambient` °C, `gravity` (down, for natural convection), `duration` s and `frames` (over time), `mesh_size`, `materials: {"all" \| body: {k, cp, emissivity}}`; on loads: `fan` (an id or `{flow, pressure, curve}`), `vector`, `count`, `ambient`; `materials: {body: {k, k_through, normal} or {pcb: {layers, copper_oz, coverage, thickness}}}`; `air: "cfd"` with `cfd: {enclosure (a body id, or false), quality (quick, normal, fine), buoyancy, radiation, cell_size, upstream, downstream, padding, flow_iterations, heat_iterations, passes, settle, streamlines}` |
| Sweep | `study` (id or name), `params: [{name, values} or {name, from, to, steps}]`, `objective: {of, bodies, goal}`, `refine`, `screening` (settings merged into the study's for the points), `confirm`, `max_points` |
| Printed part | `print: {profile, material (or {base, E, nu, kt, kz, X, Y, Z, S, C, density}), build_direction, layer_height, line_width, walls, top_layers, bottom_layers, infill, pattern, infill_angle, flow, bodies}` |

The full reference is the agent guide's *Motion and simulation* section (`core/res/agent_guide.md`), which agents read
through MCP. `opad-cli commands` lists every command and its arguments.
