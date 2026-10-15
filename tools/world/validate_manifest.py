#!/usr/bin/env python
"""Dependency-free validator and Unity dry run for a castlemist world export.

    python -I tools/world/validate_manifest.py <exportDir> [--schema <path>] [--plan] [--self-test]

Exit 0 when valid (prints "OK ..."), 1 with one "ERROR <path>: <message>" line
per problem. Standard library only. The schema checker supports exactly:
$schema $id $defs $ref title description type properties required
additionalProperties items enum const minimum maximum minItems maxItems
(type may be an array). Any other schema keyword is "ERROR schema: unsupported
keyword <k>". If jsonschema happens to be importable it is run as a cross-check.
"""
import json
import math
import os
import shutil
import sys
import tempfile

KEYWORDS = {"$schema", "$id", "$defs", "$ref", "title", "description", "type", "properties", "required",
            "additionalProperties", "items", "enum", "const", "minimum", "maximum", "minItems", "maxItems"}
TYPES = {"object", "array", "string", "number", "integer", "boolean", "null"}


# ---------------------------------------------------------------- schema check
def _is_type(v, t):
    if t == "null":
        return v is None
    if t == "boolean":
        return isinstance(v, bool)
    if t == "string":
        return isinstance(v, str)
    if t == "array":
        return isinstance(v, list)
    if t == "object":
        return isinstance(v, dict)
    if t == "integer":
        return (isinstance(v, int) and not isinstance(v, bool)) or (
            isinstance(v, float) and math.isfinite(v) and v.is_integer())
    if t == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    return False


def _jeq(a, b):
    """JSON equality: bool never equals a number."""
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_jeq(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_jeq(a[k], b[k]) for k in a)
    return type(a) is type(b) and a == b


def schema_keywords(schema, errors):
    """Report every keyword outside KEYWORDS (and unknown type names)."""
    if not isinstance(schema, dict):
        return
    for k, v in schema.items():
        if k not in KEYWORDS:
            e = "ERROR schema: unsupported keyword %s" % k
            if e not in errors:
                errors.append(e)
            continue
        if k in ("properties", "$defs") and isinstance(v, dict):
            for sub in v.values():
                schema_keywords(sub, errors)
        elif k in ("items", "additionalProperties") and isinstance(v, dict):
            schema_keywords(v, errors)
        elif k == "type":
            for t in (v if isinstance(v, list) else [v]):
                if t not in TYPES:
                    errors.append("ERROR schema: unsupported type %r" % (t,))


def _resolve(ref, root):
    if not ref.startswith("#"):
        raise ValueError("unsupported $ref " + ref)
    node = root
    for part in [p for p in ref[1:].split("/") if p]:
        node = node[part.replace("~1", "/").replace("~0", "~")]
    return node


def _join(path, key):
    if isinstance(key, int):
        return "%s[%d]" % (path, key)
    return key if not path else "%s.%s" % (path, key)


def _tname(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "boolean"
    if isinstance(v, int):
        return "integer"
    if isinstance(v, float):
        return "number"
    return {str: "string", list: "array", dict: "object"}.get(type(v), type(v).__name__)


def check(instance, schema, path="", root=None, out=None):
    """Validate `instance` against `schema`; returns a list of ERROR lines."""
    if root is None:
        root = schema
    if out is None:
        out = []

    def err(msg):
        out.append("ERROR %s: %s" % (path or "manifest", msg))

    if "$ref" in schema:
        check(instance, _resolve(schema["$ref"], root), path, root, out)
    if "type" in schema:
        ts = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_is_type(instance, t) for t in ts):
            err("expected type %s, got %s" % ("|".join(ts), _tname(instance)))
            return out
    if "const" in schema and not _jeq(instance, schema["const"]):
        err("must equal %s" % json.dumps(schema["const"]))
    if "enum" in schema and not any(_jeq(instance, e) for e in schema["enum"]):
        err("must be one of %s" % json.dumps(schema["enum"]))
    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            err("%s is below minimum %s" % (instance, schema["minimum"]))
        if "maximum" in schema and instance > schema["maximum"]:
            err("%s is above maximum %s" % (instance, schema["maximum"]))
    if isinstance(instance, dict):
        for r in schema.get("required", []):
            if r not in instance:
                err("missing required property: %s" % _join(path, r))
        props = schema.get("properties", {})
        ap = schema.get("additionalProperties", True)
        for k, v in instance.items():
            if k in props:
                check(v, props[k], _join(path, k), root, out)
            elif ap is False:
                err("unexpected property: %s" % _join(path, k))
            elif isinstance(ap, dict):
                check(v, ap, _join(path, k), root, out)
    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            err("has %d items, minimum %d" % (len(instance), schema["minItems"]))
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            err("has %d items, maximum %d" % (len(instance), schema["maxItems"]))
        if isinstance(schema.get("items"), dict):
            for i, v in enumerate(instance):
                check(v, schema["items"], _join(path, i), root, out)
    return out


