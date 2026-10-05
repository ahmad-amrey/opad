# OPAD agent guide

What an agent needs to model in OPAD and cannot read from the tool schemas. The same text is served by both MCP
servers as the resource `opad://guide/agent` and by `live_diagnostics` with `include_guide: true`.

## Units and expressions

- Geometry is in millimetres and degrees. A plain number is mm for a length input, degrees for an angle input and a
  plain number for a count.
- A string is an expression: `"20 mm"`, `"width / 2"`, `"2 * pi * r"`, `"30 deg"`, `"count - 1"`. Units: `mm cm m
  um in ft deg rad`. Functions: `sin cos tan asin acos atan atan2 sqrt abs min max floor ceil round pow exp ln log
  hypot sign clamp mod if select assert`. Constants: `pi PI E`.
- Conditions: `< <= > >= == !=` (1 or 0; units must match), `&&`, `||`, `!`, `c ? a : b`, `if(c, a, b)`,
  `select(i, v0, v1, ...)` (counting from 0). The branch not taken may fail, so `if(x > 0 mm, sqrt(x * 1 mm), 0 mm)`
  is a guard. `clamp(x, lo, hi)`; `mod(a, b)` and `a % b` take the divisor's sign (`mod(-30 deg, 360 deg)` is 330 deg).
- Checks: `assert(condition, "message")` is 1 while the condition holds. A change that makes any parameter fail (an
  assert, a sqrt of a negative) is refused with that parameter's name and message, and the document is unchanged.
- Values carry their dimension: `10 mm * 2 mm` is an area and is refused where a length is asked for. Write units
  explicitly; inside `sin()` a plain number is radians, so write `sin(30 deg)`. A typed plain number added to an angle
  is degrees (`30 deg + 15` is 45 deg), but a plain number a function computed (`tan(x)`, a comparison) is refused
  there: multiply it by `1 rad` or `1 deg`. A power must give a whole power of length: `(x / 1 mm)^1.5`, not `x^1.5`.
- Parameters (`param`) are named expressions; `param` with `rename` renames one and rewrites every expression that
  uses it. A change regenerates what depends on it, and only that.

## Identifiers and results

- `feature_id` is a history operation, `body_ids` are bodies (scene nodes), `sketch_id` a sketch, `component_id` a
  component. A feature id is never a body id. `ids`/`operation_ids` are operation ids.
- `body_ids` lists the bodies a feature made or changed:
  - creation features (box, cylinder, extrude, revolve, ...) with `operation: new`: one body per separate solid, so an
    extrude of three separate profiles gives three;
  - with `operation: join | cut | intersect`: the target bodies that changed;
  - modifying features (fillet, chamfer, shell, draft, press pull, scale, move without copy): the bodies they changed;
  - mirror, pattern_rect, pattern_circ: only the new copies (a count of 3 gives 2); `all_body_ids` adds the picked
    bodies first;
  - move with `copy: true`: the copies; split: every piece, the original body first;
  - combine: the target; removed bodies (tools, remove) are not listed.

## Planes and frames

- Base planes, each seen from its positive normal with x to the right:
  - `xy`: x = +X, y = +Y, normal = +Z (top);
  - `xz`: x = +X, y = +Z, normal = -Y (front);
  - `yz`: x = +Y, y = +Z, normal = +X (right).
- A plane input is `{"base": "xy"}`, `{"face": <planar face ref>}`, `{"feature": <construction plane feature id>}`,
  `{"frame": {"origin": [..], "x": [..], "y": [..]}}` or `{"origin": [x, y, z], "normal": [x, y, z]}` with an optional
  `"x"`. Without `x`, world X laid onto the plane is its x (world Y when X is the normal), so a cylinder along Y is
  just `"plane": {"origin": [0, 0, 0], "normal": [0, 1, 0]}`. The resolved frame is returned and stored.
- Sketch coordinates (u, v) are in the sketch's frame: world point = origin + u * x + v * y.

## Sketch geometry

- `{"points": [...], "entities": [...], "constraints": [...]}`. Every id is unique across the whole sketch: points,
  entities and constraints share one id space, so point 1 and entity 1 collide. Number them apart, for example points
  1-99, entities 100-199, constraints 200 and up.
- Stored geometry keeps the joins older builds cannot read (Smooth, Curvature or Tangent between a spline and a line,
  circle or arc) in `more_constraints`. `sketch_details` lists both in its constraints section; send new constraints in
  `constraints`.
