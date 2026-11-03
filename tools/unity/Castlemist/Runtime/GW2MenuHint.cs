// castlemist: first-join nudge to open the menu (2026-10-26). A small world-space token floats a little
// below the centre of view at a comfortable distance and follows the head with a damped lag (head
// position is followed directly, so walking never makes it swim; only turning lags). Desktop shows
// "Tab", VR the GMMenu's stick gesture. It stays until the player opens the menu: with the GMMenu, when
// its toggle reports open; without, on Tab or right stick down-then-up. Distance and size scale with
// the avatar's eye height. rememberAcrossVisits keeps it gone on later visits (VRChat player data).
// Editor/GW2MenuLook.cs builds one; any scene can have it.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDK3.Persistence;
using VRC.SDKBase;
using VRC.Udon.Common;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2MenuHint : UdonSharpBehaviour
    {
#if CASTLEMIST_GMMENU
        public Sylan.GMMenu.GMMenuToggle menuToggle;
#endif
        public Transform token;            // the hint's world-space canvas
        public CanvasGroup group;
        public GameObject desktopLabel, vrLabel;
        public float distance = 1.1f;      // metres at a 1.6 m eye height
        public float dropDegrees = 14f;    // below the centre of view
        public float followSeconds = 0.35f;
        public float showDelay = 3f;
        public bool rememberAcrossVisits = false;
        public string playerDataKey = "castlemist.menuHintDone";

        VRCPlayerApi local;
        Vector3 dir, baseScale;
        float shownAt = -1f, fade = 1f;
        bool done, fading, stickDown;

        void Start()
        {
            local = Networking.LocalPlayer;
            if (token == null || local == null) { enabled = false; return; }
            baseScale = token.localScale;
            bool vr = local.IsUserInVR();
            if (desktopLabel != null) desktopLabel.SetActive(!vr);
            if (vrLabel != null) vrLabel.SetActive(vr);
            token.gameObject.SetActive(false);
            SendCustomEventDelayedSeconds(nameof(_Show), showDelay);
        }

        public override void OnPlayerRestored(VRCPlayerApi player)
        {
            if (!rememberAcrossVisits || !player.isLocal) return;
            bool seen;
            if (PlayerData.TryGetBool(player, playerDataKey, out seen) && seen) Finish(false);
        }

        public void _Show()
        {
            if (done) return;
            shownAt = Time.time;
            dir = TargetDir(local.GetTrackingData(VRCPlayerApi.TrackingDataType.Head));
            token.gameObject.SetActive(true);
        }

        public override void PostLateUpdate()
        {
            if (shownAt < 0f || local == null) return;
            if (!done && Opened()) Finish(true);
            if (fading)
            {
                fade -= Time.deltaTime / 0.5f;
                if (fade <= 0f) { token.gameObject.SetActive(false); enabled = false; return; }
            }
            VRCPlayerApi.TrackingData head = local.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            float k = Mathf.Clamp(local.GetAvatarEyeHeightAsMeters() / 1.6f, 0.25f, 4f);
            dir = Vector3.Slerp(dir, TargetDir(head), 1f - Mathf.Exp(-Time.deltaTime / Mathf.Max(0.01f, followSeconds))).normalized;
            token.position = head.position + dir * distance * k;
            token.rotation = Quaternion.LookRotation(dir, Vector3.up);
            token.localScale = baseScale * k;
            if (group != null) group.alpha = fade * (0.85f + 0.15f * Mathf.Sin((Time.time - shownAt) * 3f));
        }

        Vector3 TargetDir(VRCPlayerApi.TrackingData head)
        {
            Vector3 f = head.rotation * Vector3.forward;
            Vector3 flat = new Vector3(f.x, 0f, f.z);
            if (flat.sqrMagnitude < 1e-4f) flat = head.rotation * Vector3.up;   // looking straight up / down
            flat.Normalize();
            float pitch = Mathf.Asin(Mathf.Clamp(f.y, -1f, 1f)) * Mathf.Rad2Deg - dropDegrees;
            Vector3 side = Vector3.Cross(Vector3.up, flat);
            return Quaternion.AngleAxis(-pitch, side) * flat;
        }

        bool Opened()
        {
#if CASTLEMIST_GMMENU
            if (menuToggle != null) return menuToggle.MenuState();
#endif
            return Input.GetKeyDown(KeyCode.Tab);
        }

        public override void InputLookVertical(float value, UdonInputEventArgs args)
        {
#if CASTLEMIST_GMMENU
            if (menuToggle != null) return;
#endif
            if (value <= -0.65f) stickDown = true;
            else if (value >= 0.5f && stickDown && !done) Finish(true);
        }

        void Finish(bool save)
        {
            done = true;
            fading = true;
            if (shownAt < 0f) { token.gameObject.SetActive(false); enabled = false; }
            if (save && rememberAcrossVisits) PlayerData.SetBool(playerDataKey, true);
        }
    }
}
#endif