# ---------------------------------------------------------------- semantics
def _finite_walk(v, path, errors):
    if isinstance(v, float) and not math.isfinite(v):
        errors.append("ERROR %s: non-finite number" % (path or "manifest"))
    elif isinstance(v, list):
        for i, x in enumerate(v):
            _finite_walk(x, _join(path, i), errors)
    elif isinstance(v, dict):
        for k, x in v.items():
            _finite_walk(x, _join(path, k), errors)


def _mat_mul3(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


CU = [[1, 0, 0], [0, 0, -1], [0, 1, 0]]  # map_to_unity: (x, y, z) -> (x, -z, y)
CU_T = [[CU[j][i] for j in range(3)] for i in range(3)]


def _quat_matrix(q):
    n = sum(c * c for c in q)
    if n == 0:
        return None
    x, y, z, w = [c / math.sqrt(n) for c in q]
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def _exists(root, rel):
    return os.path.exists(os.path.join(root, rel))


def semantic(m, root):
    errors = []
    _finite_walk(m, "", errors)

    def need(rel, p):
        if isinstance(rel, str) and not _exists(root, rel):
            errors.append("ERROR %s: missing file: %s" % (p, rel))

    models = m.get("models") or []
    for i, mod in enumerate(models):
        for k in ("folder", "glb", "fbx", "materials"):
            if mod.get(k) is not None:
                need(mod[k], "models[%d].%s" % (i, k))
    tex = m.get("textures")
    if isinstance(tex, dict) and isinstance(tex.get("dir"), str):
        d = os.path.join(root, tex["dir"])
        if not os.path.isdir(d):
            errors.append("ERROR textures.dir: missing file: %s" % tex["dir"])
        elif isinstance(tex.get("count"), int):
            n = len([f for f in os.listdir(d) if os.path.isfile(os.path.join(d, f))])
            if n != tex["count"]:
                errors.append("ERROR textures.count: %d declared, %d files in %s" % (tex["count"], n, tex["dir"]))
    sky = m.get("sky")
    if isinstance(sky, dict):
        for k in ("json", "default", "equirect"):
            if sky.get(k) is not None:
                need(sky[k], "sky." + k)
    fp = m.get("frameProbe")
    if isinstance(fp, dict):
        for k in ("glb", "fbx"):
            if fp.get(k) is not None:
                need(fp[k], "frameProbe." + k)

    cells = m.get("cells") or []
    inst_total = 0
    for ci, c in enumerate(cells):
        p = "cells[%d]" % ci
        for k in ("glb", "fbx", "albedo"):
            if c.get(k) is not None:
                need(c[k], p + "." + k)
        w = c.get("water")
        if isinstance(w, dict) and w.get("texture") is not None:
            need(w["texture"], p + ".water.texture")
        origin = c.get("origin")
        for k, inst in enumerate(c.get("instances") or []):
            ip = "%s.instances[%d]" % (p, k)
            inst_total += 1
            mi = inst.get("model")
            if isinstance(mi, int) and not (0 <= mi < len(models)):
                errors.append("ERROR %s.model: index out of range: cells[%d].instances[%d].model (%d, models has %d)"
                              % (ip, ci, k, mi, len(models)))
            _check_unity(inst, origin, ip, errors)

    # chunk rects tile the terrain region
    terr = m.get("terrain") or {}
    rects = [(ci, c["chunks"]) for ci, c in enumerate(cells)
             if isinstance(c.get("chunks"), list) and len(c["chunks"]) == 4]
    if terr.get("present") and isinstance(terr.get("region"), list) and len(terr["region"]) == 4 and rects:
        x0, y0, x1, y1 = terr["region"]
        owner = {}
        for ci, r in rects:
            for cx in range(r[0], r[2] + 1):
                for cy in range(r[1], r[3] + 1):
                    if (cx, cy) in owner:
                        errors.append("ERROR cells[%d].chunks: overlaps cells[%d] at chunk (%d, %d)"
                                      % (ci, owner[(cx, cy)], cx, cy))
                    elif not (x0 <= cx <= x1 and y0 <= cy <= y1):
                        errors.append("ERROR cells[%d].chunks: chunk (%d, %d) outside terrain region" % (ci, cx, cy))
                    owner[(cx, cy)] = ci
        for cx in range(x0, x1 + 1):
            for cy in range(y0, y1 + 1):
                if (cx, cy) not in owner:
                    errors.append("ERROR cells: chunk (%d, %d) of the terrain region is in no cell" % (cx, cy))

    cnt = m.get("counts") or {}
    try:
        if cnt["propsWritten"] + cnt["propsSkipped"] != cnt["props"]:
            errors.append("ERROR counts: propsWritten + propsSkipped != props (%d + %d != %d)"
                          % (cnt["propsWritten"], cnt["propsSkipped"], cnt["props"]))
        if inst_total != cnt["propsWritten"]:
            errors.append("ERROR counts.propsWritten: %d but cells hold %d instances" % (cnt["propsWritten"], inst_total))
    except (KeyError, TypeError):
        pass
    return errors


def _check_unity(inst, origin, p, errors):
    """unity must equal C_u (m - origin) C_u^-1, as t, r (xyzw), s, within 1e-4."""
    try:
        mm, u = inst["m"], inst["unity"]
        if len(mm) != 16 or len(origin) != 3:
            return
        a = [[float(mm[col * 4 + row]) for col in range(3)] for row in range(3)]
        t = [float(mm[12 + i]) - float(origin[i]) for i in range(3)]
        want_a = _mat_mul3(_mat_mul3(CU, a), CU_T)
        want_t = [sum(CU[i][j] * t[j] for j in range(3)) for i in range(3)]
        got_t, s = u["t"], float(u["s"])
        qm = _quat_matrix(u["r"])
        if qm is None:
            errors.append("ERROR %s.unity: unity transform mismatch (zero quaternion)" % p)
            return
        tol = 1e-4
        bad = any(abs(got_t[i] - want_t[i]) > tol * max(1.0, abs(want_t[i])) for i in range(3))
        for i in range(3):
            for j in range(3):
                if abs(s * qm[i][j] - want_a[i][j]) > tol * max(1.0, abs(want_a[i][j])):
                    bad = True
        if bad:
            errors.append("ERROR %s.unity: unity transform mismatch" % p)
    except (KeyError, TypeError, ValueError, IndexError):
        pass  # shape problems are reported by the schema check


# ---------------------------------------------------------------- plan
def plan(m, root):
    """Returns (PLAN line, errors): what an importer would build."""
    errors = []
    models = m.get("models") or []
    ok = [mod for mod in models if mod.get("status") == "ok"]
    mats = 0
    for i, mod in enumerate(models):
        if mod.get("status") != "ok" or not mod.get("materials"):
            continue
        try:
            with open(os.path.join(root, mod["materials"]), encoding="utf-8") as f:
                j = json.load(f)
            mats += sum(1 for x in j["materials"] if x.get("usedByMeshes") is not False)
        except (OSError, ValueError, KeyError, TypeError) as e:
            errors.append("ERROR models[%d].materials: unreadable: %s" % (i, e))
    cells = m.get("cells") or []
    terrain = 1 if (m.get("terrain") or {}).get("present") else 0
    water_cells = sum(1 for c in cells if c.get("water"))
    pairs = set()
    inst = 0
    for ci, c in enumerate(cells):
        for k, i in enumerate(c.get("instances") or []):
            inst += 1
            mi = i.get("model")
            if not isinstance(mi, int) or not (0 <= mi < len(models)):
                errors.append("ERROR cells[%d].instances[%d].model: index out of range: cells[%d].instances[%d].model"
                              % (ci, k, ci, k))
            elif i.get("clip"):
                pairs.add((mi, i["clip"]))
    line = ("PLAN materials=%d prefabs=%d controllers=%d cells=%d instances=%d colliders=%d terrainColliders=%d "
            "water=%d lights=1 skybox=%d" % (
                mats + terrain * len(cells) + water_cells, len(ok), len(pairs), len(cells), inst,
                sum(1 for c in cells if c.get("colliderParts", 0) > 0), terrain * len(cells), water_cells,
                1 if m.get("sky") else 0))
    return line, errors


# ---------------------------------------------------------------- driver
def _no_constant(name):
    raise ValueError("non-finite JSON constant " + name)


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f, parse_constant=_no_constant)