- Point: `{"id": 1, "x": 0, "y": 0}` in sketch mm.
- Entities, `p` lists point ids:
  - line `[start, end]`;
  - circle `[centre]` plus `r`;
  - arc `[centre, start, end]`, counter-clockwise from start to end, both ends at the same radius (the solver keeps
    them there); for a clockwise arc swap start and end;
  - ellipse `[centre, end of the major axis]` plus `r` = the minor radius;
  - spline: `p` are fit points, interpolated in order (knots at chord lengths). It is closed and smooth (C2) through
    the seam with `periodic: true`, with the first point's id repeated last, or with a last point lying on the first;
    a closed one is the same curve whichever point comes first and whichever way they run. `start_tangent` and
    `end_tangent` ([dx, dy]) set an open one's end directions;
  - a spline with `equation: {"x": ..., "y": ..., "t0": "0 deg", "t1": "360 deg", "tolerance": 0.001}` is the
    curve x(t), y(t) (expressions over `t` and the parameters, lengths in mm): computing the sketch samples it until
    the spline through the samples is within `tolerance` mm (default 0.01) everywhere, so the point count follows
    from the tolerance, and parameters reshape it. `p` may be left out; given as [start, end], those two point ids
    become the curve's ends, so lines can join it. Its points are fixed. Give t0 and t1 in deg when the equations
    add t to angles (a plain t is not taken as degrees). Ends that meet make it closed;
  - with `degree`, `p` are the control poles of an exact B-spline. `knots` with `multiplicities` (same length) and
    `weights` (one per pole) are optional: without them the knots are uniform, clamped at both ends (the curve starts
    and ends on the end poles) or periodic with `periodic: true`, and the weights 1. Given, an open spline's
    multiplicities add up to poles + degree + 1, a clamped end taking degree + 1;
  - `construction: true` for guide geometry that is never part of a profile.
- Curves share points by id: two lines meeting at point 3 are joined; no coincident constraint is needed.
- Constraints, `refs` lists ids: `coincident horizontal vertical parallel perpendicular collinear tangent equal
  concentric midpoint symmetric fix smooth curvature`; dimensions `distance hdistance vdistance radius diameter angle
  arc_length` take `value` (mm, or radians for angles) and optionally `expr` (`"width / 2"`), which then drives it.
- `smooth` (G2) and `curvature` join a spline's end to another spline, a line, a circle or an arc: smooth puts the end on
  it, along it and bending as it does, curvature only bends it so (straight by a line); `tangent` takes a spline too.
  These take open control-point splines of degree 2 or more (`degree` given), not fit splines. Both curves move as
  little as they can: fix the one that must stay.
- `hdistance`/`vdistance` between two points drive the size of the difference and keep the side it is on; with
  `signed: true` they drive `q - p` itself, so a negative value or expression puts q before p. On one point they are
  its coordinate from the sketch origin, signed: two of them fix a point at (expression, expression).
- A change the solver cannot satisfy is refused; nothing is over-constrained. `sketch_details` reports the degrees of
  freedom and the closed profiles.

## Shapes

- `geometry.shapes: [{"kind", "picks": [[u, v], ...], "options", "first_id"}]` draws common outlines without working
  out points, centres or tangents by hand. Shapes are added after the listed points, entities and constraints and
  become ordinary ones before the sketch is stored. The result's `id_map` has one entry per shape with the
  `points`, `entities` and `constraints` it made (and for outline text the `profiles`: a point inside each letter).
- Any point, entity or constraint without an `id` gets the next free one; `first_id` makes a shape's ids run
  consecutively from that number, so constraints in the same call can refer to them.
