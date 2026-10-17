// castlemist world export: the importer's options and what they place.
//
// World-foliage spec D8 ("Builds by mode") and D9 (the knobs). FoliagePlan.Build
// sorts every foliage record of the manifest into what the Unity importer
// builds, by the same rules as tools/world/validate_manifest.py plan_foliage,
// so the PLAN line's foliage counts equal the dry run's:
//   - a record whose model has status ok is a tree when its layer has type 3 or
//     is 0xFFFF (a geometry-only collider), a small record when its layer has
//     type 1 or 2; anything else is not placed (counted in Unplaced);
//   - it is placed when the Foliage knob shows it (Off; Collider trees = bit 0
//     only; Trees; All) and the Density knob keeps it (FoliageRecords.Keep);
//   - Terrain mode: bit 3 -> a Terrain tree of its cell's tile; a small record
//     whose model is foliage.models[].terrainDetail, without bit 4 and inside
//     its tile's real area -> one count in the 48-unit detail cell
//     (floor((x - tileX0)/48), floor((tileY1 - y)/48)), row 0 north as the
//     export's PNGs;
//   - otherwise a tree is a prefab instance and small records are patches: per
//     (cell, model, 1536-unit square from the cell's rect x0, y0), in record
//     order, split every PatchCapacity(models[].triangles) records (at most
//     65 535 vertices, 3 a triangle).
// Distance changes no count. No UnityEngine dependency (Tests~ compiles it).

using System;
using System.Collections.Generic;
using System.Globalization;

namespace Castlemist
{
    public enum TerrainMode { Mesh, UnityTerrain }
    public enum FoliageMode { Off, ColliderTrees, Trees, All }

    /// The importer window's Terrain / Foliage / Density / Distance (spec D8, D9).
    public sealed class ImportOptions
    {
        public TerrainMode terrain = TerrainMode.Mesh;
        public FoliageMode foliage = FoliageMode.All;
        /// Density 0..1 (D9); Distance x0.1..2, multiplying every fadeFar the importer uses
        /// (on top of the manifest's fadeQualityScale, ruling S5).
        public float density = 1f, distance = 1f;

        public const float MinDistance = 0.1f, MaxDistance = 2f;

        /// The Quest preset (D9; the user's default): Terrain Mesh, Foliage Collider trees, Distance 0.5.
        public static ImportOptions Quest()
        {
            return new ImportOptions { terrain = TerrainMode.Mesh, foliage = FoliageMode.ColliderTrees, density = 1f, distance = 0.5f };
        }

        /// Throws ArgumentException for a density outside 0..1 or a distance outside 0.1..2.
        public void Validate()
        {
            if (!(density >= 0f && density <= 1f)) throw new ArgumentException("density must be in 0..1, not " + density);
            if (!(distance >= MinDistance && distance <= MaxDistance))
                throw new ArgumentException("distance must be in 0.1..2, not " + distance);
        }

        /// The batch / dry-run spelling: mesh|terrain, off|colliders|trees|all.
        public static string Name(TerrainMode t) { return t == TerrainMode.UnityTerrain ? "terrain" : "mesh"; }

        public static string Name(FoliageMode f)
        {
            switch (f)
            {
                case FoliageMode.Off: return "off";
                case FoliageMode.ColliderTrees: return "colliders";
                case FoliageMode.Trees: return "trees";
                default: return "all";
            }
        }

        public static TerrainMode ParseTerrain(string s)
        {
            if (s == "mesh") return TerrainMode.Mesh;
            if (s == "terrain") return TerrainMode.UnityTerrain;
            throw new ArgumentException("terrain must be mesh or terrain, not '" + s + "'");
        }

        public static FoliageMode ParseFoliage(string s)
        {
            switch (s)
            {
                case "off": return FoliageMode.Off;
                case "colliders": return FoliageMode.ColliderTrees;
                case "trees": return FoliageMode.Trees;
                case "all": return FoliageMode.All;
            }
            throw new ArgumentException("foliage must be off, colliders, trees or all, not '" + s + "'");
        }

        public override string ToString()
        {
            return "terrain=" + Name(terrain) + " foliage=" + Name(foliage) + " density=" +
                   density.ToString("R", CultureInfo.InvariantCulture) + " distance=" + distance.ToString("R", CultureInfo.InvariantCulture);
        }
    }