def validate(export_dir, schema_path, want_plan=False):
    """Returns (lines, ok). Lines are OK / PLAN / ERROR output."""
    try:
        schema = load_json(schema_path)
    except (OSError, ValueError) as e:
        return ["ERROR schema: cannot read %s: %s" % (schema_path, e)], False
    errors = []
    schema_keywords(schema, errors)
    if errors:
        return errors, False
    mpath = os.path.join(export_dir, "manifest.json")
    try:
        m = load_json(mpath)
    except (OSError, ValueError) as e:
        return ["ERROR manifest: cannot read %s: %s" % (mpath, e)], False
    try:
        errors += check(m, schema)
    except (KeyError, ValueError) as e:
        errors.append("ERROR schema: bad $ref: %s" % e)
    try:  # optional cross-check
        import jsonschema
        try:
            jsonschema.validate(m, schema)
        except jsonschema.ValidationError as e:
            errors.append("ERROR jsonschema: %s" % e.message)
    except ImportError:
        pass
    shape_ok = not errors
    if shape_ok:  # semantic checks assume a schema-valid shape
        errors += semantic(m, export_dir)
    lines = list(errors)
    if want_plan and shape_ok:
        pl, perr = plan(m, export_dir)
        lines.append(pl)
        for e in perr:
            if e not in lines:
                lines.append(e)
    if not errors:
        lines.insert(0, "OK %s (models=%d cells=%d)" % (mpath.replace("\\", "/"), len(m["models"]), len(m["cells"])))
    return lines, not errors


