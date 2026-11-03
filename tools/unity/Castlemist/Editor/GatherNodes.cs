// castlemist world import: Halloween candy corn gather nodes (2026-10-27). GW2 spawns its resource nodes
// from the server and neither the API nor the wiki lists where the Labyrinth's Raw Candy Corn veins stand,
// so the spots are the scene's own: open maze floor (collision facing up, at the floor's usual height,
// nothing within ClearRadius above it), at least MinSpacing apart and away from the Haunted Door spawn
// points, picked by a fixed seed (the same spots on every run). The game's node models (Raw Candy Corn
// 499015, Rich Candy Corn Vein 499019; added to the export with gw2dat_cli model-export) idle in a pose
// the export does not have: their zeropose hides the corn (boneVis) and their bind pose is a scattered
// rig. So the nodes are built from GW2's own candy corn mesh and material (the projectile 499021): Raw
// Candy Corn one tall corn that shrinks with each of its 3 strikes (wiki: "at least 3 times"), a Rich
// Candy Corn Vein a cluster of 5 that loses one every 2 of its 10 strikes; the same corn is the piece
// that flies out of each strike. Runtime/GW2GatherNode: each player gathers on their own.
// 2026-10-27, sounds (sounds/gather, gw2dat_cli audio-extract; found with soundscan): each strike GW2's
// pick hit (character script 13211 "genhitpickh") over an ore hit ("ore", 2603272), the last strike the
// node running dry (3189096 "oreoff"). "Candy Corn Node Sounds" adds them to the nodes in place.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class GatherNodes
    {
        public const string RootName = "GW2 Gather Nodes";
        const long PieceModel = 499021;   // Raw Candy Corn 499015, Rich Candy Corn Vein 499019: see the header
        const int RawCount = 8, RichCount = 2;
        const float MinSpacing = 25f, ClearRadius = 1.2f, FloorTolerance = 2.5f, Step = 4f, Extent = 195f;

        static readonly int[] StrikeIds = { 12912, 12913, 12914, 12915, 12916 };
        static readonly int[] ImpactIds = { 46562, 46563, 46564, 46565, 46566, 51949, 51950, 51946, 51947, 51948 };
        static readonly int[] SpentIds = { 60707, 60708, 60709, 60710, 60711 };

        /// Gather visuals hide and scale at runtime, so imported prefab static flags are invalid here.
        /// Exempt these few small interactables from occlusion, including stale baked visibility data.
        public static void MakeVisualDynamic(GameObject visual)
        {
            if (visual == null) return;
            foreach (Transform child in visual.GetComponentsInChildren<Transform>(true))
                if (GameObjectUtility.GetStaticEditorFlags(child.gameObject) != 0)
                    GameObjectUtility.SetStaticEditorFlags(child.gameObject, 0);
            foreach (Renderer renderer in visual.GetComponentsInChildren<Renderer>(true))
            {
                renderer.allowOcclusionWhenDynamic = false;
                SceneSetup.ProbesOnly(renderer);
            }
        }

        [MenuItem("Castlemist/Candy Corn Node Culling (in place)")]
        public static void CullingMenu()
        {
            Type nodeType = SceneSetup.FindType("Castlemist.GW2GatherNode");
            if (nodeType == null) { Debug.LogWarning("CASTLEMIST GW2GatherNode is not compiled"); return; }
            var scene = EditorSceneManager.GetActiveScene();
            int count = 0;
            foreach (Component node in Object.FindObjectsOfType(nodeType, true).Cast<Component>())
            {
                if (node.gameObject.scene != scene) continue;
                var visual = nodeType.GetField("visual")?.GetValue(node) as GameObject;
                if (visual == null) continue;
                foreach (Transform child in visual.GetComponentsInChildren<Transform>(true))
                    Undo.RecordObject(child.gameObject, "Repair gather node culling");
                foreach (Renderer renderer in visual.GetComponentsInChildren<Renderer>(true))
                    Undo.RecordObject(renderer, "Repair gather node culling");
                MakeVisualDynamic(visual);
                foreach (Transform child in visual.GetComponentsInChildren<Transform>(true))
                    if (PrefabUtility.IsPartOfPrefabInstance(child.gameObject)) PrefabUtility.RecordPrefabInstancePropertyModifications(child.gameObject);
                foreach (Renderer renderer in visual.GetComponentsInChildren<Renderer>(true))
                    if (PrefabUtility.IsPartOfPrefabInstance(renderer)) PrefabUtility.RecordPrefabInstancePropertyModifications(renderer);
                ++count;
            }
            EditorSceneManager.MarkSceneDirty(scene);
            Debug.Log("CASTLEMIST gather node culling repaired: " + count + " dynamic visual(s), occlusion disabled");
        }

        [MenuItem("Castlemist/Candy Corn Node Sounds (in place)")]
        public static void SoundsMenu() { Debug.Log("CASTLEMIST " + Sounds()); }

        /// The mining sounds on every gather node in the open scene, where it stands.
        public static string Sounds()
        {
            GameObject env = ImmersiveLighting.EnvRoot();
            if (env == null) return "gather node sounds: no GW2_<id>_Environment root in the open scene";
            string root = Path.GetDirectoryName(env.scene.path).Replace('\\', '/');
            Type nodeType = SceneSetup.FindType("Castlemist.GW2GatherNode");
            if (nodeType == null) return "gather node sounds: GW2GatherNode not compiled";
            int n = 0;
            foreach (Component c in Object.FindObjectsOfType(nodeType, true).Cast<Component>())
            {
                if (!AddSounds(c, root)) return "gather node sounds: no clips in " + root + "/sounds/gather (gw2dat_cli audio-extract)";
                ++n;
            }
            EditorSceneManager.MarkSceneDirty(env.scene);
            return "gather node sounds: " + n + " node(s)";
        }

        static AudioClip[] Clips(string root, int[] ids)
        {
            return ids.Select(id => AssetDatabase.LoadAssetAtPath<AudioClip>(root + "/sounds/gather/" + id + ".mp3")).Where(c => c != null).ToArray();
        }

        static bool AddSounds(Component comp, string root)
        {
            AudioClip[] strike = Clips(root, StrikeIds), impact = Clips(root, ImpactIds), spent = Clips(root, SpentIds);
            if (strike.Length == 0) return false;
            AudioSource src = comp.GetComponent<AudioSource>();
            if (src == null) src = comp.gameObject.AddComponent<AudioSource>();
            src.playOnAwake = false; src.loop = false; src.spatialBlend = 1f;
            src.rolloffMode = AudioRolloffMode.Logarithmic; src.minDistance = 2f; src.maxDistance = 25f;
            src.volume = 0.8f;
            SceneSetup.SetField(comp, "strikeSound", src);
            SceneSetup.SetField(comp, "strikeClips", strike);
            SceneSetup.SetField(comp, "impactClips", impact);
            SceneSetup.SetField(comp, "spentClips", spent);
            SceneSetup.CopyProxyToUdon(comp);
            return true;
        }

        [MenuItem("Castlemist/Candy Corn Nodes (gatherable)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Setup()); }

        public static string Setup()
        {
            GameObject env = ImmersiveLighting.EnvRoot();
            if (env == null) return "gather nodes: no GW2_<id>_Environment root in the open scene";
            if (!long.TryParse(env.name.Replace("GW2_", "").Replace("_Environment", ""), out long mapId)) return "gather nodes: cannot read the map id";
            GameObject mapRoot = env.scene.GetRootGameObjects().FirstOrDefault(g => g.name == "GW2_" + mapId);
            string root = Path.GetDirectoryName(env.scene.path).Replace('\\', '/');
            var made = new List<string>();
            if (!Demolishables.EnsurePrograms(made)) return "gather nodes: UdonSharp is not in this project";
            if (made.Count > 0) return "gather nodes: UdonSharp program assets made: run again once UdonSharp has compiled";
            Type nodeType = SceneSetup.FindType("Castlemist.GW2GatherNode");
            if (nodeType == null || mapRoot == null) return "gather nodes: GW2GatherNode not compiled, or no GW2_" + mapId + " root";
            GameObject piece = Prefab(root, PieceModel);
            if (piece == null) return "gather nodes: no prefab for " + PieceModel + " (refresh it first)";

            Transform old = env.transform.Find(RootName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            var group = new GameObject(RootName);
            group.transform.SetParent(env.transform, false);
            group.transform.localScale = new Vector3(1f / env.transform.lossyScale.x, 1f / env.transform.lossyScale.y, 1f / env.transform.lossyScale.z);
            Physics.SyncTransforms();

            List<Vector3> spots = Spots(mapRoot.transform.position, env.transform.Find(HauntedDoors.RootName), RawCount + RichCount);
            Mesh pieceMesh = null; Material pieceMat = null;
            if (piece != null)
            {
                MeshFilter mf = piece.GetComponentsInChildren<MeshFilter>(true).FirstOrDefault(f => f.sharedMesh != null);
                if (mf != null) { pieceMesh = mf.sharedMesh; pieceMat = mf.GetComponent<Renderer>()?.sharedMaterial; }
                else
                {
                    // an animated model stays skinned: its mesh as stored
                    SkinnedMeshRenderer sk = piece.GetComponentsInChildren<SkinnedMeshRenderer>(true).FirstOrDefault(k => k.sharedMesh != null);
                    if (sk != null) { pieceMesh = sk.sharedMesh; pieceMat = sk.sharedMaterial; }
                }
            }
            if (pieceMesh == null) return "gather nodes: " + PieceModel + " has no mesh";
            float scale = mapRoot.transform.lossyScale.x;
            Quaternion up = Upright(pieceMesh, out float length, out float baseOffset);
            var rng = new System.Random((int)mapId);
            for (int i = 0; i < spots.Count; ++i)
            {
                bool isRich = i < RichCount;
                Build(group.transform, isRich ? "Rich Candy Corn Vein" : "Raw Candy Corn", isRich ? 10 : 3, isRich ? RichCorns : RawCorns,
                      spots[i], (float)rng.NextDouble() * 360f, scale, pieceMesh, pieceMat, up, length, baseOffset, nodeType);
            }
            Sounds();
            MapMarkers.Setup();   // 2026-10-27: their ore icons on the GMMenu map
            EditorSceneManager.MarkSceneDirty(env.scene);
            return "gather nodes: " + Math.Min(spots.Count, RichCount) + " Rich Candy Corn Vein(s) and " + Math.Max(0, spots.Count - RichCount) +
                   " Raw Candy Corn on open maze floor (no source lists the game's spots; fixed seed, move them freely)";
        }

        // Corns per node: height (metres), offset from the centre (metres), outward tilt (degrees). Raw:
        // one (the screenshot's lone corn); Rich: a cluster (the screenshot's five).
        static readonly Vector4[] RawCorns = { new Vector4(1.3f, 0f, 0f, 0f) };
        static readonly Vector4[] RichCorns =
        {
            new Vector4(1.45f, 0.05f, 0.05f, 6f), new Vector4(1.15f, 0.42f, -0.1f, 18f), new Vector4(1.05f, -0.38f, 0.22f, 20f),
            new Vector4(0.85f, -0.12f, -0.42f, 24f), new Vector4(0.7f, 0.25f, 0.4f, 28f),
        };

        /// The rotation standing @p m's long axis upright, its narrow end (the white tip) up; its length and
        /// how far its base sits below the origin along that axis (mesh units).
        static Quaternion Upright(Mesh m, out float length, out float baseOffset)
        {
            Vector3[] v = m.vertices;
            Bounds b = m.bounds;
            int ax = b.size.x >= b.size.y && b.size.x >= b.size.z ? 0 : b.size.y >= b.size.z ? 1 : 2;
            float lo = b.min[ax], hi = b.max[ax];
            float len = hi - lo;
            length = len;
            // the spread across the axis near each end: the tip is the narrower
            float SpreadNear(float at)
            {
                float sum = 0f; int n = 0;
                foreach (Vector3 p in v)
                {
                    if (Mathf.Abs(p[ax] - at) > len * 0.2f) continue;
                    Vector3 q = p - b.center; q[ax] = 0f;
                    sum += q.magnitude; ++n;
                }
                return n > 0 ? sum / n : 0f;
            }
            bool tipHigh = SpreadNear(hi) < SpreadNear(lo);
            Vector3 axis = Vector3.zero; axis[ax] = tipHigh ? 1f : -1f;
            baseOffset = tipHigh ? lo : -hi;   // the base's coordinate along the (signed) axis
            return Quaternion.FromToRotation(axis, Vector3.up);
        }

        static GameObject Prefab(string root, long id)
        {
            return AssetDatabase.LoadAssetAtPath<GameObject>(root + "/Prefabs/" + id + ".prefab");
        }

        /// Open floor spots (see the header), spread greedily from a seeded shuffle.
        static List<Vector3> Spots(Vector3 centre, Transform doors, int want)
        {
            var hits = new List<Vector3>();
            for (float x = -Extent; x <= Extent; x += Step)
                for (float z = -Extent; z <= Extent; z += Step)
                {
                    Vector3 o = new Vector3(centre.x + x, 3000f, centre.z + z);
                    if (Physics.Raycast(o, Vector3.down, out RaycastHit h, 6000f, ~0, QueryTriggerInteraction.Ignore) && h.normal.y > 0.9f)
                        hits.Add(h.point);
                }
            if (hits.Count == 0) return new List<Vector3>();
            // the maze floor: the commonest ground height (1 m bins)
            float floor = hits.GroupBy(p => Mathf.Round(p.y)).OrderByDescending(g => g.Count()).First().Key;
            var avoid = new List<Vector3>();
            if (doors != null) foreach (Transform d in doors) avoid.Add(d.position);
            var ok = hits.Where(p => Mathf.Abs(p.y - floor) < FloorTolerance
                                  && !Physics.CheckCapsule(p + Vector3.up * (ClearRadius + 0.2f), p + Vector3.up * 2.5f, ClearRadius, ~0, QueryTriggerInteraction.Ignore)
                                  && avoid.All(a => (a - p).sqrMagnitude > 12f * 12f)).ToList();
            var rng = new System.Random(2499169);
            ok = ok.OrderBy(_ => rng.Next()).ToList();
            var chosen = new List<Vector3>();
            foreach (Vector3 p in ok)
            {
                if (chosen.All(c => (c - p).sqrMagnitude > MinSpacing * MinSpacing)) chosen.Add(p);
                if (chosen.Count >= want) break;
            }
            return chosen;
        }

        static void Build(Transform group, string name, int strikes, Vector4[] corns, Vector3 at, float yaw, float scale,
                          Mesh pieceMesh, Material pieceMat, Quaternion up, float length, float baseOffset, Type nodeType)
        {
            var node = new GameObject(name);
            node.transform.SetParent(group, false);
            node.transform.position = at;
            node.transform.rotation = Quaternion.Euler(0f, yaw, 0f);

            var model = new GameObject("Model");
            model.transform.SetParent(node.transform, false);
            var pieces = new List<GameObject>();
            foreach (Vector4 c in corns)
            {
                var corn = new GameObject("Candy Corn");
                corn.transform.SetParent(model.transform, false);
                float k = c.x / Mathf.Max(length, 1e-4f);   // metres per mesh unit for this corn's height
                Vector3 offset = new Vector3(c.y, 0f, c.z);
                Quaternion tilt = offset.sqrMagnitude > 1e-6f ? Quaternion.AngleAxis(c.w, Vector3.Cross(Vector3.up, offset.normalized)) : Quaternion.identity;
                corn.transform.localRotation = tilt * up;
                corn.transform.localScale = Vector3.one * k;
                // the base on the ground: the mesh's base point lifted to y 0
                corn.transform.localPosition = offset - (tilt * Vector3.up) * (baseOffset * k) - Vector3.up * 0.05f;
                corn.AddComponent<MeshFilter>().sharedMesh = pieceMesh;
                var mr = corn.AddComponent<MeshRenderer>();
                mr.sharedMaterial = pieceMat;
                pieces.Add(corn);
            }

            // the click box and the behaviour stay active while the model is hidden (its respawn timer)
            var click = new GameObject("GW2 Gather Node");
            click.transform.SetParent(node.transform, false);
            click.transform.localPosition = new Vector3(0f, 1f, 0f);
            var box = click.AddComponent<BoxCollider>();
            box.isTrigger = true;
            box.size = new Vector3(2.2f, 2.2f, 2.2f);

            ParticleSystem burst = null;
            if (pieceMesh != null)
            {
                var b = new GameObject("Pieces");
                b.transform.SetParent(click.transform, false);
                burst = b.AddComponent<ParticleSystem>();
                burst.Stop(true, ParticleSystemStopBehavior.StopEmittingAndClear);
                ParticleSystem.MainModule m = burst.main;
                m.playOnAwake = false; m.loop = false; m.duration = 0.2f;
                m.startLifetime = new ParticleSystem.MinMaxCurve(0.9f, 1.4f);
                m.startSpeed = new ParticleSystem.MinMaxCurve(2.5f, 4.5f);
                m.startSize = new ParticleSystem.MinMaxCurve(0.12f / Mathf.Max(length, 1e-4f), 0.2f / Mathf.Max(length, 1e-4f));   // 12-20 cm pieces
                m.startRotation3D = true;
                m.startRotationX = new ParticleSystem.MinMaxCurve(0f, 6.283f);
                m.startRotationY = new ParticleSystem.MinMaxCurve(0f, 6.283f);
                m.startRotationZ = new ParticleSystem.MinMaxCurve(0f, 6.283f);
                m.gravityModifier = 1.2f;
                m.simulationSpace = ParticleSystemSimulationSpace.World;
                m.maxParticles = 24;
                ParticleSystem.EmissionModule em = burst.emission;
                em.rateOverTime = 0f;
                em.SetBursts(new[] { new ParticleSystem.Burst(0f, 8, 12) });
                ParticleSystem.ShapeModule sh = burst.shape;
                sh.shapeType = ParticleSystemShapeType.Cone; sh.angle = 35f; sh.radius = 0.2f;
                b.transform.localRotation = Quaternion.Euler(-90f, 0f, 0f);   // the cone points up
                ParticleSystem.RotationOverLifetimeModule rot = burst.rotationOverLifetime;
                rot.enabled = true; rot.separateAxes = true;
                rot.x = new ParticleSystem.MinMaxCurve(-6f, 6f); rot.y = new ParticleSystem.MinMaxCurve(-6f, 6f); rot.z = new ParticleSystem.MinMaxCurve(-6f, 6f);
                ParticleSystem.SizeOverLifetimeModule size = burst.sizeOverLifetime;
                size.enabled = true;
                size.size = new ParticleSystem.MinMaxCurve(1f, new AnimationCurve(new Keyframe(0f, 1f), new Keyframe(0.75f, 1f), new Keyframe(1f, 0f)));
                var r = b.GetComponent<ParticleSystemRenderer>();
                r.renderMode = ParticleSystemRenderMode.Mesh;
                r.mesh = pieceMesh;
                r.sharedMaterial = pieceMat;
                r.alignment = ParticleSystemRenderSpace.Local;
                r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            }

            MakeVisualDynamic(model);
            Component comp = SceneSetup.AddUdonAware(click, nodeType);
            SceneSetup.SetField(comp, "visual", model);
            if (pieces.Count > 1) SceneSetup.SetField(comp, "pieces", pieces.ToArray());
            SceneSetup.SetField(comp, "burst", burst);
            SceneSetup.SetField(comp, "clickCollider", box);
            SceneSetup.SetField(comp, "strikes", strikes);
            SceneSetup.CopyProxyToUdon(comp);
        }
    }
}
