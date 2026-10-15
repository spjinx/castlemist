// castlemist world export: the frame probe check in Unity.
//
// Spec docs/superpowers/specs/2026-10-15-world-export-design.md, D6 "Frame
// probe". The probe (probe.glb -> Blender -> probe.fbx) holds three bars,
// Probe_East / Probe_North / Probe_Up, 3 : 2 : 1 long. The expected Unity
// directions are exportgltf::frame_probe_expect()["unity"]
// (include/castlemist/exportgltf/world_probe.h), from world/frame.h's
// map_to_unity: east +X, north +Z, up +Y.
//
// Check rule, the same as castlemist_world.py --check-probe:
//  1. each bar's world-space bounds centre minus the probe root's world
//     position, normalised;
//  2. a bar passes when its dot with the expected direction is > 0.99;
//  3. the extents along the three directions are in the ratio 3 : 2 : 1
//     within 1 %.
// It also fails a bar whose triangles (Unity: clockwise front faces) or
// vertex normals point into the box, and reports negScale=1 when any
// transform under the root has a negative determinant.
// If the check fails, the fix goes into castlemist_world.py's FBX axis
// settings, never into this importer.

using System.Collections.Generic;
using System.Globalization;
using UnityEngine;

namespace Castlemist
{
    public static class FrameProbe
    {
        public static readonly Vector3 East = new Vector3(1f, 0f, 0f);
        public static readonly Vector3 North = new Vector3(0f, 0f, 1f);
        public static readonly Vector3 Up = new Vector3(0f, 1f, 0f);
        public static readonly float[] Lengths = { 3f, 2f, 1f };

        /// ModelImporter.bakeAxisConversion for every castlemist world .fbx.
        /// Measured with the probe (Task 1, 2026-10-15, Unity 6000.0.42f1,
        /// Blender 4.5 FBX with axis_forward '-Z', axis_up 'Y'): off, east and
        /// north import as -X and -Z; on, as +X and +Z. The same on Unity
        /// 2022.3.22f1 (VRChat's version; world export Task 12), where the
        /// skinned probe also imports un-mirrored with its clip moving the
        /// right way.
        public const bool BakeAxisConversion = true;

        static readonly string[] Axes = { "east", "north", "up" };
        static readonly string[] Bars = { "Probe_East", "Probe_North", "Probe_Up" };

        /// Measures the probe under probeRoot and writes the PROBE line.
        /// Returns true when every bar passes and no transform is mirrored.
        public static bool Check(GameObject probeRoot, out string line)
        {
            Vector3[] want = { East, North, Up };
            Vector3 origin = probeRoot.transform.position;
            var dirs = new Vector3[3];
            var extents = new float[3];
            var fails = new List<string>();

            bool neg = false;
            foreach (Transform t in probeRoot.GetComponentsInChildren<Transform>(true))
                if (t.localToWorldMatrix.determinant < 0f) neg = true;

            for (int a = 0; a < 3; ++a)
            {
                Renderer r = FindBar(probeRoot, Bars[a]);
                if (r == null) { fails.Add(Axes[a]); continue; }
                Bounds b = r.bounds;
                Vector3 c = b.center - origin;
                dirs[a] = c.sqrMagnitude > 0f ? c.normalized : Vector3.zero;
                extents[a] = Mathf.Abs(Vector3.Dot(b.size, want[a]));
                if (Vector3.Dot(dirs[a], want[a]) <= 0.99f) fails.Add(Axes[a]);
                if (InwardFaces(r, b.center) > 0) fails.Add(Axes[a] + "-winding");
            }
            if (fails.Count == 0)
            {
                float unit = extents[2];
                for (int a = 0; a < 2; ++a)
                    if (unit <= 0f || Mathf.Abs(extents[a] / unit - Lengths[a]) > 0.01f * Lengths[a])
                        fails.Add(Axes[a] + "-length");
            }

            line = "PROBE east=" + Fmt(dirs[0]) + " north=" + Fmt(dirs[1]) + " up=" + Fmt(dirs[2]) +
                   " negScale=" + (neg ? "1" : "0") +
                   (fails.Count == 0 ? " OK" : " FAIL " + string.Join(",", fails));
            line += "\nPROBE_EXTENTS east=" + extents[0].ToString("F4", CultureInfo.InvariantCulture) +
                    " north=" + extents[1].ToString("F4", CultureInfo.InvariantCulture) +
                    " up=" + extents[2].ToString("F4", CultureInfo.InvariantCulture);
            return fails.Count == 0 && !neg;
        }

        static Renderer FindBar(GameObject root, string name)
        {
            foreach (Renderer r in root.GetComponentsInChildren<Renderer>(true))
                if (r.gameObject.name == name || r.gameObject.name.StartsWith(name + ".")) return r;
            return null;
        }

        /// Triangles whose winding normal (Unity: Cross(b - a, c - a) for a
        /// clockwise front face), or vertex normals, point into the box.
        static int InwardFaces(Renderer r, Vector3 centre)
        {
            var mf = r.GetComponent<MeshFilter>();
            Mesh mesh = mf != null ? mf.sharedMesh : null;
            if (mesh == null) return 1;
            Transform t = r.transform;
            float flip = t.localToWorldMatrix.determinant < 0f ? -1f : 1f;
            Vector3[] v = mesh.vertices;
            Vector3[] n = mesh.normals;
            int[] tri = mesh.triangles;
            int bad = 0;
            for (int i = 0; i + 2 < tri.Length; i += 3)
            {
                Vector3 a = t.TransformPoint(v[tri[i]]), b = t.TransformPoint(v[tri[i + 1]]), c = t.TransformPoint(v[tri[i + 2]]);
                Vector3 outward = (a + b + c) / 3f - centre;
                if (flip * Vector3.Dot(Vector3.Cross(b - a, c - a), outward) <= 0f) ++bad;
                if (n.Length == v.Length)
                    for (int k = 0; k < 3; ++k)
                        if (Vector3.Dot(t.TransformDirection(n[tri[i + k]]), outward) <= 0f) ++bad;
            }
            return bad;
        }

        static string Fmt(Vector3 v)
        {
            return "(" + (v.x + 0f).ToString("F3", CultureInfo.InvariantCulture) + "," +
                   (v.y + 0f).ToString("F3", CultureInfo.InvariantCulture) + "," +
                   (v.z + 0f).ToString("F3", CultureInfo.InvariantCulture) + ")";
        }
    }
}
