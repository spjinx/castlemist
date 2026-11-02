// castlemist NPC import (2026-11-02): one `gw2dat_cli npc-folder --fbx` model folder (materials.json,
// Textures/, <stem>.fbx, optional <stem>_particles.json) copied into the open map's export folder becomes a
// prefab: the model's materials as WorldImporter builds them, the rig and its clips (looping), an Animator
// on one idle clip, and the model's particle clouds (ParticleEffects). Placed under
// "<env root>/GW2 NPC Models" (metres; Dialogues' "GW2 NPCs" is rebuilt by its menu, this root is not),
// standing on the collision below the given spot. Rerunning replaces the prefab and the placed copy of
// the same name, keeping where it was moved to.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEditor.Animations;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class NpcImporter
    {
        public const string RootName = "GW2 NPC Models";

        [MenuItem("Castlemist/Import NPC Model Folder (at the Scene view)...")]
        static void Menu()
        {
            string abs = EditorUtility.OpenFolderPanel("npc-folder model folder (inside Assets)", Application.dataPath, "");
            if (string.IsNullOrEmpty(abs)) return;
            string rel = ToAsset(abs);
            if (rel == null) { Debug.LogError("CASTLEMIST npc: the folder must be inside Assets/"); return; }
            Vector3 at = SceneView.lastActiveSceneView != null ? SceneView.lastActiveSceneView.pivot : Vector3.zero;
            Debug.Log("CASTLEMIST " + Import(rel, Path.GetFileName(rel), null, at, 0f));
        }

        static string ToAsset(string abs)
        {
            string a = abs.Replace('\\', '/'), d = Application.dataPath.Replace('\\', '/');
            return a.StartsWith(d, StringComparison.OrdinalIgnoreCase) ? "Assets" + a.Substring(d.Length) : null;
        }

        /// The model folder @p folder (an Assets path) as the prefab <folder>/<name>.prefab, placed as
        /// "<name>" at world @p at (dropped onto the collision) turned @p yaw degrees, playing @p idle
        /// (a clip name, trailing blanks ignored; null: "ummidleb", then "idlepose"). A one-line report.
        public static string Import(string folder, string name, string idle, Vector3 at, float yaw)
        {
            var warnings = new List<string>();
            string fbx = Directory.GetFiles(folder, "*.fbx").Select(p => p.Replace('\\', '/')).FirstOrDefault();
            if (fbx == null) return "npc " + name + ": no .fbx in " + folder + " (npc-folder --fbx)";
            string stem = Path.GetFileNameWithoutExtension(fbx);
            string matsPath = folder + "/materials.json";
            if (!File.Exists(matsPath)) return "npc " + name + ": no materials.json";
            MaterialsFile mf;
            try { mf = MaterialsFile.Parse(File.ReadAllText(matsPath), matsPath); }
            catch (ManifestException ex) { return "npc " + name + ": " + ex.Message; }

            GameObject env = ImmersiveLighting.EnvRoot();
            if (env == null) return "npc " + name + ": no GW2_<id>_Environment root in the open scene";
            GameObject mapRoot = env.scene.GetRootGameObjects().FirstOrDefault(g => g.name == env.name.Replace("_Environment", ""));
            float unityScale = mapRoot != null ? mapRoot.transform.localScale.x : 0.0254f;

            // Materials, as WorldImporter.Materials (one .mat per entry, Blender's names remapped).
            var ctx = MaterialContext.Create(warnings, ShaderMode.Castlemist);
            ctx.Uv1InUv4 = true;   // npc-folder exports with uv1CopyToUv4
            ctx.UnityScale = unityScale;
            string matFolder = folder + "/Materials";
            EnsureFolder(matFolder);
            var remap = new Dictionary<string, Material>(StringComparer.Ordinal);
            var seen = new Dictionary<string, int>(StringComparer.Ordinal);
            int mats = 0;
            foreach (MaterialsFile.MaterialEntry me in mf.Materials)
            {
                int k;
                seen.TryGetValue(me.Name, out k);
                seen[me.Name] = k + 1;
                if (!me.UsedByMeshes) continue;
                Material mat = PoiyomiMaterials.BuildModelMaterial(ctx, me, folder, false, null, matFolder + "/" + Safe(me.FileName ?? me.Name) + ".mat");
                string suffix = k > 0 ? "." + k.ToString("D3") : "";
                foreach (string key in new[] { me.Name + suffix, me.FileName, (me.Name + suffix).Replace(':', '_') })
                    if (key != null && !remap.ContainsKey(key)) remap[key] = mat;
                ++mats;
            }

            var mi = AssetImporter.GetAtPath(fbx) as ModelImporter;
            if (mi == null) return "npc " + name + ": " + fbx + " is not imported as a model (refresh the AssetDatabase)";
            CastlemistPostprocessor.ApplyModelSettings(mi, Path.GetFileName(fbx));
            mi.importCameras = false;
            mi.importLights = false;
            foreach (KeyValuePair<string, Material> kv in remap)
                mi.AddRemap(new AssetImporter.SourceAssetIdentifier(typeof(Material), kv.Key), kv.Value);
            foreach (Object sub in AssetDatabase.LoadAllAssetRepresentationsAtPath(fbx))
            {
                if (!(sub is Material fm) || remap.ContainsKey(fm.name)) continue;
                Match mm = Regex.Match(fm.name, @"^(.*)\.\d{3}$");
                if (mm.Success && remap.TryGetValue(mm.Groups[1].Value, out Material target))
                    mi.AddRemap(new AssetImporter.SourceAssetIdentifier(typeof(Material), fm.name), target);
            }
            mi.animationType = ModelImporterAnimationType.Generic;
            mi.importAnimation = true;
            mi.SaveAndReimport();
            ModelImporterClipAnimation[] clips = mi.clipAnimations;
            if (clips == null || clips.Length == 0) clips = mi.defaultClipAnimations;
            foreach (ModelImporterClipAnimation ca in clips) ca.loopTime = true;
            mi.clipAnimations = clips;
            mi.SaveAndReimport();

            AnimationClip clip = null;
            foreach (string want in idle != null ? new[] { idle } : new[] { "ummidleb", "ummidlelong", "idlepose" })
                if ((clip = FindClip(fbx, want)) != null) break;
            if (clip == null) warnings.Add("no idle clip " + (idle ?? "ummidleb/idlepose") + ": bind pose");
            AnimatorController ac = null;
            if (clip != null)
            {
                string acPath = folder + "/" + Safe(name) + ".controller";
                if (AssetDatabase.LoadAssetAtPath<AnimatorController>(acPath) != null) AssetDatabase.DeleteAsset(acPath);
                ac = AnimatorController.CreateAnimatorControllerAtPathWithClip(acPath, clip);
                // Every other clip as its own state beside the idle (the default), for picking in the
                // Animator window or from Udon by state name (the clip's name, trailing blanks trimmed).
                AnimatorStateMachine sm = ac.layers[0].stateMachine;
                var names = new HashSet<string>(sm.states.Select(st => st.state.name), StringComparer.Ordinal);
                int col = 0;
                foreach (AnimationClip c in AssetDatabase.LoadAllAssetsAtPath(fbx).OfType<AnimationClip>()
                                                         .Where(c => !c.name.StartsWith("__preview__", StringComparison.Ordinal) && c != clip)
                                                         .OrderBy(c => ClipName(c), StringComparer.Ordinal))
                {
                    string n = ClipName(c);
                    if (!names.Add(n)) continue;
                    AnimatorState st = sm.AddState(n, new Vector3(300f + 220f * (col / 30), 60f * (col % 30), 0f));
                    st.motion = c;
                    ++col;
                }
                sm.states[0].state.name = ClipName(clip);
            }

            var asset = AssetDatabase.LoadAssetAtPath<GameObject>(fbx);
            var inst = (GameObject)PrefabUtility.InstantiatePrefab(asset);
            GameObject saved;
            int systems = 0;
            try
            {
                inst.name = name;
                // One Animator: the Generic rig's own once the .fbx has reimported, else one added (not ??:
                // a missing component is Unity's fake null). Two left the rig's, with no controller, in charge.
                Animator[] ans = inst.GetComponents<Animator>();
                Animator an = ans.Length > 0 ? ans[0] : inst.AddComponent<Animator>();
                for (int k = 1; k < ans.Length; ++k) Object.DestroyImmediate(ans[k]);
                an.runtimeAnimatorController = ac;
                an.cullingMode = AnimatorCullingMode.CullUpdateTransforms;
                if (ac != null) WorldImporter.FitSkinnedBounds(inst, ac);
                Lods(inst);
                long id;
                string sidecar = long.TryParse(stem, out id) ? ParticleEffects.SidecarFor(folder, id) : null;
                if (sidecar != null)
                {
                    string ptc = folder + "/Materials/Particles";
                    EnsureFolder(ptc);
                    systems = ParticleEffects.AddTo(inst, sidecar, mf, folder, ctx, ptc, warnings);
                }
                bool ok;
                saved = PrefabUtility.SaveAsPrefabAsset(inst, folder + "/" + Safe(name) + ".prefab", out ok);
                if (!ok || saved == null) return "npc " + name + ": prefab not saved";
            }
            finally { Object.DestroyImmediate(inst); }

            // Placed in metres under the env root; a copy already there keeps its spot and turn.
            Transform root = env.transform.Find(RootName);
            if (root == null)
            {
                root = new GameObject(RootName).transform;
                root.SetParent(env.transform, false);
                Vector3 ls = env.transform.lossyScale;
                root.localScale = new Vector3(1f / ls.x, 1f / ls.y, 1f / ls.z);
            }
            Transform old = root.Find(name);
            Vector3 pos;
            Quaternion rot;
            if (old != null) { pos = old.position; rot = old.rotation; Object.DestroyImmediate(old.gameObject); }
            else
            {
                Physics.SyncTransforms();
                pos = Physics.Raycast(at + Vector3.up * 50f, Vector3.down, out RaycastHit hit, 500f, ~0, QueryTriggerInteraction.Ignore) ? hit.point : at;
                rot = Quaternion.Euler(0f, yaw, 0f) * saved.transform.rotation;
            }
            var go = (GameObject)PrefabUtility.InstantiatePrefab(saved, root);
            go.name = name;
            go.transform.position = pos;
            go.transform.rotation = rot;
            go.transform.localScale = saved.transform.localScale * unityScale;
            // As WorldImporter's animated props: the controller set on the placed copy (on the saved
            // model-prefab variant the reference read back empty, 2026-11-02).
            Animator placed = go.GetComponent<Animator>();
            if (placed != null) placed.runtimeAnimatorController = ac;
            // The Animator plays only in Play mode: the placed copy stands in the idle's first frame
            // in the editor too, not the bind pose.
            if (clip != null) clip.SampleAnimation(go, 0f);
            Undo.RegisterCreatedObjectUndo(go, "Import NPC " + name);
            EditorSceneManager.MarkSceneDirty(env.scene);
            AssetDatabase.SaveAssets();
            return "npc " + name + ": " + mats + " materials, " + clips.Length + " clips, idle " + (clip != null ? ClipName(clip) : "none") +
                   ", " + systems + " particle systems, at " + pos.ToString("F1") +
                   (warnings.Count > 0 ? " | " + string.Join(" | ", warnings.Distinct().Take(8)) : "");
        }

        /// The meshes' "<name>_LOD<n>" copies (write_vrchat_folder exportLods) as one LODGroup: without it
        /// every level drew at once. Levels past the model's own fall back to the last it has.
        static void Lods(GameObject inst)
        {
            var levels = new SortedDictionary<int, List<Renderer>>();
            foreach (Renderer r in inst.GetComponentsInChildren<Renderer>(true))
            {
                Match lm = Regex.Match(r.name, @"_LOD(\d+)$");
                int n = lm.Success ? int.Parse(lm.Groups[1].Value) : 0;
                if (!levels.TryGetValue(n, out List<Renderer> list)) levels[n] = list = new List<Renderer>();
                list.Add(r);
            }
            if (levels.Count < 2) return;
            var lods = new List<LOD>();
            int i = 0;
            foreach (KeyValuePair<int, List<Renderer>> kv in levels)
            {
                // A part with no copy at this level keeps its last one (coat_2 has no LOD1).
                var rs = new List<Renderer>(kv.Value);
                foreach (KeyValuePair<int, List<Renderer>> lower in levels)
                {
                    if (lower.Key >= kv.Key) break;
                    foreach (Renderer r in lower.Value)
                    {
                        string bare = Regex.Replace(r.name, @"_LOD\d+$", "");
                        bool has = levels.Where(l => l.Key > lower.Key && l.Key <= kv.Key).Any(l => l.Value.Any(x => Regex.Replace(x.name, @"_LOD\d+$", "") == bare));
                        if (!has && !rs.Contains(r)) rs.Add(r);
                    }
                }
                lods.Add(new LOD(0.30f * Mathf.Pow(0.4f, i), rs.ToArray()));   // 30 %, 12 %, 4.8 % of the screen
                ++i;
            }
            LODGroup g = inst.GetComponent<LODGroup>();
            if (g == null) g = inst.AddComponent<LODGroup>();
            g.SetLODs(lods.ToArray());
            g.RecalculateBounds();
        }

        static AnimationClip FindClip(string fbx, string want)
        {
            want = want.Trim();
            foreach (AnimationClip c in AssetDatabase.LoadAllAssetsAtPath(fbx).OfType<AnimationClip>())
            {
                if (c.name.StartsWith("__preview__", StringComparison.Ordinal)) continue;
                if (ClipName(c) == want) return c;
            }
            return null;
        }

        /// The GW2 clip name of @p c: Blender's "<armature>|" prefix and the padding blanks dropped.
        static string ClipName(AnimationClip c)
        {
            string n = c.name.Trim();
            int bar = n.LastIndexOf('|');
            return (bar >= 0 ? n.Substring(bar + 1) : n).Trim();
        }

        static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            string parent = Path.GetDirectoryName(path).Replace('\\', '/');
            EnsureFolder(parent);
            AssetDatabase.CreateFolder(parent, Path.GetFileName(path));
        }

        static string Safe(string s)
        {
            foreach (char c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
            return s.Trim();
        }
    }
}
