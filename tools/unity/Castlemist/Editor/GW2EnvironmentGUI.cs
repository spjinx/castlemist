// castlemist: the material inspectors of Castlemist/GW2 Particle Field, GW2 Haze, GW2 Cloud and GW2 Sky Card
// (tools/unity/Castlemist/Shaders; the shaders name them with CustomEditor), on GW2GUIBase's
// sections. The values are GW2's own (PackMapEnvDataPField, PackMapEnvDataHaze) as the importer
// writes them (PackMapEnvDataLayer for the clouds); each section says what the field means in the client.

using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public class GW2ParticleFieldGUI : GW2GUIBase
    {
        static readonly string[] Types = { "Type 1 (horizontal wobble, TwinkleStar)", "Type 2 (full wobble, stars / motes)" };

        public override void OnGUI(MaterialEditor materialEditor, MaterialProperty[] properties)
        {
            editor = materialEditor;
            props = properties;

            EditorGUILayout.LabelField("castlemist GW2 Particle Field", EditorStyles.boldLabel);
            Note("A map's ambient particles (environment.particleFields): one tile of motes that wraps around the " +
                 "camera, so the camera always flies through them. The mesh holds the tile; ParticleFields.cs builds it " +
                 "and the importer sets these from the manifest (docs/research/gw2-particle-fields.md).");

            if (Section("Field"))
            {
                MaterialProperty type = P("_FieldType");
                if (type != null)
                {
                    EditorGUI.showMixedValue = type.hasMixedValue;
                    EditorGUI.BeginChangeCheck();
                    int t = EditorGUILayout.Popup(new GUIContent("type", "The client's shader by field type"),
                                                  type.floatValue < 1.5f ? 0 : 1, Types);
                    if (EditorGUI.EndChangeCheck()) type.floatValue = t + 1;
                    EditorGUI.showMixedValue = false;
                }
                Prop("_Extent", "extent (tile size, GW2 units)", "The tile's edge; particleCount of them fill it (fixed in the mesh)");
                Prop("_Visibility", "Visibility", "The client's time-of-day / height factor (flags 0x2, 0x4); 1 here");
                EndSection();
            }
            if (Section("Motion"))
            {
                Vec2Pair("_Speed", "speed (min, max)", "Drift along fieldDirection", "deviationSpeed (min, max)", "Wobble rate");
                Prop("_Direction", "fieldDirection (map axes)", "Normalised; zero = no drift");
                Prop("_Deviation", "deviation (GW2 units)", "Wobble amplitude");
                EndSection();
            }
            if (Section("Life"))
            {
                Vec2Pair("_Life", "period (min, max) s", "Hidden part of each particle's cycle",
                         "lifetime (min, max) s", "Visible part of the cycle (the order is a reading, UNPROVEN)");
                EndSection();
            }
            if (Section("Texture"))
            {
                Tex("_MainTex", "texPath", "Drawn x 2, additive; alpha x the particle's");
                MaterialProperty fr = P("_Frames");
                if (fr != null)
                {
                    Vector4 v = fr.vectorValue;
                    EditorGUI.BeginChangeCheck();
                    Vector2 cr = EditorGUILayout.Vector2Field(new GUIContent("texColRow (cols, rows)", "Each particle takes a random frame"), new Vector2(v.y, v.z));
                    float fps = EditorGUILayout.FloatField(new GUIContent("texFPS", "0 = still (flag 0x200 animates)"), v.w);
                    if (EditorGUI.EndChangeCheck()) fr.vectorValue = new Vector4(v.x, Mathf.Max(1, cr.x), Mathf.Max(1, cr.y), Mathf.Max(0, fps));
                }
                EndSection();
            }
            if (Section("Advanced", false))
            {
                Prop("_UnityScale", "Unity units per GW2 unit");
                Prop("_SoftFade", "Soft against the scene (needs a depth texture)");
                editor.RenderQueueField();
                EndSection();
            }
        }

        /// One float4 property shown as two {min, max} pairs.
        void Vec2Pair(string name, string label0, string tip0, string label1, string tip1)
        {
            MaterialProperty p = P(name);
            if (p == null) return;
            Vector4 v = p.vectorValue;
            EditorGUI.showMixedValue = p.hasMixedValue;
            EditorGUI.BeginChangeCheck();
            Vector2 a = EditorGUILayout.Vector2Field(new GUIContent(label0, tip0), new Vector2(v.x, v.y));
            Vector2 b = EditorGUILayout.Vector2Field(new GUIContent(label1, tip1), new Vector2(v.z, v.w));
            if (EditorGUI.EndChangeCheck()) p.vectorValue = new Vector4(a.x, a.y, b.x, b.y);
            EditorGUI.showMixedValue = false;
        }
    }

    public class GW2HazeGUI : GW2GUIBase
    {
        public override void OnGUI(MaterialEditor materialEditor, MaterialProperty[] properties)
        {
            editor = materialEditor;
            props = properties;

            EditorGUILayout.LabelField("castlemist GW2 Haze", EditorStyles.boldLabel);
            Note("GW2's map haze (distance + height fog) as a screen-space volume: a cube with this material covers the " +
                 "map and fogs everything drawn before it. Needs the camera depth texture (in VRChat, a directional light " +
                 "with shadows). The importer sets these from manifest.environment.haze.");

            if (Section("Haze (PackMapEnvDataHaze)"))
            {
                Prop("_HazeNear", "nearColor (rgb, a)");
                Prop("_HazeFar", "farColor (rgb, a)");
                Prop("_HazeHeight", "heightColor (rgb, a = max amount)");
                Prop("_DistRange", "distRange (d0, d1) GW2 units", "Distance fog from d0 to d1");
                Prop("_HeightRange", "heightRange (h0, h1) map z", "Height fog; map z is up = -Z");
                Prop("_DepthCue", "depthCue (per GW2 unit)", "How fast the height fog builds with distance");
                Prop("_Amount", "Amount", "1 = GW2's");
                EndSection();
            }
            if (Section("Map frame", false))
            {
                Prop("_UnityScale", "Unity units per GW2 unit");
                Prop("_MapOriginY", "Map root world Y");
                EndSection();
            }
            if (Section("Probe scattering (Silent)", false))
            {
                Prop("_ProbeBlend", "Tint by the reflection probe");
                Prop("_BlurLevel", "Probe blur level");
                EndSection();
            }
            if (Section("Advanced", false))
            {
                Prop("_ApplySkybox", "Don't fog the skybox");
                Prop("_CullMode", "Cull");
                editor.RenderQueueField();
                EndSection();
            }
        }
    }

    public class GW2CloudGUI : GW2GUIBase
    {
        public override void OnGUI(MaterialEditor materialEditor, MaterialProperty[] properties)
        {
            editor = materialEditor;
            props = properties;

            EditorGUILayout.LabelField("castlemist GW2 Cloud", EditorStyles.boldLabel);
            Note("One of a map's cloud layers (environment.clouds): a flat plane at the layer's altitude that follows the " +
                 "camera, so it draws from above and below alike and stops short of the horizon, as in the game. " +
                 "GW2Clouds.cs builds it and the importer sets these from the manifest (docs/research/gw2-sky.md §10).");

            if (Section("Layer (PackMapEnvDataLayer)"))
            {
                Tex("_MainTex", "texture", "Sampled raw (sRGB off); alpha is the cloud cover and the parallax height");
                Prop("_Altitude", "altitude (GW2 units)", "The plane sits at map z = -altitude");
                Prop("_HeightOffset", "Height offset (Unity units)", "castlemist's: raises the plane above GW2's altitude; kept on a rebuild");
                Prop("_Plane", "skyDistance F, m, scale, depth", "Half-size F m (m = 0.5925 + 0.4075 extent); the texture tiles every 2F / scale; depth = parallax");
                Prop("_CutOut", "cutOut (GW2 units)", "Soft edge where the scene crosses the plane");
                Prop("_Fade", "fadeEnd, fadeWidth (GW2 units)", "Fades the layer out as the camera reaches its height");
                EndSection();
            }
            if (Section("Attributes (set 0, day)"))
            {
                Prop("_Attr", "brightness, density, haze, light", "light = lightIntensity x the sky's LightIntensity (sunClr.w)");
                Prop("_Velocity", "velocity (GW2 units/s)", "Scroll; the texture is anchored to the world");
                EndSection();
            }
            if (Section("Sun and fog"))
            {
                Prop("_SunDir", "sun direction (map axes)", "The GW2 sun's travel direction (lights[0])");
                Prop("_SunColor", "sun colour x intensity");
                Prop("_Ambient", "ambient (SH stand-in)", "GW2's shRed/Green/Blue are UNPROVEN; the importer uses the sun's light");
                Prop("_SceneSH", "Use Unity's scene SH instead");
                Prop("_FogFar", "haze farColor", "FogColorFar; the fog colour the layer fades to");
                Prop("_FogDist", "distRange (d0, d1), nearColor.w", "Linear fog over view depth (VS 1645)");
                Prop("_FogAmount", "Fog amount", "1 = GW2's");
                EndSection();
            }
            if (Section("Advanced", false))
            {
                Prop("_UnityScale", "Unity units per GW2 unit");
                Prop("_SoftFade", "Soft against the scene (needs a depth texture)");
                editor.RenderQueueField();
                EndSection();
            }
        }
    }

    public class GW2SkyCardGUI : GW2GUIBase
    {
        public override void OnGUI(MaterialEditor materialEditor, MaterialProperty[] properties)
        {
            editor = materialEditor;
            props = properties;

            EditorGUILayout.LabelField("castlemist GW2 Sky Card", EditorStyles.boldLabel);
            Note("One of a map's sky cards (environment.skyCards): a quad at the sky distance around the camera, turning " +
                 "about the zenith at its speed (the moon, cloud wisps, star fields). GW2SkyCards.cs builds it; the mesh " +
                 "holds the quad's size and texture rectangle (docs/research/gw2-sky.md §9).");

            if (Section("Card (PackMapEnvDataSkyCard, day)"))
            {
                Tex("_MainTex", "texture", "Sampled raw (sRGB off), clamped");
                Prop("_Card", "azimuth, latitude (rad), speed (rad/s)", "az = azimuth + speed x time; latitude 0 = horizon, pi/2 = zenith");
                Prop("_Attr", "brightness, density, minHaze, light", "light = lightIntensity x the sky's LightIntensity (sunClr.w)");
                Prop("_Haze", "sky hazeBottom, hazeFalloff, hazeDensity", "Fades the card toward the haze colour near the horizon");
                EndSection();
            }
            if (Section("Sun and fog"))
            {
                Prop("_SunDir", "sun direction (map axes)", "The GW2 sun's travel direction (lights[0])");
                Prop("_SunColor", "sun colour x intensity");
                Prop("_Ambient", "ambient (SH stand-in)", "GW2's shRed/Green/Blue are UNPROVEN; the importer uses the sun's light");
                Prop("_SceneSH", "Use Unity's scene SH instead");
                Prop("_FogFar", "haze farColor", "FogColorFar");
                EndSection();
            }
            if (Section("Advanced", false))
            {
                Prop("_UnityScale", "Unity units per GW2 unit");
                editor.RenderQueueField();
                EndSection();
            }
        }
    }
}
