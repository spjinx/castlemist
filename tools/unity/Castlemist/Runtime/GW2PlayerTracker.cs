// castlemist: keeps this object on the local player at all times (2026-10-28), like CyanTrigger's
// PlayerTrackerV2. Local only: every client moves its own copy onto its own player, nothing is
// synced. Moved in PostLateUpdate, after VRChat has posed the avatar, so it does not lag a frame.
// Targets: the playspace origin, the player's position (feet), the head, either hand, or any
// humanoid bone (an avatar without that bone falls back to the player's position). Position,
// rotation and yaw-only rotation are each optional; the offset is in the target's space and can
// grow with the avatar's eye height. Put it on an empty parent of the thing to carry; keep
// colliders off it (or on a layer that ignores the player) or it pushes the player around.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    public enum GW2TrackTarget { Origin, Player, Head, LeftHand, RightHand, Bone }

    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2PlayerTracker : UdonSharpBehaviour
    {
        public GW2TrackTarget target = GW2TrackTarget.Player;
        public HumanBodyBones bone = HumanBodyBones.Chest;   // with target Bone
        public bool followPosition = true;
        public bool followRotation = true;
        public bool yawOnly = false;           // turn with the target around world up only
        public Vector3 positionOffset;         // in the target's space
        public Vector3 rotationOffset;         // euler degrees, after the target's rotation
        public bool scaleWithAvatar = false;   // offset and scale x eye height / 1.6 m

        VRCPlayerApi local;
        Vector3 baseScale;

        void Start()
        {
            local = Networking.LocalPlayer;
            if (local == null) { enabled = false; return; }
            baseScale = transform.localScale;
        }

        public override void PostLateUpdate()
        {
            if (local == null || !local.IsValid()) return;

            Vector3 pos = local.GetPosition();
            Quaternion rot = local.GetRotation();
            if (target == GW2TrackTarget.Bone)
            {
                Vector3 bp = local.GetBonePosition(bone);
                if (bp != Vector3.zero) { pos = bp; rot = local.GetBoneRotation(bone); }   // else no such bone
            }
            else if (target != GW2TrackTarget.Player)
            {
                VRCPlayerApi.TrackingDataType type = VRCPlayerApi.TrackingDataType.Origin;
                if (target == GW2TrackTarget.Head) type = VRCPlayerApi.TrackingDataType.Head;
                else if (target == GW2TrackTarget.LeftHand) type = VRCPlayerApi.TrackingDataType.LeftHand;
                else if (target == GW2TrackTarget.RightHand) type = VRCPlayerApi.TrackingDataType.RightHand;
                VRCPlayerApi.TrackingData t = local.GetTrackingData(type);
                pos = t.position;
                rot = t.rotation;
            }

            if (yawOnly) rot = Quaternion.Euler(0f, rot.eulerAngles.y, 0f);
            float k = scaleWithAvatar ? Mathf.Clamp(local.GetAvatarEyeHeightAsMeters() / 1.6f, 0.1f, 10f) : 1f;

            if (followPosition) transform.position = pos + rot * (positionOffset * k);
            if (followRotation) transform.rotation = rot * Quaternion.Euler(rotationOffset);
            if (scaleWithAvatar) transform.localScale = baseScale * k;
        }
    }
}
#endif