- Kinds (picks, then options):
  - `point` [p]; `line` [a, b]; `circle` [centre] with `radius` or `diameter` (or [centre, point on it]);
  - `rect2` [corner, corner] and `rect_center` [centre, corner], axis-aligned; `rect3` [a, b, height point], turned;
  - `rounded_rect` [corner, corner] with `radius` (`center: true`: [centre, corner]);
  - `arc3` [start, middle, end]; `arc_radius` [start, end] with `radius`, `direction` ccw (default) or cw, `large`;
  - `slot` [centre, centre] with `width`; `cslot` [middle, end, width point]; `arcslot` [centre, start, end] with
    `width`;
  - `path` [points...]: `closed` (default true), `fillet` (radius at every corner between straight segments),
    `fillets` (one per point, 0 for none), `segments` (one per segment: "line", "tangent" = an arc tangent to the
    segment before, "tangent_next" = tangent to the one after, or a radius, + counter-clockwise, - clockwise);
  - `offset` with `shape` (index of an earlier shape in the list), `entity` or `entities`, `distance` (+ outward,
    - inward) and `round`;
  - `text` [baseline start] with `text`, `height` (mm), `style` "outline" (closed block letters to extrude) or
    "stroke" (single lines), `weight` (stroke width / height, default 0.14), `align` left, center or right. The
    built-in font has Latin capitals (lower case is drawn in capitals), digits and `- _ + / . , : ( ) ! ?`; it is
    the same on every machine;
  - also `polygon_outer` [centre, edge midpoint] with `sides` (every side touches its construction circle, whose diameter
    is the size across the flats), `circle2` [a, b], `conic`, `control_spline`,
    `tangent_arc` [line end, end] with `line`, `tangent_circle` [near point] with `lines` and `radius`.
- A boat hull section with straight sides, rounded stern corners and a pointed bow of two arcs tangent to the sides:
  `{"kind": "path", "picks": [[0,-12], [40,-12], [60,0], [40,12], [0,12]], "options": {"segments": ["line",
  "tangent", "tangent_next", "line", "line"], "fillets": [4, 0, 0, 0, 4]}}`.
- A frame outline and its inner offset: `[{"kind": "rounded_rect", "picks": [[0,0], [70,150]], "options":
  {"radius": 8}}, {"kind": "offset", "options": {"shape": 0, "distance": -2}}]`.
- Engraving: add a `text` shape to the part's sketch, extrude every region (`{"sketch": id}`) for the part, then the
  letters (`{"sketch": id, "at": p}` for each of the shape's `profiles`) with `operation: "cut"` and a
  `start_offset`.

## Profiles

- A profile input takes sketch regions or planar faces: `{"sketch": id, "at": [u, v]}` is the region containing that
  point; `{"sketch": id, "boundary": [21, -22]}` is the region bounded by those entities (signed ids, as
  `sketch_details` lists them; in any order, and without signs while that names one region), which keeps naming it
  when parameters move it; `{"sketch": id}` or `{"sketch": id, "all": true}` is every closed region of the sketch.
  `sketch_details` with `section: "profiles"` lists the regions with a point inside each and their boundaries. A
  boundary no region has is an error listing the ones there are.
- A planar body face (`"<body>/face/3"`) is a profile too.

## References

- Bodies: the body id, or `{"body": id, "kind": "body"}`; a component id stands for every body under it.
- Faces, edges, vertices: `"<body>/face/<n>"`, `"<body>/edge/<n>"`, `"<body>/vertex/<n>"` (zero-based ordinals), or
  `{"body": id, "kind": "face", "index": n}`. `"<body>/center/<n>"` is the centre of circular edge n. A free point is
  `{"kind": "point", "point": [x, y, z]}`.
- Ordinals change when a body's topology changes. Features store a `hint` with each reference (centre, size, entity
  counts): with the same counts the ordinal is trusted, otherwise the nearest matching entity is taken (the result's
  `rehinted` lists those, and the timeline marks the feature).
- Rules pick by what an entity is, not its number: `{"body": id, "kind": "edge", "select": {...}, "expect": 4}` is
  every edge of the body matching the `query_entities` filters in `select` (`curve`, `surface`, `radius_min`,
  `radius_max`, `parallel_to` "x"|"y"|"z" or [x,y,z], `normal` "+z".. or [x,y,z], `at_plane` {axis, value}, `bounds`
  {min, max}) when the feature is computed or regenerated. A different count than `expect` (or than one with
  `ambiguity: "unique"`) fails the feature instead of guessing. The result's `selected` records what each rule
  matched. Example: the four top edges of a box of height 20: `{"curve": "line", "at_plane": {"axis": "z", "value":
  20}}` with `"expect": 4`.
- Recognised details as a rule (faces only): `"select": {"recognized": "hole", "diameter": 6, "through": true}` is
  every face of the body's holes of that size; also `fillet` (`radius`), `chamfer` (`distance`), `wall`
  (`thickness`), `boss`, `pocket`; hole `type` simple|counterbore|countersink, `depth`, `cb_diameter`,
  `cs_diameter`. `query_entities` takes the same keys in `filters`.
