// castlemist: a Halloween Carving Pumpkin (2026-10-27; wiki "Carving Pumpkin": "Interacting with one will
// turn it into a jack-o-lantern ... can be interacted with again after a period of time and reset on a
// per-character, per-instance basis"). Local, as in GW2: Interact carves it (the uncarved shell hidden,
// the carved pumpkin and its candle shown: the model's "On" clip), and resetSeconds later it is uncarved
// again ("Off"). scoreKeeper, when set, hears "_Carved" (the leaderboard). The importer's Pumpkins.cs
// places them.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2CarvingPumpkin : UdonSharpBehaviour
    {
        public Renderer[] uncarved;        // shown before carving
        public Renderer[] carved;          // the carved pumpkin, candle and flame
        public ParticleSystem carveBurst;  // optional
        public AudioSource carveSound;     // optional: plays the clips below
        public AudioClip[] slashClips;     // 2026-10-27: one at random per carve (GW2's foley "stab")
        public AudioClip[] splatClips;     // layered over it (GW2's prop "splat")
        public UdonSharpBehaviour scoreKeeper;
        public float resetSeconds = 1200f;
        public GameObject mapMarker;       // 2026-10-27: a jack-o'-lantern on your GMMenu map while it is carved (yours alone)

        bool isCarved;

        void Start() { Set(false); }

        public override void Interact()
        {
            if (isCarved) return;
            Set(true);
            if (carveBurst != null) carveBurst.Play();
            if (carveSound != null)
            {
                bool any = PlayOne(slashClips) | PlayOne(splatClips);
                if (!any) carveSound.Play();
            }
            if (scoreKeeper != null) scoreKeeper.SendCustomEvent("_Carved");
            SendCustomEventDelayedSeconds(nameof(_Reset), resetSeconds);
        }

        public void _Reset() { Set(false); }

        bool PlayOne(AudioClip[] clips)
        {
            if (clips == null || clips.Length == 0) return false;
            AudioClip c = clips[Random.Range(0, clips.Length)];
            if (c == null) return false;
            carveSound.pitch = Random.Range(0.95f, 1.08f);
            carveSound.PlayOneShot(c);
            return true;
        }

        void Set(bool c)
        {
            isCarved = c;
            if (uncarved != null) foreach (Renderer r in uncarved) if (r != null) r.enabled = !c;
            if (carved != null) foreach (Renderer r in carved) if (r != null) r.enabled = c;
            DisableInteractive = c;
            if (mapMarker != null) mapMarker.SetActive(c);
            InteractionText = "Carve";
        }
    }
}
#endif
