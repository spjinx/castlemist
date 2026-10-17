"""Unit tests for validate_manifest.py: python -I tools/world/test_validate_manifest.py <exportDir>"""
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("validate_manifest", os.path.join(HERE, "validate_manifest.py"))
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)
EXPORT = sys.argv.pop(1) if len(sys.argv) > 1 else None
SCHEMA = os.path.join(HERE, "castlemist-world.schema.json")


def bad(inst, schema, frag):
    out = vm.check(inst, schema)
    return any(frag in e for e in out)


def good(inst, schema):
    return vm.check(inst, schema) == []


class Keywords(unittest.TestCase):
    def test_type(self):
        for t, ok, no in [("string", "a", 1), ("number", 1.5, "x"), ("integer", 3, 1.5), ("boolean", True, 1),
                          ("null", None, 0), ("array", [], {}), ("object", {}, [])]:
            self.assertTrue(good(ok, {"type": t}), t)
            self.assertTrue(bad(no, {"type": t}, "expected type"), t)
        self.assertTrue(bad(True, {"type": "integer"}, "expected type"))
        self.assertTrue(bad(True, {"type": "number"}, "expected type"))

    def test_type_union(self):
        s = {"type": ["object", "null"]}
        self.assertTrue(good(None, s) and good({}, s))
        self.assertTrue(bad(1, s, "object|null"))

    def test_enum_const(self):
        self.assertTrue(good("a", {"enum": ["a", "b"]}))
        self.assertTrue(bad("c", {"enum": ["a", "b"]}, "one of"))
        self.assertTrue(bad(True, {"enum": [1]}, "one of"))
        self.assertTrue(good(1, {"const": 1}))
        self.assertTrue(bad(2, {"const": 1}, "must equal"))

    def test_min_max(self):
        self.assertTrue(good(0, {"minimum": 0}) and bad(-1, {"minimum": 0}, "below minimum"))
        self.assertTrue(good(1, {"maximum": 1}) and bad(2, {"maximum": 1}, "above maximum"))

    def test_items_counts(self):
        s = {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 3}
        self.assertTrue(good([1, 2], s))
        self.assertTrue(bad([1], s, "minimum 2"))
        self.assertTrue(bad([1, 2, 3, 4], s, "maximum 3"))
        self.assertTrue(bad([1, "x"], s, "[1]"))

    def test_object_keywords(self):
        s = {"type": "object", "required": ["a"], "additionalProperties": False,
             "properties": {"a": {"type": "integer"}}}
        self.assertTrue(good({"a": 1}, s))
        self.assertTrue(bad({}, s, "missing required property: a"))
        self.assertTrue(bad({"a": 1, "b": 2}, s, "unexpected property: b"))
        self.assertTrue(bad({"a": "x"}, s, "ERROR a:"))
        self.assertTrue(bad({"a": 1, "b": "x"}, {"additionalProperties": {"type": "integer"}}, "ERROR b:"))

    def test_ref_defs_and_metadata(self):
        s = {"$schema": "x", "$id": "y", "title": "t", "description": "d", "$defs": {"n": {"type": "integer"}},
             "properties": {"a": {"$ref": "#/$defs/n"}}}
        self.assertTrue(good({"a": 1}, s))
        self.assertTrue(bad({"a": "x"}, s, "ERROR a:"))

    def test_unknown_keyword(self):
        errs = []
        vm.schema_keywords({"type": "object", "properties": {"a": {"pattern": "x", "type": "string"}}}, errs)
        self.assertEqual(errs, ["ERROR schema: unsupported keyword pattern"])
        errs = []
        vm.schema_keywords({"$defs": {"d": {"oneOf": []}}}, errs)
        self.assertEqual(errs, ["ERROR schema: unsupported keyword oneOf"])
        errs = []
        vm.schema_keywords(vm.load_json(SCHEMA), errs)
        self.assertEqual(errs, [])  # the real schema uses only the supported set


