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
        self.assertTrue(any(l.startswith("PLAN ") and l.endswith("skybox=1") for l in lines), lines)
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
