// Castlemist > Sun Glow (2026-10-29): a soft halo around the sun / moon (Shaders/GW2 Sun Glow) on the
// open scene, as "GW2 Sun Glow" under the map root, in place of Silent's Sunshine Flare: any renderer
// using "Silent/Sunshine Flare" is switched off (kept in the scene). The quad mesh lives in the scene
// with bounds large enough never to be culled (the shader places it on the light's direction). One
// material per scene, <scene>_SunGlow.mat, made once and then left to the user's tuning. Rerunning
// replaces the object. Setup Immersive Lighting runs it too.

using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class SunGlow
    {
        public const string ObjectName = "GW2 Sun Glow";
        public const string ShaderName = "Castlemist/GW2 Sun Glow";
        public const string SilentShader = "Silent/Sunshine Flare";

        [MenuItem("Castlemist/Sun Glow (soft halo, replaces the lens flare)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Apply()); }

        public static string Apply()
        {
            Shader sh = Shader.Find(ShaderName);
            if (sh == null) return "sun glow: shader " + ShaderName + " not found";
            Light sun = RenderSettings.sun;
            if (sun == null) return "sun glow: no RenderSettings.sun";
            Transform root = sun.transform.parent != null ? sun.transform.parent : null;

            Material mat = null;
            string scenePath = sun.gameObject.scene.path;
            if (!string.IsNullOrEmpty(scenePath))
            {
                string matPath = System.IO.Path.ChangeExtension(scenePath, null) + "_SunGlow.mat";
                mat = AssetDatabase.LoadAssetAtPath<Material>(matPath);
                if (mat == null)
                {
                    mat = new Material(sh);
                    AssetDatabase.CreateAsset(mat, matPath);
                }
            }
            if (mat == null) mat = new Material(sh);

            Transform old = root != null ? root.Find(ObjectName) : null;
            if (old != null) Undo.DestroyObjectImmediate(old.gameObject);
            var go = new GameObject(ObjectName);
            Undo.RegisterCreatedObjectUndo(go, "Sun glow");
            if (root != null) go.transform.SetParent(root, false);
            else SceneManagerMove(go, sun.gameObject);
            var mesh = new Mesh { name = "GW2 Sun Glow Quad" };
            mesh.vertices = new[] { Vector3.zero, Vector3.zero, Vector3.zero, Vector3.zero };
            mesh.uv = new[] { new Vector2(-1, -1), new Vector2(1, -1), new Vector2(1, 1), new Vector2(-1, 1) };
            mesh.triangles = new[] { 0, 2, 1, 0, 3, 2 };
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one * 1e6f);
            go.AddComponent<MeshFilter>().sharedMesh = mesh;
            var r = go.AddComponent<MeshRenderer>();
            r.sharedMaterial = mat;
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            r.receiveShadows = false;
            r.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
            r.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;
            r.allowOcclusionWhenDynamic = false;

            int off = 0;
            foreach (Renderer f in Object.FindObjectsByType<Renderer>(FindObjectsInactive.Exclude, FindObjectsSortMode.None)
                                         .Where(x => x.sharedMaterial != null && x.sharedMaterial.shader != null && x.sharedMaterial.shader.name == SilentShader))
            {
                Undo.RecordObject(f.gameObject, "Sun glow");
                f.gameObject.SetActive(false);
                ++off;
            }
            EditorSceneManager.MarkSceneDirty(go.scene);
            return "sun glow: " + ObjectName + " (" + AssetDatabase.GetAssetPath(mat) + ")" + (off > 0 ? "; " + off + " Sunshine Flare(s) switched off" : "");
        }

        static void SceneManagerMove(GameObject go, GameObject sameSceneAs)
        {
            UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(go, sameSceneAs.scene);
        }
    }
}