class Thinning(unittest.TestCase):
    """world-foliage D9: fmix32 and the keep rule (the C# FoliageRecords.Keep is the same)."""

    def test_fmix32_vectors(self):
        self.assertEqual(vm.fmix32(0), 0)
        self.assertEqual(vm.fmix32(1), 0x514E28B7)
        self.assertEqual(vm.fmix32(3), 0x85F0B427)

    def test_density_half_keeps_colliders(self):
        # placement 3 hashes to 0.523: dropped at 0.5 unless it has a collider (bit 0).
        self.assertFalse(vm.foliage_keep(3, 0, 0.5))
        self.assertTrue(vm.foliage_keep(3, 1, 0.5))
        self.assertTrue(vm.foliage_keep(1, 0, 0.5))           # 0.318
        self.assertTrue(vm.foliage_keep(0xFFFFFFFF, 0, 0.0))  # a geometry-only collider is always kept
        self.assertTrue(vm.foliage_keep(3, 1, 0.0))
        self.assertFalse(vm.foliage_keep(0, 0, 0.0))          # fmix32(0) = 0 is not < 0
        self.assertTrue(vm.foliage_keep(0xFFFFFFFE, 0, 1.0))  # density 1 keeps every placement

    def test_kept_counts(self):
        n = 20000
        for d, lo, hi in [(0.0, 0, 0), (0.5, 0.48 * n, 0.52 * n), (1.0, n, n)]:
            k = sum(1 for p in range(n) if vm.foliage_keep(p, 0, d))
            self.assertTrue(lo <= k <= hi, (d, k))


