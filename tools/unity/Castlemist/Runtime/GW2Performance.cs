// castlemist: the GMMenu's "Performance" tab (2026-10-29): local switches for the costly parts of a
// GW2 map, so a slow PC or a Quest can turn them off, and the music / sound effect volumes. Each
// player's choice only changes their own view and is kept in VRChat player data across visits.
// GW2MenuLook (Editor) fills it per scene:
//   0 Fog            the Light Volume fog boxes (a ray march over the screen while inside one), and
//                    everything else that reads the camera depth texture without a fallback: GW2's
//                    height haze and the GW2 Distort surfaces (their renderers)
//   1 Sun            the Sun and its avatar copy (Light Volumes and ambient light the map without it)
//   2 Shadows        the Sun's realtime shadow map (the biggest fixed cost: every caster per cascade)
//   3 Point Lights   every Light Volume point light: with none active the package's count is 0, so
//                    no shader loops over them and its clustering stops
//   4 Post FX        the post-processing volume (bloom, grading, tonemap)
//   5 (Haze: folded into Fog; the index stays so saved choices keep their keys)
//   6 Clouds         the cloud layers (large transparent overdraw)
//   7 Particles      the particle fields and effects
//   8 Foliage        grass and plants (many small draws)
//   9 Sun Flare      the soft sun / moon glow (Shaders/GW2 Sun Glow) and any Silent Sunshine Flare
// Objects are switched with SetActive (targets / targetSwitch), renderers with enabled (renderTargets /
// renderSwitch); switching on gives each back the state it had when switched off (festival layers and
// the cell switcher move some too). Sun and Shadows act on the lights; Particles also tells the cell
// switcher, which owns the per-cell particle systems; Point Lights tells the light streamer, which owns
// the streamed ones. Fog reads the camera depth texture, which Unity only renders for a shadowed
// directional light: with the Sun's shadows off and Fog on (or a Sunshine Flare, which ray-marches depth,
// when flareNeedsDepth), depth is asked for directly; with them off, nothing asks for it.
// _T0 .. _T9 flip one switch (menu buttons); _Lowest turns all off, _Default all on.
// Volumes (0..1): Music scales the GW2MusicPlayers; Sound Effects is read by every GW2SoundPlayer
// (sfxVolume, when it plays and on its next check) and scales the other AudioSources listed in
// sfxSources. Their sliders call _OnMusic / _OnSfx.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Persistence;
using VRC.SDK3.Rendering;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2Performance : UdonSharpBehaviour
    {
        public const int Count = 10;
        public const int Fog = 0, Sun = 1, Shadows = 2, Lamps = 3, PostFx = 4, Haze = 5, Clouds = 6, Particles = 7, Foliage = 8, Flare = 9;

        public string[] names = { "Fog", "Sun", "Sun Shadows", "Point Lights", "Post FX", "Haze", "Clouds", "Particles", "Foliage", "Sun Flare" };
        public bool flareNeedsDepth;       // an active Silent Sunshine Flare is among the Sun Flare targets
        public GameObject[] targets;       // every switched object
        public int[] targetSwitch;         // per target: its switch
        public Renderer[] renderTargets;   // every switched renderer
        public int[] renderSwitch;         // per renderer: its switch
        public Light sun;
        public Light avatarSun;
        public GW2CellSwitcher cellSwitcher;
        public GW2Streamer lightStreamer;
        public Text[] labels;              // per switch, "Fog: On"; null where the map has none
        public string playerDataPrefix = "castlemist.perf.";

        public GW2MusicPlayer[] music;
        public AudioSource[] sfxSources;   // sound effect sources no GW2SoundPlayer drives
        public Slider musicSlider, sfxSlider;
        public Text musicLabel, sfxLabel;
        [HideInInspector] public float sfxVolume = 1f;   // read by GW2SoundPlayer

        bool[] on;
        bool[] applied;                    // per switch: the state Apply last put the targets in
        bool[] wasActive;                  // per target: activeSelf when its switch went off
        bool[] wasEnabled;                 // per renderer: enabled when its switch went off
        float[] sfxBase;                   // per sfxSources: its volume as saved
        float musicVolume = 1f;
        bool restoring;
        LightShadows sunShadows = LightShadows.Soft;
        DepthTextureMode baseDepth;

        void Start()
        {
            Init();
            Apply();
            ShowVolumes();
        }

        void Init()
        {
            if (on != null) return;
            on = new bool[Count];
            applied = new bool[Count];
            for (int i = 0; i < Count; ++i) { on[i] = true; applied[i] = true; }
            wasActive = new bool[targets != null ? targets.Length : 0];
            wasEnabled = new bool[renderTargets != null ? renderTargets.Length : 0];
            sfxBase = new float[sfxSources != null ? sfxSources.Length : 0];
            for (int k = 0; k < sfxBase.Length; ++k) sfxBase[k] = sfxSources[k] != null ? sfxSources[k].volume : 1f;
            if (sun != null && sun.shadows != LightShadows.None) sunShadows = sun.shadows;
            baseDepth = VRCCameraSettings.ScreenCamera.DepthTextureMode;
        }

        public override void OnPlayerRestored(VRCPlayerApi player)
        {
            if (!player.isLocal) return;
            Init();
            for (int i = 0; i < Count; ++i)
            {
                bool v;
                if (PlayerData.TryGetBool(player, playerDataPrefix + i, out v)) on[i] = v;
            }
            float f;
            if (PlayerData.TryGetFloat(player, playerDataPrefix + "music", out f)) musicVolume = Mathf.Clamp01(f);
            if (PlayerData.TryGetFloat(player, playerDataPrefix + "sfx", out f)) sfxVolume = Mathf.Clamp01(f);
            Apply();
            ShowVolumes();
        }

        public void _T0() { Flip(0); }
        public void _T1() { Flip(1); }
        public void _T2() { Flip(2); }
        public void _T3() { Flip(3); }
        public void _T4() { Flip(4); }
        public void _T5() { Flip(5); }
        public void _T6() { Flip(6); }
        public void _T7() { Flip(7); }
        public void _T8() { Flip(8); }
        public void _T9() { Flip(9); }

        public void _Lowest() { SetAll(false); }
        public void _Default() { SetAll(true); }

        public void _OnMusic()
        {
            if (restoring || musicSlider == null) return;
            Init();
            musicVolume = musicSlider.value;
            PlayerData.SetFloat(playerDataPrefix + "music", musicVolume);
            ApplyVolumes();
        }

        public void _OnSfx()
        {
            if (restoring || sfxSlider == null) return;
            Init();
            sfxVolume = sfxSlider.value;
            PlayerData.SetFloat(playerDataPrefix + "sfx", sfxVolume);
            ApplyVolumes();
        }

        void Flip(int i)
        {
            Init();
            on[i] = !on[i];
            PlayerData.SetBool(playerDataPrefix + i, on[i]);
            Apply();
        }

        void SetAll(bool v)
        {
            Init();
            for (int i = 0; i < Count; ++i)
            {
                on[i] = v;
                PlayerData.SetBool(playerDataPrefix + i, v);
            }
            Apply();
        }

        void Apply()
        {
            if (targets != null)
                for (int k = 0; k < targets.Length; ++k)
                {
                    int i = targetSwitch[k];
                    if (targets[k] == null || on[i] == applied[i]) continue;
                    if (!on[i]) { wasActive[k] = targets[k].activeSelf; targets[k].SetActive(false); }
                    else targets[k].SetActive(wasActive[k]);
                }
            if (renderTargets != null)
                for (int k = 0; k < renderTargets.Length; ++k)
                {
                    int i = renderSwitch[k];
                    if (renderTargets[k] == null || on[i] == applied[i]) continue;
                    if (!on[i]) { wasEnabled[k] = renderTargets[k].enabled; renderTargets[k].enabled = false; }
                    else renderTargets[k].enabled = wasEnabled[k];
                }
            for (int i = 0; i < Count; ++i) applied[i] = on[i];
            if (cellSwitcher != null) cellSwitcher.SetParticles(on[Particles]);
            if (lightStreamer != null && lightStreamer.itemsOn != on[Lamps]) lightStreamer.SetItems(on[Lamps]);
            // Enums through ifs, not ?: : UdonSharp compiled "c ? mode | Depth : mode" to pass the bool to
            // the DepthTextureMode setter, and the VM halted the behaviour (2026-10-29).
            if (sun != null)
            {
                sun.enabled = on[Sun];
                if (on[Shadows]) sun.shadows = sunShadows;
                else sun.shadows = LightShadows.None;
            }
            if (avatarSun != null) avatarSun.enabled = on[Sun];
            bool shadowDepth = sun != null && on[Sun] && on[Shadows];
            DepthTextureMode mode = baseDepth;
            bool needDepth = on[Fog] || (flareNeedsDepth && on[Flare]);
            if (needDepth && !shadowDepth) mode = (DepthTextureMode)((int)baseDepth | (int)DepthTextureMode.Depth);
            VRCCameraSettings.ScreenCamera.DepthTextureMode = mode;
            if (labels != null)
                for (int i = 0; i < labels.Length && i < Count; ++i)
                    if (labels[i] != null) labels[i].text = names[i] + ": " + (on[i] ? "On" : "Off");
        }

        void ApplyVolumes()
        {
            if (music != null)
                foreach (GW2MusicPlayer m in music) if (m != null) m.SetUserVolume(musicVolume);
            for (int k = 0; k < sfxBase.Length; ++k)
                if (sfxSources[k] != null) sfxSources[k].volume = sfxBase[k] * sfxVolume;
            if (musicLabel != null) musicLabel.text = "Music: " + Mathf.RoundToInt(musicVolume * 100f) + "%";
            if (sfxLabel != null) sfxLabel.text = "Sound Effects: " + Mathf.RoundToInt(sfxVolume * 100f) + "%";
        }

        /// The sliders moved to the saved volumes (without saving again), then applied.
        void ShowVolumes()
        {
            restoring = true;
            if (musicSlider != null) musicSlider.value = musicVolume;
            if (sfxSlider != null) sfxSlider.value = sfxVolume;
            restoring = false;
            ApplyVolumes();
        }
    }
}
#endif