    /// One patch: records of one model and 1536-unit square of a cell, in record order.
    public sealed class FoliagePatchPlan
    {
        public int Model, K;   // K: the patch's number among its cell's patches of this model (the asset name <fileId>_<K>)
        public readonly List<FoliageRecord> Records = new List<FoliageRecord>();
    }

    public sealed class FoliageCellPlan
    {
        public WorldManifest.CellEntry Cell;
        /// Prefab instances (Cell_<id>/Foliage/Trees), in record order.
        public readonly List<FoliageRecord> Trees = new List<FoliageRecord>();
        public readonly List<FoliagePatchPlan> Patches = new List<FoliagePatchPlan>();
    }

    public sealed class FoliageTilePlan
    {
        public WorldManifest.TerrainTile Tile;
        /// Terrain trees (bit 3), in cell then record order.
        public readonly List<FoliageRecord> Trees = new List<FoliageRecord>();
        /// detail prototype (unityTerrain.detailPrototypes index) -> counts, detailResolution^2, index dy * res + dx (row 0 north).
        public readonly SortedDictionary<int, int[]> Details = new SortedDictionary<int, int[]>();
    }

    public sealed class FoliagePlan
    {
        public const double PatchUnits = 1536.0;   // foliagePatchUnits (castlemist's choice)
        public const int PatchMaxVertices = 65535;

        public TerrainMode Mode;   // the mode in force (Mesh when Terrain mode has no unityTerrain block)
        public readonly List<FoliageCellPlan> Cells = new List<FoliageCellPlan>();
        public readonly List<FoliageTilePlan> Tiles = new List<FoliageTilePlan>();
        /// Records not placed: hidden by Foliage, thinned by Density, or of a failed model / unknown layer type.
        public int Hidden, Thinned, FailedModel, Unplaced;

        public int TreeCount { get { int n = 0; foreach (FoliageCellPlan c in Cells) n += c.Trees.Count; return n; } }
        public int PatchCount { get { int n = 0; foreach (FoliageCellPlan c in Cells) n += c.Patches.Count; return n; } }
        public int TreeInstanceCount { get { int n = 0; foreach (FoliageTilePlan t in Tiles) n += t.Trees.Count; return n; } }
        public int DetailPrototypeCount { get { int n = 0; foreach (FoliageTilePlan t in Tiles) n += t.Details.Count; return n; } }

        public int DetailCellCount
        {
            get
            {
                int n = 0;
                foreach (FoliageTilePlan t in Tiles)
                    foreach (int[] c in t.Details.Values)
                        foreach (int v in c) if (v > 0) ++n;
                return n;
            }
        }

        /// Records per patch: max(1, 65535 / (3 x triangles)) (validate_manifest.py patch_capacity).
        public static int PatchCapacity(long triangles)
        {
            long t = triangles > 0 ? triangles : 1;
            return (int)Math.Max(1L, PatchMaxVertices / (3L * t));
        }

        /// Whether the Foliage knob shows a record of this kind (tree: type 3 or 0xFFFF; else type 1-2).
        public static bool Shown(FoliageMode f, bool tree, ushort flags)
        {
            switch (f)
            {
                case FoliageMode.Off: return false;
                case FoliageMode.ColliderTrees: return (flags & FoliageRecords.FlagCollider) != 0;
                case FoliageMode.Trees: return tree;
                default: return true;
            }
        }

