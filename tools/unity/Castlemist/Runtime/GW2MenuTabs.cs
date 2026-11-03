// castlemist: GW2-style selected-tab highlight for the GMMenu (2026-10-26). The menu's MenuTab shows
// a panel by its CanvasGroup alpha and has no selected look of its own; while the menu canvas is on
// this lights each tab's highlight when its panel is the one showing.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2MenuTabs : UdonSharpBehaviour
    {
        public Canvas canvas;
        public CanvasGroup[] panels;
        public GameObject[] highlights;    // same order as panels

        void Update()
        {
            if (canvas == null || !canvas.enabled || panels == null || highlights == null) return;
            int n = Mathf.Min(panels.Length, highlights.Length);
            for (int k = 0; k < n; ++k)
            {
                if (panels[k] == null || highlights[k] == null) continue;
                bool on = panels[k].alpha > 0.5f;
                if (highlights[k].activeSelf != on) highlights[k].SetActive(on);
            }
        }
    }
}
#endif
