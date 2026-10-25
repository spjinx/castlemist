// castlemist skill effects: gw2dat_cli skillfx-export folder -> Unity prefabs.
//
// Spec docs/superpowers/specs/2026-10-22-skill-effects-design.md section 2, plan
// docs/superpowers/plans/2026-10-22-skill-effects-stage2.md Task 3. Built entirely from the world
// importer's public pieces (Castlemist.Editor), so an effect model comes out as a world prop would:
//   models/<id>/      CastlemistMaterials.BuildModelMaterial per mesh material, the .fbx with
//                     CastlemistPostprocessor's settings and the WorldImporter remap name rule,
//                     then ParticleEffects.AddTo for <id>_particles.json
// What it writes, under <folder>/Unity/:
//   Materials/<id>/<FileName>.mat, Materials/Particles/<id>/*.mat (Castlemist/GW2 Particle)
//   Effects/<id>.prefab       the model (or an empty root) + its particle systems, GW2 units
//   Skills/<id>-<slug>.prefab one per skill: localScale = UnityScale (GW2 inches -> metres,
//                             WorldImporter's default, UNPROVEN), one instance per effect
// Re-import updates the same assets (GUIDs kept). Never edits tools/unity/Castlemist.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist.SkillFx
{
    public static class SkillFxImporter
    {
        public const float UnityScale = 0.0254f;

        [MenuItem("Castlemist/Import Skill Effects...")]
        static void Menu()
        {
            string abs = EditorUtility.OpenFolderPanel("skillfx-export folder (under Assets/)", Application.dataPath, "");
            if (string.IsNullOrEmpty(abs)) return;
            string rel = ToAssetPath(abs);
            if (rel == null) { EditorUtility.DisplayDialog("Castlemist", "Pick a folder under Assets/.", "OK"); return; }
            Log(Import(rel));
        }

        /// Batchmode: -executeMethod Castlemist.SkillFx.SkillFxImporter.BatchImport
        ///   -castlemistSkillFx Assets/<folder> [-castlemistSkills 44364,41258]
        public static void BatchImport()
        {
            string[] a = Environment.GetCommandLineArgs();
            string folder = Arg(a, "-castlemistSkillFx");
            string only = Arg(a, "-castlemistSkills");
            List<long> ids = only?.Split(',').Select(long.Parse).ToList();
            ImportSummary sum = Import(folder, ids);
            Log(sum);
            EditorApplication.Exit(sum.errors.Count == 0 ? 0 : 1);
        }

        /// Imports the skills of @p assetFolder (all, or @p onlySkills).
        public static ImportSummary Import(string assetFolder, IList<long> onlySkills = null)
        {
            var sum = new ImportSummary();
            string root = assetFolder.Replace('\\', '/').TrimEnd('/');
            SkillSetFile set = SkillFxFiles.ReadSkillSet(File.ReadAllText(root + "/skill_set.json"));
            var ctx = MaterialContext.Create(sum.warnings, ShaderMode.Castlemist);
            ctx.Uv1InUv4 = true;          // skillfx-export writes models as the world export does
            ctx.UnityScale = UnityScale;
            sum.shaderLine = ctx.Line;
            string outRoot = root + "/Unity";
            EnsureFolder(outRoot + "/Materials/Particles");
            // ParticleEffects reuses a particle .mat already on its shader: mark the existing ones
            // stale (another shader), so each is rebuilt in place once, with the current mapping
            // (same asset, same GUID; deleting them mid-import left the prefabs' references null).
            Shader stale = Shader.Find("Hidden/InternalErrorShader");
            foreach (string g in AssetDatabase.FindAssets("t:Material", new[] { outRoot + "/Materials/Particles" }))
            {
                var m = AssetDatabase.LoadAssetAtPath<Material>(AssetDatabase.GUIDToAssetPath(g));
                if (m != null && stale != null && m.shader == ctx.ParticleShader) m.shader = stale;
            }
            EnsureFolder(outRoot + "/Effects");
            EnsureFolder(outRoot + "/Skills");

            var effects = new Dictionary<long, GameObject>();   // model -> Effects/<id>.prefab
            int notCopied = 0;
            foreach (SkillRef sr in set.Skills)
            {
                if (onlySkills != null && !onlySkills.Contains(sr.Id)) continue;
                string skillPath = root + "/" + sr.Folder + "/skill.json";
                if (!File.Exists(skillPath))
                {
                    // A subset copy (README step 2) is normal; a skill asked for by id is not.
                    if (onlySkills != null) sum.errors.Add(skillPath + ": missing");
                    else ++notCopied;
                    continue;
                }
                SkillFile skill = SkillFxFiles.ReadSkill(File.ReadAllText(skillPath));

                var go = new GameObject(sr.Id + " " + skill.Label);
                try
                {
                    go.transform.localScale = Vector3.one * UnityScale;
                    foreach (EffectRef e in skill.Effects)
                    {
                        if (e.Model == 0) continue;
                        if (string.IsNullOrEmpty(e.Folder))
                        {
                            sum.warnings.Add("skill " + sr.Id + ": effect " + e.Codename + " (model " + e.Model + ") not exported: " + e.Error);
                            continue;
                        }
                        GameObject prefab;
                        if (!effects.TryGetValue(e.Model, out prefab))
                            effects[e.Model] = prefab = BuildEffect(root, e.Folder, e.Model, ctx, outRoot, sum);
                        if (prefab == null) continue;
                        var inst = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
                        inst.name = e.Model + (string.IsNullOrEmpty(e.EiName) ? "" : " " + e.EiName);
                        inst.transform.SetParent(go.transform, false);
                    }
                    string path = outRoot + "/Skills/" + sr.Id + "-" + SkillFxFiles.Slug(skill.Label) + ".prefab";
                    bool ok;
                    PrefabUtility.SaveAsPrefabAsset(go, path, out ok);
                    if (!ok) sum.errors.Add(path + ": prefab not saved");
                    else sum.prefabs++;
                }
                finally { Object.DestroyImmediate(go); }
            }
            if (notCopied > 0)
                sum.warnings.Add(notCopied + " skill(s) in skill_set.json have no folder here (a subset copy): skipped");
            // Particle .mats an earlier import left directly in Materials/Particles (before the
            // per-model folders): nothing references them once every effect is rebuilt.
            if (onlySkills == null)
                foreach (string f in Directory.GetFiles(outRoot + "/Materials/Particles", "*.mat"))
                    AssetDatabase.DeleteAsset(f.Replace('\\', '/'));
            AssetDatabase.SaveAssets();
            return sum;
        }

        /// One effect model -> Effects/<id>.prefab: its materials, the .fbx (or an empty root) and
        /// its particle systems. Null when it cannot be built (the reason is in @p sum).
        static GameObject BuildEffect(string root, string folderRel, long id, MaterialContext ctx, string outRoot, ImportSummary sum)
        {
            string folder = root + "/" + folderRel;
            string matsPath = folder + "/materials.json";
            if (!File.Exists(matsPath)) { sum.errors.Add(matsPath + ": missing"); return null; }
            MaterialsFile mf;
            try { mf = MaterialsFile.Parse(File.ReadAllText(matsPath), matsPath); }
            catch (ManifestException ex) { sum.errors.Add(ex.Message); return null; }

            // Mesh materials, as WorldImporter.Materials (no shared keys, no dark decals, no scroll).
            string matFolder = outRoot + "/Materials/" + id;
            EnsureFolder(matFolder);
            var remap = new Dictionary<string, Material>(StringComparer.Ordinal);
            var seen = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (MaterialsFile.MaterialEntry me in mf.Materials)
            {
                int k;
                seen.TryGetValue(me.Name, out k);
                seen[me.Name] = k + 1;
                if (!me.UsedByMeshes) continue;
                Material mat = CastlemistMaterials.BuildModelMaterial(ctx, me, folder, false, null, matFolder + "/" + Safe(me.FileName) + ".mat");
                foreach (string key in FbxMaterialNames(me, k)) remap[key] = mat;
                sum.materials++;
            }

            GameObject inst;
            string fbx = folder + "/" + id + ".fbx";
            if (File.Exists(fbx))
            {
                var mi = AssetImporter.GetAtPath(fbx) as ModelImporter;
                if (mi == null) { sum.errors.Add(fbx + ": not imported as a model (refresh the AssetDatabase)"); return null; }
                CastlemistPostprocessor.ApplyModelSettings(mi, folderRel + "/" + id + ".fbx");
                mi.importCameras = false;
                mi.importLights = false;
                foreach (KeyValuePair<string, Material> kv in remap)
                    mi.AddRemap(new AssetImporter.SourceAssetIdentifier(typeof(Material), kv.Key), kv.Value);
                mi.animationType = ModelImporterAnimationType.None;   // skill effects: no clips yet (stage 3+)
                mi.importAnimation = false;
                mi.SaveAndReimport();
                inst = (GameObject)PrefabUtility.InstantiatePrefab(AssetDatabase.LoadAssetAtPath<GameObject>(fbx));
            }
            else
            {
                sum.warnings.Add("model " + id + ": no .fbx (skillfx-export --fbx): particles only");
                inst = new GameObject(id.ToString());
            }
            try
            {
                inst.name = id.ToString();
                string sidecar = ParticleEffects.SidecarFor(folder, id);
                // One particle folder per model: materials.json names repeat across models for
                // different textures (Mat_19092 is four textures in five Firebrand models), and
                // ParticleEffects keys the .mat by that name.
                string ptcFolder = outRoot + "/Materials/Particles/" + id;
                EnsureFolder(ptcFolder);
                if (sidecar != null)
                    sum.particleSystems += ParticleEffects.AddTo(inst, sidecar, mf, folder, ctx, ptcFolder, sum.warnings);
                string path = outRoot + "/Effects/" + id + ".prefab";
                bool ok;
                GameObject saved = PrefabUtility.SaveAsPrefabAsset(inst, path, out ok);
                if (!ok) { sum.errors.Add(path + ": prefab not saved"); return null; }
                return saved;
            }
            finally { Object.DestroyImmediate(inst); }
        }

        /// The names the .fbx may give material @p me, the k-th (from 0) of that name in
        /// materials.json: WorldImporter's rule (Blender names glTF materials, "<name>.001", 63 bytes).
        static IEnumerable<string> FbxMaterialNames(MaterialsFile.MaterialEntry me, int k)
        {
            string suffix = k > 0 ? "." + k.ToString("D3") : "";
            string b = CutUtf8(me.Name, 63 - suffix.Length) + suffix;
            if (k == 0) yield return me.Name;
            yield return me.FileName;
            yield return b;
            yield return b.Replace(':', '_');
        }

        static string CutUtf8(string s, int maxBytes)
        {
            if (System.Text.Encoding.UTF8.GetByteCount(s) <= maxBytes) return s;
            int n = s.Length;
            while (n > 0 && System.Text.Encoding.UTF8.GetByteCount(s.Substring(0, n)) > maxBytes) --n;
            if (n > 0 && char.IsHighSurrogate(s[n - 1])) --n;
            return s.Substring(0, n);
        }

        public static void Log(ImportSummary sum)
        {
            foreach (string w in sum.warnings) Debug.LogWarning("CASTLEMIST skillfx: " + w);
            foreach (string e in sum.errors) Debug.LogError("CASTLEMIST skillfx: " + e);
            Debug.Log("CASTLEMIST skillfx: " + sum.prefabs + " skill prefab(s), " + sum.materials + " material(s), " +
                      sum.particleSystems + " particle system(s), " + sum.errors.Count + " error(s); " + sum.shaderLine);
        }

        static string Safe(string s)
        {
            foreach (char c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
            return s;
        }

        static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            int slash = path.LastIndexOf('/');
            EnsureFolder(path.Substring(0, slash));
            AssetDatabase.CreateFolder(path.Substring(0, slash), path.Substring(slash + 1));
        }

        static string ToAssetPath(string abs)
        {
            string a = abs.Replace('\\', '/'), data = Application.dataPath.Replace('\\', '/');
            return a.StartsWith(data, StringComparison.OrdinalIgnoreCase) ? "Assets" + a.Substring(data.Length) : null;
        }

        static string Arg(string[] a, string name)
        {
            int i = Array.IndexOf(a, name);
            return i >= 0 && i + 1 < a.Length ? a[i + 1] : null;
        }
    }
}
