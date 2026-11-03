// castlemist: a Haunted Door's handle (2026-10-27). The door itself only takes a knock (everyone hears
// it); its handles (one per leaf, on the GW2 ring knockers) open and close it, for GMs first: clickable
// from minPermission up (GMMenu: 1 facilitator, 2 GM; 0, or no PlayerPermissions, lets everyone). The
// door (GW2HauntedDoor) sets the label, "Open" or "Close".

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2DoorHandle : UdonSharpBehaviour
    {
        public GW2HauntedDoor door;
        public int minPermission = 2;
#if CASTLEMIST_GMMENU
        public Sylan.GMMenu.PlayerPermissions permissions;
#endif

        void Start()
        {
#if CASTLEMIST_GMMENU
            if (permissions != null) permissions.AddListener(this);
#endif
            OnPermissionUpdate();
        }

        /// GMMenu's PlayerPermissions event.
        public void OnPermissionUpdate() { DisableInteractive = !Allowed(); }

        bool Allowed()
        {
#if CASTLEMIST_GMMENU
            if (permissions != null && minPermission > 0) return permissions.getPermissionLevel() >= minPermission;
#endif
            return true;
        }

        public override void Interact() { if (door != null && Allowed()) door._Handle(); }
    }
}
#endif
