// castlemist: the GMMenu's time-of-day slider for GMs (2026-10-26). Dragging it holds GW2DayNight at
// that day weight for everyone (0 night .. 1 day; SetFixedWeight turns the clock cycle off); the
// menu's "GW2 clock" button calls GW2DayNight.CycleOn to hand it back to the clock. While nobody drags,
// the slider follows the current weight (the clock's, or another GM's). onValueChanged calls _OnSlider.

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2DayNightSlider : UdonSharpBehaviour
    {
        public GW2DayNight dayNight;
        public Slider slider;
        public Text label;                 // "Daylight 75% (held)"

        bool restoring;
        float next, draggedAt = -10f;

        void Update()
        {
            if (dayNight == null || slider == null || Time.time < next) return;
            next = Time.time + 0.5f;
            if (Time.time - draggedAt < 1f) return;   // the local drag wins until it settles
            float w = dayNight.Weight();
            restoring = true;
            slider.value = w;
            restoring = false;
            Label(w);
        }

        public void _OnSlider()
        {
            if (restoring || dayNight == null) return;
            draggedAt = Time.time;
            dayNight.SetFixedWeight(slider.value);
            Label(slider.value);
        }

        void Label(float w)
        {
            if (label == null) return;
            string phase = w >= 0.99f ? "Day" : w <= 0.01f ? "Night" : "Daylight " + Mathf.RoundToInt(w * 100f) + "%";
            label.text = "Time of day: " + phase + (dayNight.cycleOn ? " (GW2 clock)" : " (held)");
        }
    }
}
#endif
