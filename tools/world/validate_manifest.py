#!/usr/bin/env python
"""Dependency-free validator and Unity dry run for a castlemist world export.

    python -I tools/world/validate_manifest.py <exportDir> [--schema <path>] [--self-test]
        [--plan [--mode mesh|terrain] [--foliage off|colliders|trees|all] [--foliage-density d] [--foliage-distance D]]

Exit 0 when valid (prints "OK ..."), 1 with one "ERROR <path>: <message>" line
per problem. Standard library only. The schema checker supports exactly:
$schema $id $defs $ref title description type properties required
additionalProperties items enum const minimum maximum minItems maxItems
(type may be an array). Any other schema keyword is "ERROR schema: unsupported
keyword <k>". If jsonschema happens to be importable it is run as a cross-check.

Foliage (world-foliage spec D7): every cells[].foliage.file is read as
castlemist-foliage-1 records (100 bytes each, little-endian; the layout is in
the schema's foliage description). Its size must be records x 100, each
record's model and layer must be in range, the records sorted by (model,
placement), the bit-0 count equal to colliderTrees, and each record's unity
TRS must reproduce its m relative to the cell origin within 1e-4. The cells'
sums must equal counts.foliage.

unityTerrain (world-foliage spec D7): every tile's heights file must be
heightmapResolution^2 x 2 bytes, its holes PNG (heightmapResolution - 1)
square, its albedo PNG albedoSize, its detail PNGs detailResolution square, its
trees file records x 24 bytes; prototype and model indices in range; the
tiles' tree records equal the foliage records with flag bit 3; and every check
point inside a tile's real chunks.

--plan prints what the Unity importer builds with the given options (default
mesh, all, density 1, distance 1; world-foliage D8, D9), ending with
foliageTrees= foliagePatches= terrains= detailPrototypes= detailCells=
treeInstances= (plan_foliage). Distance changes no count. In Terrain mode the
cells' mesh terrain and its material are not built (terrainColliders=0) and one
shared Nature/Terrain/Standard template material is.
"""
import json
import math
import os
import posixpath
import shutil
import struct
import sys
import tempfile

KEYWORDS = {"$schema", "$id", "$defs", "$ref", "title", "description", "type", "properties", "required",
            "additionalProperties", "items", "enum", "const", "minimum", "maximum", "minItems", "maxItems"}
TYPES = {"object", "array", "string", "number", "integer", "boolean", "null"}
FOLIAGE_RECORD = struct.Struct("<IHHI12f3f4f3f")  # castlemist-foliage-1, 100 bytes
FOLIAGE_GEOMETRY_ONLY_LAYER = 0xFFFF


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
        for k in ("json", "equirect"):
            if sky.get(k) is not None:
                need(sky[k], "sky." + k)
        # default names a mode: write_skybox's folder of that name beside sky.json.
        if isinstance(sky.get("default"), str) and isinstance(sky.get("json"), str):
            mode = posixpath.join(posixpath.dirname(sky["json"]), sky["default"])
            if not os.path.isdir(os.path.join(root, mode)):
                errors.append("ERROR sky.default: missing file: %s" % mode)
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

    _check_foliage(m, root, models, cells, errors)
    _check_unity_terrain(m, root, models, cells, errors)

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


def read_foliage_records(path):
    """The castlemist-foliage-1 records of one bin as dicts (model, layer, flags, placement, m, unity, yaw, tilt)."""
    with open(path, "rb") as f:
        data = f.read()
    out = []
    for at in range(0, len(data) - FOLIAGE_RECORD.size + 1, FOLIAGE_RECORD.size):
        v = FOLIAGE_RECORD.unpack_from(data, at)
        m3 = v[4:16]
        m16 = [m3[0], m3[1], m3[2], 0.0, m3[3], m3[4], m3[5], 0.0, m3[6], m3[7], m3[8], 0.0,
               m3[9], m3[10], m3[11], 1.0]
        out.append({"model": v[0], "layer": v[1], "flags": v[2], "placement": v[3], "m": m16,
                    "unity": {"t": list(v[16:19]), "r": list(v[19:23]), "s": v[23]},
                    "unityYaw": v[24], "tiltDeg": v[25]})
    return out