@unittest.skipUnless(EXPORT, "needs an export dir")
class Export(unittest.TestCase):
    def run_copy(self, edit=None, remove=None, plan=False, prep=None):
        d = tempfile.mkdtemp(prefix="cm_vm_")
        try:
            dst = os.path.join(d, "x")
            shutil.copytree(EXPORT, dst)
            if prep:
                prep(dst)
            if remove:
                os.remove(os.path.join(dst, remove))
            if edit:
                p = os.path.join(dst, "manifest.json")
                with open(p) as f:
                    j = json.load(f)
                edit(j)
                with open(p, "w") as f:
                    json.dump(j, f)
            return vm.validate(dst, SCHEMA, plan)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_valid_and_plan(self):
        lines, ok = self.run_copy(plan=True)
        self.assertTrue(ok, lines)
        self.assertTrue(any(l.startswith("PLAN materials=5 ") for l in lines), lines)

    def test_plan_lods(self):
        # Task 15b: the three fixture models have LOD1/LOD2, the trees are model 0, prop 1 has its own distances.
        lines, ok = self.run_copy(plan=True)
        self.assertTrue(any(l.startswith("PLAN ") and l.endswith(" lodPrefabs=3 lodTreePrefabs=1 lodOverrides=1")
                            for l in lines), lines)

        def no_lods(j):
            for mod in j["models"]:
                mod["lods"]["levels"] = 1
                mod["lods"]["triangles"] = mod["lods"]["triangles"][:1]
        lines, ok = self.run_copy(edit=no_lods, plan=True)
        self.assertTrue(ok, lines)
        self.assertTrue(any(l.endswith(" lodPrefabs=0 lodTreePrefabs=0 lodOverrides=0") for l in lines), lines)

    def test_lods_checked(self):
        def short(j):
            j["models"][0]["lods"]["triangles"] = [12, 8]
        lines, ok = self.run_copy(edit=short)
        self.assertFalse(ok)
        self.assertTrue(any("models[0].lods.triangles: 2 entries for 3 levels" in l for l in lines), lines)

        def rising(j):
            j["models"][0]["lods"]["triangles"] = [12, 13, 4]
        lines, ok = self.run_copy(edit=rising)
        self.assertTrue(any("a coarser level has more triangles" in l for l in lines), lines)

        def bad_lod(j):
            j["cells"][0]["instances"][1]["lod"] = [1000]
        lines, ok = self.run_copy(edit=bad_lod)
        self.assertFalse(ok)

    def test_plan_foliage_trees(self):
        # Layer type 3 or layer 0xFFFF (world-foliage D8): the collider tree and the geometry-only one.
        lines, ok = self.run_copy(plan=True)
        self.assertTrue(any(l.startswith("PLAN ") and " foliageTrees=2 " in l for l in lines), lines)
        # Layer 0 no longer a tree layer: only the geometry-only collider is left.
        lines, ok = self.run_copy(edit=lambda j: j["foliage"]["layers"][0].__setitem__("type", 1), plan=True)
        self.assertTrue(any(l.startswith("PLAN ") and " foliageTrees=1 " in l for l in lines), lines)

    def plan_line(self, **kw):
        d = tempfile.mkdtemp(prefix="cm_vm_")
        try:
            dst = os.path.join(d, "x")
            shutil.copytree(EXPORT, dst)
            if "prep" in kw:
                kw.pop("prep")(dst)
            m = vm.load_json(os.path.join(dst, "manifest.json"))
            line, errs = vm.plan(m, dst, **kw)
            return line
        finally:
            shutil.rmtree(d, ignore_errors=True)

    @staticmethod
    def fields(line):
        return dict(kv.split("=") for kv in line.split()[1:])

    def test_plan_foliage_modes(self):
        # Mesh: trees 2 (collider tree, geometry-only collider); the two grass records in squares (1,1) and (3,1).
        f = self.fields(self.plan_line())
        self.assertEqual((f["foliageTrees"], f["foliagePatches"], f["terrains"], f["treeInstances"]), ("2", "2", "0", "0"))
        f = self.fields(self.plan_line(foliage="off"))
        self.assertEqual((f["foliageTrees"], f["foliagePatches"]), ("0", "0"))
        f = self.fields(self.plan_line(foliage="colliders"))
        self.assertEqual((f["foliageTrees"], f["foliagePatches"]), ("2", "0"))
        f = self.fields(self.plan_line(foliage="trees"))
        self.assertEqual((f["foliageTrees"], f["foliagePatches"]), ("2", "0"))
        # Density 0.5: grass placement 1 (0.318) kept, placement 3 (0.523) dropped.
        f = self.fields(self.plan_line(density=0.5))
        self.assertEqual((f["foliageTrees"], f["foliagePatches"]), ("2", "1"))
        f = self.fields(self.plan_line(density=0.0))
        self.assertEqual((f["foliageTrees"], f["foliagePatches"]), ("2", "0"))
        # Terrain: the collider tree is a Terrain tree, the grass are details in 2 cells; no mesh terrain.
        f = self.fields(self.plan_line(mode="terrain"))
        self.assertEqual((f["foliageTrees"], f["foliagePatches"], f["terrains"], f["detailPrototypes"], f["detailCells"],
                          f["treeInstances"], f["terrainColliders"], f["materials"]), ("1", "0", "1", "1", "2", "1", "0", "5"))
        f = self.fields(self.plan_line(mode="terrain", density=0.5))
        self.assertEqual((f["detailPrototypes"], f["detailCells"], f["treeInstances"]), ("1", "1", "1"))
        f = self.fields(self.plan_line(mode="terrain", foliage="colliders"))
        self.assertEqual((f["foliageTrees"], f["detailPrototypes"], f["detailCells"], f["treeInstances"]), ("1", "0", "0", "1"))
        # Quest preset: mesh, collider trees, distance 0.5 (distance changes no count).
        f = self.fields(self.plan_line(foliage="colliders", distance=0.5))
        self.assertEqual((f["foliageTrees"], f["foliagePatches"], f["terrains"]), ("2", "0", "0"))

    def test_plan_density_keeps_colliders(self):
        # The collider tree's placement set to 3 (0.523): still kept at 0.5, and at 0.
        prep = self.patch_bin(8, "<I", 3, record=0)
        for d in (0.5, 0.0):
            f = self.fields(self.plan_line(prep=prep, density=d))
            self.assertEqual(f["foliageTrees"], "2", d)
        # Without its collider bit it is thinned like any other tree.
        def no_collider(dst):
            prep(dst)
            self.patch_bin(6, "<H", 8, record=0)(dst)
        f = self.fields(self.plan_line(prep=no_collider, density=0.5))
        self.assertEqual(f["foliageTrees"], "1")

    def test_plan_patch_split(self):
        # A patch holds at most max(1, 65535 // (3 x triangles)) records of one model and square, in record order.
        def tri(n):
            def prep(dst):
                p = os.path.join(dst, "manifest.json")
                j = vm.load_json(p)
                j["models"][2]["triangles"] = n
                with open(p, "w") as f:
                    json.dump(j, f)
            return prep
        self.assertEqual(self.fields(self.plan_line(prep=tri(21845)))["foliagePatches"], "2")
        # Both grass records in one square (the second moved onto the first's x, y): one patch at cap 2, two at cap 1.
        def same_square(n):
            def prep(dst):
                tri(n)(dst)
                with open(os.path.join(dst, self.BIN), "r+b") as f:
                    f.seek(2 * 100 + 12 + 36)
                    xy = f.read(8)
                    f.seek(3 * 100 + 12 + 36)
                    f.write(xy)
            return prep
        self.assertEqual(self.fields(self.plan_line(prep=same_square(10922)))["foliagePatches"], "1")
        self.assertEqual(self.fields(self.plan_line(prep=same_square(10923)))["foliagePatches"], "2")

    def test_plan_cli_options(self):
        import io
        import contextlib
        for bad_args in (["--mode", "x"], ["--foliage", "some"], ["--foliage-density", "1.5"],
                         ["--foliage-distance", "0.05"], ["--foliage-distance", "3"]):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = vm.main(["vm", EXPORT, "--plan"] + bad_args)
            self.assertEqual(code, 2, bad_args)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = vm.main(["vm", EXPORT, "--plan", "--mode", "terrain", "--foliage", "colliders",
                            "--foliage-density", "0.5", "--foliage-distance", "0.5"])
        self.assertEqual(code, 0, out.getvalue())
        self.assertIn(" treeInstances=1", out.getvalue())

    def test_model_index_out_of_range_in_plan(self):
        lines, ok = self.run_copy(edit=lambda j: j["cells"][0]["instances"][1].__setitem__("model", 9), plan=True)
        self.assertFalse(ok)
        self.assertTrue(any("index out of range: cells[0].instances[1].model" in l for l in lines), lines)

    def test_missing_files(self):
        for rel, key in [("cells/c0_0_2x2.glb", "cells[0].glb"), ("models/101/101.glb", "models[0].glb")]:
            lines, ok = self.run_copy(remove=rel)
            self.assertFalse(ok)
            self.assertTrue(any("missing file: " + rel in l for l in lines), lines)

    def test_missing_textures_dir(self):
        def rm(j):
            j["textures"]["dir"] = "nope"
        lines, ok = self.run_copy(edit=rm)
        self.assertTrue(any("textures.dir: missing file" in l for l in lines), lines)

    @staticmethod
    def sky_folder(dst):
        # What write_skybox leaves: sky/sky.json and a folder per mode beside it.
        os.makedirs(os.path.join(dst, "sky", "day", "baked"))
        with open(os.path.join(dst, "sky", "sky.json"), "w") as f:
            json.dump({"modes": [{"name": "day", "aliasOf": None, "baked": True}]}, f)
        with open(os.path.join(dst, "sky", "day", "baked", "equirect.png"), "wb") as f:
            f.write(b"PNG")

    def test_sky_default_is_a_mode_folder(self):
        # sky.default names a mode (a folder beside sky.json), not a root-relative path.
        sky = {"json": "sky/sky.json", "default": "day", "equirect": "sky/day/baked/equirect.png"}
        lines, ok = self.run_copy(edit=lambda j: j.__setitem__("sky", dict(sky)), prep=self.sky_folder, plan=True)
        self.assertTrue(ok, lines)
        self.assertTrue(any(l.startswith("PLAN ") and " skybox=1 foliageTrees=" in l for l in lines), lines)
        lines, ok = self.run_copy(edit=lambda j: j.__setitem__("sky", dict(sky, default="dusk")), prep=self.sky_folder)
        self.assertFalse(ok)
        self.assertTrue(any("sky.default: missing file: sky/dusk" in l for l in lines), lines)

    def test_export_log_tolerated(self):
        def log(dst):
            with open(os.path.join(dst, "export.log"), "w") as f:
                f.write("{}\n")
        lines, ok = self.run_copy(prep=log)
        self.assertTrue(ok, lines)

    def test_unity_mismatch(self):
        lines, ok = self.run_copy(edit=lambda j: j["cells"][0]["instances"][0]["unity"]["t"].__setitem__(0, 5.0))
        self.assertTrue(any("unity transform mismatch" in l for l in lines), lines)

    def test_counts_and_tiling(self):
        lines, ok = self.run_copy(edit=lambda j: j["counts"].__setitem__("propsSkipped", 3))
        self.assertTrue(any("propsWritten + propsSkipped" in l for l in lines), lines)
        lines, ok = self.run_copy(edit=lambda j: j["cells"][0].__setitem__("chunks", [0, 0, 0, 1]))
        self.assertTrue(any("is in no cell" in l for l in lines), lines)

    # ------------------------------------------------ foliage (world-foliage spec D7)
    BIN = "foliage/c0_0_2x2.bin"

    @classmethod
    def patch_bin(cls, offset, fmt, value, record=0):
        def prep(dst):
            p = os.path.join(dst, cls.BIN)
            with open(p, "r+b") as f:
                f.seek(record * 100 + offset)
                f.write(vm.struct.pack(fmt, value))
        return prep

    def test_foliage_records_valid(self):
        lines, ok = self.run_copy()
        self.assertTrue(ok, lines)
        recs = vm.read_foliage_records(os.path.join(EXPORT, self.BIN))
        self.assertEqual(len(recs), 4)
        self.assertEqual([r["flags"] & 1 for r in recs], [1, 1, 0, 0])
        self.assertEqual(recs[1]["layer"], 0xFFFF)
        self.assertEqual(recs[1]["placement"], 0xFFFFFFFF)

    def test_foliage_size(self):
        def grow(dst):
            p = os.path.join(dst, self.BIN)
            with open(p, "rb") as f:
                b = f.read()
            with open(p, "wb") as f:
                f.write(b + b"\0")
        lines, ok = self.run_copy(prep=grow)
        self.assertFalse(ok)
        self.assertTrue(any("foliage size: " + self.BIN in l for l in lines), lines)

    def test_foliage_missing_bin(self):
        lines, ok = self.run_copy(remove=self.BIN)
        self.assertFalse(ok)
        self.assertTrue(any("missing file: " + self.BIN in l for l in lines), lines)

    def test_foliage_model_and_layer_range(self):
        lines, ok = self.run_copy(prep=self.patch_bin(0, "<I", 9999))
        self.assertTrue(any("index out of range: %s[0].model" % self.BIN in l for l in lines), lines)
        lines, ok = self.run_copy(prep=self.patch_bin(4, "<H", 7, record=2))
        self.assertTrue(any("index out of range: %s[2].layer" % self.BIN in l for l in lines), lines)
        # 0xFFFF is the geometry-only layer, not out of range.
        lines, ok = self.run_copy(prep=self.patch_bin(4, "<H", 0xFFFF, record=0))
        self.assertTrue(ok, lines)

    def test_foliage_unity_mismatch(self):
        lines, ok = self.run_copy(prep=self.patch_bin(60, "<f", 12345.0, record=3))
        self.assertTrue(any("%s[3].unity: unity transform mismatch" % self.BIN in l for l in lines), lines)

    def test_foliage_sorted_and_counts(self):
        lines, ok = self.run_copy(prep=self.patch_bin(8, "<I", 7, record=2))   # placement 7 before 3
        self.assertTrue(any("not sorted" in l for l in lines), lines)
        lines, ok = self.run_copy(edit=lambda j: j["counts"]["foliage"].__setitem__("records", 5))
        self.assertTrue(any("counts.foliage.records" in l for l in lines), lines)
        lines, ok = self.run_copy(edit=lambda j: j["cells"][0]["foliage"].__setitem__("colliderTrees", 1))
        self.assertTrue(any("colliderTrees: 1 declared, 2 records with bit 0" in l for l in lines), lines)

    def test_foliage_extra_property(self):
        lines, ok = self.run_copy(edit=lambda j: j["foliage"].__setitem__("extra", 1))
        self.assertTrue(any("unexpected property: foliage.extra" in l for l in lines), lines)
        lines, ok = self.run_copy(edit=lambda j: j["cells"][0]["foliage"].__setitem__("extra", 1))
        self.assertTrue(any("unexpected property: cells[0].foliage.extra" in l for l in lines), lines)
        lines, ok = self.run_copy(edit=lambda j: j["counts"]["foliage"].__setitem__("extra", 1))
        self.assertTrue(any("unexpected property: counts.foliage.extra" in l for l in lines), lines)

    def test_non_finite_rejected(self):
        d = tempfile.mkdtemp(prefix="cm_vm_")
        try:
            dst = os.path.join(d, "x")
            shutil.copytree(EXPORT, dst)
            p = os.path.join(dst, "manifest.json")
            with open(p) as f:
                s = f.read()
            s2 = re.sub(r'"z":\s*0\.0', '"z": NaN', s, count=1)
            self.assertNotEqual(s, s2)
            with open(p, "w") as f:
                f.write(s2)
            lines, ok = vm.validate(dst, SCHEMA)
            self.assertFalse(ok)
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
