// castlemist world import: a map's GW2 music playlist as world music (2026-10-26). GW2 picks a map's
// music by its music script's conditions (AMSP 105242, bytecode: not decoded); the playlists themselves
// are named in the global audio settings (AMSP 14897, MusicPlaylistData), so each map's table here names
// its playlist with that playlist's files and timing as stored. The files are the decoded audio
// (gw2dat_cli audio-extract) at <export>/music/<playlist>/<fileId>.mp3; Runtime/GW2MusicPlayer plays them.
//   Mad King's Labyrinth (2499169): "madkingmaze" (ambient; 6 files, initial silence 8-15 s, between
//   tracks 60-180 s, fade out 5 s, volume 0.7079, iterate mode 2).

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
    public static class Music
    {
        public const string ObjectName = "GW2 Music";

        sealed class Playlist
        {
            public string Name;
            public long[] Files;
            public float InitialMin, InitialMax, IntervalMin, IntervalMax, FadeOut, Volume;
        }

        static readonly Dictionary<long, Playlist> Maps = new Dictionary<long, Playlist>
        {
            [2499169] = new Playlist { Name = "madkingmaze", Files = new long[] { 77539, 274721, 274720, 274746, 274677, 77538 },
                                       InitialMin = 8f, InitialMax = 15f, IntervalMin = 60f, IntervalMax = 180f, FadeOut = 5f, Volume = 0.7079f },
        };

        [MenuItem("Castlemist/World Music (the map's GW2 playlist)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Setup()); }

        public static string Setup()
        {
            GameObject env = ImmersiveLighting.EnvRoot();
            if (env == null) return "music: no GW2_<id>_Environment root in the open scene";
            if (!long.TryParse(env.name.Replace("GW2_", "").Replace("_Environment", ""), out long mapId)) return "music: cannot read the map id";
            if (!Maps.TryGetValue(mapId, out Playlist pl)) return "music: map " + mapId + " has no playlist";
            var made = new List<string>();
            if (!Demolishables.EnsurePrograms(made)) return "music: UdonSharp is not in this project";
            if (made.Count > 0) return "music: UdonSharp program assets made: run again once UdonSharp has compiled";
            Type playerType = SceneSetup.FindType("Castlemist.GW2MusicPlayer");
            if (playerType == null) return "music: GW2MusicPlayer is not compiled yet";

            string dir = Path.GetDirectoryName(env.scene.path).Replace('\\', '/') + "/music/" + pl.Name;
            var clips = new List<AudioClip>();
            var missing = new List<long>();
            foreach (long f in pl.Files)
            {
                string path = new[] { ".mp3", ".ogg", ".wav" }.Select(x => dir + "/" + f + x).FirstOrDefault(File.Exists);
                if (path == null) { missing.Add(f); continue; }
                if (AssetImporter.GetAtPath(path) is AudioImporter ai)
                {
                    AudioImporterSampleSettings s = ai.defaultSampleSettings;
                    if (s.loadType != AudioClipLoadType.Streaming || s.compressionFormat != AudioCompressionFormat.Vorbis)
                    {
                        s.loadType = AudioClipLoadType.Streaming;   // long tracks: not held in memory
                        s.compressionFormat = AudioCompressionFormat.Vorbis;
                        s.quality = 0.6f;
                        ai.defaultSampleSettings = s;
                        ai.loadInBackground = true;
                        ai.SaveAndReimport();
                    }
                }
                AudioClip c = AssetDatabase.LoadAssetAtPath<AudioClip>(path);
                if (c != null) clips.Add(c);
            }
            if (clips.Count == 0) return "music: no files in " + dir + " (gw2dat_cli audio-extract --file-ids " + string.Join(",", pl.Files) + " --out-dir <that folder>)";

            Transform old = env.transform.Find(ObjectName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            var go = new GameObject(ObjectName);
            go.transform.SetParent(env.transform, false);
            var src = go.AddComponent<AudioSource>();
            src.playOnAwake = false; src.loop = false; src.spatialBlend = 0f; src.priority = 0;
            Component comp = SceneSetup.AddUdonAware(go, playerType);
            SceneSetup.SetField(comp, "source", src);
            SceneSetup.SetField(comp, "clips", clips.ToArray());
            SceneSetup.SetField(comp, "weights", clips.Select(_ => 1f).ToArray());
            SceneSetup.SetField(comp, "volume", pl.Volume);
            SceneSetup.SetField(comp, "initialMin", pl.InitialMin);
            SceneSetup.SetField(comp, "initialMax", pl.InitialMax);
            SceneSetup.SetField(comp, "intervalMin", pl.IntervalMin);
            SceneSetup.SetField(comp, "intervalMax", pl.IntervalMax);
            SceneSetup.SetField(comp, "fadeOut", pl.FadeOut);
            SceneSetup.CopyProxyToUdon(comp);
            EditorSceneManager.MarkSceneDirty(env.scene);
            return "music: \"" + pl.Name + "\", " + clips.Count + " track(s) (silence " + pl.InitialMin + "-" + pl.InitialMax + " s, then " + pl.IntervalMin + "-" + pl.IntervalMax +
                   " s between tracks)" + (missing.Count > 0 ? "; missing " + string.Join(", ", missing) : "");
        }
    }
}
