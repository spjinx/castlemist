// castlemist: a GW2 prop that can be demolished in a VRChat world (2026-10-25). The world importer
// (Demolishables.cs) puts it on each placed prop whose model has a "Demolish" clip (map 506592's
// 84108), with an Animator on its controller (Idle -> Demolish -> Rubble on the "Demolished" bool),
// the dust particle systems the clip's bone keys time (ParticleEffects.Fit, played once on demand),
// the prop's own collider (PropCollider_<prop>, split out of the cell's by the exporter) and a
// trigger box to click.
//   Interact (GMMenu facilitator or GM only, when the GMMenu is in the world; anyone without it):
//   toggles the synced state. Demolishing plays the clip and the dust and turns the collider off;
//   clicking again rebuilds. A late joiner, or anyone deserialising within the first seconds after
//   load, snaps to the state without the dust.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.Manual)]
    public class GW2Demolishable : UdonSharpBehaviour
    {
        public Animator animator;          // bool "Demolished"; states "Idle", "Demolish", "Rubble"
        public ParticleSystem[] dust;      // played from the start of the clip
        public GameObject propCollider;    // off while demolished; may be null
        public int minPermission = 1;      // GMMenu: 1 facilitator, 2 GM
        public float snapWindow = 8f;      // seconds after load in which a state arrives without dust
#if CASTLEMIST_GMMENU
        public Sylan.GMMenu.PlayerPermissions permissions;
#endif

        [UdonSynced] public bool demolished;

        void Start()
        {
#if CASTLEMIST_GMMENU
            if (permissions != null) permissions.AddListener(this);
#endif
            OnPermissionUpdate();
            Apply(false);
        }

        /// GMMenu's PlayerPermissions event: clickable only for facilitators and GMs.
        public void OnPermissionUpdate()
        {
            bool allowed = true;
#if CASTLEMIST_GMMENU
            if (permissions != null) allowed = permissions.getPermissionLevel() >= minPermission;
#endif
            DisableInteractive = !allowed;
        }

        public override void Interact()
        {
            Networking.SetOwner(Networking.LocalPlayer, gameObject);
            demolished = !demolished;
            RequestSerialization();
            Apply(true);
        }

        public override void OnDeserialization()
        {
            Apply(Time.timeSinceLevelLoad > snapWindow);
        }

        void Apply(bool play)
        {
            InteractionText = demolished ? "Rebuild" : "Demolish";
            if (propCollider != null) propCollider.SetActive(!demolished);
            if (animator != null) animator.SetBool("Demolished", demolished);
            if (demolished)
            {
                if (play)
                {
                    if (dust != null) foreach (ParticleSystem ps in dust) if (ps != null) ps.Play(false);
                }
                else if (animator != null) animator.Play("Rubble", 0, 0f);
            }
            else
            {
                if (dust != null) foreach (ParticleSystem ps in dust) if (ps != null) ps.Stop(false, ParticleSystemStopBehavior.StopEmittingAndClear);
                if (animator != null) animator.Play("Idle", 0, 0f);
            }
        }
    }
}
#endif
