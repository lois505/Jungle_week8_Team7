#include "Common/Functions.hlsli"
#include "Common/SystemResources.hlsli"
#include "Common/SystemSamplers.hlsli"

cbuffer ShadowBlurCB : register(b2)
{
    float2 InvTextureSize;
    float2 BlurDirection;
    float2 RectMin;
    float2 RectSize;
    uint SourceKind;
    uint SourceSlice;
    uint _Pad0;
    uint _Pad1;
};

PS_Input_UV VS(uint vertexID : SV_VertexID)
{
    return FullscreenTriangleVS(vertexID);
}

float2 PS(PS_Input_UV input) : SV_Target0
{
    float2 localUV = saturate(input.uv);
    float2 tileTexel = InvTextureSize / max(RectSize, float2(1e-6f, 1e-6f));
    float2 halfTexel = tileTexel * 0.5f;
    float2 dir = BlurDirection * tileTexel;

    const float w0 = 0.25f;
    const float w1 = 0.50f;
    const float w2 = 0.25f;

    float2 s0Local = clamp(localUV - dir, halfTexel, 1.0f - halfTexel);
    float2 s1Local = clamp(localUV, halfTexel, 1.0f - halfTexel);
    float2 s2Local = clamp(localUV + dir, halfTexel, 1.0f - halfTexel);

    float2 uv0 = RectMin + s0Local * RectSize;
    float2 uv1 = RectMin + s1Local * RectSize;
    float2 uv2 = RectMin + s2Local * RectSize;

    float2 m0;
    float2 m1;
    float2 m2;
    if (SourceKind == 0)
    {
        m0 = ShadowMapAtlasTexture.Sample(LinearClampSampler, uv0).xy;
        m1 = ShadowMapAtlasTexture.Sample(LinearClampSampler, uv1).xy;
        m2 = ShadowMapAtlasTexture.Sample(LinearClampSampler, uv2).xy;
    }
    else
    {
        m0 = DirectionalShadowArray.Sample(LinearClampSampler, float3(uv0, (float)SourceSlice)).xy;
        m1 = DirectionalShadowArray.Sample(LinearClampSampler, float3(uv1, (float)SourceSlice)).xy;
        m2 = DirectionalShadowArray.Sample(LinearClampSampler, float3(uv2, (float)SourceSlice)).xy;
    }

    return m0 * w0 + m1 * w1 + m2 * w2;
}
