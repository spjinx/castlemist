// castlemist: GW2-styled UI (2026-10-26), Unity's UI-Default with three additions for the GMMenu skin
// (Editor/GW2MenuLook.cs):
//   _MaskTex: a ragged GW2 window mask (red channel) over the image rect: painted panels get torn edges.
//   _Remap:   the GMMenu scripts set hard-coded pink button colours at runtime (active 207,44,179,
//             inactive 153,76,140, selected 255,204,247); with _Remap on, those vertex colours become
//             _ActiveColor / _InactiveColor / _SelectedColor, so the package needs no edits.
//   _Stroke:  GW2's brush strokes (22054, 22059, 22065) are white on black with no alpha: the red
//             channel becomes alpha and the colour comes from the vertex colour.
//   _ZTest:   Always for the first-join hint (Runtime/GW2MenuHint), which must show through walls.
Shader "Castlemist/GW2 UI"
{
    Properties
    {
        [PerRendererData] _MainTex ("Sprite Texture", 2D) = "white" {}
        _Color ("Tint", Color) = (1,1,1,1)
        _MaskTex ("Edge Mask (R)", 2D) = "white" {}
        [Toggle] _Stroke ("Brush stroke (alpha from red)", Float) = 0
        [Toggle] _Remap ("Remap GMMenu colours", Float) = 0
        _InactiveColor ("Inactive", Color) = (0.11, 0.12, 0.14, 0.85)
        _ActiveColor ("Active", Color) = (0.78, 0.6, 0.3, 0.95)
        _SelectedColor ("Selected", Color) = (0.95, 0.84, 0.58, 0.9)
        [Enum(UnityEngine.Rendering.CompareFunction)] _ZTest ("ZTest", Float) = 4

        _StencilComp ("Stencil Comparison", Float) = 8
        _Stencil ("Stencil ID", Float) = 0
        _StencilOp ("Stencil Operation", Float) = 0
        _StencilWriteMask ("Stencil Write Mask", Float) = 255
        _StencilReadMask ("Stencil Read Mask", Float) = 255
        _ColorMask ("Color Mask", Float) = 15
        [Toggle(UNITY_UI_ALPHACLIP)] _UseUIAlphaClip ("Use Alpha Clip", Float) = 0
    }

    SubShader
    {
        Tags { "Queue"="Transparent" "IgnoreProjector"="True" "RenderType"="Transparent" "PreviewType"="Plane" "CanUseSpriteAtlas"="True" }

        Stencil
        {
            Ref [_Stencil]
            Comp [_StencilComp]
            Pass [_StencilOp]
            ReadMask [_StencilReadMask]
            WriteMask [_StencilWriteMask]
        }

        Cull Off
        Lighting Off
        ZWrite Off
        ZTest [_ZTest]
        Blend SrcAlpha OneMinusSrcAlpha
        ColorMask [_ColorMask]

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 2.0
            #include "UnityCG.cginc"
            #include "UnityUI.cginc"
            #pragma multi_compile_local _ UNITY_UI_CLIP_RECT
            #pragma multi_compile_local _ UNITY_UI_ALPHACLIP

            struct appdata_t
            {
                float4 vertex : POSITION;
                float4 color : COLOR;
                float2 texcoord : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f
            {
                float4 vertex : SV_POSITION;
                fixed4 color : COLOR;
                float2 texcoord : TEXCOORD0;
                float4 worldPosition : TEXCOORD1;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            sampler2D _MainTex, _MaskTex;
            fixed4 _Color, _TextureSampleAdd, _InactiveColor, _ActiveColor, _SelectedColor;
            float4 _ClipRect, _MainTex_ST;
            float _Remap, _Stroke;

            v2f vert(appdata_t v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.worldPosition = v.vertex;
                o.vertex = UnityObjectToClipPos(v.vertex);
                o.texcoord = TRANSFORM_TEX(v.texcoord, _MainTex);
                float4 c = v.color;
                if (_Remap > 0.5)
                {
                    // gamma or linear canvas: selected is the only one with much green; active has
                    // the widest red-green gap (gamma 0.64 / linear 0.6 against 0.3 / 0.25)
                    if (c.g > 0.45) c = _SelectedColor;
                    else if (c.r - c.g > 0.45) c = _ActiveColor;
                    else c = float4(_InactiveColor.rgb, _InactiveColor.a * saturate(c.a / 0.75));
                }
                o.color = c * _Color;
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                half4 tex = tex2D(_MainTex, i.texcoord) + _TextureSampleAdd;
                if (_Stroke > 0.5) tex = half4(1, 1, 1, tex.r * tex.a);
                half4 col = tex * i.color;
                col.a *= tex2D(_MaskTex, i.texcoord).r;
                #ifdef UNITY_UI_CLIP_RECT
                col.a *= UnityGet2DClipping(i.worldPosition.xy, _ClipRect);
                #endif
                #ifdef UNITY_UI_ALPHACLIP
                clip(col.a - 0.001);
                #endif
                return col;
            }
            ENDCG
        }
    }
}
