// castlemist: one Halloween Haunted Door (2026-10-26; wiki "Haunted Door", Mad King's Labyrinth). Its
// manager (GW2HauntedDoors) decides which doors stand and syncs them; this one only shows its state.
// 2026-10-27: the door takes knocks (Interact while it stands closed: everyone hears the knock, it stays
// shut); its handles (GW2DoorHandle, on the ring knockers, GMs first) open it, and close it, when it
// sinks away (Lurk), is hidden entirely (model, mist, particles) and the manager raises another door
// somewhere else later. On the door model's Animator
// (bools "Present" and "Open"; its clips' events play the GW2 door sounds through GW2SoundPlayer).

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.NoVariableSync)]   // the knock is a network event
    public class GW2HauntedDoor : UdonSharpBehaviour
    {
        public GW2HauntedDoors manager;
        public int index;
        public Animator animator;          // bools "Present", "Open"
        public AudioSource knock;
        public Collider[] blockers;        // the door's own colliders, off while it is gone
        public Collider clickCollider;
        public float knockCooldown = 1.5f; // seconds between one player's knocks
        public GW2DoorHandle[] handles;    // open / close
        public GameObject mapMarker;       // 2026-10-27: its event circle on the GMMenu map, shown while it stands
        public Collider[] handleColliders;
        public Renderer[] visuals;         // everything drawn: off while gone
        public float lurkSeconds = 2f;     // the sinking-away clip, then hidden

        int shown = -1;                    // 0 gone, 1 closed, 2 open
        bool knockBusy;

        /// A knock on the closed door, for everyone.
        public override void Interact()
        {
            if (shown != 1 || knockBusy) return;
            knockBusy = true;
            SendCustomEventDelayedSeconds(nameof(_KnockReady), knockCooldown);
            SendCustomNetworkEvent(VRC.Udon.Common.Interfaces.NetworkEventTarget.All, nameof(KnockAll));
        }

        public void KnockAll() { if (knock != null) knock.Play(); }
        public void _KnockReady() { knockBusy = false; }

        /// A handle's Interact: closed -> open, open -> sent away (the manager syncs it).
        public void _Handle() { if (manager != null) manager.Use(index); }

        /// The manager's state for this door (0 gone, 1 closed, 2 open).
        public void Show(int s)
        {
            if (s == shown) return;
            bool first = shown < 0;
            shown = s;
            if (animator != null)
            {
                animator.SetBool("Present", s != 0);
                animator.SetBool("Open", s == 2);
                // a door gone at load starts hidden, not sinking away in front of everyone
                if (first && s == 0) animator.Play("Hidden", 0, 1f);
            }
            if (mapMarker != null) mapMarker.SetActive(s != 0);
            if (s != 0) SetVisible(true);
            else if (first) SetVisible(false);
            else SendCustomEventDelayedSeconds(nameof(_Hide), lurkSeconds);
            if (blockers != null)
                foreach (Collider c in blockers) if (c != null) c.enabled = s != 0;
            if (clickCollider != null) clickCollider.enabled = s == 1;
            InteractionText = "Knock";
            if (handleColliders != null)
                foreach (Collider c in handleColliders) if (c != null) c.enabled = s != 0;
            if (handles != null)
                foreach (GW2DoorHandle h in handles) if (h != null) h.InteractionText = s == 2 ? "Close" : "Open";
        }

        public void _Hide() { if (shown == 0) SetVisible(false); }

        void SetVisible(bool v)
        {
            if (visuals == null) return;
            foreach (Renderer r in visuals) if (r != null) r.enabled = v;
        }
    }
}
#endif