        /// The plan for every cell with records; `records` gives a cell's records (null: none / unreadable).
        /// Terrain mode without a unityTerrain block adds an error and plans mesh terrain.
        public static FoliagePlan Build(WorldManifest m, Func<WorldManifest.CellEntry, List<FoliageRecord>> records,
                                        ImportOptions o, List<string> errors)
        {
            var p = new FoliagePlan { Mode = o.terrain };
            WorldManifest.UnityTerrainBlock ut = m.UnityTerrain;
            if (p.Mode == TerrainMode.UnityTerrain && ut == null)
            {
                errors.Add("unityTerrain: missing: Terrain mode needs the export's unityTerrain block " +
                           "(world-export without --no-unity-terrain); mesh terrain is built");
                p.Mode = TerrainMode.Mesh;
            }
            bool terrainMode = p.Mode == TerrainMode.UnityTerrain;
            var tilePlans = new Dictionary<WorldManifest.TerrainTile, FoliageTilePlan>();
            if (terrainMode)
                foreach (WorldManifest.TerrainTile t in ut.Tiles)
                {
                    var tp = new FoliageTilePlan { Tile = t };
                    tilePlans[t] = tp;
                    p.Tiles.Add(tp);
                }
            if (o.foliage == FoliageMode.Off) return p;
            WorldManifest.FoliageBlock fol = m.Foliage;
            var detailModel = new HashSet<int>();
            if (fol != null) foreach (WorldManifest.FoliageModel fm in fol.Models) if (fm.TerrainDetail) detailModel.Add(fm.Model);

            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                if (c.FoliageFile == null) continue;
                List<FoliageRecord> recs = records(c);
                if (recs == null || recs.Count == 0) continue;
                var cp = new FoliageCellPlan { Cell = c };
                FoliageTilePlan tile = null;
                double[] real = null;
                if (terrainMode && c.Chunks != null)
                {
                    WorldManifest.TerrainTile t = ut.TileOfChunk(c.Chunks[0], c.Chunks[1]);
                    if (t != null) { tile = tilePlans[t]; real = t.Real(ut.TileChunks); }
                }
                double rx0 = c.Rect != null ? c.Rect[0] : 0, ry0 = c.Rect != null ? c.Rect[1] : 0;
                // (model, sx, sy) -> the open patch of that square; next K per model.
                var open = new Dictionary<string, FoliagePatchPlan>(StringComparer.Ordinal);
                var nextK = new Dictionary<int, int>();
                foreach (FoliageRecord r in recs)
                {
                    int mi = (int)r.Model;
                    if (r.Model >= (uint)m.Models.Count || !m.Models[mi].Ok) { p.FailedModel++; continue; }
                    int type = fol != null && r.Layer < fol.Layers.Count ? fol.Layers[r.Layer].Type : -1;
                    bool tree = r.Layer == FoliageRecords.GeometryOnlyLayer || type == 3;
                    bool small = !tree && (type == 1 || type == 2);
                    if (!tree && !small) { p.Unplaced++; continue; }
                    if (!Shown(o.foliage, tree, r.Flags)) { p.Hidden++; continue; }
                    if (!FoliageRecords.Keep(r.Placement, r.Flags, o.density)) { p.Thinned++; continue; }
                    double x = r.M[9], y = r.M[10];
                    if (tree)
                    {
                        if (terrainMode && (r.Flags & FoliageRecords.FlagTerrainTree) != 0 && tile != null) tile.Trees.Add(r);
                        else if (terrainMode && (r.Flags & FoliageRecords.FlagTerrainTree) != 0)
                        {
                            errors.Add("cell " + c.Id + ": a Terrain tree record (placement " + r.Placement + ") has no tile: placed as a prefab instance");
                            cp.Trees.Add(r);
                        }
                        else cp.Trees.Add(r);
                        continue;
                    }
                    int proto = terrainMode ? ut.DetailPrototypeOf(mi) : -1;
                    if (tile != null && proto >= 0 && detailModel.Contains(mi) && (r.Flags & FoliageRecords.FlagOutsideTerrain) == 0 &&
                        x >= real[0] && x <= real[2] && y >= real[1] && y <= real[3])
                    {
                        int res = ut.DetailResolution;
                        double cellUnits = ut.DetailCellUnits;
                        double tx0 = tile.Tile.Position[0], ty1 = tile.Tile.Position[2] + tile.Tile.Size[2];
                        int dx = Math.Min(Math.Max((int)Math.Floor((x - tx0) / cellUnits), 0), res - 1);
                        int dy = Math.Min(Math.Max((int)Math.Floor((ty1 - y) / cellUnits), 0), res - 1);
                        int[] counts;
                        if (!tile.Details.TryGetValue(proto, out counts)) { counts = new int[res * res]; tile.Details[proto] = counts; }
                        counts[dy * res + dx]++;
                        continue;
                    }
                    long sx = (long)Math.Floor((x - rx0) / PatchUnits), sy = (long)Math.Floor((y - ry0) / PatchUnits);
                    string key = mi + "/" + sx + "/" + sy;
                    FoliagePatchPlan patch;
                    if (!open.TryGetValue(key, out patch) || patch.Records.Count >= PatchCapacity(m.Models[mi].Triangles))
                    {
                        int k;
                        nextK.TryGetValue(mi, out k);
                        nextK[mi] = k + 1;
                        patch = new FoliagePatchPlan { Model = mi, K = k };
                        open[key] = patch;
                        cp.Patches.Add(patch);
                    }
                    patch.Records.Add(r);
                }
                if (cp.Trees.Count > 0 || cp.Patches.Count > 0) p.Cells.Add(cp);
            }
            return p;
        }
    }
}
