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
//  - unityTerrain (world-foliage D7) is optional: Terrain mode needs it.
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
        public readonly List<ModelEntry> Models = new List<ModelEntry>();
        public readonly List<CellEntry> Cells = new List<CellEntry>();
        public WaterEntry Water;   // null: no water block
        public SkyEntry Sky;       // null: no sky
        public LightEntry Light;   // null: not finite or absent
        public double[] SpawnUnity;  // null: no spawn hint
        public ProbeEntry FrameProbe;  // null: no probe
        public FoliageBlock Foliage;   // null: no foliage block (world-foliage D7)
        public UnityTerrainBlock UnityTerrain;  // null: no unityTerrain block (world-foliage D7)
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
            public bool Ok { get { return Status == "ok"; } }
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
                };
                JObj lods = mo.OptObj("lods");
                if (lods != null)
                {
                    e.LodLevels = (int)lods.OptNum("levels", 1);
                    if (e.LodLevels < 1 || e.LodLevels > 3) throw new ManifestException(lods.Path + ".levels: " + e.LodLevels + " is not 1..3");
                    e.LodDistances = lods.Vec("distances", 2);
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
            public string File;  // relative to the model folder
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
            public bool UsedByMeshes = true, Exact = true;
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
            public readonly List<string> Warnings = new List<string>();

            public MapRef Map(string role) { MapRef r; return Maps.TryGetValue(role, out r) ? r : null; }
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
                    Preset = m.OptStr("preset"),
                    RenderPreset = m.OptStr("renderPreset"),
                    Cull = m.OptStr("cull") ?? "Back",
                    UsedByMeshes = m.Bool("usedByMeshes", true),
                    Exact = m.Bool("exact", true),
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
                }
                JObj em = m.OptObj("emission");
                if (em != null)
                {
                    e.HasEmission = true;
                    e.EmissionColor = em.OptVec("color", 3) ?? new double[] { 1, 1, 1 };
                    e.EmissionStrength = em.OptNum("strength", 1);
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
}
