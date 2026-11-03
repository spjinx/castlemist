// castlemist: far map cells drawn as one proxy each (2026-10-26). A vista over Lion's Arch drew ~12000
// props (a draw call each, twice with the sun's depth pass): CellProxies.cs merges each cell's solid
// static props into one flat-coloured mesh. This switches, per cell, between its real renderers
// (within nearDistance metres of the cell's XZ rectangle, from the local player's head) and its proxy
// (beyond), checked every checkInterval seconds; a cell's renderers are only touched when it flips.
// 2026-10-26: a "cell" is a 64 m tile (CellProxies); a far tile's ambient Animators and particle systems
// are off too.
// ToggleProxies (for a menu button) turns proxies off everywhere, for comparing.
// 2026-10-29: SetParticles(false) (GW2Performance's Particles switch) keeps every cell's particles off.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2CellSwitcher : UdonSharpBehaviour
    {
        public GameObject[] proxies;       // per cell
        public Vector2[] rectMin, rectMax; // per cell: world XZ
        public int[] cellStart;            // per cell: its first renderer in renderers; cells + 1 long
        public Renderer[] renderers;       // the props each proxy stands in for, cell by cell
        public int[] animStart;            // per cell: its first Animator in animators; cells + 1 long
        public Animator[] animators;       // ambient animated props, frozen while their cell is far
        public int[] particleStart;        // per cell: its first entry in particles; cells + 1 long
        public GameObject[] particles;     // every placement's particle systems, off while their cell is far
        public float nearDistance = 150f;
        public float checkInterval = 0.5f;
        public bool proxiesOn = true;
        public bool particlesOn = true;

        bool[] detailed;

        void Start()
        {
            detailed = new bool[proxies != null ? proxies.Length : 0];
            for (int c = 0; c < detailed.Length; ++c) detailed[c] = true;   // saved with the props on, proxies off
            _Check();
        }

        public void _Check()
        {
            SendCustomEventDelayedSeconds(nameof(_Check), checkInterval);
            Apply();
        }

        public void ToggleProxies()
        {
            proxiesOn = !proxiesOn;
            Apply();
        }

        public void SetParticles(bool v)
        {
            particlesOn = v;
            if (detailed == null || particleStart == null) return;
            for (int c = 0; c < detailed.Length; ++c)
                for (int k = particleStart[c]; k < particleStart[c + 1]; ++k)
                    if (particles[k] != null) particles[k].SetActive(detailed[c] && particlesOn);
        }

        void Apply()
        {
            VRCPlayerApi me = Networking.LocalPlayer;
            if (me == null || detailed == null) return;
            Vector3 p = me.GetTrackingData(VRCPlayerApi.TrackingDataType.Head).position;
            for (int c = 0; c < detailed.Length; ++c)
            {
                float dx = Mathf.Max(0f, Mathf.Max(rectMin[c].x - p.x, p.x - rectMax[c].x));
                float dz = Mathf.Max(0f, Mathf.Max(rectMin[c].y - p.z, p.z - rectMax[c].y));
                bool want = !proxiesOn || dx * dx + dz * dz < nearDistance * nearDistance;
                if (want == detailed[c]) continue;
                detailed[c] = want;
                for (int k = cellStart[c]; k < cellStart[c + 1]; ++k)
                    if (renderers[k] != null) renderers[k].enabled = want;
                if (proxies[c] != null) proxies[c].SetActive(!want);
                if (animStart != null)
                    for (int k = animStart[c]; k < animStart[c + 1]; ++k)
                        if (animators[k] != null) animators[k].enabled = want;
                if (particleStart != null)
                    for (int k = particleStart[c]; k < particleStart[c + 1]; ++k)
                        if (particles[k] != null) particles[k].SetActive(want && particlesOn);
            }
        }
    }
}
#endif
