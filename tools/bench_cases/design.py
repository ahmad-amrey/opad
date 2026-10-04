"""gui_benches cases of the Design workspace's tools as the help guides show them (TODO 11 wave 3): clicks in the view reach the
input the guide's animation clicks (app/PickRoutingBench.cpp)."""


def routing_parts(root, document):
    """A box away from the origin (x -55..-25), a cylinder on the other side (x 32..48) and a sketch "Ring" on XZ
    (x 15..22, z 0..10) to revolve about Z: the origin planes and axes stay clear of the bodies."""
    path = root / "routing-parts.opad"
    if path.exists():
        return path
    return document("routing-parts", ("feature", "--kind", "box", "--inputs", '{"x":"-40 mm","length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "cylinder", "--inputs", '{"x":"40 mm","diameter":"16 mm","height":"20 mm"}'),
                    ("sketch", "--name", "Ring", "--plane", '{"base":"xz"}', "--geometry", '{"shapes":[{"kind":"rect2","picks":[[15,0],[22,10]]}]}'))


CASES = [
    # P3: the region then the Z axis (revolve), the body then the YZ plane (mirror), target then tool (combine), a face then
    # the XY plane and the Neutral plane box not taking that face (draft), axes for a circular pattern, a round face for a
    # construction axis, a face and an origin plane for a construction plane, the XY plane and Enter for a new sketch.
    ("pick-routing", routing_parts, {"OPAD_BENCH_PICKROUTING": "{prefix}"}),
]
