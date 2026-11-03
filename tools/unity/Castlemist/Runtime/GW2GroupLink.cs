// castlemist: a button that opens a VRChat group's page in the client, where the player can join it
// (2026-10-26). The GMMenu's first tab (Editor/GW2MenuLook.cs) uses it for the GW2VRC.2917 group.

#if CASTLEMIST_VRC
using UdonSharp;
using VRC.Economy;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2GroupLink : UdonSharpBehaviour
    {
        public string groupId = "grp_c5aadfcd-e567-4018-a36a-8f32916a285e";

        public void OpenGroup()
        {
            if (!string.IsNullOrEmpty(groupId)) Store.OpenGroupPage(groupId);
        }
    }
}
#endif
