// castlemist world import: places from the GW2 wiki's location maps (2026-10-26). Those maps are
// screenshots of the in-game map, so each is a scaled, unrotated window on the map's continent
// coordinates: two waypoints found in the image, against their continent coordinates from the GW2 API
// (/v2/continents/<c>/floors/<f>, points_of_interest), give the pixel -> continent transform, and the
// API's map_rect / continent_rect turn continent coordinates into map units (as Waypoints.cs does).
// The wiki thumbnails are small (200-300 px, ~2.5 m a pixel) and JPEG: expect ~5-10 m.

using System.Collections.Generic;
using UnityEngine;

namespace Castlemist
{
    public static class WikiMaps
    {
        public sealed class MapInfo
        {
            public double[] MapRect, ContinentRect;    // x0, y0, x1, y1 (the API's order)
            public Vector2 WaypointA, WaypointB;       // continent coordinates
        }

        public sealed class Image
        {
            public string File;                        // the wiki's file name (for the record)
            public Vector2 WaypointA, WaypointB;       // the same two waypoints, image pixels
        }

        // Mad King's Labyrinth (fileId 2499169, API map 866, continent 2 floor -27): the northwest and
        // northeast "Mad King's Labyrinth Waypoint"s.
        public static readonly Dictionary<long, MapInfo> Maps = new Dictionary<long, MapInfo>
        {
            [2499169] = new MapInfo
            {
                MapRect = new double[] { -9216, -9216, 9216, 9216 },
                ContinentRect = new double[] { 1792, 13696, 2560, 14464 },
                WaypointA = new Vector2(1898.43f, 13822.3f), WaypointB = new Vector2(2463.99f, 13832.2f),
            },
        };

        public static readonly Image MadSpirits = new Image { File = "Mad Spirits Labyrinth map.jpg (212 px)", WaypointA = new Vector2(37.0f, 40.0f), WaypointB = new Vector2(181.8f, 45.8f) };
        public static readonly Image HauntedDoors = new Image { File = "Haunted Door (Mad King's Labyrinth).jpg (227 px)", WaypointA = new Vector2(39.3f, 62.0f), WaypointB = new Vector2(187.7f, 64.7f) };
        public static readonly Image CarvingPumpkins = new Image { File = "Carving Pumpkin locations (Mad King's Labyrinth).jpg (300 px)", WaypointA = new Vector2(52.5f, 56.9f), WaypointB = new Vector2(258.0f, 64.9f) };

        /// @p px on @p img -> world XZ (y 0) under @p mapRoot (the GW2_<id> root, map units), false when the map is unknown.
        public static bool ToWorld(long mapId, Image img, Vector2 px, Transform mapRoot, out Vector3 world)
        {
            world = Vector3.zero;
            if (!Maps.TryGetValue(mapId, out MapInfo m) || mapRoot == null) return false;
            // pixel -> continent: one scale (the waypoints' x spread), the offset from both waypoints' mean
            float s = (m.WaypointB.x - m.WaypointA.x) / (img.WaypointB.x - img.WaypointA.x);
            Vector2 pMid = (img.WaypointA + img.WaypointB) * 0.5f, cMid = (m.WaypointA + m.WaypointB) * 0.5f;
            Vector2 c = cMid + (px - pMid) * s;
            double[] mr = m.MapRect, cr = m.ContinentRect;
            double x = mr[0] + (c.x - cr[0]) / (cr[2] - cr[0]) * (mr[2] - mr[0]);
            double y = mr[3] - (c.y - cr[1]) / (cr[3] - cr[1]) * (mr[3] - mr[1]);
            world = mapRoot.TransformPoint(new Vector3((float)x, 0f, (float)y));
            return true;
        }

        /// The topmost collision at world (x, z), NaN when none.
        public static float Ground(float x, float z)
        {
            return Physics.Raycast(new Vector3(x, 3000f, z), Vector3.down, out RaycastHit hit, 6000f, ~0, QueryTriggerInteraction.Ignore) ? hit.point.y : float.NaN;
        }
    }
}
