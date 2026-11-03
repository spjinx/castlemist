// castlemist: a GW2 music playlist as world music (2026-10-26). GW2's audio settings (AMSP 14897's
// MusicPlaylistData) give each playlist its files and timing: silence before the first track
// (initialSilence), silence between tracks (intervalSilence), the fade out and the volume. This plays
// one playlist the same way, local to each player (2D): a random track (weighted, never the same twice in
// a row), then silence, then another. The importer's Music.cs fills it.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2MusicPlayer : UdonSharpBehaviour
    {
        public AudioSource source;
        public AudioClip[] clips;
        public float[] weights;
        public float volume = 0.7079f;                 // the playlist's
        public float userVolume = 1f;                  // a menu slider's (0..1)
        public float initialMin = 8f, initialMax = 15f;
        public float intervalMin = 60f, intervalMax = 180f;
        public float fadeOut = 5f;

        int last = -1;

        void Start() { Schedule(Random.Range(initialMin, initialMax)); }

        void Schedule(float seconds) { SendCustomEventDelayedSeconds(nameof(_Next), seconds); }

        public void _Next()
        {
            if (clips == null || clips.Length == 0 || source == null) return;
            int k = Pick();
            last = k;
            source.clip = clips[k];
            source.volume = volume * userVolume;
            source.Play();
            float len = clips[k].length;
            Schedule(len + Random.Range(intervalMin, intervalMax));
            if (fadeOut > 0f && len > fadeOut * 2f) SendCustomEventDelayedSeconds(nameof(_Fade), len - fadeOut);
        }

        int Pick()
        {
            float total = 0f;
            for (int i = 0; i < clips.Length; ++i) if (i != last || clips.Length == 1) total += Weight(i);
            float r = Random.Range(0f, total);
            for (int i = 0; i < clips.Length; ++i)
            {
                if (i == last && clips.Length > 1) continue;
                r -= Weight(i);
                if (r <= 0f) return i;
            }
            return clips.Length - 1;
        }

        float Weight(int i) { return weights != null && i < weights.Length ? Mathf.Max(weights[i], 0f) : 1f; }

        /// The last fadeOut seconds of a track, in steps.
        public void _Fade()
        {
            if (source == null || !source.isPlaying) return;
            float left = source.clip.length - source.time;
            source.volume = volume * userVolume * Mathf.Clamp01(left / Mathf.Max(fadeOut, 0.01f));
            if (left > 0.1f) SendCustomEventDelayedSeconds(nameof(_Fade), 0.1f);
        }

        /// A menu slider's value (0..1).
        public void SetUserVolume(float v)
        {
            userVolume = Mathf.Clamp01(v);
            if (source != null && source.isPlaying) source.volume = volume * userVolume;
        }
    }
}
#endif
