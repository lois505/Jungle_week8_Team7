#include "Common/Functions.hlsli"
#include "Common/ForwardLightData.hlsli"

#define SHADOW_FILTER_VSM 2
#define SHADOW_FILTER_ESM 3

struct ShadowVSOutput
{
    float4 position : SV_POSITION;
};

ShadowVSOutput VS(VS_Input_PNCT input)
{
    ShadowVSOutput output;
    output.position = ApplyMVP(input.position);
    return output;
}

float2 PS(ShadowVSOutput input) : SV_Target0
{
    float depth = saturate(input.position.z);

    if (ShadowFilterMode == SHADOW_FILTER_ESM)
    {
        const float exponent = 100.f;
        return float2(exp(exponent * depth), 0.0f);
    }

    return float2(depth, depth * depth);
}
