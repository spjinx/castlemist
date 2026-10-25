// castlemist: everyone joins as a GMMenu player but can switch to facilitator or GM from the menu's
// options (2026-10-25). The GMMenu's PlayerPermissions keeps "players are GM" as the base level, which
// shows the role buttons to everyone; this drops each player's current level to startPermission once
// the menu has set it up. No whitelist.

#if CASTLEMIST_VRC && CASTLEMIST_GMMENU
using UdonSharp;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2StartAsPlayer : UdonSharpBehaviour
    {
        public Sylan.GMMenu.PlayerPermissions permissions;
        public int startPermission = 0;   // 0 player, 1 facilitator, 2 GM

        void Start()
        {
            // after PlayerPermissions.Start, which sets the current level to the base one
            SendCustomEventDelayedFrames(nameof(_Drop), 3);
        }

        public void _Drop()
        {
            if (permissions != null) permissions.SetTempPermission(startPermission);
        }
    }
}
#endif
