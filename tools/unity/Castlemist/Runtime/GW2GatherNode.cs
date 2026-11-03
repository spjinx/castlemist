// castlemist: a GW2 resource node (2026-10-27; Halloween's Raw Candy Corn / Rich Candy Corn Vein). Each
// player gathers on their own, as in GW2 (local, no sync): Interact strikes it (a burst of pieces flies
// out, and the node shrinks: its pieces go one by one, or a single piece scales down), after strikes it is
// spent and hidden, and it comes back respawnSeconds later. Each strike plays one of strikeClips (GW2's
// pick hit) with one of impactClips (its ore hit) through strikeSound, and the last one of spentClips
// (the node running dry). The wiki: Raw Candy
// Corn "can be mined at least 3 times", a Rich Candy Corn Vein 10 times. The importer's GatherNodes.cs
// places them.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2GatherNode : UdonSharpBehaviour
    {
        public GameObject visual;          // the node model, hidden while spent
        public GameObject[] pieces;        // hidden one by one as it is mined (none: visual scales down)
        public ParticleSystem burst;       // pieces flying out at each strike
        public AudioSource strikeSound;    // optional: plays the clips below
        public AudioClip[] strikeClips;    // one at random per strike
        public AudioClip[] impactClips;    // layered over it
        public AudioClip[] spentClips;     // the last strike
        public Collider clickCollider;
        public int strikes = 3;
        public float respawnSeconds = 600f;
        public float cooldown = 0.8f;      // seconds between strikes (GW2's gathering swing)
        public GameObject mapMarker;       // 2026-10-27: its ore-node icon on the GMMenu map, hidden while spent

        int left;
        bool busy;
        Vector3 fullScale;

        void Start() { left = strikes; if (visual != null) fullScale = visual.transform.localScale; }

        public override void Interact()
        {
            if (busy || left <= 0) return;
            busy = true;
            --left;
            if (burst != null) burst.Play();
            if (strikeSound != null)
            {
                bool any = PlayOne(strikeClips) | PlayOne(impactClips);
                if (left <= 0) any |= PlayOne(spentClips);
                if (!any) strikeSound.Play();
            }
            if (left <= 0) SendCustomEventDelayedSeconds(nameof(_Spent), 0.6f);
            else Wear();
            SendCustomEventDelayedSeconds(nameof(_Ready), cooldown);
            InteractionText = left > 0 ? "Gather (" + left + ")" : "Gather";
        }

        public void _Ready() { busy = false; }

        bool PlayOne(AudioClip[] clips)
        {
            if (clips == null || clips.Length == 0) return false;
            AudioClip c = clips[Random.Range(0, clips.Length)];
            if (c == null) return false;
            strikeSound.pitch = Random.Range(0.95f, 1.05f);
            strikeSound.PlayOneShot(c);
            return true;
        }

        /// What is left shows: pieces by share, or one piece scaled to 55..100%.
        void Wear()
        {
            float f = Mathf.Clamp01((float)left / Mathf.Max(strikes, 1));
            if (pieces != null && pieces.Length > 0)
            {
                int show = Mathf.CeilToInt(f * pieces.Length);
                for (int i = 0; i < pieces.Length; ++i) if (pieces[i] != null) pieces[i].SetActive(i < show);
            }
            else if (visual != null) visual.transform.localScale = fullScale * Mathf.Lerp(0.55f, 1f, f);
        }

        public void _Spent()
        {
            if (mapMarker != null) mapMarker.SetActive(false);
            if (visual != null) visual.SetActive(false);
            if (clickCollider != null) clickCollider.enabled = false;
            SendCustomEventDelayedSeconds(nameof(_Respawn), respawnSeconds);
        }

        public void _Respawn()
        {
            if (mapMarker != null) mapMarker.SetActive(true);
            left = strikes;
            Wear();
            if (visual != null) visual.SetActive(true);
            if (clickCollider != null) clickCollider.enabled = true;
            InteractionText = "Gather (" + left + ")";
        }
    }
}
#endif
