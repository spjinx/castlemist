// castlemist world export: typed reader for manifest.json and materials.json.
//
// Spec docs/superpowers/specs/2026-10-15-world-export-design.md, "manifest.json
// (version 1)" and D6 steps 1-2; the schema is
// tools/world/castlemist-world.schema.json. materials.json is the VRChat model
// export's (docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md §4).
//
// Rules the importer relies on (world export Task 7 carry-ins):
//  - instance.model is a position in the manifest's models array;
//  - instance unity.t is in map units relative to the cell origin, Unity axes;
//    unityScale is applied once, on the map root (ruling W7);
//  - unity.r is a Unity quaternion (x, y, z, w);
//  - light, water, frameProbe, sky, spawn and map.bounds may be null;
//  - foliage (the world-foliage block) and cells[].foliage are optional; the
//    records themselves are read by FoliageRecords;
//  - unityTerrain (world-foliage D7) is optional: Terrain mode needs it;
//  - environment (2026-10-22, the env chunk: lights, haze, effect, zones) is
//    optional and null for a map without one; inside it every block may be
//    null and every effect field absent (the map's struct version). Read raw;
//    EnvironmentMath (end of this file) turns it into Unity settings.
// An unknown format or major version, or a missing required field, throws
// ManifestException naming the field. No UnityEngine dependency (the
// tools/unity/Tests~ harness compiles it with plain dotnet).

using System;
using System.Collections.Generic;
using System.Globalization;

namespace Castlemist
{
    public sealed class ManifestException : Exception
    {
        public ManifestException(string message) : base(message) { }
    }

    /// Typed access to a MiniJson object, with the path in every error.
    public sealed class JObj
    {
        public readonly Dictionary<string, object> Raw;
        public readonly string Path;

        public JObj(Dictionary<string, object> raw, string path) { Raw = raw; Path = path; }

        public static JObj Root(object parsed, string what)
        {
            var d = parsed as Dictionary<string, object>;
            if (d == null) throw new ManifestException(what + ": top level is not an object");
            return new JObj(d, what);
        }

        public bool Has(string k) { return Raw.ContainsKey(k); }
        public bool IsNull(string k) { object v; return !Raw.TryGetValue(k, out v) || v == null; }

        object Need(string k)
        {
            object v;
            if (!Raw.TryGetValue(k, out v)) throw new ManifestException(Path + "." + k + ": missing");
            return v;
        }

        public JObj Obj(string k)
        {
            var d = Need(k) as Dictionary<string, object>;
            if (d == null) throw new ManifestException(Path + "." + k + ": not an object");
            return new JObj(d, Path + "." + k);
        }

        /// Null when the key is absent or null.
        public JObj OptObj(string k)
        {
            object v;
            if (!Raw.TryGetValue(k, out v) || v == null) return null;
            var d = v as Dictionary<string, object>;
            if (d == null) throw new ManifestException(Path + "." + k + ": not an object");
            return new JObj(d, Path + "." + k);
        }

        public List<object> Arr(string k)
        {
            var a = Need(k) as List<object>;
            if (a == null) throw new ManifestException(Path + "." + k + ": not an array");
            return a;
        }

        public List<object> OptArr(string k)
        {
            object v;
            if (!Raw.TryGetValue(k, out v) || v == null) return new List<object>();
            var a = v as List<object>;
            if (a == null) throw new ManifestException(Path + "." + k + ": not an array");
            return a;
        }

        public IEnumerable<JObj> Objs(string k)
        {
            List<object> a = OptArr(k);
            for (int i = 0; i < a.Count; ++i)
            {
                var d = a[i] as Dictionary<string, object>;
                if (d == null) throw new ManifestException(Path + "." + k + "[" + i + "]: not an object");
                yield return new JObj(d, Path + "." + k + "[" + i + "]");
            }
        }

        public string Str(string k)
        {
            var s = Need(k) as string;
            if (s == null) throw new ManifestException(Path + "." + k + ": not a string");
            return s;
        }

        public string OptStr(string k)
        {
            object v;
            if (!Raw.TryGetValue(k, out v) || v == null) return null;
            var s = v as string;
            if (s == null) throw new ManifestException(Path + "." + k + ": not a string");
            return s;
        }

        public double Num(string k)
        {
            object v = Need(k);
            if (!(v is double)) throw new ManifestException(Path + "." + k + ": not a number");
            return (double)v;
        }

        public double OptNum(string k, double fallback)
        {
            object v;
            if (!Raw.TryGetValue(k, out v) || v == null) return fallback;
            if (!(v is double)) throw new ManifestException(Path + "." + k + ": not a number");
            return (double)v;
        }

        public long Long(string k)
        {
            double d = Num(k);
            if (d != Math.Floor(d) || Math.Abs(d) > 9007199254740992.0)
                throw new ManifestException(Path + "." + k + ": not an integer");
            return (long)d;
        }

        public int Int(string k)
        {
            long l = Long(k);
            if (l < int.MinValue || l > int.MaxValue) throw new ManifestException(Path + "." + k + ": out of range");
            return (int)l;
        }

        public bool Bool(string k, bool fallback)
        {
            object v;
            if (!Raw.TryGetValue(k, out v) || v == null) return fallback;
            if (!(v is bool)) throw new ManifestException(Path + "." + k + ": not a boolean");
            return (bool)v;
        }

        public double[] Vec(string k, int n)
        {
            return ToVec(Need(k), n, Path + "." + k);
        }

        public double[] OptVec(string k, int n)
        {
            object v;
            if (!Raw.TryGetValue(k, out v) || v == null) return null;
            return ToVec(v, n, Path + "." + k);
        }

        public static double[] ToVec(object v, int n, string path)
        {
            var a = v as List<object>;
            if (a == null || a.Count != n) throw new ManifestException(path + ": not an array of " + n + " numbers");
            var r = new double[n];
            for (int i = 0; i < n; ++i)
            {
                if (!(a[i] is double)) throw new ManifestException(path + "[" + i + "]: not a number");
                r[i] = (double)a[i];
            }
            return r;
        }

        public List<string> Strings(string k)
        {
            var r = new List<string>();
            List<object> a = OptArr(k);
            for (int i = 0; i < a.Count; ++i)
            {
                var s = a[i] as string;
                if (s == null) throw new ManifestException(Path + "." + k + "[" + i + "]: not a string");
                r.Add(s);
            }
            return r;
        }
    }

    public sealed class WorldManifest
    {
        public const string Format = "castlemist-world";
        /// The major version this importer knows (spec "Versioning").
        public const int KnownVersion = 1;
        /// spec D7: "GW2 inches" (gltf_export.h:57), UNPROVEN.
        public const double DefaultUnityScale = 0.0254;

