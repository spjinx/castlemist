// castlemist: one GW2 map layer (the laye chunk: festival dressing, story states) that can be
// switched on and off in a VRChat world (2026-10-25). The world importer (MapLayers.cs) puts one on
// "GW2 Map Layers/<name>" per layer with props, listing them. Synced: whoever toggles it owns it.
// ToggleLayer / LayerOn / LayerOff are for a GMMenu button. A prop on several layers is shown only
// while all of them are on (each layer hides it; showing asks the others). 2026-10-26: a festival's
// layers are driven by GW2Festival instead (SetLocal / _Refresh, no sync of their own).

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.Manual)]
    public class GW2MapLayer : UdonSharpBehaviour
    {
        public GameObject[] objects;
        public GW2MapLayer[] others;       // every other layer, to ask before showing a shared prop
        [UdonSynced] public bool on = true;

        void Start() { Apply(); }

        public override void OnDeserialization() { Apply(); }

        public void ToggleLayer() { Set(!on); }
        public void LayerOn() { Set(true); }
        public void LayerOff() { Set(false); }

        void Set(bool v)
        {
            Networking.SetOwner(Networking.LocalPlayer, gameObject);
            on = v;
            RequestSerialization();
            Apply();
        }

        /// GW2Festival's: this layer on or off for the local client only (the festival's own sync carries
        /// it); call _Refresh on every layer after setting them all.
        public void SetLocal(bool v) { on = v; }

        public void _Refresh() { Apply(); }

        /// True when @p go is listed by an off layer.
        public bool Hides(GameObject go)
        {
            if (on || objects == null) return false;
            foreach (GameObject o in objects) if (o == go) return true;
            return false;
        }

        void Apply()
        {
            if (objects == null) return;
            foreach (GameObject o in objects)
            {
                if (o == null) continue;
                bool show = on;
                if (show && others != null)
                    foreach (GW2MapLayer l in others) if (l != null && l != this && l.Hides(o)) { show = false; break; }
                o.SetActive(show);
            }
        }
    }
}
#endif
