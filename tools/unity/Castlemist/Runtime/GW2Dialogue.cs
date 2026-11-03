// castlemist: a GW2 NPC's dialogue (2026-10-26): Interact opens a floating panel above the NPC (local to
// the player who talks), the NPC's name and line over a scrolling list of the reply options; a reply
// goes to another node, or closes the panel (GW2's "talk end" options). The panel turns to face the
// player and closes when they walk off. The importer's Dialogues.cs fills the nodes from the GW2 wiki's
// transcriptions (the game's dialogue trees are server-side).
//   nodes:   lines[n] the NPC's line; optionStart[n] / optionCount[n] its replies in options
//   replies: options[k] the text, optionNext[k] the node it opens (-1 closes), optionEnd[k] the end icon

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2Dialogue : UdonSharpBehaviour
    {
        public string speaker;
        public string[] lines;
        public int[] optionStart;
        public int[] optionCount;
        public string[] options;
        public int[] optionNext;
        public bool[] optionEnd;

        public GameObject panel;           // the world-space canvas, off until talked to
        public Text speakerText, lineText;
        public GameObject[] rows;          // reply buttons (up to 8), shown per node
        public Text[] rowTexts;
        public GameObject[] rowEndIcons, rowMoreIcons;
        public ScrollRect scroll;
        public float closeDistance = 7f;

        int node;

        void Start() { if (panel != null) panel.SetActive(false); }

        public override void Interact() { Open(0); }

        void Open(int n)
        {
            if (lines == null || n < 0 || n >= lines.Length || panel == null) { _Close(); return; }
            bool wasOpen = panel.activeSelf;
            node = n;
            if (speakerText != null) speakerText.text = speaker;
            if (lineText != null) lineText.text = lines[n];
            int start = optionStart[n], count = optionCount[n];
            for (int i = 0; i < rows.Length; ++i)
            {
                bool on = i < count;
                rows[i].SetActive(on);
                if (!on) continue;
                rowTexts[i].text = options[start + i];
                bool end = optionEnd[start + i];
                if (rowEndIcons[i] != null) rowEndIcons[i].SetActive(end);
                if (rowMoreIcons[i] != null) rowMoreIcons[i].SetActive(!end);
            }
            panel.SetActive(true);
            if (scroll != null) scroll.verticalNormalizedPosition = 1f;
            Face();
            if (!wasOpen) SendCustomEventDelayedSeconds(nameof(_Check), 0.25f);
        }

        void Pick(int i)
        {
            if (lines == null || node >= optionCount.Length || i >= optionCount[node]) return;
            int next = optionNext[optionStart[node] + i];
            if (next < 0) _Close();
            else Open(next);
        }

        public void _O0() { Pick(0); }
        public void _O1() { Pick(1); }
        public void _O2() { Pick(2); }
        public void _O3() { Pick(3); }
        public void _O4() { Pick(4); }
        public void _O5() { Pick(5); }
        public void _O6() { Pick(6); }
        public void _O7() { Pick(7); }

        public void _Close() { if (panel != null) panel.SetActive(false); }

        /// While open: turned to the player, closed past closeDistance.
        public void _Check()
        {
            if (panel == null || !panel.activeSelf) return;
            VRCPlayerApi p = Networking.LocalPlayer;
            if (p == null) return;
            if (Vector3.Distance(p.GetPosition(), transform.position) > closeDistance) { _Close(); return; }
            Face();
            SendCustomEventDelayedSeconds(nameof(_Check), 0.25f);
        }

        void Face()
        {
            VRCPlayerApi p = Networking.LocalPlayer;
            if (p == null) return;
            Vector3 head = p.GetTrackingData(VRCPlayerApi.TrackingDataType.Head).position;
            Vector3 d = panel.transform.position - head;
            d.y = 0f;
            if (d.sqrMagnitude > 1e-4f) panel.transform.rotation = Quaternion.LookRotation(d.normalized, Vector3.up);
        }
    }
}
#endif
