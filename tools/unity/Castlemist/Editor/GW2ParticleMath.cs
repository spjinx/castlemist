// The GW2 client's particle maths that a Unity import needs, one function per client routine.
// The C++ originals are castlemist::fx (src/fx/fx_math.cpp); the addresses and the evidence are in
// docs/research/gw2-particle-system.md (2026-09 build, amendments 2026-10-22 .. 2026-10-29).
// Keep the two in step: tools/unity/Tests~ checks the same hand values as tests/test_fx.cpp.
// No UnityEngine dependency (the Unity-free tests compile it); ParticleEffects builds the curves.

using System;

namespace Castlemist
{
    public static class GW2ParticleMath
    {
        /// The client's 18 curve presets (table 0x14257d870): 0 constant 1, 1 t, 2..5 a quartic bump
        /// and its 0.75 / 0.5 / 0.25-lifted copies, 6..8 three more bumps, 9 t^2, 10 t^4, 11..14 a
        /// cubic ease and its lifted copies, 15..17 (1 - |2t - 1|)^2, ^4, ^1. Past the table: 1.
        public static float CurvePreset(int id, float t)
        {
            float t2 = t * t, t3 = t2 * t, t4 = t3 * t;
            float bumpA = -1.0347f * t + 21.174f * t2 - 40.278f * t3 + 20.139f * t4;
            float ease = -0.6f * t + 4.8f * t2 - 3.2f * t3;
            float tent = 1f - MathF.Abs((t - 0.5f) + (t - 0.5f));
            switch (id)
            {
                case 0: return 1f;
                case 1: return t;
                case 2: return bumpA;
                case 3: return bumpA * 0.75f + 0.25f;
                case 4: return bumpA * 0.5f + 0.5f;
                case 5: return bumpA * 0.25f + 0.75f;
                case 6: return 8.0606f * t - 20.059f * t2 + 16.354f * t3 - 4.3561f * t4;
                case 7: return 9.3333f * t - 30.667f * t2 + 42.667f * t3 - 21.333f * t4;
                case 8: return 6.3333f * t - 11.4f * t2 + 5.0667f * t3;
                case 9: return t2;
                case 10: return t4;
                case 11: return ease;
                case 12: return ease * 0.75f + 0.25f;
                case 13: return ease * 0.5f + 0.5f;
                case 14: return ease * 0.25f + 0.75f;
                case 15: return tent * tent;
                case 16: return tent * tent * tent * tent;
                case 17: return tent;
                default: return 1f;
            }
        }

        /// The emitter flags word as the client keeps it (FUN_140d52160): runtime bit k = file bit kFrom[k].
        public static uint RuntimeFlags(uint fileFlags)
        {
            int[] kFrom = { 19, 0, 1, 6, 2, 12, 8, 7, 11, 10, 9, 4, 18, 16, 17, 13, 5, 3, 20, 21 };
            uint r = 0;
            for (int k = 0; k < kFrom.Length; ++k)
                if ((fileFlags & (1u << kFrom[k])) != 0) r |= 1u << k;
            return r;
        }

        /// The scale preset as applied: file flag 0x400 runs t backwards, 0x200 takes 1 - value.
        public static float ScalePreset(int id, float t, uint fileFlags)
        {
            uint rt = RuntimeFlags(fileFlags);
            float v = CurvePreset(id, (rt & 0x200) != 0 ? 1f - t : t);
            return (rt & 0x400) != 0 ? 1f - v : v;
        }

        /// The opacity preset as applied: file flag 0x80 takes 1 - value.
        public static float OpacityPreset(int id, float t, uint fileFlags)
        {
            float v = CurvePreset(id, t);
            return (RuntimeFlags(fileFlags) & 0x80) != 0 ? 1f - v : v;
        }

        /// The begin -> end colour weight at life fraction @p life (FUN_140ad3a30): ping-pongs every
        /// @p period, then the falloff window {lo, hi}; a zero window is a step at lo.
        public static float ColourWeight(float life, float period, float lo, float hi)
        {
            float x = 0f;
            if (period > 0f)
            {
                float n = MathF.Floor(life / period);
                float m = life - n * period;
                x = ((long)n & 1) == 0 ? m / period : 1f - m / period;
            }
            if (lo == hi) return x >= lo ? 1f : 0f;
            return Math.Clamp((x - lo) / (hi - lo), 0f, 1f);
        }

        /// The sprite builder's branch for a plane emitter's file alignmentType (FUN_140d52110):
        /// 0 -> 1 camera-facing, 1 -> 5 axis billboard along the motion, 2 -> 4 along the on-screen
        /// motion, 3 -> 2 faces the motion, 4 -> 0 faces alignmentDir, 5 -> 3 faces away. -1 past 5.
        public static int AlignmentCase(int fileType)
        {
            int[] k = { 1, 5, 4, 2, 0, 3 };
            return fileType >= 0 && fileType < k.Length ? k[fileType] : -1;
        }
    }
}