- Live server: a face/edge/vertex input is accepted when the connection was given that reference (by
  `entity_details`, `query_entities`, the selection or a change list) and its body still has the same key and
  placement; otherwise put its token in the call's `references`. A token needs only `ref`, `geometry` and
  `placement` (`document` is the bound one, `signature` is only used by `resolve_reference` with `remap`). A body
  that changed since makes the reference stale: ask for it again.
- Axes: `{"base": "x"}`, `{"edge": ref}` (a straight edge's line or a circular edge's centre axis), `{"face": ref}`
  (a cylinder's, cone's or torus's axis, or a planar face's normal), `{"sketch": id, "entity": line id}` or
  `{"feature": construction axis id}`.
- The construction `plane` feature's `mode: "point_normal"` goes through `point` square to `normal` (any axis form).

## Feature conventions

- Primitives (box, cylinder, sphere, cone, torus, coil) sit on their `plane` at (`x`, `y`) in its frame and grow along
  its normal.
  - `box`: `length` along the plane's x, `width` along its y, `height` along the normal; a negative height grows the
    other way. `centered` (default true) centres the box in x and y only, never in height.
  - `cylinder` and `cone`: the axis is the plane normal, the base on the plane; a negative cylinder height grows the
    other way.
- `extrude`: `direction: "one"` pulls `distance` along the profile normal (`flip` reverses it); `"symmetric"` splits
  `distance` in half on each side; `"two"` uses `distance` and `distance2`. `extent: "all"` goes through everything; `"to_face"` (with
  `extent_face`) ends on that face, a planar one on its whole plane even when tilted; `"to_body"` (with
  `extent_body`) ends where the extrusion meets that body. Up-to extents go one way, and a profile that partly misses
  the target is refused.
  `start: "offset"` moves the start by `start_offset` along the sketch normal, so one sketch can feed features at
  several heights; `start: "face"` starts at a parallel planar face. `taper` tilts the sides.
- `revolve`: `axis` and `angle` (default 360 deg); `symmetric` splits the angle.
- Creation features take `operation: new | join | cut | intersect` and optional `targets`. Without targets, every body
  the new material touches takes part, and the ones that did are written back into the op. Named join targets take
  the material even where it does not touch them: a body may hold several separate solids (three screws, a word's
  letters). A cut that parts a body keeps the pieces in it; `split` makes separate bodies.
- Patterns: `count` includes the original, which stays where it is; `pattern_rect` steps `spacing` along `axis` (and
  optionally a second direction), `pattern_circ` spreads the copies evenly over `angle` about `axis`. With
  `operation: join` the copies join the targets instead of becoming bodies; `targets: [<the original>]` makes the
  original and its copies one body (without targets only bodies the copies touch take them).
- `combine`: `target` may list several bodies: `cut` and `intersect` work on each of them, `join` makes the first one
  body with the others and the tools.
- Construction `axis` in `mode: "two_points"` (and `point` in `mode: "normal"`) take vertices, sketch points
  `{sketch, point}` or points in space: `"point/x,y,z"`, `{"point": [x, y, z]}` or `[x, y, z]`.
- Sketch `patterns`: `{"id", "seeds": [entity ids], "inputs": {"count", "rows", "dx", "dy"}}` repeats the seed curves in
  rows and columns, `{"polar": true, "count", "angle", "cx", "cy"}` about a centre; inputs may be expressions (a
  parameter for the count). The copies are made when the sketch is computed; its `id` shares the sketch's id space.
- `move`: with `rotate: true` the bodies first turn by `angle` about `axis`, then move by `dx dy dz` in world axes;
  `copy: true` keeps the original.
- `feature` and `feature_edit` take `suppress_if`, an expression over the parameters: while it is true (nonzero) the
  feature is suppressed, and a parameter change that flips it regenerates (`"joint_type == 1"`, `"joints < 3"`);
  `feature_edit` with `suppress_if: ""` removes it. What it last made is kept for when it comes back.
- Locked bodies (`appearance` `locked: true`, everything under a locked component, a drawing layer locked in its
  file; `context` nodes report `locked`) are not changed, moved (`transform`, `reparent`) or removed: such a command is
  refused naming the body.
  They still serve as references and sources of copies, and automatic join/cut targets leave them out.

## New bodies: names, colours, components

- A body made from scratch is named after its feature (`name`); several are numbered "Board screw 1", "Board screw 2".
- Copies and pieces (patterns, mirror, move with copy, split) are named after their source ("Screw" -> "Screw 2") and
  go into its component with its colour.
