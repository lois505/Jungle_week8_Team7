#include "Common/Functions.hlsli"
#include "Common/SystemSamplers.hlsli"

Texture2D<float4> ShadowPreviewSource2D : register(t21);
Texture2DArray<float4> ShadowPreviewSourceArray : register(t22);

cbuffer ShadowDepthPreviewCB : register(b2)
{
    float U0;
    float V0;
    float U1;
    float V1;
    uint bSourceArray;
    float3 Padding;
}

PS_Input_UV VS(uint vertexID : SV_VertexID)
{
    return FullscreenTriangleVS(vertexID);
}

float4 PS(PS_Input_UV input) : SV_TARGET
{
    float2 uv = lerp(float2(U0, V0), float2(U1, V1), input.uv);
    float rawDepth = bSourceArray != 0
        ? ShadowPreviewSourceArray.SampleLevel(PointClampSampler, float3(uv, 0.0f), 0).r
        : ShadowPreviewSource2D.SampleLevel(PointClampSampler, uv, 0).r;

    // Preview-only contrast stretch for reverse-Z depth. The source shadow texture is unchanged.
    float display = pow(saturate(rawDepth), 0.15f);

    return float4(display, 0.0f, 0.0f, 1.0f);
}
