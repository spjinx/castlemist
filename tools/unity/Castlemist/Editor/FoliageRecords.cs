// castlemist world export: the castlemist-foliage-1 record reader.
//
// World-foliage spec D7: foliage/<cellId>.bin holds 100-byte little-endian
// records, sorted by (model, placement):
//   0  u32     model      position in the manifest's models[]
//   4  u16     layer      index into foliage.layers[]; 0xFFFF: a collider linked by geometry only
//   6  u16     flags      bit 0 collider; bits 1-2 source; bit 3 Terrain tree; bit 4 outside the terrain
//   8  u32     placement  zone placement index; 0xFFFFFFFF for a geometry-only collider
//   12 f32[12] m          map space 3 x 4, column-major, absolute
//   60 f32[3]  unity.t    cell-relative, Unity axes, before unityScale
//   72 f32[4]  unity.r    quaternion x, y, z, w
//   88 f32     unity.s
//   92 f32     unityYaw
//   96 f32     tiltDeg
// No UnityEngine dependency (the tools/unity/Tests~ harness compiles it).

using System;
using System.Collections.Generic;
using System.IO;

namespace Castlemist
{
    public struct FoliageRecord
    {
        public uint Model;
        public ushort Layer, Flags;
        public uint Placement;
        /// Map space, column-major 3 x 4 (columns 0-2 the 3 x 3, column 3 the translation).
        public float[] M;
        public float[] T;  // unity.t
        public float[] R;  // unity.r (x, y, z, w)
        public float S, UnityYaw, TiltDeg;

        public bool Collider { get { return (Flags & 1) != 0; } }
    }

    public static class FoliageRecords
    {
        public const int RecordBytes = 100;
        public const ushort GeometryOnlyLayer = 0xFFFF;
        /// lodCullFovDeg (castlemist's choice, spec D8).
        public const double CullFovDeg = 60.0;

        /// The records of one bin's bytes. Throws ManifestException naming `what`
        /// when the size is not a whole number of records.
        public static List<FoliageRecord> Parse(byte[] data, string what)
        {
            if (data.Length % RecordBytes != 0)
                throw new ManifestException(what + ": foliage size " + data.Length + " is not a multiple of " + RecordBytes);
            if (!BitConverter.IsLittleEndian) throw new ManifestException(what + ": big-endian hosts are not supported");
            var r = new List<FoliageRecord>(data.Length / RecordBytes);
            for (int at = 0; at < data.Length; at += RecordBytes)
            {
                var f = new FoliageRecord
                {
                    Model = BitConverter.ToUInt32(data, at),
                    Layer = BitConverter.ToUInt16(data, at + 4),
                    Flags = BitConverter.ToUInt16(data, at + 6),
                    Placement = BitConverter.ToUInt32(data, at + 8),
                    M = Floats(data, at + 12, 12),
                    T = Floats(data, at + 60, 3),
                    R = Floats(data, at + 72, 4),
                    S = BitConverter.ToSingle(data, at + 88),
                    UnityYaw = BitConverter.ToSingle(data, at + 92),
                    TiltDeg = BitConverter.ToSingle(data, at + 96),
                };
                r.Add(f);
            }
            return r;
        }

        /// Reads <path>; a missing file is a ManifestException naming it.
        public static List<FoliageRecord> Read(string path)
        {
            if (!File.Exists(path)) throw new ManifestException("foliage: missing file " + path);
            return Parse(File.ReadAllBytes(path), path);
        }

        static float[] Floats(byte[] d, int at, int n)
        {
            var r = new float[n];
            for (int i = 0; i < n; ++i) r[i] = BitConverter.ToSingle(d, at + 4 * i);
            return r;
        }

        /// A tree record (spec D8, mesh terrain): its layer has type 3, or it is a
        /// collider linked by geometry only (layer 0xFFFF). An unknown layer is not a tree.
        public static bool IsTree(FoliageRecord r, WorldManifest.FoliageBlock f)
        {
            if (r.Layer == GeometryOnlyLayer) return true;
            return f != null && r.Layer < f.Layers.Count && f.Layers[r.Layer].Type == 3;
        }

        public const ushort FlagCollider = 1, FlagTerrainTree = 1 << 3, FlagOutsideTerrain = 1 << 4;
        public const uint NoPlacement = 0xFFFFFFFF;

        /// MurmurHash3's 32-bit finaliser (foliageThinHash, spec D9): 0 -> 0, 1 -> 0x514E28B7.
        public static uint Fmix32(uint h)
        {
            unchecked
            {
                h ^= h >> 16;
                h *= 0x85EBCA6B;
                h ^= h >> 13;
                h *= 0xC2B2AE35;
                h ^= h >> 16;
                return h;
            }
        }

        /// The Density knob (spec D9; validate_manifest.py foliage_keep): a record with a collider (bit 0) or
        /// without a placement (0xFFFFFFFF, a geometry-only collider) is always kept; otherwise
        /// fmix32(placement) < density * 2^32.
        public static bool Keep(uint placement, ushort flags, float density)
        {
            if ((flags & FlagCollider) != 0 || placement == NoPlacement) return true;
            return Fmix32(placement) < DensityThreshold(density);
        }

        /// density * 2^32, with the float taken at its shortest decimal (0.3f -> 0.3, as Python parses
        /// "--foliage-density 0.3"), so the importer and the dry run keep the same records.
        public static double DensityThreshold(float density)
        {
            if (!(density > 0f)) return 0.0;
            if (density >= 1f) return 4294967296.0;
            return (double)(decimal)density * 4294967296.0;
        }

        /// The LODGroup cull height (spec D8):
        /// clamp(size / (2 fadeFar D tan(fov / 2)), 1e-4, 1). A fadeFar that is not
        /// positive and finite gives 1e-4 (never culled in practice).
        public static double CullHeight(double size, double fadeFar, double distance = 1.0, double fovDeg = CullFovDeg)
        {
            double den = 2.0 * fadeFar * distance * Math.Tan(fovDeg * Math.PI / 360.0);
            if (!(den > 0) || double.IsInfinity(den) || !(size >= 0) || double.IsInfinity(size)) return 1e-4;
            return Math.Min(1.0, Math.Max(1e-4, size / den));
        }
    }
}
