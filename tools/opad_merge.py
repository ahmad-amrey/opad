"""Conservative Git merge driver for append-only OPAD documents.

Usage: python tools/opad_merge.py BASE OURS THEIRS
Only OURS is written, and only after a complete successful merge. Conflicts
return nonzero, preserving the user's version for normal Git resolution.
"""
import hashlib
import json
from pathlib import Path
import sys


def read(path):
    text = Path(path).read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    if len(lines) < 4 or lines[0].strip() not in ("#opad 1", "#opad 2") or lines[2].strip() != "#ops":
        raise ValueError("unsupported OPAD header")
    header = json.loads(lines[1])
    decoder = json.JSONDecoder()
    ops, bodies = {}, {}
    index = 3
    while index < len(lines) and lines[index].strip() != "#bodies":
        if not lines[index].strip() or lines[index].startswith("#"):
            # Unknown metadata must never be silently discarded by a merge.
            raise ValueError("unexpected metadata in operation log")
        start = index
        depth, quoted, escaped = 0, False, False
        while index < len(lines):
            for char in lines[index]:
                if escaped:
                    escaped = False
                elif quoted and char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = not quoted
                elif not quoted:
                    depth += (char in "{[") - (char in "}]")
            index += 1
            if depth <= 0:
                break
        raw = "".join(lines[start:index]).rstrip("\n")
        op = decoder.decode(raw)
        if not isinstance(op, dict) or not isinstance(op.get("id"), str):
            raise ValueError("operation requires an ID")
        key = op["id"]
        if key in ops and ops[key][0] != op:
            raise ValueError(f"conflicting operation ID {key}")
        ops.setdefault(key, (op, raw))
    if index >= len(lines):
        raise ValueError("missing body store")
    index += 1
    while index < len(lines):
        if not lines[index].strip():
            index += 1
            continue
        fields = lines[index].rstrip("\n").split(" ", 3)
        if len(fields) != 4 or fields[0] != "#body":
            raise ValueError("invalid body header")
        key, count = fields[1], int(fields[2])
        if count < 1 or count > len(lines) - index - 1:
            raise ValueError("invalid body length")
        json.loads(fields[3])
        raw = "".join(lines[index:index + count + 1])
        geometry = "".join(lines[index + 1:index + count + 1])
        if hashlib.sha256(geometry.encode("utf-8")).hexdigest() != key:
            raise ValueError("body hash mismatch")
        bodies.setdefault(key, raw)
        index += count + 1
    return lines[:3], header, ops, bodies


def effects(op):
    """Conservative conflict keys. Geometry regen conflicts need CAD review."""
    kind, target = op.get("op"), op.get("target")
    if kind == "regen":
        return {(key, "geometry") for key in op.get("results", {})}
    if kind == "param":
        return {("parameter:" + op.get("name", ""), "*")}
    if kind == "edit":
        return {(target, "geometry" if key in ("geometry_delta", "geometry", "result", "inputs", "plane") else key)
                for key in op.get("set", {})}
    if kind == "properties":
        # Part properties are set key by key: two people setting different ones on one part do not collide.
        return {(target, "property:" + key) for key in op.get("set", {})}
    if kind == "delete":
        return {(target, "*")}
    if target:
        return {(target, key) for key in op if key not in ("op", "id", "ts", "by", "target")}
    # Records without a target (sheets, their views and items, notes, sketches) never collide; edits of one do, by field.
    return set()


def merge(base_path, ours_path, theirs_path):
    base, ours, theirs = [read(path) for path in (base_path, ours_path, theirs_path)]
    if base[1] != ours[1] or base[1] != theirs[1]:
        raise ValueError("document header changed; manual review required")
    for version in (ours, theirs):
        for key, value in base[2].items():
            if key not in version[2] or value[0] != version[2][key][0]:
                raise ValueError("history was rewritten; manual review required")
        if not base[3].keys() <= version[3].keys():
            raise ValueError("body store was pruned; manual review required")
        # Preserving operation order is essential for regeneration and tombstones.
        if [key for key in version[2] if key in base[2]] != list(base[2]):
            raise ValueError("operation history was reordered")
    left = {key: value for key, value in ours[2].items() if key not in base[2]}
    right = {key: value for key, value in theirs[2].items() if key not in base[2]}
    for key in left.keys() & right.keys():
        if left[key][0] != right[key][0]:
            raise ValueError(f"conflicting operation ID {key}")
    left_effects = set().union(*(effects(value[0]) for key, value in left.items() if key not in right))
    right_effects = set().union(*(effects(value[0]) for key, value in right.items() if key not in left))
    for target, field in left_effects:
        for other, other_field in right_effects:
            if target == other and (field == other_field or "*" in (field, other_field)):
                raise ValueError(f"concurrent changes to {target}/{field}; manual review required")
    ops = dict(ours[2])
    for key, value in theirs[2].items():
        if key in ops and ops[key][0] != value[0]:
            raise ValueError(f"conflicting operation ID {key}")
        ops.setdefault(key, value)
    if [key for key in ops if key in theirs[2]] != list(theirs[2]):
        raise ValueError("branch operation order conflicts; manual review required")
    bodies = dict(ours[3])
    for key, value in theirs[3].items():
        bodies.setdefault(key, value)
    version = max(int(item[0][0].split()[1]) for item in (base, ours, theirs))
    header_lines = [f"#opad {version}\n", *ours[0][1:]]
    return "".join(header_lines) + "".join(raw + "\n" for _, raw in ops.values()) + "#bodies\n" + "".join(bodies.values())


def main():
    try:
        if len(sys.argv) != 4:
            raise ValueError(__doc__)
        result = merge(*sys.argv[1:])
        Path(sys.argv[2]).write_text(result, encoding="utf-8", newline="\n")
        return 0
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f"OPAD merge conflict: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
