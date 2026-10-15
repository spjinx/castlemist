// castlemist world export: batchmode entry points.
//
//   Unity.exe -batchmode -nographics -quit -projectPath <project>
//     -executeMethod Castlemist.BatchEntry.CheckProbe -castlemistFile Assets/probe.fbx
//     [-castlemistNoBakeAxis] -logFile -
//
// CheckProbe imports the probe .fbx with ModelImporter.bakeAxisConversion =
// FrameProbe.BakeAxisConversion (on; -castlemistNoBakeAxis turns it off to
// reproduce the failing case), instantiates it in a scratch scene, logs
// FrameProbe.Check's PROBE line and exits with 0 (OK) or 1.

using System;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Castlemist
{
    public static class BatchEntry
    {
        public static void CheckProbe()
        {
            int code = 1;
            try
            {
                string file = Arg("-castlemistFile");
                if (string.IsNullOrEmpty(file)) throw new ArgumentException("-castlemistFile <Assets/...fbx> is required");
                bool bake = FrameProbe.BakeAxisConversion && !HasArg("-castlemistNoBakeAxis");

                AssetDatabase.ImportAsset(file, ImportAssetOptions.ForceUpdate);
                var importer = AssetImporter.GetAtPath(file) as ModelImporter;
                if (importer == null) throw new ArgumentException("not a model: " + file);
                // Readable, so FrameProbe can walk the triangles and normals.
                if (importer.bakeAxisConversion != bake || !importer.isReadable)
                {
                    importer.bakeAxisConversion = bake;
                    importer.isReadable = true;
                    importer.SaveAndReimport();
                }

                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(file);
                if (asset == null) throw new ArgumentException("no GameObject in " + file);
                EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                var root = (GameObject)UnityEngine.Object.Instantiate(asset);
                root.name = asset.name;

                string line;
                bool ok = FrameProbe.Check(root, out line);
                Debug.Log("CASTLEMIST bakeAxisConversion=" + (bake ? "1" : "0") + " unity=" + Application.unityVersion);
                Debug.Log(line);
                foreach (Transform t in root.GetComponentsInChildren<Transform>(true))
                    Debug.Log("PROBE_NODE " + t.name + " rot=" + t.localEulerAngles.ToString("F1") +
                              " scale=" + t.localScale.ToString("F3"));
                code = ok ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogError("PROBE ERROR " + e.Message);
                code = 1;
            }
            EditorApplication.Exit(code);
        }

        static string Arg(string name)
        {
            string[] a = Environment.GetCommandLineArgs();
            for (int i = 0; i + 1 < a.Length; ++i)
                if (a[i] == name) return a[i + 1];
            return null;
        }

        static bool HasArg(string name)
        {
            return Array.IndexOf(Environment.GetCommandLineArgs(), name) >= 0;
        }
    }
}