def _check_foliage(m, root, models, cells, errors):
    """cells[].foliage bins against the foliage block and counts.foliage (module docstring)."""
    fol = m.get("foliage")
    layers = fol.get("layers") if isinstance(fol, dict) else None
    if isinstance(fol, dict):
        for k, fm in enumerate(fol.get("models") or []):
            mi = fm.get("model")
            if isinstance(mi, int) and not (0 <= mi < len(models)):
                errors.append("ERROR foliage.models[%d].model: index out of range: foliage.models[%d].model (%d, models has %d)"
                              % (k, k, mi, len(models)))
        for li, layer in enumerate(layers or []):
            for k, mi in enumerate(layer.get("models") or []):
                if isinstance(mi, int) and not (0 <= mi < len(models)):
                    errors.append("ERROR foliage.layers[%d].models[%d]: index out of range: foliage.layers[%d].models[%d] "
                                  "(%d, models has %d)" % (li, k, li, k, mi, len(models)))
    rb = FOLIAGE_RECORD.size
    records = colliders = 0
    any_cell = False
    for ci, c in enumerate(cells):
        f = c.get("foliage")
        if not isinstance(f, dict):
            continue
        any_cell = True
        p = "cells[%d].foliage" % ci
        rel = f.get("file")
        n = f.get("records")
        if not isinstance(rel, str) or not isinstance(n, int):
            continue
        records += n
        colliders += f.get("colliderTrees", 0) if isinstance(f.get("colliderTrees"), int) else 0
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            errors.append("ERROR %s.file: missing file: %s" % (p, rel))
            continue
        size = os.path.getsize(path)
        if size != n * rb:
            errors.append("ERROR %s.file: foliage size: %s (%d bytes, %d records x %d)" % (p, rel, size, n, rb))
            continue
        bits = 0
        prev = None
        for i, r in enumerate(read_foliage_records(path)):
            rp = "%s[%d]" % (rel, i)
            vals = r["m"] + r["unity"]["t"] + r["unity"]["r"] + [r["unity"]["s"], r["unityYaw"], r["tiltDeg"]]
            if not all(math.isfinite(x) for x in vals):
                errors.append("ERROR %s: non-finite number" % rp)
                continue
            if not r["model"] < len(models):
                errors.append("ERROR %s.model: index out of range: %s.model (%d, models has %d)"
                              % (rp, rp, r["model"], len(models)))
            if (layers is not None and r["layer"] != FOLIAGE_GEOMETRY_ONLY_LAYER
                    and not r["layer"] < len(layers)):
                errors.append("ERROR %s.layer: index out of range: %s.layer (%d, foliage.layers has %d)"
                              % (rp, rp, r["layer"], len(layers)))
            if (r["flags"] >> 1) & 3 == 3:
                errors.append("ERROR %s.flags: source 3 is not defined" % rp)
            if r["flags"] & 1:
                bits += 1
            key = (r["model"], r["placement"])
            if prev is not None and key < prev:
                errors.append("ERROR %s: records not sorted by (model, placement)" % rp)
            prev = key
            _check_unity(r, c.get("origin"), rp, errors)
        if isinstance(f.get("colliderTrees"), int) and bits != f["colliderTrees"]:
            errors.append("ERROR %s.colliderTrees: %d declared, %d records with bit 0 in %s"
                          % (p, f["colliderTrees"], bits, rel))
    cf = (m.get("counts") or {}).get("foliage")
    if isinstance(cf, dict):
        if cf.get("records") != records:
            errors.append("ERROR counts.foliage.records: %s but cells hold %d records" % (cf.get("records"), records))
        if cf.get("colliderTrees") != colliders:
            errors.append("ERROR counts.foliage.colliderTrees: %s but cells hold %d" % (cf.get("colliderTrees"), colliders))
    elif any_cell:
        errors.append("ERROR counts.foliage: missing, but cells hold foliage")


TERRAIN_TREE_RECORD = 24
FOLIAGE_FLAG_TERRAIN_TREE = 1 << 3


def png_size(path):
    """(width, height) from a PNG's IHDR, or None when it is not a PNG."""
    with open(path, "rb") as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", head[16:24])


