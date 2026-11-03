#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2ClickTeleport : UdonSharpBehaviour
    {
        public Transform destination;

        public override void Interact()
        {
            VRCPlayerApi player = Networking.LocalPlayer;
            if (player == null || destination == null) return;
            player.TeleportTo(destination.position, destination.rotation);
        }
    }
}
#endif
