// castlemist: a floating platform that drifts up and down (2026-10-27; Mad King's Labyrinth's flower
// platforms 2498697 by the north-west waypoint, from the user's video of the game: each rises and
// sinks slowly, out of step with the others). The game moves them server-side; here the height is a
// sine of the server clock (amplitude, period, phase), so every client shows the same place without
// sync. The platform carries the local player while they stand on it (this behaviour's trigger, on
// top of the platform): each frame they are grounded on it, they move by the platform's step
// (TeleportTo, lerped for the others). The importer's FloatingPlatforms.cs sets them up.
// 2026-10-27: that per-frame TeleportTo turned players around (the user); carryPlayers is off and the
// scene's PlatformLink (jetdog8808's, vendored) carries them instead: it links to a collider with a
// Rigidbody, so the platform moves in FixedUpdate through its kinematic Rigidbody.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2FloatingPlatform : UdonSharpBehaviour
    {
        public Transform platform;          // what moves (the placed prop, with its collider)
        public float amplitude = 3f;        // metres up and down from where it was placed
        public float period = 12f;          // seconds for one rise and fall
        public float phase;                 // radians
        public bool carryPlayers = false;   // PlatformLink does it (see the header)

        Vector3 basePos, lastPos;
        bool localOn;
        Rigidbody body;

        void Start()
        {
            if (platform == null) platform = transform.parent;
            basePos = platform.position;
            lastPos = basePos;
            body = platform.GetComponent<Rigidbody>();
        }

        void FixedUpdate()
        {
            if (platform == null || period <= 0f) return;
            int ms = Networking.GetServerTimeInMilliseconds() & 0x7FFFFFFF;
            int cycle = Mathf.Max(1, Mathf.RoundToInt(period * 1000f));
            float t = (ms % cycle) / (float)cycle;
            Vector3 pos = basePos + Vector3.up * (amplitude * Mathf.Sin(t * 2f * Mathf.PI + phase));
            Vector3 step = pos - lastPos;
            if (body != null) body.MovePosition(pos);
            else platform.position = pos;
            lastPos = pos;
            if (!carryPlayers || !localOn || step.sqrMagnitude < 1e-10f) return;
            VRCPlayerApi me = Networking.LocalPlayer;
            if (me == null || !me.IsPlayerGrounded()) return;
            me.TeleportTo(me.GetPosition() + step, me.GetRotation(), VRC_SceneDescriptor.SpawnOrientation.Default, true);
        }

        public override void OnPlayerTriggerEnter(VRCPlayerApi player) { if (player != null && player.isLocal) localOn = true; }
        public override void OnPlayerTriggerExit(VRCPlayerApi player) { if (player != null && player.isLocal) localOn = false; }
    }
}
#endif