        public int Version;
        public long MapFileId;
        /// frame.units.unityScale.value (UNPROVEN, spec D7).
        public double UnityScale = DefaultUnityScale;
        /// frame.toUnity, row-major: unity = ToUnity * map.
        public double[,] ToUnity = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } };
        public bool TerrainPresent;
        public bool DarkDecalRule = true;
        /// settings.uv1InUv4: the models' meshes carry GW2's UV1 in UV4 too (false in an older export).
        public bool Uv1InUv4;
        public readonly List<ModelEntry> Models = new List<ModelEntry>();
        public readonly List<CellEntry> Cells = new List<CellEntry>();
        public WaterEntry Water;   // null: no water block
        public SkyEntry Sky;       // null: no sky
        public LightEntry Light;   // null: not finite or absent
        public double[] SpawnUnity;  // null: no spawn hint
        public ProbeEntry FrameProbe;  // null: no probe
        public EnvironmentEntry Environment;  // null: no environment block, or null in the export
        public FoliageBlock Foliage;   // null: no foliage block (world-foliage D7)
        public UnityTerrainBlock UnityTerrain;  // null: no unityTerrain block (world-foliage D7)
        /// sharedMaterials.entries (Task 15d); empty for an export before it.
        public readonly List<SharedMaterial> SharedMaterials = new List<SharedMaterial>();
        /// settings.zoneFadeQualityScale.value (1 when absent); UnityTerrain.FadeQualityScale wins when present.
        public double ZoneFadeQualityScale = 1.0;
        public readonly List<string> Warnings = new List<string>();

        public sealed class UvScroll
        {
            public string Material;
            public int UvIndex;
            public double U, V;
        }

        public sealed class ModelEntry
        {
            /// models[].index: the scene.models index (not the position on a region export).
            public int Index;
            /// The entry's position in models[]: what instances, foliage records and prototypes name.
            public int Position;
            public long FileId;
            public string Folder, Glb, Fbx, Materials, Status, Error;
            /// models[].triangles (0 when absent): the patch split's vertex bound (FoliagePlan.PatchCapacity).
            public long Triangles;
            public List<string> Clips = new List<string>();
            public List<string> DarkDecalMaterials = new List<string>();
            public List<UvScroll> UvScroll = new List<UvScroll>();
            /// models[].lods (Task 15b): LodLevels 1 when absent (no LODGroup);
            /// LodDistances the model's switch distances, model units (null when absent).
            public int LodLevels = 1;
            public double[] LodDistances;
            /// models[].lods.farMeshes: the FBX object names of the model's "lod" stand-in meshes
            /// (mesh flag 0x4), drawn as its last level only (settings.lodFarMeshes); empty when none.
            public List<string> FarMeshes = new List<string>();
            /// models[].lods.hiddenMeshes: the "lod01" stand-ins (mesh flag 0x1) of a model that also
            /// has a flag-4 one; never drawn. Empty when none.
            public List<string> HiddenMeshes = new List<string>();
            /// models[].materialKeys (Task 15d): the key of each materials.json entry; empty when absent.
            public List<string> MaterialKeys = new List<string>();
            public bool Ok { get { return Status == "ok"; } }
        }

        /// sharedMaterials.entries[] (Task 15d): one per material key.
        public sealed class SharedMaterial
        {
            public string Key, Name;
            public int Model, Material, Slots;
            public bool UsedByMeshes;
        }

        public sealed class InstanceEntry
        {
            public long Prop;
            public int Model;
            public string Group;
            public double[] T;   // map units, cell-relative, Unity axes
            public double[] R;   // Unity quaternion x, y, z, w
            public double S;
            public string Clip;  // null: static
            public double[] M;   // map space, column-major, absolute (null if absent)
            public double[] Lod; // instances[].lod (Task 15b): its own switch distances, model units; null = the model's
        }

        public sealed class CellEntry
        {
            public string Id;
            public double[] Origin;  // map space
            public string Glb, Fbx, Albedo, WaterTexture;
            public bool HasTerrain;
            public int ColliderParts;
            public readonly List<InstanceEntry> Instances = new List<InstanceEntry>();
            /// cells[].foliage (world-foliage D7); FoliageFile null: no records.
            public string FoliageFile;
            public int FoliageRecords, FoliageColliderTrees;
            /// rect (map x0, y0, x1, y1; null when absent) and chunks (cx0, cy0, cx1, cy1; null for a cell without terrain).
            public double[] Rect;
            public int[] Chunks;
        }

        public sealed class FoliageLayer
        {
            public int Index, Type;
            public double FadeFar;  // fadeRange[1]
            /// Manifest models[] positions by slot; -1 for an unused slot.
            public readonly List<int> Models = new List<int>();
        }

        public sealed class FoliageModel
        {
            public int Model;
            public double FadeFar;
            /// foliage.models[].terrainDetail: one mesh, one material, types 1-2 (a Unity detail in Terrain mode).
            public bool TerrainDetail;
        }

        /// unityTerrain (world-foliage D7): positions and sizes in Unity axes before unityScale, map units.
        public sealed class UnityTerrainBlock
        {
            public int TileChunks = 4, HeightmapResolution = 129, DetailResolution = 256;
            public double DetailCellUnits = 48, HMin, HMax, FadeQualityScale = 1.0;
            public readonly List<TerrainTile> Tiles = new List<TerrainTile>();
            public readonly List<DetailPrototypeEntry> DetailPrototypes = new List<DetailPrototypeEntry>();
            public readonly List<TreePrototypeEntry> TreePrototypes = new List<TreePrototypeEntry>();
            /// unityTerrain.check as parsed: [{ "unity": [x, z], "height": y }], map units before unityScale.
            public List<object> Check = new List<object>();

            public int DetailPrototypeOf(int model)
            {
                for (int i = 0; i < DetailPrototypes.Count; ++i) if (DetailPrototypes[i].Model == model) return i;
                return -1;
            }

            public int TreePrototypeOf(int model)
            {
                for (int i = 0; i < TreePrototypes.Count; ++i) if (TreePrototypes[i].Model == model) return i;
                return -1;
            }

            /// The tile whose block holds chunk (cx, cy), or null.
            public TerrainTile TileOfChunk(int cx, int cy)
            {
                int bx = (cx / TileChunks) * TileChunks, by = (cy / TileChunks) * TileChunks;
                foreach (TerrainTile t in Tiles) if (t.Chunks[0] == bx && t.Chunks[1] == by) return t;
                return null;
            }
        }

        public sealed class TerrainTile
        {
            public string Id, Heights, Holes, Albedo, TreesFile;
            public int[] Chunks;             // cx0, cy0, cx1, cy1 (the real chunks)
            public double[] Position, Size;  // x, up, z
            public int TreesRecords;
            public readonly List<TileDetail> Details = new List<TileDetail>();

            /// The real (unpadded) area, map x0, y0, x1, y1 (= Unity x, z).
            public double[] Real(int tileChunks)
            {
                double cw = Size[0] / tileChunks, cd = Size[2] / tileChunks;
                double x0 = Position[0], z1 = Position[2] + Size[2];
                return new[] { x0, z1 - (Chunks[3] - Chunks[1] + 1) * cd, x0 + (Chunks[2] - Chunks[0] + 1) * cw, z1 };
            }
        }

        public sealed class TileDetail
        {
            public int Prototype, Records;
            public string Counts;
        }

        public sealed class DetailPrototypeEntry
        {
            public int Model;
            public double MinScale = 1, MaxScale = 1, FadeFar;
        }

        public sealed class TreePrototypeEntry
        {
            public int Model;
            public double FadeFar;
        }

        /// The importers' fade factor (ruling S5): unityTerrain.fadeQualityScale, else settings.zoneFadeQualityScale, else 1.
        public double FadeQualityScale { get { return UnityTerrain != null ? UnityTerrain.FadeQualityScale : ZoneFadeQualityScale; } }

        static int[] Ints(double[] v)
        {
            if (v == null) return null;
            var r = new int[v.Length];
            for (int i = 0; i < v.Length; ++i) r[i] = (int)v[i];
            return r;
        }

        public sealed class FoliageBlock
        {
            public string Scatter;
            public readonly List<FoliageLayer> Layers = new List<FoliageLayer>();
            public readonly List<FoliageModel> Models = new List<FoliageModel>();

            /// foliage.models[].fadeFar for a model, else the largest far value of
            /// the layers naming it, else 0.
            public double FadeFar(int model)
            {
                foreach (FoliageModel fm in Models) if (fm.Model == model) return fm.FadeFar;
                double f = 0;
                foreach (FoliageLayer l in Layers) if (l.Models.Contains(model)) f = Math.Max(f, l.FadeFar);
                return f;
            }
        }

        public sealed class WaterEntry
        {
            public bool Plane;
            public double Z;
        }

        public sealed class SkyEntry
        {
            public string Json, Default, Equirect;  // Equirect may be null
        }

        public sealed class LightEntry
        {
            public bool Present;
            public int PresetIndex;
            public double[] SunUnityForward, SunColor, Ambient, FillColor;
            public double SunIntensity, FillIntensity;
        }

        /// One light of environment.lights ([0] the sun, [1..] the fills) or the backlight.
        public sealed class EnvLight
        {
            public double[] Color;          // 0..1 RGB
            public double Intensity;
            public double[] UnityForward;   // the direction the light travels, Unity frame; null for the backlight
        }

        public sealed class HazeEntry
        {
            public double[] Near, Far, Height;           // 0..1 RGBA; null when absent
            public double[] DistRange;                   // [near, far], GW2 units; null when absent
            public double[] HeightRange, HeightRangeUnity, SunDirRange;
            public double DepthCue;
        }

        public sealed class ClutLayout
        {
            public int Width, Height, Size;
            public string Kind;   // e.g. "strip"
        }

        /// environment.effect; a field absent from the map's struct version is null.
        public sealed class EffectEntry
        {
            public double[] Glow, GlowLevel, TintColor, TintTargetColor;   // RGBA
            public double? GlowAmplify, TintAmount, TintFocus, Saturation, FocalDepth, FocalRange,
                           SsaoAmount, SsaoBrighten, SsaoContrast, SsaoSunScale;
            public long? Flags, ClutFileId;
            public string Clut;            // PNG path relative to the export folder, or null
            public ClutLayout ClutLayout;  // null when absent
        }

        public sealed class EnvZone
        {
            public string Name;
            public HazeEntry Haze;
            public double[] BoundsUnity;   // [minX, minZ, maxX, maxZ], GW2 units, Unity frame
        }

        /// environment.wind: dataGlobal->wind[] of the day slot, the raw bytes (gustSpeed null before V30).
        public sealed class WindEntry
        {
            public int Azimuth, Elevation, Noise, Speed, Gust, GustFreq;
            public int? GustSpeed;
        }

        public sealed class EnvironmentEntry
        {
            public int Preset;
            public WindEntry Wind;       // null
            public readonly List<EnvLight> Lights = new List<EnvLight>();
            public double ShadowInfluence;
            public EnvLight Backlight;   // null
            public HazeEntry Haze;       // null
            public EffectEntry Effect;   // null
            public readonly List<EnvZone> Zones = new List<EnvZone>();
        }

        static double? OptNumN(JObj o, string k) { return o.IsNull(k) ? (double?)null : o.Num(k); }

        static HazeEntry ParseHaze(JObj h)
        {
            if (h == null) return null;
            return new HazeEntry
            {
                Near = h.OptVec("near", 4), Far = h.OptVec("far", 4), Height = h.OptVec("height", 4),
                DistRange = h.OptVec("distRange", 2), HeightRange = h.OptVec("heightRange", 2),
                HeightRangeUnity = h.OptVec("heightRangeUnity", 2), SunDirRange = h.OptVec("sunDirRange", 2),
                DepthCue = h.OptNum("depthCue", 0),
            };
        }

        static EnvironmentEntry ParseEnvironment(JObj e)
        {
            var env = new EnvironmentEntry { Preset = (int)e.OptNum("preset", 0), ShadowInfluence = e.OptNum("shadowInfluence", 0) };
            foreach (JObj l in e.Objs("lights"))
                env.Lights.Add(new EnvLight { Color = l.Vec("color", 3), Intensity = l.OptNum("intensity", 0), UnityForward = l.OptVec("unityForward", 3) });
            JObj bl = e.OptObj("backlight");
            if (bl != null) env.Backlight = new EnvLight { Color = bl.Vec("color", 3), Intensity = bl.OptNum("intensity", 0) };
            env.Haze = ParseHaze(e.OptObj("haze"));
            JObj w = e.OptObj("wind");
            if (w != null)
                env.Wind = new WindEntry
                {
                    Azimuth = (int)w.OptNum("azimuth", 0), Elevation = (int)w.OptNum("elevation", 0), Noise = (int)w.OptNum("noise", 0),
                    Speed = (int)w.OptNum("speed", 0), Gust = (int)w.OptNum("gust", 0), GustFreq = (int)w.OptNum("gustFreq", 0),
                    GustSpeed = w.IsNull("gustSpeed") ? (int?)null : (int)w.Num("gustSpeed"),
                };
            JObj fx = e.OptObj("effect");
            if (fx != null)
            {
                var f = new EffectEntry
                {
                    Glow = fx.OptVec("glow", 4), GlowLevel = fx.OptVec("glowLevel", 4),
                    TintColor = fx.OptVec("tintColor", 4), TintTargetColor = fx.OptVec("tintTargetColor", 4),
                    GlowAmplify = OptNumN(fx, "glowAmplify"), TintAmount = OptNumN(fx, "tintAmount"), TintFocus = OptNumN(fx, "tintFocus"),
                    Saturation = OptNumN(fx, "saturation"), FocalDepth = OptNumN(fx, "focalDepth"), FocalRange = OptNumN(fx, "focalRange"),
                    SsaoAmount = OptNumN(fx, "ssaoAmount"), SsaoBrighten = OptNumN(fx, "ssaoBrighten"),
                    SsaoContrast = OptNumN(fx, "ssaoContrast"), SsaoSunScale = OptNumN(fx, "ssaoSunScale"),
                    Flags = fx.IsNull("flags") ? (long?)null : fx.Long("flags"),
                    ClutFileId = fx.IsNull("clutFileId") ? (long?)null : fx.Long("clutFileId"),
                    Clut = fx.OptStr("clut"),
                };
                JObj cl = fx.OptObj("clutLayout");
                if (cl != null)
                    f.ClutLayout = new ClutLayout { Width = (int)cl.OptNum("width", 0), Height = (int)cl.OptNum("height", 0),
                                                    Size = (int)cl.OptNum("size", 0), Kind = cl.OptStr("kind") };
                env.Effect = f;
            }
            foreach (JObj z in e.Objs("zones"))
                env.Zones.Add(new EnvZone { Name = z.OptStr("name"), Haze = ParseHaze(z.OptObj("haze")), BoundsUnity = z.OptVec("boundsUnity", 4) });
            return env;
        }

        public sealed class ProbeEntry
        {
            public string Glb, Fbx;   // Fbx may be null
            public double[] Map, Unity;  // Unity: map_to_unity(map), root-relative map units
        }

        public static WorldManifest Parse(string text)
        {
            object parsed;
            try { parsed = MiniJson.Parse(text); }
            catch (FormatException e) { throw new ManifestException("manifest.json: " + e.Message); }
            JObj r = JObj.Root(parsed, "manifest");

            var m = new WorldManifest();
            string format = r.OptStr("format");
            if (format != Format)
                throw new ManifestException("manifest.format: '" + format + "' is not '" + Format + "'");
            m.Version = r.Int("version");
            if (m.Version != KnownVersion)
                throw new ManifestException("manifest.version: major version " + m.Version +
                                            " is not known to this importer (knows " + KnownVersion + ")");

            m.MapFileId = r.Obj("map").Long("fileId");

            JObj frame = r.OptObj("frame");
            if (frame != null)
            {
                if (frame.Has("toUnity"))
                {
                    List<object> rows = frame.Arr("toUnity");
                    if (rows.Count != 3) throw new ManifestException("manifest.frame.toUnity: not 3x3");
                    for (int i = 0; i < 3; ++i)
                    {
                        double[] row = JObj.ToVec(rows[i], 3, "manifest.frame.toUnity[" + i + "]");
                        for (int j = 0; j < 3; ++j) m.ToUnity[i, j] = row[j];
                    }
                }
                JObj units = frame.OptObj("units");
                JObj us = units != null ? units.OptObj("unityScale") : null;
                if (us != null) m.UnityScale = us.OptNum("value", DefaultUnityScale);
            }

            JObj settings = r.OptObj("settings");
            JObj dd = settings != null ? settings.OptObj("darkDecalRule") : null;
            if (dd != null) m.DarkDecalRule = dd.Bool("value", true);
            JObj u4 = settings != null ? settings.OptObj("uv1InUv4") : null;
            if (u4 != null) m.Uv1InUv4 = u4.Bool("value", false);
            JObj fq = settings != null ? settings.OptObj("zoneFadeQualityScale") : null;
            if (fq != null) m.ZoneFadeQualityScale = fq.OptNum("value", 1.0);

            JObj terrain = r.OptObj("terrain");
            m.TerrainPresent = terrain != null && terrain.Bool("present", false);

            foreach (JObj mo in r.Objs("models"))
            {
                var e = new ModelEntry
                {
                    Index = mo.Int("index"),
                    Position = m.Models.Count,
                    FileId = mo.Long("fileId"),
                    Folder = mo.OptStr("folder"),
                    Glb = mo.OptStr("glb"),
                    Fbx = mo.OptStr("fbx"),
                    Materials = mo.OptStr("materials"),
                    Status = mo.Str("status"),
                    Error = mo.OptStr("error"),
                    Triangles = (long)mo.OptNum("triangles", 0),
                    Clips = mo.Strings("clips"),
                    DarkDecalMaterials = mo.Strings("darkDecalMaterials"),
                    MaterialKeys = mo.Strings("materialKeys"),
                };
                JObj lods = mo.OptObj("lods");
                if (lods != null)
                {
                    e.LodLevels = (int)lods.OptNum("levels", 1);
                    if (e.LodLevels < 1 || e.LodLevels > 3) throw new ManifestException(lods.Path + ".levels: " + e.LodLevels + " is not 1..3");
                    e.LodDistances = lods.Vec("distances", 2);
                    e.FarMeshes = lods.Strings("farMeshes");
                    e.HiddenMeshes = lods.Strings("hiddenMeshes");
                }
                foreach (JObj u in mo.Objs("uvScroll"))
                {
                    double[] sc = u.Vec("scroll", 2);
                    e.UvScroll.Add(new UvScroll { Material = u.Str("material"), UvIndex = (int)u.OptNum("uvIndex", 0), U = sc[0], V = sc[1] });
                }
                m.Models.Add(e);
            }

            foreach (JObj c in r.Objs("cells"))
            {
                var cell = new CellEntry
                {
                    Id = c.Str("id"),
                    Origin = c.Vec("origin", 3),
                    Glb = c.OptStr("glb"),
                    Fbx = c.OptStr("fbx"),
                    Albedo = c.OptStr("albedo"),
                    HasTerrain = m.TerrainPresent && !c.IsNull("chunks"),
                    ColliderParts = (int)c.OptNum("colliderParts", 0),
                    Rect = c.OptVec("rect", 4),
                    Chunks = Ints(c.OptVec("chunks", 4)),
                };
                JObj w = c.OptObj("water");
                if (w != null) cell.WaterTexture = w.OptStr("texture");
                JObj fo = c.OptObj("foliage");
                if (fo != null)
                {
                    cell.FoliageFile = fo.Str("file");
                    cell.FoliageRecords = fo.Int("records");
                    cell.FoliageColliderTrees = (int)fo.OptNum("colliderTrees", 0);
                }
                foreach (JObj inst in c.Objs("instances"))
                {
                    JObj u = inst.Obj("unity");
                    cell.Instances.Add(new InstanceEntry
                    {
                        Prop = inst.Long("prop"),
                        Model = inst.Int("model"),
                        Group = inst.OptStr("group"),
                        T = u.Vec("t", 3),
                        R = u.Vec("r", 4),
                        S = u.Num("s"),
                        Clip = inst.OptStr("clip"),
                        M = inst.OptVec("m", 16),
                        Lod = inst.OptVec("lod", 2),
                    });
                }
                m.Cells.Add(cell);
            }

            JObj sm = r.OptObj("sharedMaterials");
            if (sm != null)
                foreach (JObj e in sm.Objs("entries"))
                    m.SharedMaterials.Add(new SharedMaterial
                    {
                        Key = e.Str("key"), Name = e.OptStr("name"), Model = e.Int("model"), Material = e.Int("material"),
                        Slots = (int)e.OptNum("slots", 0), UsedByMeshes = e.Bool("usedByMeshes", true),
                    });

            JObj fol = r.OptObj("foliage");
            if (fol != null)
            {
                m.Foliage = new FoliageBlock { Scatter = fol.OptStr("scatter") };
                foreach (JObj l in fol.Objs("layers"))
                {
                    var layer = new FoliageLayer { Index = l.Int("index"), Type = l.Int("type"), FadeFar = l.Vec("fadeRange", 2)[1] };
                    List<object> slots = l.OptArr("models");
                    for (int i = 0; i < slots.Count; ++i)
                    {
                        if (slots[i] == null) { layer.Models.Add(-1); continue; }
                        if (!(slots[i] is double)) throw new ManifestException(l.Path + ".models[" + i + "]: not a number");
                        layer.Models.Add((int)(double)slots[i]);
                    }
                    m.Foliage.Layers.Add(layer);
                }
                foreach (JObj fm in fol.Objs("models"))
                    m.Foliage.Models.Add(new FoliageModel { Model = fm.Int("model"), FadeFar = fm.OptNum("fadeFar", 0),
                                                            TerrainDetail = fm.Bool("terrainDetail", false) });
            }

            JObj ut = r.OptObj("unityTerrain");
            if (ut != null)
            {
                var b = new UnityTerrainBlock
                {
                    TileChunks = ut.Int("tileChunks"),
                    HeightmapResolution = ut.Int("heightmapResolution"),
                    DetailResolution = ut.Int("detailResolution"),
                    DetailCellUnits = ut.OptNum("detailCellUnits", 48),
                    FadeQualityScale = ut.OptNum("fadeQualityScale", m.ZoneFadeQualityScale),
                };
                if (b.TileChunks < 1) throw new ManifestException("manifest.unityTerrain.tileChunks: must be positive");
                JObj hr = ut.Obj("heightRange");
                b.HMin = hr.Num("min");
                b.HMax = hr.Num("max");
                foreach (JObj t in ut.Objs("tiles"))
                {
                    var tile = new TerrainTile
                    {
                        Id = t.Str("id"),
                        Chunks = Ints(t.Vec("chunks", 4)),
                        Position = t.Vec("position", 3),
                        Size = t.Vec("size", 3),
                        Heights = t.Str("heights"),
                        Holes = t.OptStr("holes"),
                        Albedo = t.Str("albedo"),
                    };
                    foreach (JObj d in t.Objs("details"))
                        tile.Details.Add(new TileDetail { Prototype = d.Int("prototype"), Counts = d.Str("counts"), Records = d.Int("records") });
                    JObj tr = t.OptObj("trees");
                    if (tr != null) { tile.TreesFile = tr.Str("file"); tile.TreesRecords = tr.Int("records"); }
                    b.Tiles.Add(tile);
                }
                foreach (JObj d in ut.Objs("detailPrototypes"))
                    b.DetailPrototypes.Add(new DetailPrototypeEntry { Model = d.Int("model"), MinScale = d.OptNum("minScale", 1),
                                                                      MaxScale = d.OptNum("maxScale", 1), FadeFar = d.OptNum("fadeFar", 0) });
                foreach (JObj d in ut.Objs("treePrototypes"))
                    b.TreePrototypes.Add(new TreePrototypeEntry { Model = d.Int("model"), FadeFar = d.OptNum("fadeFar", 0) });
                b.Check = ut.OptArr("check");
                m.UnityTerrain = b;
            }

            JObj water = r.OptObj("water");
            if (water != null) m.Water = new WaterEntry { Plane = water.Bool("plane", false), Z = water.OptNum("z", 0) };

            JObj sky = r.OptObj("sky");
            if (sky != null) m.Sky = new SkyEntry { Json = sky.Str("json"), Default = sky.Str("default"), Equirect = sky.OptStr("equirect") };

            JObj light = r.OptObj("light");
            if (light != null)
            {
                JObj sun = light.Obj("sun"), fill = light.Obj("fill");
                m.Light = new LightEntry
                {
                    Present = light.Bool("present", true),
                    PresetIndex = (int)light.OptNum("presetIndex", 0),
                    SunUnityForward = sun.Vec("unityForward", 3),
                    SunColor = sun.Vec("color", 3),
                    SunIntensity = sun.Num("intensity"),
                    FillColor = fill.Vec("color", 3),
                    FillIntensity = fill.Num("intensity"),
                };
                // spec D5: ambient = fillColor * min(fillIntensity, 1); written by the export.
                m.Light.Ambient = light.OptVec("ambient", 3);
                if (m.Light.Ambient == null)
                {
                    double k = Math.Min(m.Light.FillIntensity, 1.0);
                    m.Light.Ambient = new[] { m.Light.FillColor[0] * k, m.Light.FillColor[1] * k, m.Light.FillColor[2] * k };
                }
            }

            JObj spawn = r.OptObj("spawn");
            if (spawn != null) m.SpawnUnity = spawn.Vec("unity", 3);

            JObj env = r.OptObj("environment");
            if (env != null) m.Environment = ParseEnvironment(env);

            JObj probe = r.OptObj("frameProbe");
            if (probe != null)
            {
                m.FrameProbe = new ProbeEntry { Glb = probe.Str("glb"), Fbx = probe.OptStr("fbx"), Map = probe.Vec("map", 3) };
                m.FrameProbe.Unity = probe.OptVec("unity", 3) ?? m.MapToUnity(m.FrameProbe.Map);
            }

            m.Warnings.AddRange(r.Strings("warnings"));
            return m;
        }

        /// frame.toUnity applied to a map-space vector (no scale).
        public double[] MapToUnity(double[] p)
        {
            var r = new double[3];
            for (int i = 0; i < 3; ++i) r[i] = ToUnity[i, 0] * p[0] + ToUnity[i, 1] * p[1] + ToUnity[i, 2] * p[2];
            return r;
        }

        public static string Fmt(double v)
        {
            return v.ToString("R", CultureInfo.InvariantCulture);
        }
    }

    /// materials.json of one model folder (VRChat model export §4).
    public sealed class MaterialsFile
    {
        public string Poiyomi;
        public readonly List<MaterialEntry> Materials = new List<MaterialEntry>();

        public sealed class MapRef
        {
            public string File;  // relative to the model folder (a world export's: ../../textures/gen/<sha1>_<Role>.png)
            public int Uv;
        }

        public sealed class BlendRaw
        {
            // bgfx factors (1 Zero ... 10 InvDstColor, 0 unset) and equations
            // (0 Add, 1 Sub, 2 RevSub, 3 Min, 4 Max): exportgltf/blend_mode.h.
            public int SrcRgb, DstRgb, SrcA, DstA, EqRgb, EqA;
        }

        public sealed class MaterialEntry
        {
            public string Name, FileName, Preset, RenderPreset, Cull;
            /// The material's content key (Task 15d); null in an export before it.
            public string Key;
            public bool UsedByMeshes = true, Exact = true;
            /// The game shades it without lighting (fog only); false in an export before the field.
            public bool Unlit;
            public int Mode;
            public BlendRaw Blend;  // null: no game blend word
            public double AlphaCutoff = 0.25;
            public int RenderQueueOffset;
            /// Role ("baseColor", "normal", "packed", "emissionMap", ...) -> map.
            public readonly Dictionary<string, MapRef> Maps = new Dictionary<string, MapRef>();
            public bool HasEmission;
            public double[] EmissionColor;  // sRGB, 0..1
            public double EmissionStrength;
            public double[] SpecularTint, ReflectionTint;  // null: none
            public bool HasRim, HasProjector, HasExtras;
            /// maps.extras[] with a file: the layers castlemist ships raw, with their role and
            /// hinted use ("detail-multiply2x" for a mod layer).
            public readonly List<Extra> Extras = new List<Extra>();
            /// The shader profile castlemist matched ("default" when the AMAT is not in its table).
            public string Profile;
            /// The material's AMAT (GW2 shader) fileId; 0 when the export had none.
            public long Amat;
            /// materials.json cloth: the GW2 cloth using this material (its windiest piece), or null.
            public double? ClothWind, ClothRigidness;
            /// gw2: the named material constants (x); gw2Vectors: their whole float4 (empty in an older export).
            public readonly Dictionary<string, double> Gw2 = new Dictionary<string, double>(StringComparer.Ordinal);
            public readonly Dictionary<string, double[]> Gw2Vectors = new Dictionary<string, double[]>(StringComparer.Ordinal);
            /// texTransforms: the MODL per-UV-set animation, as stored (empty in an older export).
            public readonly List<TexTransform> TexTransforms = new List<TexTransform>();
            /// constAnims: the constants a clip animates (ModelAnimationData.matConstAnim), per clip.
            public readonly List<ConstAnim> ConstAnims = new List<ConstAnim>();

            public Extra ExtraOf(string role) { return Extras.Find(x => x.Role == role); }
            /// The DDS cubemap layers (maps.cubemaps; the water 3121604's projection / reflect).
            public readonly List<Extra> Cubemaps = new List<Extra>();
            public Extra CubeOf(string role) { return Cubemaps.Find(x => x.Role == role); }

            /// maps.layerConstants: layers whose texture is a placeholder for one colour, role -> RGBA 0..1.
            public readonly Dictionary<string, Color32Value> LayerConstants = new Dictionary<string, Color32Value>(StringComparer.Ordinal);

            /// Constant `name` component `c` (0..3): gw2Vectors when present, else gw2 for x, else `fallback`.
            public double Const(string name, int c, double fallback)
            {
                double[] v;
                if (Gw2Vectors.TryGetValue(name, out v) && v != null && c < v.Length) return v[c];
                double x;
                if (c == 0 && Gw2.TryGetValue(name, out x)) return x;
                return fallback;
            }
            public readonly List<string> Warnings = new List<string>();

            public MapRef Map(string role) { MapRef r; return Maps.TryGetValue(role, out r) ? r : null; }
        }

        /// A placeholder layer's constant colour, 0..1 per channel.
        public sealed class Color32Value
        {
            public double R, G, B, A;
            public double Channel(int c) { return c == 0 ? R : c == 1 ? G : c == 2 ? B : A; }
        }

        /// One materials.json constAnims entry: constant `Constant` over clip `Clip` (its glTF
        /// animation name), sampled at `Times` (seconds) as float4 `Values`; one key when the clip
        /// holds it (`Animated` false).
        public sealed class ConstAnim
        {
            public string Clip, Constant;
            public double Duration;
            public bool Animated;
            public double[] Times = new double[0];
            public double[][] Values = new double[0][];
        }

        /// One MODL texTransform: what TexTransform<Uv> animates in the game's vertex shader.
        public sealed class TexTransform
        {
            public int Uv;
            public double[] Scroll = { 0, 0 };
            public double Rotate;
        }

        public sealed class Extra
        {
            public MapRef Map;
            public string Role, Use;
        }

        public static readonly string[] MapRoles =
        {
            "baseColor", "normal", "packed", "emissionMap", "emissionMask", "emissionBaked",
            "distortion", "decal", "decalMask", "alphaMask",
        };

        public static MaterialsFile Parse(string text, string what)
        {
            object parsed;
            try { parsed = MiniJson.Parse(text); }
            catch (FormatException e) { throw new ManifestException(what + ": " + e.Message); }
            JObj r = JObj.Root(parsed, what);
            var f = new MaterialsFile { Poiyomi = r.OptStr("poiyomi") };
            foreach (JObj m in r.Objs("materials"))
            {
                var e = new MaterialEntry
                {
                    Name = m.Str("name"),
                    FileName = m.OptStr("fileName") ?? m.Str("name"),
                    Key = m.OptStr("key"),
                    Preset = m.OptStr("preset"),
                    RenderPreset = m.OptStr("renderPreset"),
                    Cull = m.OptStr("cull") ?? "Back",
                    UsedByMeshes = m.Bool("usedByMeshes", true),
                    Exact = m.Bool("exact", true),
                    Unlit = m.Bool("unlit", false),
                    Mode = (int)m.OptNum("mode", 0),
                    AlphaCutoff = m.OptNum("alphaCutoff", 0.25),
                    RenderQueueOffset = (int)m.OptNum("renderQueueOffset", 0),
                };
                JObj b = m.OptObj("blend");
                if (b != null)
                    e.Blend = new BlendRaw
                    {
                        SrcRgb = (int)b.OptNum("srcRgb", 0), DstRgb = (int)b.OptNum("dstRgb", 0),
                        SrcA = (int)b.OptNum("srcA", 0), DstA = (int)b.OptNum("dstA", 0),
                        EqRgb = (int)b.OptNum("eqRgb", b.OptNum("op", 0)), EqA = (int)b.OptNum("eqA", 0),
                    };
                JObj maps = m.OptObj("maps");
                if (maps != null)
                {
                    foreach (string role in MapRoles)
                    {
                        JObj mr = maps.OptObj(role);
                        if (mr == null || mr.IsNull("file")) continue;
                        e.Maps[role] = new MapRef { File = mr.Str("file"), Uv = (int)mr.OptNum("uv", 0) };
                    }
                    e.HasExtras = maps.OptArr("extras").Count > 0;
                    foreach (JObj lc in maps.Objs("layerConstants"))
                    {
                        double[] v = lc.OptVec("rgba", 4);
                        if (v != null)
                            e.LayerConstants[lc.Str("role")] = new Color32Value { R = v[0] / 255.0, G = v[1] / 255.0, B = v[2] / 255.0, A = v[3] / 255.0 };
                    }
                    foreach (JObj x in maps.Objs("cubemaps"))
                    {
                        if (x.IsNull("file")) continue;
                        e.Cubemaps.Add(new Extra
                        {
                            Map = new MapRef { File = x.Str("file"), Uv = (int)x.OptNum("uv", 0) },
                            Role = x.OptStr("role"),
                        });
                    }
                    foreach (JObj x in maps.Objs("extras"))
                    {
                        if (x.IsNull("file")) continue;
                        e.Extras.Add(new Extra
                        {
                            Map = new MapRef { File = x.Str("file"), Uv = (int)x.OptNum("uv", 0) },
                            Role = x.OptStr("role"),
                            Use = x.OptStr("use"),
                        });
                    }
                }
                JObj em = m.OptObj("emission");
                if (em != null)
                {
                    e.HasEmission = true;
                    e.EmissionColor = em.OptVec("color", 3) ?? new double[] { 1, 1, 1 };
                    e.EmissionStrength = em.OptNum("strength", 1);
                }
                e.Profile = m.OptStr("profile");
                e.Amat = m.IsNull("amat") ? 0 : (long)m.OptNum("amat", 0);
                JObj cloth = m.OptObj("cloth");
                if (cloth != null)
                {
                    e.ClothWind = cloth.OptNum("wind", 0.5);
                    e.ClothRigidness = cloth.OptNum("rigidness", 0);
                }
                JObj g = m.OptObj("gw2");
                if (g != null)
                    foreach (KeyValuePair<string, object> kv in g.Raw)
                        if (kv.Value is double) e.Gw2[kv.Key] = (double)kv.Value;
                JObj gv = m.OptObj("gw2Vectors");
                if (gv != null)
                    foreach (string k in gv.Raw.Keys) e.Gw2Vectors[k] = gv.OptVec(k, 4);
                foreach (JObj t in m.Objs("texTransforms"))
                    e.TexTransforms.Add(new TexTransform
                    {
                        Uv = (int)t.OptNum("uv", 0),
                        Scroll = t.OptVec("scroll", 2) ?? new double[] { 0, 0 },
                        Rotate = t.OptNum("rotate", 0),
                    });
                foreach (JObj a in m.Objs("constAnims"))
                {
                    var ca = new ConstAnim
                    {
                        Clip = a.Str("clip"),
                        Constant = a.Str("constant"),
                        Duration = a.OptNum("duration", 0),
                        Animated = a.Bool("animated", false),
                    };
                    List<object> times = a.OptArr("times"), values = a.OptArr("values");
                    int n = Math.Min(times.Count, values.Count);
                    ca.Times = new double[n];
                    ca.Values = new double[n][];
                    for (int i = 0; i < n; ++i)
                    {
                        ca.Times[i] = times[i] is double d ? d : 0;
                        var v = values[i] as List<object>;
                        ca.Values[i] = new double[4];
                        for (int c = 0; v != null && c < 4 && c < v.Count; ++c) ca.Values[i][c] = v[c] is double x ? x : 0;
                    }
                    e.ConstAnims.Add(ca);
                }
                e.SpecularTint = m.OptVec("specularTint", 3);
                e.ReflectionTint = m.OptVec("reflectionTint", 3);
                e.HasRim = !m.IsNull("rim");
                e.HasProjector = !m.IsNull("projector");
                e.Warnings.AddRange(m.Strings("warnings"));
                f.Materials.Add(e);
            }
            return f;
        }
    }
    /// 2026-10-22: the scene setup's readings of manifest.environment as numbers
    /// (SceneSetup applies them). Every mapping here is a reading of raw GW2 data,
    /// not a measurement against the client: UNPROVEN unless stated. No UnityEngine
    /// dependency (Tests~ checks the maths).
    public static class EnvironmentMath
    {
        static double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
        static bool Finite(double v) { return !double.IsNaN(v) && !double.IsInfinity(v); }

        public sealed class Fog
        {
            public double Start, End;   // Unity units (Linear fog)
            public double[] Color;      // RGB 0..1
        }

        /// Unity Linear fog from haze. UNPROVEN reading: the haze amount is a straight
        /// line through (distRange[0], near.a) and (distRange[1], far.a); Unity's Linear
        /// fog is 0 at fogStartDistance and 1 at fogEndDistance, so start and end are
        /// that line's 0 and 1 crossings (start clamped at 0). When far.a is not above
        /// near.a (the alphas are then not read as amounts) the haze runs 0 -> 1 over
        /// distRange itself. Linear rather than Exponential: the data is a distance
        /// range, and Exponential has no start distance to put distRange[0] in. The one
        /// fog colour is near and far blended by their alphas (far alone when both are
        /// 0): where fog is thick, the far colour dominates. Height haze is not mapped
        /// (Unity's built-in fog has no height term). Null when there is no usable range.
        public static Fog FogFromHaze(WorldManifest.HazeEntry h, double unityScale)
        {
            if (h == null || h.DistRange == null || !(unityScale > 0)) return null;
            double n = h.DistRange[0], f = h.DistRange[1];
            if (!Finite(n) || !Finite(f) || !(f > n) || f <= 0) return null;
            double a0 = h.Near != null ? Clamp01(h.Near[3]) : 0, a1 = h.Far != null ? Clamp01(h.Far[3]) : 1;
            double start = n, end = f;
            if (a1 > a0 + 1e-3)
            {
                double slope = (a1 - a0) / (f - n);
                start = n - a0 / slope;
                end = n + (1 - a0) / slope;
            }
            if (start < 0) start = 0;
            var fog = new Fog { Start = start * unityScale, End = end * unityScale };
            double[] nc = h.Near ?? h.Far, fc = h.Far ?? h.Near;
            if (fc == null) fog.Color = new[] { 0.5, 0.5, 0.5 };
            else
            {
                double w = a0 + a1 > 1e-6 ? a1 / (a0 + a1) : 1;
                fog.Color = new double[3];
                for (int i = 0; i < 3; ++i) fog.Color[i] = Clamp01(nc[i] * (1 - w) + fc[i] * w);
            }
            return fog;
        }

        /// The Castlemist/GW2 Haze volume's _HeightRange (h0, h1): haze.heightRange as stored
        /// (map z, up = -Z); without it, from heightRangeUnity (lo, hi) as (-hi, -lo) --
        /// fromUnity true: UNPROVEN, the stored order is lost. Null when neither is there.
        public static double[] HazeHeightRange(WorldManifest.HazeEntry h, out bool fromUnity)
        {
            fromUnity = false;
            if (h == null) return null;
            if (h.HeightRange != null) return new[] { h.HeightRange[0], h.HeightRange[1] };
            if (h.HeightRangeUnity == null) return null;
            fromUnity = true;
            return new[] { -h.HeightRangeUnity[1], -h.HeightRangeUnity[0] };
        }

        /// Trilight sky / equator / ground (RGB each, may exceed 1) from the fills
        /// (environment.lights[1..]): each fill is a directional light of colour x
        /// intensity travelling along unityForward, evaluated with Lambert on an
        /// unshadowed surface -- up: max(0, L.y), down: max(0, -L.y), horizontal: the
        /// mean of max(0, n.L) over the horizontal ring, sqrt(1 - L.y^2) / pi -- with
        /// L = -unityForward. UNPROVEN (GW2's fill shading is not measured; this is
        /// what Unity's own diffuse would do with the fills as lights). Null when no
        /// fill has a direction.
        public static double[][] TrilightFromFills(IList<WorldManifest.EnvLight> lights)
        {
            if (lights == null) return null;
            var sky = new double[3]; var eq = new double[3]; var ground = new double[3];
            int used = 0;
            for (int k = 1; k < lights.Count; ++k)
            {
                WorldManifest.EnvLight l = lights[k];
                if (l == null || l.UnityForward == null || l.Color == null) continue;
                double x = l.UnityForward[0], y = l.UnityForward[1], z = l.UnityForward[2];
                double len = Math.Sqrt(x * x + y * y + z * z);
                if (!(len > 1e-9) || !Finite(l.Intensity)) continue;
                double ly = -y / len;
                double up = Math.Max(0, ly), down = Math.Max(0, -ly), side = Math.Sqrt(Math.Max(0, 1 - ly * ly)) / Math.PI;
                double k2 = Math.Max(0, l.Intensity);
                for (int i = 0; i < 3; ++i)
                {
                    double c = Math.Max(0, l.Color[i]) * k2;
                    sky[i] += c * up; eq[i] += c * side; ground[i] += c * down;
                }
                ++used;
            }
            return used == 0 ? null : new[] { sky, eq, ground };
        }

        public sealed class Bloom
        {
            public double Intensity, Threshold;
            public double[] Color;   // RGB, max component 1
        }

        /// PPv2 Bloom from effect.glow / glowLevel / glowAmplify. UNPROVEN reading:
        /// glowAmplify (glow.a when absent) is the intensity; the threshold is PPv2's 1 (glowLevel
        /// read as the threshold, 0.07 on the Mists vault, bloomed every surface: 2026-10-22
        /// render), glow's RGB the tint
        /// (normalised to a max of 1; white when black). Null when the intensity is 0.
        public static Bloom BloomFromEffect(WorldManifest.EffectEntry e)
        {
            if (e == null) return null;
            double amp = e.GlowAmplify ?? (e.Glow != null ? e.Glow[3] : 0);
            if (!Finite(amp) || amp <= 0) return null;
            var b = new Bloom { Intensity = Math.Min(amp, 20), Threshold = 1, Color = new[] { 1.0, 1.0, 1.0 } };
            if (e.Glow != null)
            {
                double mx = Math.Max(e.Glow[0], Math.Max(e.Glow[1], e.Glow[2]));
                if (mx > 1e-6) for (int i = 0; i < 3; ++i) b.Color[i] = Clamp01(e.Glow[i] / mx);
            }
            return b;
        }

        /// PPv2 ColorGrading saturation (-100..100, 0 neutral) from GW2's (1 neutral):
        /// (s - 1) x 100. UNPROVEN (assumes a linear lerp from grey, as PPv2's).
        public static double Saturation(double gw2)
        {
            if (!Finite(gw2)) return 0;
            return Math.Max(-100, Math.Min(100, (gw2 - 1) * 100));
        }

        /// PPv2 colour filter: white lerped to tintColor's RGB by tintAmount (0..1).
        /// UNPROVEN (tintTargetColor and tintFocus are not mapped).
        public static double[] ColorFilter(double[] tint, double? amount)
        {
            var c = new[] { 1.0, 1.0, 1.0 };
            if (tint == null || amount == null || !Finite(amount.Value)) return c;
            double a = Clamp01(amount.Value);
            for (int i = 0; i < 3; ++i) c[i] = 1 + (Clamp01(tint[i]) - 1) * a;
            return c;
        }

        /// PPv2 AmbientOcclusion (MSVO) intensity from ssaoAmount: 0.15 x ssaoAmount, at most 0.6.
        /// UNPROVEN: taken 1:1 (the Mists vault's 3) MSVO crushed the scene to black (2026-10-22).
        public static double AoIntensity(double? ssao)
        {
            if (ssao == null || !Finite(ssao.Value)) return 0;
            return Math.Max(0, Math.Min(0.6, 0.15 * ssao.Value));
        }

        /// A usable PPv2 LDR lookup strip (ColorGrading.ldrLut): kind "strip" (or no
        /// layout) and width = height^2, PPv2's own test.
        public static bool IsLutStrip(WorldManifest.ClutLayout l, int width, int height)
        {
            if (l != null && l.Kind != null && l.Kind != "strip") return false;
            return height > 1 && width == height * height;
        }

        /// VRC Light Volumes voxels per Unity unit for a box: as many as fit `budget`
        /// voxels, at most `max` and at least 0.05. Castlemist's choice, not GW2 data.
        public static double VoxelsPerUnit(double sx, double sy, double sz, double budget, double max)
        {
            double vol = Math.Max(1e-6, sx) * Math.Max(1e-6, sy) * Math.Max(1e-6, sz);
            double v = Math.Pow(Math.Max(1, budget) / vol, 1.0 / 3.0);
            return Math.Max(0.05, Math.Min(max, v));
        }
    }
}