- `feature` takes `body_name` (several bodies are numbered; `{n}` marks where the number goes), `color` `[r, g, b]` in
  0..1 and `parent` (a component id) for the bodies it makes, applied in the same step. A feature that makes no new
  body says so in `warnings`.
- `feature` and `sketch` take `component`: the component they are made in. New bodies (and a construction plane or
  axis) go into it and stay there when the feature regenerates; `parent` still moves the bodies afterwards. A sketch,
  plane or axis made in a component moves with it when the component moves later (`transform`, `reparent`), as its
  bodies do, and features made afterwards read it where it is now.
- `rename`, `appearance` and `reparent` take `targets` (a list) instead of `target`; `rename` then numbers the names.

## Batches

- `model_batch` runs up to 100 typed steps atomically. `@{<step id>#/<path>}` (or `@{<step id>/<path>}`) is a value
  from an earlier step's result; the path starts at that result: `@{cabin#/body_ids/0}`, `@{group#/component_id}`.
- `@{<step id>#/body_ids/*}` is the whole list wherever a list is accepted: as the list itself (`"targets":
  "@{row#/body_ids/*}"`) or spliced into one (`"targets": ["@{cabin#/body_ids/0}", "@{row#/body_ids/*}"]`).
- A reference may name a step of an earlier batch on the same connection, when this batch has no step with that id
  (an id this batch defines is always its own step, so a reference to a later one is an error).
- The batch's `parent` is the default component for every body its feature steps make; its `component` the default
  component its feature and sketch steps are made in.
- A failing step discards the whole batch and names the step; inputs and backward references are checked before
  anything runs.

## History and files

- Headless scripts can set `OPAD_DETERMINISTIC=<seed>` for `opad-cli` / `opad-cli mcp`: ids then follow from the seed,
  the command and the document's state, and timestamps are fixed, so the same script writes the same file (one seed
  per document).
- `undo` and `redo` (live) step the document's history as Edit > Undo does, one step at a time, whoever made it; they
  need `expected_revision` and `request_id`, and not during a transaction.
- `export` inside a transaction writes its staged state; outside, the document. STEP files are reproducible: the
  header carries the document's last change time and the file's name, so the same document exports the same bytes.
- Exploded views: `explode` lists the units (what moves together) and their moves at `t` (0 assembled, 1 exploded).
  `levels` is how deep components split (0 = all); `keep` holds a component together (a PCB), `split` spreads its
  parts beyond the level (screws), `groups` move node lists as one, small parts ride on what they touch; `mode`
  radial, axis or stack (along `axis`). `name` saves it as a view op, `view` + `update` saves into that view (an
  `edit`). `measure` and `render` take `explode` (a view id or the spec) and see the parts where they are drawn.
- `import` with `link: true` links the file instead of copying it (a bought part, a KiCad board, a picture to design
  around): the document records where it is and its hash, and reads it whenever it opens. Its parts are read-only:
  use them as references, sketch projections and tools (`combine` cut with `keep_tools: true`); a feature that would
  change, move, copy or consume one is refused ("embed the file first"), and automatic join/cut targets skip them.
  Status, sync (after the file changed), embed (an editable copy) and pack (a copy in `assets/`) are the `opad-cli
  asset` command.
- Pictures (`.png .jpg .bmp .gif .webp`) import as a flat canvas on XY at the picture's resolution (96 dpi unless the
  file says), or on `plane` at `width` mm (with `center: true` its centre on the plane's origin). The headless `canvas`
  command places it in its plane (`set`: `x`, `y` of its centre, `width` or `height` keeping its proportions, both to
  stretch it, `angle` in degrees), calibrates it (`points` [a, b] of it and their real `distance`), aligns it
  (`points` [a, a_to, b, b_to]: its points onto model points), sets its flags (`set`: `selectable`, `display_through`,
  `flip`), replaces its picture in place and turns a sketch's backdrop images into canvases (`from_backdrop`); `info`
  reports all of it. Opacity and lock are `appearance`.
- Which feature made a face: `entity_details` of a face or edge has `created_by` (`op`, `name`, `kind`, `category`:
  body, boss, pocket, hole, groove, fillet, chamfer, shell, draft, press_pull, pattern, mirror, combine, split,
  transform or imported). `related` with `refs` lists, for the picked faces/edges, each owning feature with every
  face it made on those bodies (`refs`, `count`), the one holding the whole selection first: "delete the boss" is a
  `delete` of that feature's `op`. Copies (pattern, mirror, move with copy) are the copying feature's, with `via`
  naming the source's feature; `merged` marks a face that a Boolean made one with an older coplanar face (it is
  listed under the older feature). Imported bodies have no history: their faces belong to the import.
