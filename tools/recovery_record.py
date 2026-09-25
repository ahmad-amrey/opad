"""Read OPAD recovery artifacts in integration tests; never modifies the source."""
import hashlib
import json
from pathlib import Path


def document_text(path):
    path = Path(path)
    record = json.loads(path.read_text(encoding="utf-8"))
    if record["format"] == 1:
        return record["document"]
    delta = record["delta"]
    name = delta["base"]
    assert Path(name).name == name and "/" not in name and "\\" not in name
    base = (path.parent / name).read_bytes()
    assert hashlib.sha256(base).hexdigest() == delta["base_sha256"]
    text, bodies = base.decode("utf-8").split("#bodies\n", 1)
    header, operations = text.split("#ops\n", 1)
    decoder = json.JSONDecoder()
    ops = []
    while operations.strip():
        operations = operations.lstrip()
        op, end = decoder.raw_decode(operations)
        ops.append(op)
        operations = operations[end:]
    assert delta["keep_ops"] <= len(ops)
    ops = ops[:delta["keep_ops"]] + delta["ops"]
    for body in delta["bodies"]:
        brep = body["brep"]
        assert hashlib.sha256(brep.encode()).hexdigest() == body["key"]
        bodies += f'#body {body["key"]} {brep.count(chr(10))} {json.dumps(body["meta"])}\n{brep}'
    return header + "#ops\n" + "".join(json.dumps(op) + "\n" for op in ops) + "#bodies\n" + bodies
