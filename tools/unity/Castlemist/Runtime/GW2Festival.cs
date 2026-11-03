// castlemist: which festival dressing a GW2 map shows (2026-10-26): "Normal", or one festival's map
// layers (Lion's Arch: Halloween = layers 0 and 2). The world importer (MapLayers.cs) puts one on
// "GW2 Map Layers/GW2 Festivals", listing the festival-managed GW2MapLayers and the festival each
// belongs to (festivalOf; names[0] is "Normal", no festival). Synced: whoever picks a festival owns
// it; late joiners get it from OnDeserialization. _F0 .. _F7 select by index (menu buttons); a layer
// on several festivals' layers shows only while all of them are on (GW2MapLayer).

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.Manual)]
    public class GW2Festival : UdonSharpBehaviour
    {
        public string[] names;             // [0] "Normal", then one per festival
        public GW2MapLayer[] layers;       // every festival-managed layer
        public int[] festivalOf;           // per layer: its festival's index in names
        public Text label;                 // optional: shows the current festival
        [UdonSynced] public int current = 0;

        void Start() { Apply(); }

        public override void OnDeserialization() { Apply(); }

        public void Select(int f)
        {
            Networking.SetOwner(Networking.LocalPlayer, gameObject);
            current = names != null ? Mathf.Clamp(f, 0, names.Length - 1) : 0;
            RequestSerialization();
            Apply();
        }

        public void _F0() { Select(0); }
        public void _F1() { Select(1); }
        public void _F2() { Select(2); }
        public void _F3() { Select(3); }
        public void _F4() { Select(4); }
        public void _F5() { Select(5); }
        public void _F6() { Select(6); }
        public void _F7() { Select(7); }

        void Apply()
        {
            if (layers == null) return;
            // Every layer's state first, then show / hide: a prop shared by layers asks all of them.
            for (int i = 0; i < layers.Length; ++i)
                if (layers[i] != null) layers[i].SetLocal(festivalOf[i] == current);
            for (int i = 0; i < layers.Length; ++i)
                if (layers[i] != null) layers[i]._Refresh();
            if (label != null && names != null && current < names.Length) label.text = names[current];
        }
    }
}
#endif
