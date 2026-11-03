// castlemist: a GW2 door that opens while a player is near it (2026-10-25). The world importer
// (ProximityDoors.cs) puts it on a "GW2 Door Trigger" child of each placed prop whose model has
// "Open" and "Close" clips, with a trigger sphere around the door and the prop's Animator on its
// controller (Closed -> Opening -> Open -> Closing -> Closed on the bool "Open").
// Local: every client sees every player, so each opens the door for the same players without sync.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2ProximityDoor : UdonSharpBehaviour
    {
        public Animator animator;   // bool "Open"
        int inside;

        public override void OnPlayerTriggerEnter(VRCPlayerApi player)
        {
            ++inside;
            if (animator != null) animator.SetBool("Open", true);
        }

        public override void OnPlayerTriggerExit(VRCPlayerApi player)
        {
            inside = Mathf.Max(0, inside - 1);
            if (inside == 0 && animator != null) animator.SetBool("Open", false);
        }

        // A player who leaves the world inside the trigger gets no exit: start the count again.
        public override void OnPlayerLeft(VRCPlayerApi player)
        {
            inside = 0;
            if (animator != null) animator.SetBool("Open", false);
        }
    }
}
#endif