def _check_unity_terrain(m, root, models, cells, errors):
    """The unityTerrain block's files, indices and check points (module docstring)."""
    ut = m.get("unityTerrain")
    if not isinstance(ut, dict):
        return
    n = ut.get("heightmapResolution")
    dres = ut.get("detailResolution")
    nd = len(ut.get("detailPrototypes") or [])
    nt = len(ut.get("treePrototypes") or [])
    for key in ("detailPrototypes", "treePrototypes"):
        for k, pr in enumerate(ut.get(key) or []):
            mi = pr.get("model")
            if isinstance(mi, int) and not (0 <= mi < len(models)):
                errors.append("ERROR unityTerrain.%s[%d].model: index out of range: unityTerrain.%s[%d].model "
                              "(%d, models has %d)" % (key, k, key, k, mi, len(models)))

    def png_is(rel, p, w, h):
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            errors.append("ERROR %s: missing file: %s" % (p, rel))
            return
        got = png_size(path)
        if got != (w, h):
            errors.append("ERROR %s: tile file size: %s is %s, expected %d x %d" % (p, rel, got, w, h))

    tree_records = 0
    reals = []
    for ti, t in enumerate(ut.get("tiles") or []):
        p = "unityTerrain.tiles[%d]" % ti
        rel = t.get("heights")
        if isinstance(rel, str) and isinstance(n, int):
            path = os.path.join(root, rel)
            if not os.path.isfile(path):
                errors.append("ERROR %s.heights: missing file: %s" % (p, rel))
            elif os.path.getsize(path) != n * n * 2:
                errors.append("ERROR %s.heights: tile file size: %s (%d bytes, %d x %d x 2)"
                              % (p, rel, os.path.getsize(path), n, n))
        if isinstance(t.get("holes"), str) and isinstance(n, int):
            png_is(t["holes"], p + ".holes", n - 1, n - 1)
        size = t.get("albedoSize")
        if isinstance(t.get("albedo"), str) and isinstance(size, list) and len(size) == 2:
            png_is(t["albedo"], p + ".albedo", size[0], size[1])
        for k, d in enumerate(t.get("details") or []):
            dp = "%s.details[%d]" % (p, k)
            if isinstance(d.get("prototype"), int) and not d["prototype"] < nd:
                errors.append("ERROR %s.prototype: index out of range: %s.prototype (%d, detailPrototypes has %d)"
                              % (dp, dp, d["prototype"], nd))
            if isinstance(d.get("counts"), str) and isinstance(dres, int):
                png_is(d["counts"], dp + ".counts", dres, dres)
        tr = t.get("trees")
        if isinstance(tr, dict) and isinstance(tr.get("file"), str) and isinstance(tr.get("records"), int):
            tree_records += tr["records"]
            path = os.path.join(root, tr["file"])
            if not os.path.isfile(path):
                errors.append("ERROR %s.trees.file: missing file: %s" % (p, tr["file"]))
            elif os.path.getsize(path) != tr["records"] * TERRAIN_TREE_RECORD:
                errors.append("ERROR %s.trees.file: tile file size: %s (%d bytes, %d records x %d)"
                              % (p, tr["file"], os.path.getsize(path), tr["records"], TERRAIN_TREE_RECORD))
            else:
                with open(path, "rb") as f:
                    data = f.read()
                for i in range(tr["records"]):
                    proto = struct.unpack_from("<I", data, i * TERRAIN_TREE_RECORD)[0]
                    if not proto < nt:
                        errors.append("ERROR %s[%d]: index out of range: prototype %d (treePrototypes has %d)"
                                      % (tr["file"], i, proto, nt))
                        break
        pos, sz, ch = t.get("position"), t.get("size"), t.get("chunks")
        if isinstance(pos, list) and isinstance(sz, list) and isinstance(ch, list) and len(ch) == 4:
            tc = ut.get("tileChunks")
            if isinstance(tc, int) and tc > 0:
                cw, cd = sz[0] / tc, sz[2] / tc
                x0 = pos[0]
                z1 = pos[2] + sz[2]
                reals.append((x0, z1 - (ch[3] - ch[1] + 1) * cd, x0 + (ch[2] - ch[0] + 1) * cw, z1))
    # Terrain trees = the foliage records with bit 3.
    bit3 = 0
    for c in cells:
        f = c.get("foliage")
        if isinstance(f, dict) and isinstance(f.get("file"), str):
            path = os.path.join(root, f["file"])
            if os.path.isfile(path) and isinstance(f.get("records"), int) and \
                    os.path.getsize(path) == f["records"] * FOLIAGE_RECORD.size:
                bit3 += sum(1 for r in read_foliage_records(path) if r["flags"] & FOLIAGE_FLAG_TERRAIN_TREE)
    if bit3 != tree_records:
        errors.append("ERROR unityTerrain.tiles: %d Terrain tree records, but %d foliage records have bit 3"
                      % (tree_records, bit3))
    eps = 1e-3
    for k, pt in enumerate(ut.get("check") or []):
        u = pt.get("unity")
        if not (isinstance(u, list) and len(u) == 2):
            continue
        x, z = u
        if not any(r[0] - eps <= x <= r[2] + eps and r[1] - eps <= z <= r[3] + eps for r in reals):
            errors.append("ERROR unityTerrain.check[%d]: point (%s, %s) is inside no tile" % (k, x, z))


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
PLAN_MODES = ("mesh", "terrain")
PLAN_FOLIAGE = ("off", "colliders", "trees", "all")
FOLIAGE_PATCH_UNITS = 1536.0      # foliagePatchUnits (castlemist's choice, world-foliage D8)
FOLIAGE_PATCH_MAX_VERTICES = 65535
FOLIAGE_FLAG_COLLIDER = 1
FOLIAGE_FLAG_OUTSIDE_TERRAIN = 1 << 4
FOLIAGE_NO_PLACEMENT = 0xFFFFFFFF
DETAIL_MAX_COUNT = 255


