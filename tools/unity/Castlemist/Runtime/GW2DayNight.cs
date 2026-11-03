// castlemist: GW2's day / night cycle in a VRChat world (2026-10-25). The world importer
// (DayNight.cs) puts one on "GW2 Day Night" under the map root, filled from the export's day
// (environment lights / haze, slot 0) and night (environment.night, slot 1) values.
//
// Every UpdateInterval seconds it takes the day weight t (1 day, 0 night; gw2-sky.md §3: one t drives
// every sky layer) and lerps by it: the Sun's colour, intensity and direction, the Trilight ambient,
// every listed material vector (cloud and card attributes, the haze volume's colours: vecMats /
// vecNames / vecDay / vecNight), _DayWeight on weightMats (the sky blend, the cards' textures) and
// the Light Volumes' tint (lightVolumes: current Trilight / day Trilight).
//
// t comes from the clock: one cycle is CycleSeconds of real time (GW2: 2 hours = one Tyrian day);
// Tyrian hour h = 24 x frac((UTC seconds + OffsetSeconds) / CycleSeconds) (the anchor, Tyrian
// midnight on even UTC hours, is the commonly cited one: UNPROVEN, OffsetSeconds moves it). Dawn
// DawnHour..DayHour ramps 0 -> 1, dusk DuskHour..NightHour 1 -> 0 (the wiki's table: dawn 5:00, day
// 6:00, dusk 20:00, night 21:00). With cycleOn off (synced; ToggleCycle / CycleOn / CycleOff for a
// GMMenu button) it holds fixedWeight (1: day; synced), which SetFixedWeight sets and turns the cycle
// off for (the GMMenu's time-of-day slider, Runtime/GW2DayNightSlider).

#if CASTLEMIST_VRC
using System;
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.Manual)]
    public class GW2DayNight : UdonSharpBehaviour
    {
        public Light sun;
        // 2026-10-28: the unshadowed sun that lights only the player layers (ImmersiveLighting.SplitSun): a
        // child of the Sun, so it turns with it; given the Sun's colour and intensity here.
        public Light avatarSun;
        public Color sunDayColor = Color.white, sunNightColor = new Color(0.5f, 0.6f, 1f);
        public float sunDayIntensity = 1.3f, sunNightIntensity = 0.4f;
        public Quaternion sunDayRotation = Quaternion.identity, sunNightRotation = Quaternion.identity;
        public bool trilight = true;
        public Color skyDay, equatorDay, groundDay, skyNight, equatorNight, groundNight;

        public Material[] vecMats;
        public string[] vecNames;
        public Vector4[] vecDay, vecNight;
        public Material[] weightMats;
        // 2026-10-26: the Light Volumes hold the day's ambient (Bakery's sky light is the day Trilight):
        // tinted by the current Trilight / the day's, channel by channel (the package's own Color).
        public VRC.Udon.UdonBehaviour[] lightVolumes;

        public float cycleSeconds = 7200f, offsetSeconds = 0f;
        public float dawnHour = 5f, dayHour = 6f, duskHour = 20f, nightHour = 21f;
        public float updateInterval = 2f;
        [UdonSynced] public float fixedWeight = 1f;
        [UdonSynced] public bool cycleOn = true;

        float next;
        float last = -1f;

        void Start() { Apply(Weight()); }

        void Update()
        {
            if (Time.time < next) return;
            next = Time.time + updateInterval;
            float t = Weight();
            if (Mathf.Abs(t - last) > 0.0005f) Apply(t);
        }

        /// The day weight now (see the header).
        public float Weight()
        {
            if (!cycleOn) return fixedWeight;
            double s = (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds + offsetSeconds;
            double c = Math.Max(1.0, cycleSeconds);
            float h = (float)((s % c + c) % c / c * 24.0);
            if (h >= dayHour && h < duskHour) return 1f;
            if (h >= dawnHour && h < dayHour) return Mathf.SmoothStep(0f, 1f, (h - dawnHour) / Mathf.Max(dayHour - dawnHour, 1e-3f));
            if (h >= duskHour && h < nightHour) return Mathf.SmoothStep(1f, 0f, (h - duskHour) / Mathf.Max(nightHour - duskHour, 1e-3f));
            return 0f;
        }

        public void Apply(float t)
        {
            last = t;
            if (sun != null)
            {
                sun.color = Color.Lerp(sunNightColor, sunDayColor, t);
                sun.intensity = Mathf.Lerp(sunNightIntensity, sunDayIntensity, t);
                sun.transform.rotation = Quaternion.Slerp(sunNightRotation, sunDayRotation, t);
                if (avatarSun != null) { avatarSun.color = sun.color; avatarSun.intensity = sun.intensity; }
            }
            if (trilight)
            {
                RenderSettings.ambientSkyColor = Color.Lerp(skyNight, skyDay, t);
                RenderSettings.ambientEquatorColor = Color.Lerp(equatorNight, equatorDay, t);
                RenderSettings.ambientGroundColor = Color.Lerp(groundNight, groundDay, t);
                if (lightVolumes != null && lightVolumes.Length > 0)
                {
                    Color day = (skyDay + equatorDay + groundDay) / 3f;
                    Color now = (RenderSettings.ambientSkyColor + RenderSettings.ambientEquatorColor + RenderSettings.ambientGroundColor) / 3f;
                    var tint = new Color(now.r / Mathf.Max(day.r, 1e-4f), now.g / Mathf.Max(day.g, 1e-4f), now.b / Mathf.Max(day.b, 1e-4f));
                    foreach (VRC.Udon.UdonBehaviour v in lightVolumes) if (v != null) v.SetProgramVariable("Color", tint);
                }
            }
            if (vecMats != null)
                for (int i = 0; i < vecMats.Length; ++i)
                    if (vecMats[i] != null) vecMats[i].SetVector(vecNames[i], Vector4.Lerp(vecNight[i], vecDay[i], t));
            if (weightMats != null)
                foreach (Material m in weightMats) if (m != null) m.SetFloat("_DayWeight", t);
        }

        public override void OnDeserialization() { Apply(Weight()); }

        public void ToggleCycle() { SetCycle(!cycleOn); }
        public void CycleOn() { SetCycle(true); }
        public void CycleOff() { SetCycle(false); }

        /// Holds day weight @p w (0 night .. 1 day) for everyone.
        public void SetFixedWeight(float w)
        {
            Networking.SetOwner(Networking.LocalPlayer, gameObject);
            cycleOn = false;
            fixedWeight = Mathf.Clamp01(w);
            RequestSerialization();
            Apply(fixedWeight);
        }

        void SetCycle(bool on)
        {
            Networking.SetOwner(Networking.LocalPlayer, gameObject);
            cycleOn = on;
            RequestSerialization();
            Apply(Weight());
        }
    }
}
#endif
