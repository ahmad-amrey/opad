# OPAD agent guide

What an agent needs to model in OPAD and cannot read from the tool schemas. The same text is served by both MCP
servers as the resource `opad://guide/agent` and by `live_diagnostics` with `include_guide: true`.

## Units and expressions

- Geometry is in millimetres and degrees. A plain number is mm for a length input, degrees for an angle input and a
  plain number for a count.
- A string is an expression: `"20 mm"`, `"width / 2"`, `"2 * pi * r"`, `"30 deg"`, `"count - 1"`. Units: `mm cm m
  um in ft deg rad`. Functions: `sin cos tan asin acos atan atan2 sqrt abs min max floor ceil round pow exp ln log
  hypot sign`. Constants: `pi PI E`.
- Values carry their dimension: `10 mm * 2 mm` is an area and is refused where a length is asked for. Write units
  explicitly; inside `sin()` a plain number is radians, so write `sin(30 deg)`.
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
- Point: `{"id": 1, "x": 0, "y": 0}` in sketch mm.
- Entities, `p` lists point ids:
  - line `[start, end]`;
  - circle `[centre]` plus `r`;
  - arc `[centre, start, end]`, counter-clockwise from start to end, both ends at the same radius (the solver keeps
    them there); for a clockwise arc swap start and end;
  - ellipse `[centre, end of the major axis]` plus `r` = the minor radius;
  - spline: `p` are fit points, interpolated in order; with `degree` (plus `knots`, `multiplicities`, `weights`) `p`
    are the control poles of an exact B-spline;
  - `construction: true` for guide geometry that is never part of a profile.
- Curves share points by id: two lines meeting at point 3 are joined; no coincident constraint is needed.
- Constraints, `refs` lists ids: `coincident horizontal vertical parallel perpendicular collinear tangent equal
  concentric midpoint symmetric fix smooth curvature`; dimensions `distance hdistance vdistance radius diameter angle
  arc_length` take `value` (mm, or radians for angles) and optionally `expr` (`"width / 2"`), which then drives it.
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
  - also `polygon_outer` [centre, edge midpoint] with `sides`, `circle2` [a, b], `conic`, `control_spline`,
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
  point; `{"sketch": id}` or `{"sketch": id, "all": true}` is every closed region of the sketch. `sketch_details`
  with `section: "profiles"` lists the regions with a point inside each.
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
  `operation: join` the copies join the targets instead of becoming bodies.
- `move`: with `rotate: true` the bodies first turn by `angle` about `axis`, then move by `dx dy dz` in world axes;
  `copy: true` keeps the original.

## New bodies: names, colours, components

- A body made from scratch is named after its feature (`name`); several are numbered "Board screw 1", "Board screw 2".
- Copies and pieces (patterns, mirror, move with copy, split) are named after their source ("Screw" -> "Screw 2") and
  go into its component with its colour.
- `feature` takes `body_name` (several bodies are numbered; `{n}` marks where the number goes), `color` `[r, g, b]` in
  0..1 and `parent` (a component id) for the bodies it makes, applied in the same step. A feature that makes no new
  body says so in `warnings`.
- `rename`, `appearance` and `reparent` take `targets` (a list) instead of `target`; `rename` then numbers the names.

## Batches

- `model_batch` runs up to 50 typed steps atomically. `@{<step id>#/<path>}` (or `@{<step id>/<path>}`) is a value
  from an earlier step's result; the path starts at that result: `@{cabin#/body_ids/0}`, `@{group#/component_id}`.
- `@{<step id>#/body_ids/*}` is the whole list wherever a list is accepted: as the list itself (`"targets":
  "@{row#/body_ids/*}"`) or spliced into one (`"targets": ["@{cabin#/body_ids/0}", "@{row#/body_ids/*}"]`).
- The batch's `parent` is the default component for every body its feature steps make.
- A failing step discards the whole batch and names the step; inputs and backward references are checked before
  anything runs.

## Checking the result

- `sketch` results, and results of features that take a `plane`, include `frame`: the origin, x, y and normal the
  plane resolved to.
- Live write results include `changes.bodies`: for each body that command made, changed or moved, its tight
  `bbox` (min, max, size), `volume_mm3` and `valid`, so no `validate` round trip is needed after each step.

- `validate` checks solids and reports volumes; `context` summarises the document (sections `nodes`, `features`,
  `sketches`, `parameters`, `errors`); `entity_details` and `query_entities` describe faces and edges with their
  reference tokens; `viewport_image` (live) and `render` return pictures.
- Pictures: `views: ["iso", "front", "top", "right"]` gives one labelled grid (each view fitted; `views` in the result
  lists every cell's camera); `edges: true` (viewport_image) or `edge_lines: true` (render; there `edges` is the
  silhouette outline, on by default) draws the model's edges; `highlight: [refs]` tints faces and edges orange;
  `shading: "smooth"` shades curved surfaces smoothly. All are off by default.