def fmix32(h):
    """MurmurHash3's 32-bit finaliser (foliageThinHash, world-foliage D9)."""
    h &= 0xFFFFFFFF
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h


def foliage_keep(placement, flags, density):
    """The Density knob's keep rule (D9; C# FoliageRecords.Keep): a collider (bit 0) and a geometry-only collider
    (placement 0xFFFFFFFF) are always kept; otherwise fmix32(placement) < density * 2^32."""
    if flags & FOLIAGE_FLAG_COLLIDER or placement == FOLIAGE_NO_PLACEMENT:
        return True
    return fmix32(placement) < density * 4294967296.0


def patch_capacity(triangles):
    """Records per patch (D8): at most 65535 vertices, bounded by 3 vertices a triangle of the model's
    models[].triangles, so the dry run and the importer split alike: max(1, 65535 // (3 x triangles))."""
    t = triangles if isinstance(triangles, int) and triangles > 0 else 1
    return max(1, FOLIAGE_PATCH_MAX_VERTICES // (3 * t))


def plan(m, root, mode="mesh", foliage="all", density=1.0, distance=1.0):
    """Returns (PLAN line, errors): what the Unity importer would build with these options
    (Terrain mode, Foliage, Density; Distance changes no count)."""
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
    ut = m.get("unityTerrain")
    if mode == "terrain" and not isinstance(ut, dict):
        errors.append("ERROR unityTerrain: missing: Terrain mode needs the export's unityTerrain block "
                      "(world-export without --no-unity-terrain); mesh terrain is planned")
        mode = "mesh"
    terrain = 1 if (m.get("terrain") or {}).get("present") else 0
    mesh_terrain = terrain if mode == "mesh" else 0
    tiles = len(ut.get("tiles") or []) if mode == "terrain" else 0
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
    fp = plan_foliage(m, root, mode, foliage, density)
    line = ("PLAN materials=%d prefabs=%d controllers=%d cells=%d instances=%d colliders=%d terrainColliders=%d "
            "water=%d lights=1 skybox=%d foliageTrees=%d foliagePatches=%d terrains=%d detailPrototypes=%d "
            "detailCells=%d treeInstances=%d" % (
                mats + mesh_terrain * len(cells) + water_cells + (1 if tiles else 0), len(ok), len(pairs), len(cells),
                inst, sum(1 for c in cells if c.get("colliderParts", 0) > 0), mesh_terrain * len(cells), water_cells,
                1 if m.get("sky") else 0, fp["trees"], fp["patches"], tiles, fp["detailPrototypes"],
                fp["detailCells"], fp["treeInstances"]))
    return line, errors


def foliage_shown(foliage, kind, flags):
    """The Foliage knob (D9): Off; Collider trees = bit 0 only; Trees = type 3 and geometry-only colliders; All."""
    if foliage == "off" or kind is None:
        return False
    if foliage == "colliders":
        return bool(flags & FOLIAGE_FLAG_COLLIDER)
    if foliage == "trees":
        return kind == "tree"
    return True


def _terrain_tiles(ut):
    """unityTerrain tiles by their north-west chunk, with the real rect, origin and detail resolution."""
    out = {}
    tc = ut.get("tileChunks") or 4
    for t in ut.get("tiles") or []:
        ch, pos, sz = t["chunks"], t["position"], t["size"]
        cw, cd = sz[0] / tc, sz[2] / tc
        x0, z1 = pos[0], pos[2] + sz[2]
        real = (x0, z1 - (ch[3] - ch[1] + 1) * cd, x0 + (ch[2] - ch[0] + 1) * cw, z1)
        out[(ch[0], ch[1])] = {"id": t["id"], "x0": x0, "y1": z1, "real": real}
    return out


def plan_foliage(m, root, mode, foliage, density):
    """Foliage counts of the Unity importer (world-foliage D8, D9).

    A record whose model has status ok is a tree when its layer has type 3 or is 0xFFFF (a collider linked by
    geometry only), and a type-1/2 record when its layer has type 1 or 2; anything else is not placed. It is placed
    when the Foliage knob shows it and the Density knob keeps it (foliage_keep). Then:
    - Terrain mode: a tree with flag bit 3 in a cell with a tile is a Terrain tree (treeInstances); a type-1/2 record whose model is
      foliage.models[].terrainDetail, without bit 4 and inside its cell's tile's real area, is a detail: one count in
      the 48-unit cell (floor((x - tileX0)/48), floor((tileY1 - y)/48)) of its prototype's layer (detailPrototypes =
      the (tile, prototype) layers with a count, detailCells = the cells with one);
    - otherwise a tree is a prefab instance (foliageTrees), and type-1/2 records are grouped per (cell, model,
      1536-unit square from the cell's rect x0, y0) and split in record order every patch_capacity(triangles)
      records (foliagePatches).
    A missing or unreadable bin counts nothing here (semantic() reports it)."""
    out = {"trees": 0, "patches": 0, "detailPrototypes": 0, "detailCells": 0, "treeInstances": 0}
    if foliage == "off":
        return out
    models = m.get("models") or []
    fol = m.get("foliage")
    layers = (fol.get("layers") if isinstance(fol, dict) else None) or []
    ltype = [l.get("type") if isinstance(l, dict) else None for l in layers]
    detail_model = set()
    for fm in (fol.get("models") or []) if isinstance(fol, dict) else []:
        if fm.get("terrainDetail") is True and isinstance(fm.get("model"), int):
            detail_model.add(fm["model"])
    ut = m.get("unityTerrain") if mode == "terrain" else None
    tiles = _terrain_tiles(ut) if isinstance(ut, dict) else {}
    tc = (ut or {}).get("tileChunks") or 4
    dres = (ut or {}).get("detailResolution") or 256
    dcell = float((ut or {}).get("detailCellUnits") or 48)
    dproto = {}
    for k, p in enumerate((ut or {}).get("detailPrototypes") or []):
        dproto.setdefault(p.get("model"), k)
    detail_counts = {}   # (tile id, prototype) -> {(dx, dy): n}
    for c in m.get("cells") or []:
        f = c.get("foliage")
        if not isinstance(f, dict) or not isinstance(f.get("file"), str):
            continue
        try:
            recs = read_foliage_records(os.path.join(root, f["file"]))
        except OSError:
            continue
        rect = c.get("rect") or [0.0, 0.0, 0.0, 0.0]
        ch = c.get("chunks")
        tile = tiles.get(((ch[0] // tc) * tc, (ch[1] // tc) * tc)) if ut is not None and ch else None
        squares = {}   # (model, sx, sy) -> records
        for r in recs:
            mi = r["model"]
            if not (mi < len(models) and models[mi].get("status") == "ok"):
                continue
            li = r["layer"]
            t = ltype[li] if li < len(ltype) else None
            kind = "tree" if li == FOLIAGE_GEOMETRY_ONLY_LAYER or t == 3 else ("small" if t in (1, 2) else None)
            if not foliage_shown(foliage, kind, r["flags"]) or not foliage_keep(r["placement"], r["flags"], density):
                continue
            x, y = r["m"][12], r["m"][13]
            if ut is not None and tile is not None and kind == "tree" and r["flags"] & FOLIAGE_FLAG_TERRAIN_TREE:
                out["treeInstances"] += 1
                continue
            if kind == "tree":
                out["trees"] += 1
                continue
            if (ut is not None and tile is not None and mi in detail_model and mi in dproto
                    and not r["flags"] & FOLIAGE_FLAG_OUTSIDE_TERRAIN):
                rr = tile["real"]
                if rr[0] <= x <= rr[2] and rr[1] <= y <= rr[3]:
                    dx = min(max(int(math.floor((x - tile["x0"]) / dcell)), 0), dres - 1)
                    dy = min(max(int(math.floor((tile["y1"] - y) / dcell)), 0), dres - 1)
                    cellmap = detail_counts.setdefault((tile["id"], dproto[mi]), {})
                    cellmap[(dx, dy)] = cellmap.get((dx, dy), 0) + 1
                    continue
            key = (mi, int(math.floor((x - rect[0]) / FOLIAGE_PATCH_UNITS)),
                   int(math.floor((y - rect[1]) / FOLIAGE_PATCH_UNITS)))
            squares[key] = squares.get(key, 0) + 1
        for (mi, _, _), n in squares.items():
            cap = patch_capacity(models[mi].get("triangles"))
            out["patches"] += (n + cap - 1) // cap
    out["detailPrototypes"] = len(detail_counts)
    out["detailCells"] = sum(len(v) for v in detail_counts.values())
    return out


def plan_foliage_trees(m, root):
    """Tree records the Unity importer places as prefab instances in mesh mode with Foliage All."""
    return plan_foliage(m, root, "mesh", "all", 1.0)["trees"]


# ---------------------------------------------------------------- driver
def _no_constant(name):
    raise ValueError("non-finite JSON constant " + name)


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f, parse_constant=_no_constant)


def validate(export_dir, schema_path, want_plan=False, plan_opts=None):
    """Returns (lines, ok). Lines are OK / PLAN / ERROR output. plan_opts: plan()'s keyword options."""
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
        pl, perr = plan(m, export_dir, **(plan_opts or {}))
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

        # Foliage (world-foliage spec D7), when the export has any.
        fcell = next((c["foliage"] for c in base["cells"] if isinstance(c.get("foliage"), dict)), None)
        if isinstance(base.get("foliage"), dict) and fcell is not None:
            rel = fcell["file"]

            def truncate(d):
                p = os.path.join(d, rel)
                with open(p, "rb") as f:
                    b = f.read()
                with open(p, "wb") as f:
                    f.write(b[:-1])

            def bad_model(d):
                p = os.path.join(d, rel)
                with open(p, "r+b") as f:
                    f.write(struct.pack("<I", 9999))

            case("foliage_size", "foliage size: " + rel, truncate)
            case("foliage_range", "index out of range: %s[0].model" % rel, bad_model)
            case("foliage_extra", "unexpected property: foliage.extra",
                 lambda d: edit(d, lambda j: j["foliage"].__setitem__("extra", 1)))

        # unityTerrain (world-foliage spec D7), when the export has it.
        ut = base.get("unityTerrain")
        if isinstance(ut, dict) and ut.get("tiles"):
            heights = ut["tiles"][0]["heights"]
            case("terrain_missing", "missing file: " + heights, lambda d: os.remove(os.path.join(d, heights)))

            def short_heights(d):
                p = os.path.join(d, heights)
                with open(p, "rb") as f:
                    b = f.read()
                with open(p, "wb") as f:
                    f.write(b[:-2])

            case("terrain_size", "tile file size: " + heights, short_heights)
            if ut.get("check"):
                case("terrain_check", "unityTerrain.check[0]: point",
                     lambda d: edit(d, lambda j: j["unityTerrain"]["check"][0].__setitem__("unity", [1e7, 1e7])))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        for f in failures:
            print("ERROR " + f)
        return 1
    print("SELFTEST OK")
    return 0


USAGE = ("usage: validate_manifest.py <exportDir> [--schema <path>] [--plan [--mode mesh|terrain] "
         "[--foliage off|colliders|trees|all] [--foliage-density d] [--foliage-distance D]] [--self-test]")


def main(argv):
    schema = os.path.join(os.path.dirname(os.path.abspath(__file__)), "castlemist-world.schema.json")
    args = argv[1:]
    want_plan = want_self = False
    export_dir = None
    opts = {}
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
        elif a in ("--mode", "--foliage", "--foliage-density", "--foliage-distance") and i + 1 < len(args):
            v = args[i + 1]
            i += 1
            try:
                if a == "--mode" and v in PLAN_MODES:
                    opts["mode"] = v
                elif a == "--foliage" and v in PLAN_FOLIAGE:
                    opts["foliage"] = v
                elif a == "--foliage-density" and 0.0 <= float(v) <= 1.0:
                    opts["density"] = float(v)
                elif a == "--foliage-distance" and 0.1 <= float(v) <= 2.0:
                    opts["distance"] = float(v)
                else:
                    raise ValueError(v)
            except ValueError:
                print(USAGE)
                return 2
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
    lines, ok = validate(export_dir, schema, want_plan, opts)
    for ln in lines:
        print(ln)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
