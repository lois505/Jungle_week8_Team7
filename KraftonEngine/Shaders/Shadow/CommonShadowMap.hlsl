#include "Common/Functions.hlsli"

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

// Depth-only shadow maps do not need a pixel shader. Keep this entry point
// for pipelines/tools that expect one; the renderer can bind null PS instead.
void PS(ShadowVSOutput input)
{
}
