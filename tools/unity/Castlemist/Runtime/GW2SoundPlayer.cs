// castlemist: a GW2 model's sounds in a VRChat world (2026-10-25). The world importer (GW2Sounds.cs)
// puts it on each placed prop whose model has a sounds sidecar (<fileId>_sounds.json, from the
// model's AMSP sound script), with one AudioSource per sound under "GW2 Sounds".
//   The model's clips carry Animation Events (SendCustomEvent): "_T<k>" when trigger k's clip starts
//   (loops another trigger started stop), then "_S<n>" at each marker's time (sound n plays). A prop
//   with no Animator plays its idle trigger's sounds (zeropose, or the clip it was placed with) at
//   Start. Each play picks one of the sound's files by weight, at its volume x a random factor and
//   a random pitch ratio (GW2's linear volume and pitch ratio; 0 = 1). Local only: the animations
//   that drive it are what sync.
// UdonSharp has no dynamic event names, so the entry points are numbered: _S0.._S63 and _T0.._T15.
// 2026-10-25: beyond cullDistance from the local player (checked every checkInterval s) every source
// is switched off; back in range the loops that should be running start again.
// 2026-10-29: every play is scaled by settings.sfxVolume (the GMMenu's Sound Effects slider,
// GW2Performance); sounds already playing follow it at the next check.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2SoundPlayer : UdonSharpBehaviour
    {
        public AudioSource[] sources;      // one per sound
        public AudioClip[] clips;          // every sound's files, sound by sound
        public int[] first;                // per sound: its first file in clips[]
        public int[] count;                // per sound: its file count
        public float[] weight;             // per file: pick weight
        public float[] baseVolume;         // per file: file volume x sound volume x script volume
        public float[] volMin, volMax;     // per sound: random volume factor
        public float[] pitchMin, pitchMax; // per sound: random pitch ratio
        public bool[] loop;                // per sound
        public int[] startSounds;          // played at Start (a prop with no Animator)
        public float cullDistance = 30f;   // metres: the farthest source's range + 5
        public float checkInterval = 1f;
        public GW2Performance settings;    // optional: its sfxVolume scales every sound

        bool near = true;
        bool[] wanted;                     // per sound: a loop that should be running

        int current = -1;                  // the trigger whose clip last started
        int[] owner;                       // per sound: the trigger that started it
        float[] raw;                       // per sound: its last play's volume before the user's scale
        float scale = 1f;                  // the user's scale those plays got

        void Start()
        {
            owner = new int[sources != null ? sources.Length : 0];
            for (int i = 0; i < owner.Length; ++i) owner[i] = -1;
            wanted = new bool[owner.Length];
            if (raw == null || raw.Length != owner.Length) raw = new float[owner.Length];
            if (startSounds != null) foreach (int s in startSounds) Play(s);
            // 2026-10-26: a delayed event, not Update: 371 props each paid Udon's per-frame call to look at a clock.
            SendCustomEventDelayedSeconds(nameof(_Check), Random.Range(0.1f, checkInterval));   // spread the checks of many props
        }

        float Scale() { return settings != null ? settings.sfxVolume : 1f; }

        public void _Check()
        {
            SendCustomEventDelayedSeconds(nameof(_Check), checkInterval);
            VRCPlayerApi me = Networking.LocalPlayer;
            if (me == null || sources == null) return;
            float v = Scale();
            if (v != scale)
            {
                scale = v;
                for (int s = 0; s < sources.Length; ++s) if (sources[s] != null) sources[s].volume = raw[s] * scale;
            }
            bool now = Vector3.Distance(me.GetPosition(), transform.position) <= cullDistance;
            if (now == near) return;
            near = now;
            for (int s = 0; s < sources.Length; ++s)
            {
                if (sources[s] == null) continue;
                sources[s].enabled = near;
                if (near && wanted != null && wanted[s] && loop[s]) Play(s);
            }
        }

        /// Sound s once (or looping): one of its files by weight, random volume and pitch.
        public void Play(int s)
        {
            if (sources == null || s < 0 || s >= sources.Length || sources[s] == null || count[s] <= 0) return;
            AudioSource src = sources[s];
            if (owner == null || owner.Length != sources.Length) { owner = new int[sources.Length]; for (int i = 0; i < owner.Length; ++i) owner[i] = -1; }
            if (raw == null || raw.Length != sources.Length) raw = new float[sources.Length];   // animation events can come before Start
            if (wanted != null && s < wanted.Length && loop[s]) wanted[s] = true;
            if (!near) { owner[s] = current; return; }   // out of range: a loop is remembered, a one-shot dropped
            if (loop[s] && src.isPlaying) { owner[s] = current; return; }
            float total = 0f;
            for (int i = first[s]; i < first[s] + count[s]; ++i) total += weight[i];
            float r = Random.Range(0f, total);
            int pick = first[s];
            for (int i = first[s]; i < first[s] + count[s]; ++i) { pick = i; r -= weight[i]; if (r <= 0f) break; }
            if (clips[pick] == null) return;
            src.clip = clips[pick];
            raw[s] = baseVolume[pick] * Random.Range(volMin[s], volMax[s]);
            src.volume = raw[s] * Scale();
            src.pitch = Random.Range(pitchMin[s], pitchMax[s]);
            src.loop = loop[s];
            src.Play();
            owner[s] = current;
        }

        /// Trigger t's clip started: loops another trigger started stop.
        public void Trigger(int t)
        {
            current = t;
            if (sources == null || owner == null) return;
            for (int s = 0; s < sources.Length; ++s)
                if (loop[s] && sources[s] != null && owner[s] != t && (sources[s].isPlaying || (wanted != null && wanted[s])))
                {
                    sources[s].Stop();
                    if (wanted != null) wanted[s] = false;
                }
        }

        public void _S0() { Play(0); }
        public void _S1() { Play(1); }
        public void _S2() { Play(2); }
        public void _S3() { Play(3); }
        public void _S4() { Play(4); }
        public void _S5() { Play(5); }
        public void _S6() { Play(6); }
        public void _S7() { Play(7); }
        public void _S8() { Play(8); }
        public void _S9() { Play(9); }
        public void _S10() { Play(10); }
        public void _S11() { Play(11); }
        public void _S12() { Play(12); }
        public void _S13() { Play(13); }
        public void _S14() { Play(14); }
        public void _S15() { Play(15); }
        public void _S16() { Play(16); }
        public void _S17() { Play(17); }
        public void _S18() { Play(18); }
        public void _S19() { Play(19); }
        public void _S20() { Play(20); }
        public void _S21() { Play(21); }
        public void _S22() { Play(22); }
        public void _S23() { Play(23); }
        public void _S24() { Play(24); }
        public void _S25() { Play(25); }
        public void _S26() { Play(26); }
        public void _S27() { Play(27); }
        public void _S28() { Play(28); }
        public void _S29() { Play(29); }
        public void _S30() { Play(30); }
        public void _S31() { Play(31); }
        public void _S32() { Play(32); }
        public void _S33() { Play(33); }
        public void _S34() { Play(34); }
        public void _S35() { Play(35); }
        public void _S36() { Play(36); }
        public void _S37() { Play(37); }
        public void _S38() { Play(38); }
        public void _S39() { Play(39); }
        public void _S40() { Play(40); }
        public void _S41() { Play(41); }
        public void _S42() { Play(42); }
        public void _S43() { Play(43); }
        public void _S44() { Play(44); }
        public void _S45() { Play(45); }
        public void _S46() { Play(46); }
        public void _S47() { Play(47); }
        public void _S48() { Play(48); }
        public void _S49() { Play(49); }
        public void _S50() { Play(50); }
        public void _S51() { Play(51); }
        public void _S52() { Play(52); }
        public void _S53() { Play(53); }
        public void _S54() { Play(54); }
        public void _S55() { Play(55); }
        public void _S56() { Play(56); }
        public void _S57() { Play(57); }
        public void _S58() { Play(58); }
        public void _S59() { Play(59); }
        public void _S60() { Play(60); }
        public void _S61() { Play(61); }
        public void _S62() { Play(62); }
        public void _S63() { Play(63); }

        public void _T0() { Trigger(0); }
        public void _T1() { Trigger(1); }
        public void _T2() { Trigger(2); }
        public void _T3() { Trigger(3); }
        public void _T4() { Trigger(4); }
        public void _T5() { Trigger(5); }
        public void _T6() { Trigger(6); }
        public void _T7() { Trigger(7); }
        public void _T8() { Trigger(8); }
        public void _T9() { Trigger(9); }
        public void _T10() { Trigger(10); }
        public void _T11() { Trigger(11); }
        public void _T12() { Trigger(12); }
        public void _T13() { Trigger(13); }
        public void _T14() { Trigger(14); }
        public void _T15() { Trigger(15); }
    }
}
#endif