def self_test(export_dir, schema_path):
    base = load_json(os.path.join(export_dir, "manifest.json"))
    if (not base["models"] or not base["models"][0].get("materials")
            or not base["cells"] or not base["cells"][0]["instances"]):
        print("ERROR self-test: export needs a model with materials.json and a cell with instances")
        return 1
    tmp = tempfile.mkdtemp(prefix="cm_manifest_selftest_")
    failures = []
    try:
        def case(name, expect, corrupt):
            d = os.path.join(tmp, name)
            shutil.copytree(export_dir, d)
            corrupt(d)
            lines, ok = validate(d, schema_path)
            if ok or not any(expect in ln for ln in lines):
                failures.append("%s: expected failure containing %r, got %s" % (name, expect, lines))

        def edit(d, fn):
            p = os.path.join(d, "manifest.json")
            j = load_json(p)
            fn(j)
            with open(p, "w", encoding="utf-8") as f:
                json.dump(j, f)

        case("missing", "missing file",
             lambda d: os.remove(os.path.join(d, base["models"][0]["materials"])))
        case("range", "index out of range",
             lambda d: edit(d, lambda j: j["cells"][0]["instances"][0].__setitem__("model", 9999)))
        case("extra", "unexpected property: map.extra",
             lambda d: edit(d, lambda j: j["map"].__setitem__("extra", 1)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        for f in failures:
            print("ERROR " + f)
        return 1
    print("SELFTEST OK")
    return 0


USAGE = "usage: validate_manifest.py <exportDir> [--schema <path>] [--plan] [--self-test]"


def main(argv):
    schema = os.path.join(os.path.dirname(os.path.abspath(__file__)), "castlemist-world.schema.json")
    args = argv[1:]
    want_plan = want_self = False
    export_dir = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--schema" and i + 1 < len(args):
            schema = args[i + 1]
            i += 1
        elif a == "--plan":
            want_plan = True
        elif a == "--self-test":
            want_self = True
        elif a.startswith("--") or export_dir is not None:
            print(USAGE)
            return 2
        else:
            export_dir = a
        i += 1
    if export_dir is None:
        print(USAGE)
        return 2
    if want_self:
        return self_test(export_dir, schema)
    lines, ok = validate(export_dir, schema, want_plan)
    for ln in lines:
        print(ln)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