- `related` also lists what the geometry shows, on any solid (imported ones too): `hole` (params diameter, depth,
  through, type, cb_/cs_diameter), `fillet` chains (radius, convex), `chamfer`, `boss`, `pocket`, `wall` (thickness),
  `tangent` chains, `loop`s of edges, and `similar` rules ("All holes Ø6 through"). Groups holding the picks come
  after a feature that made part of the body and before the one that made all of it. A body picked whole with
  `kinds` lists every group of those kinds. To delete a detail of an imported body, run the feature `remove_faces`
  with its faces (or a `recognized` rule): the faces around it are extended to close the gap.

## Git

- The git tools work on a repository: headless, the one `repo` names (a folder or file inside it); live, the bound
  document's (save it first; writes need edit access and a `request_id`). Read: `git_status`, `git_log`,
  `git_branches`, `git_diff` (line counts per file and OPAD's semantic diff of each `.opad`). Write: `git_init`,
  `git_fetch`, `git_branch_create`, `git_switch`, `git_commit`, `git_merge`, `git_merge_abort`, `git_resolve`,
  `git_pull`, `git_push`, `git_tag`. A refusal is `{error: {code, message, next}}`: read the message, it says what to do.
- Protected branches are the user's (OPAD Preferences > Version control > Branch protection; default `main` and
  `master`): commits to them and merges into them are refused (`protected_branch`), pushes of them when the user set
  that. Do not ask to get around it: `git_branch_create` a branch of your own (it switches to it), commit there, push
  it, and tell the user it is ready to merge. A fast-forward `git_pull` of a protected branch is allowed.
- `git_commit` stages exactly `paths` (or everything with `all`); it refuses an empty commit. `git_switch`,
  `git_merge` and `git_pull` refuse uncommitted changes rather than carry or discard them: commit first.
- Merges of `.opad` files go through OPAD's record-aware driver. Preview first (`preview: true`): it lists what
  comes in and the conflicts the driver would stop on. A stopped merge (`state: conflicts`): `git_resolve` the file
  without `keep` or `choices` to list its conflicts, then decide them (`keep` ours|theirs, or `choices` per index),
  then `git_commit`; or `git_merge_abort`. When both sides changed the design (`regenerate_after`), run `regenerate`.
- Not offered: force push, reset, rebase, deleting branches or tags, clean, stash drop, discarding changes, history
  rewrites. If one is truly needed, it can be done with the git CLI, but only after asking the user and getting
  explicit confirmation, and only when needed.

## Checking the result

- `sketch` results, and results of features that take a `plane`, include `frame`: the origin, x, y and normal the
  plane resolved to.
- Live write results include `changes.bodies`: for each body that command made, changed or moved, its tight
  `bbox` (min, max, size), `volume_mm3` and `valid`, so no `validate` round trip is needed after each step.

- `validate` checks solids and reports volumes; `context` summarises the document (sections `nodes`, `features`,
  `sketches`, `parameters`, `errors`); `entity_details` and `query_entities` describe faces and edges with their
  reference tokens; `viewport_image` (live) and `render` return pictures.
- Sizes (`bbox` in results, properties, validate, measurements) are tight boxes of the exact geometry (a turned body
  in a union of many: the turned corners of its own tight box).
- Volumes, areas and centres of mass (properties, validate, `changes.bodies`, `entity_details`) are integrated span
  by span of every spline, so a profile made of a long spline reads its true area.
- `measure` is a read: no `expected_revision`, `request_id` or revision spent, unless `pin: true` (then it appends
  a measurement op like any write). `queries: [{kind, refs}, ...]` measures several at once and answers `results`
  in order (a failed one carries `error`). A distance is between the surfaces of what it names: a body inside
  another reports the gap between their surfaces (overlaps are `validate` with `checks: ["interference"]`).
  `approximate: true` with `tolerance_mm` means the exact search failed and the meshes' distance is given;
  `warnings` names a shape the kernel does not hold as valid when a distance comes out as 0.
- `validate` with `checks: ["interference"]` lists pairs of bodies that overlap (volume, box) and, with
  `clearance_mm`, pairs closer than that (`ignore` lists pairs meant to overlap); `checks: ["print"]` reports, per
  body, faces overhanging more than `overhang_deg` (default 45, from vertical) against `build_direction` ("+z"),
  the build-plate contact area, walls thinner than `min_wall_mm` (0.8) and thin features. Add "solid" for the usual
  validity page.
