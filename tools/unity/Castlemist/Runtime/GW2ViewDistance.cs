// castlemist: a local view-distance (camera far clip) slider for the GMMenu's options (2026-10-26).
// A near far clip culls everything beyond it, which is the cheap optimisation for a slow PC or Quest;
// a far one shows the whole map. The slider runs 0..1 on a log scale from minDistance to maxDistance
// (so the near end has fine control) and starts at the world's own far clip (the reference camera).
// Its onValueChanged calls _OnSlider. The choice is kept in VRChat player data across visits.

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
    public class GW2ViewDistance : UdonSharpBehaviour
    {
        public Slider slider;
        public Text label;                 // "View distance: 1.2 km"
        public float minDistance = 100f;
        public float maxDistance = 20000f;
        public string playerDataKey = "castlemist.viewDistanceMetres";

        float worldFar;
        bool restoring;

        void Start()
        {
            worldFar = VRCCameraSettings.ScreenCamera.FarClipPlane;
            Show(worldFar, false);
        }

        public override void OnPlayerRestored(VRCPlayerApi player)
        {
            if (!player.isLocal) return;
            float d;
            if (PlayerData.TryGetFloat(player, playerDataKey, out d) && d > 0f) Show(d, false);
        }

        public void _OnSlider()
        {
            if (restoring || slider == null) return;
            float d = minDistance * Mathf.Pow(maxDistance / minDistance, slider.value);
            Apply(d);
            PlayerData.SetFloat(playerDataKey, d);
        }

        /// Moves the slider to @p d (without saving) and applies it.
        void Show(float d, bool save)
        {
            d = Mathf.Clamp(d, minDistance, maxDistance);
            if (slider != null)
            {
                restoring = true;
                slider.value = Mathf.Log(d / minDistance) / Mathf.Log(maxDistance / minDistance);
                restoring = false;
            }
            Apply(d);
            if (save) PlayerData.SetFloat(playerDataKey, d);
        }

        void Apply(float d)
        {
            VRCCameraSettings.ScreenCamera.FarClipPlane = d;
            if (label != null) label.text = "View distance: " + (d >= 1000f ? (d / 1000f).ToString("0.0") + " km" : Mathf.RoundToInt(d) + " m");
        }
    }
}
#endif
