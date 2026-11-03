// castlemist: keeps only the objects that matter near the local player switched on (2026-10-26).
// VRC Light Volumes renders at most 128 Point Light Volumes and 32 Light Volumes at once, picked by
// registry order, not distance: Lion's Arch has 1235 map / model lights. The editor (GW2Realtime.cs)
// splits the map into square XZ cells and stores, per cell, the items to have on there (the lights
// reaching it first, then the rest by range / distance, at most the package's limit); this switches
// the GameObjects on and off when the player's head leaves the current cell by more than hysteresis
// metres. Local only: every player streams for themselves.
// 2026-10-29: SetItems(false) (GW2Performance's Lamps switch) keeps every item off until SetItems(true).

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2Streamer : UdonSharpBehaviour
    {
        public GameObject[] items;
        public Vector2 origin;             // world XZ of cell (0, 0)'s corner
        public float cellSize = 32f;
        public int cellsX, cellsZ;
        public int[] cellStart;            // per cell (x + z cellsX): its first entry in cellItems; cellsX cellsZ + 1 long
        public int[] cellItems;            // item indices, cell by cell
        public float hysteresis = 4f;      // metres past the cell edge before switching
        public float checkInterval = 0.25f;
        public bool itemsOn = true;

        int current = -2;                  // -1: outside the grid
        int[] stamp;
        int tick;
        float nextCheck;

        void Start()
        {
            stamp = new int[items != null ? items.Length : 0];
            // Everything is saved on (the package registers what is active); off until a cell wants it.
            for (int i = 0; i < stamp.Length; ++i) if (items[i] != null) items[i].SetActive(false);
            current = -2;
            nextCheck = 0f;
        }

        void Update()
        {
            if (Time.time < nextCheck) return;
            nextCheck = Time.time + checkInterval;
            VRCPlayerApi me = Networking.LocalPlayer;
            if (me == null || stamp == null || !itemsOn) return;
            Vector3 p = me.GetTrackingData(VRCPlayerApi.TrackingDataType.Head).position;
            if (current >= 0 && Inside(current, p.x, p.z, hysteresis)) return;
            int cx = Mathf.FloorToInt((p.x - origin.x) / cellSize), cz = Mathf.FloorToInt((p.z - origin.y) / cellSize);
            int cell = cx >= 0 && cz >= 0 && cx < cellsX && cz < cellsZ ? cx + cz * cellsX : -1;
            if (cell < 0) { cx = Mathf.Clamp(cx, 0, cellsX - 1); cz = Mathf.Clamp(cz, 0, cellsZ - 1); cell = cx + cz * cellsX; }   // outside: the nearest edge cell
            if (cell == current) return;
            Switch(cell);
        }

        public void SetItems(bool v)
        {
            itemsOn = v;
            if (stamp == null) return;
            if (!v) for (int i = 0; i < stamp.Length; ++i) if (items[i] != null) items[i].SetActive(false);
            current = -2;   // the next Update switches the player's cell back on
            nextCheck = 0f;
        }

        bool Inside(int cell, float x, float z, float pad)
        {
            int cx = cell % cellsX, cz = cell / cellsX;
            float x0 = origin.x + cx * cellSize - pad, z0 = origin.y + cz * cellSize - pad;
            return x >= x0 && z >= z0 && x < x0 + cellSize + 2f * pad && z < z0 + cellSize + 2f * pad;
        }

        void Switch(int cell)
        {
            ++tick;
            for (int k = cellStart[cell]; k < cellStart[cell + 1]; ++k)
            {
                int i = cellItems[k];
                stamp[i] = tick;
                if (items[i] != null && !items[i].activeSelf) items[i].SetActive(true);
            }
            if (current >= 0)
                for (int k = cellStart[current]; k < cellStart[current + 1]; ++k)
                {
                    int i = cellItems[k];
                    if (stamp[i] != tick && items[i] != null) items[i].SetActive(false);
                }
            current = cell;
        }
    }
}
#endif
