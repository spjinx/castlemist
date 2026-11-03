// castlemist: which Haunted Doors stand (2026-10-27). Only maxActive doors at once, picked at random from
// the spawn points; when one is closed (sent away), another one, never the same, rises respawnMin..
// respawnMax seconds later. Synced: the owner (the instance master, or whoever last used a door) picks;
// everyone shows the same doors. The respawn time is the server clock's, so a late joiner and an owner
// handover keep it.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.Manual)]
    public class GW2HauntedDoors : UdonSharpBehaviour
    {
        public GW2HauntedDoor[] doors;
        public int maxActive = 2;
        public float respawnMin = 240f, respawnMax = 360f;

        [UdonSynced] int[] state;          // per door: 0 gone, 1 closed, 2 open
        [UdonSynced] int nextAt;           // server ms of the next rise (wrapping, compared by difference)
        [UdonSynced] int lastClosed = -1;
        [UdonSynced] bool started;

        void Start()
        {
            if (state == null || state.Length != doors.Length) state = new int[doors.Length];
            if (Networking.IsOwner(gameObject) && !started)
            {
                started = true;
                for (int k = 0; k < maxActive; ++k) Rise();
                RequestSerialization();
            }
            Apply();
            SendCustomEventDelayedSeconds(nameof(_Tick), 5f);
        }

        public override void OnDeserialization() { Apply(); }

        /// A door's Interact: closed -> open, open -> sent away (and the next rise scheduled).
        public void Use(int i)
        {
            if (state == null || i < 0 || i >= state.Length || state[i] == 0) return;
            Networking.SetOwner(Networking.LocalPlayer, gameObject);
            if (state[i] == 1) state[i] = 2;
            else
            {
                state[i] = 0;
                lastClosed = i;
                if (Active() < maxActive) nextAt = Networking.GetServerTimeInMilliseconds() + (int)(Random.Range(respawnMin, respawnMax) * 1000f);
            }
            RequestSerialization();
            Apply();
        }

        public void _Tick()
        {
            SendCustomEventDelayedSeconds(nameof(_Tick), 5f);
            if (!Networking.IsOwner(gameObject) || state == null || Active() >= maxActive) return;
            if (Networking.GetServerTimeInMilliseconds() - nextAt < 0) return;
            Rise();
            if (Active() < maxActive) nextAt = Networking.GetServerTimeInMilliseconds() + (int)(Random.Range(respawnMin, respawnMax) * 1000f);
            RequestSerialization();
            Apply();
        }

        int Active()
        {
            int n = 0;
            for (int i = 0; i < state.Length; ++i) if (state[i] != 0) ++n;
            return n;
        }

        /// One gone door (not the one just closed, when there is a choice) raised, closed.
        void Rise()
        {
            int free = 0;
            for (int i = 0; i < state.Length; ++i) if (state[i] == 0 && i != lastClosed) ++free;
            bool allowLast = free == 0;
            if (allowLast) for (int i = 0; i < state.Length; ++i) if (state[i] == 0) ++free;
            if (free == 0) return;
            int pick = Random.Range(0, free);
            for (int i = 0; i < state.Length; ++i)
            {
                if (state[i] != 0 || (i == lastClosed && !allowLast)) continue;
                if (pick-- == 0) { state[i] = 1; return; }
            }
        }

        void Apply()
        {
            if (state == null) return;
            for (int i = 0; i < doors.Length && i < state.Length; ++i) if (doors[i] != null) doors[i].Show(state[i]);
        }
    }
}
#endif