- The `interference` feature keeps such a check in the timeline: `bodies` (all solids so far if none), `clearance`,
  `fail_on` (`nothing`, `interference` or `clearance`). Its result's `check` holds the report (status, pairs, overlap
  volumes, distances) and is computed again whenever those bodies change; with `fail_on`, a finding is the
  feature's error, so the edit that causes it reports it (and a live edit is refused).
- Write tools take `verbosity: "compact"`: `changes` then lists only that command's created, modified and deleted
  ids with counts (not the transaction's cumulative lists), references come without signatures, and model_batch
  drops its batch-wide `operation_ids` (each step receipt has its own). The default stays "full".
- Pictures: `views: ["iso", "front", "top", "right"]` gives one labelled grid (each view fitted; `views` in the result
  lists every cell's camera); `edges: true` (viewport_image) or `edge_lines: true` (render; there `edges` is the
  silhouette outline, on by default) draws the model's edges; `highlight: [refs]` tints faces and edges orange;
  `shading: "smooth"` shades curved surfaces smoothly. All are off by default.

## Drawings

Drawings are file-level: the commands below are in the CLI, Python and the headless MCP server, not in the live tool
list (a live agent saves, and the drawing is made on the file).

- `sheet` adds a drawing sheet: paper `size` (A4 to A0, ANSI-A to ANSI-E; or `width` and `height` in mm),
  `orientation`, `standard` iso|asme, `projection` first|third angle (default by standard), the views' `scale`
  ("1:2") and title block `values`. Paper coordinates are mm from the sheet's bottom-left corner, y up.
- `sheet_view` adds a base view (`orient` front, top, right, iso, ... or `dir`/`up`; `select` nodes, default all;
  `explode` a saved exploded view's op id: its parts drawn apart with trail lines, from its camera unless orient/dir;
  `at` its centre on the paper; `scale` "sheet", "1:5" or "auto") or one projected from `parent` (`side` left, right,
  top, bottom or a corner such as top-right; `gap` mm between the frames, default 20). First angle: the view right of
  the front view shows the left side and the one below it the top; third angle the other way round. Projected views
  stay aligned with their parent and take its scale. The result's `frame` is the view on the paper: `box`, `at`,
  `scale`, and its `x`, `y` and `dir` in world coordinates.
- Views of a `parent` view (`kind`): `section` takes `cut`, the cutting line as points in the parent's view
  coordinates (model mm, the parent frame's `x`/`y`: `frame.centre` is the view's middle); two points a full section,
  more an offset or half section, or an aligned one (`aligned`, set by itself when a segment is inclined to the first:
  each segment revolved onto the first one's line). It is placed on the line's left (`flip: true`: its right) and seen
  from the other side in first angle; cut faces are hatched (ISO 128-50, automatic; `hatch` {pattern general|material|steel|copper|
  aluminium|plastic|insulation|glass, angle, spacing, thin fill|hatch, bodies {node: {...}}} to choose); `whole` lists
  nodes left uncut. The part property `section: false` (part_properties) leaves a body, or everything under a component,
  uncut in every section and broken-out section (shafts, pins, keys, fasteners); a view's `sectioned` lists nodes it
  cuts anyway. `detail` takes `center` and `radius` (view coordinates) and its own `scale`; `auxiliary` an
  `angle` (degrees on the sheet from the parent: it looks along that line, e.g. square to a slanted edge). Letters come
  automatically (A, B, ...; `letter` to choose). Any view takes `crop` [x0, y0, x1, y1] and `breaks` [{axis x|y, from,
  to, gap}] in its view coordinates (sheet_edit adds them later; `style` {break: freehand} draws their break lines as
  waves instead of zigzags). A base, projected or auxiliary view takes `breakouts` [{outline [[u, v], ...], depth}]
  through sheet_edit: broken-out (local) sections, where within the smooth closed curve through the outline's points
  whatever lies nearer the viewer than `depth` (along the frame's `dir`, model mm: `p . dir`) is taken away, the floor
  hatched and the cut's edge drawn as a thin break line.
- `sheet_item` adds a dimension measured in its `view` (`type` horizontal, vertical, aligned, radius, diameter or
  angle; `refs` two vertices or edges, one edge for its length, or one circle or cylinder; `aspects` start, end, mid
  or center per reference) or a note (`text`, `at`). A circle seen at a slant, or an angle between edges not parallel
  to the view, is refused: dimension it in a view along its axis. On a section's outline the cut's edges are no
  edges of the model: reference the faces they lie on (`project` gives them as the curves' `face`): a flat face seen
  edge on measures as its line, a cylinder seen from the side as its diameter. A dimension keeps the value it was made
  with (`result`); `sheet_info` gives the value now (`current`, `changed`), so model edits show as changed dimensions.
- More `sheet_item` kinds, each measured in its `view` from `refs` and kept with its `result`: `centermark` (a circle
  or its cylinder) and `centerline` (a cylinder from the side, two circles or two parallel lines; `extend` mm);
  `hole_callout` (a hole's wall or circle: diameter, depth or THRU, counterbore, countersink, drill point,
  "4×" for equal holes; `holes` lists a body's holes as recognised); `hole_table` (`at`: the view's holes tagged A1,
  A2, ... with their positions from the view's origin); `datum` (`letter` A-Z on an edge or face), `fcf` (feature
  control frame: `characteristic` position, flatness, perpendicularity, ..., `value`, `zone` diameter, `material` M|L|S,
  `datums` up to three letters such as "B(M)"; `place` alone for a frame without a leader) and `surface` (texture:
  `value` such as "Ra 1.6", `process` any|removal|no_removal); `dimension_set` (`type` ordinate|baseline|chain, `refs`
  the origin first then the features, `axis` horizontal|vertical). Dimensions take `precision` and `tolerance` {type
  sym|dev|limits|fit, plus, minus, fit: "H7"}. `sheet_datum_dimensions` dimensions a view's features (default its
  holes) from its datum symbols in one step (`type`, `datums`, `refs`).
- Parts lists and balloons: `sheet_item` `parts_list` (`bom` {mode top|parts, root}, `columns` item, qty, name,
  part_number, description, material, mass, vendor; `at`, `width`) numbers its rows 1, 2, ... and keeps them settled
  in `numbers`, so a row keeps its number while parts come and go (`sheet_edit` `renumber: true` numbers them again);
  `balloon` (`refs` one face, edge or vertex of a part, `list`, `qty`) shows its part's row number; `sheet_balloons`
  balloons the parts a view shows that have none yet in one step (`view`; `all` every row again; `qty`), each beside
  the view on the side whose leader reaches an edge of its part that the view shows across the fewest other
  lines (another part's least of all; in an exploded view the parts where it draws them), inside the frame and off
  the title block, other views, tables and balloons, spread so none overlap,
  creating the parts list when the drawing has none. `revision_table` lists the drawing's issues.
- `sheet_issue` issues the next revision of a sheet's drawing (`rev`, default the next letter, I O Q S X Z skipped;
  `description`, `approved`, `date`): the record keeps every item's value and the views' fingerprints and frames,
  `freeze` (default true) stores the views' linework as body entries, so the revision draws as issued even after the
  model changes (`export` with `issue` writes it as issued); `out` writes its PDF as it then shows (the revision in
  the title block and revision table) and records the file's SHA-256. `tag` only records a tag name: the app's Issue
  revision… commits and tags in git, the command does not. `sheet_info` lists each issue with what changed since.
- In the app, Print… (Ctrl+Alt+P) prints the drawing's sheets at actual size or fitted, in black ink or colour.
- `sheet_edit` changes a sheet, view or item (`set`; null removes a field); moving a base view moves the views
  projected from it. A base view's `explode` (a saved exploded view's op id; null draws it assembled) keeps its side
  and draws the parts apart with trail lines; the views taken from it follow, and an update of that exploded view
  (`explode --view <id> --update`) moves the drawn parts with it. `delete` removes one; a deleted sheet takes its views
  and items with it.
- `part_properties` sets part properties on bodies or components (`part_number`, `description`, `material`, `density`
  g/cm3, `mass` g, `vendor`, `notes`, `bom` include|exclude|purchased, any other field); `properties` reports them as
  `part`, plus the `material` in force (the nearest one set upwards, else the file's), `density` and `mass` (g).
  `materials` lists the library ids; `materials --match <name>` maps a name; `appearance: true` colours as the material.
- `bom` lists parts with quantities (`mode` parts, top or indented; `root` a component), part properties and masses
  (`mass_error` says what is missing); identical parts are one row (same part number, or same solid and material).
  `format` csv gives spreadsheet text (`out` a file).
