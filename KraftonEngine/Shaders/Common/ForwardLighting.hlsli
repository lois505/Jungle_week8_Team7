#ifndef FORWARD_LIGHTING_HLSLI
#define FORWARD_LIGHTING_HLSLI

#include "Common/ForwardLightData.hlsli"
#include "Common/SystemSamplers.hlsli"
#include "Common/SystemResources.hlsli"

#define SHADOW_FILTER_NONE 0
#define SHADOW_FILTER_PCF_BOX 1
#define SHADOW_FILTER_VSM 2
#define SHADOW_FILTER_ESM 3
#define SHADOW_FILTER_PCF_POISSON 4

//  Shadow Sharpening
float ApplyShadowSharpen(float visibility, float sharpen)
{
    float width = lerp(0.5f, 0.05f, saturate(sharpen));

    float edge0 = 0.5f - width * 0.5f;
    float edge1 = 0.5f + width * 0.5f;

    return smoothstep(edge0, edge1, saturate(visibility));
}

float SampleShadowAtlas(float2 uv)
{
    return ShadowMapAtlasTexture.SampleLevel(PointClampSampler, uv, 0).r;
}

//  PCF (주변 픽셀을 다 더하고 평균내어 결정)
//  rect : xy(top-left), zw(width-height)
float SampleShadowAtlasPCFBox(float4 rect, float2 localUV, float currentDepth, float bias, float2 atlasTileSize)
{
    //  output means visibility
    float output = 0.0f;

    float2 texelInTile = 1.0f / atlasTileSize;

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float2 sampleLocalUV = localUV + float2(x, y) * texelInTile;

            sampleLocalUV = clamp(sampleLocalUV, texelInTile * 0.5f, 1.0f - texelInTile * 0.5f);

            float2 uv = rect.xy + sampleLocalUV * rect.zw;

            float storedDepth = ShadowMapAtlasTexture.SampleLevel(PointClampSampler, uv, 0).r;

            output += (currentDepth + bias >= storedDepth) ? 1.0f : 0.0f;
        }
    }

    //  Average
    return output / 9.0f;
}

//  Fixed Poisson
static const float2 PoissonDisk16[16] =
{
    float2(-0.94201624f, -0.39906216f),
    float2(0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f),
    float2(0.34495938f, 0.29387760f),
    float2(-0.91588581f, 0.45771432f),
    float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f, 0.27676845f),
    float2(0.97484398f, 0.75648379f),
    float2(0.44323325f, -0.97511554f),
    float2(0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f),
    float2(0.79197514f, 0.19090188f),
    float2(-0.24188840f, 0.99706507f),
    float2(-0.81409955f, 0.91437590f),
    float2(0.19984126f, 0.78641367f),
    float2(0.14383161f, -0.14100790f)
};

float SampleShadowAtlasPCFPoisson(float4 rect, float2 localUV, float currentDepth, float bias, float2 atlasTileSize)
{
    float output = 0.0f;

    float2 texelInTile = 1.0f / atlasTileSize;

    const float radius = 2.5f;

    [unroll]
    for (int i = 0; i < 16; i++)
    {
        float2 sampleLocalUV = localUV + PoissonDisk16[i] * radius * texelInTile;

        sampleLocalUV = clamp(sampleLocalUV, texelInTile * 0.5f, 1.0f - texelInTile * 0.5f);

        float2 uv = rect.xy + sampleLocalUV * rect.zw;

        float storedDepth = ShadowMapAtlasTexture.SampleLevel(PointClampSampler, uv, 0).r;

        output += (currentDepth + bias >= storedDepth) ? 1.0f : 0.0f;
    }

    return output / 16.0f;
}

float2 ProjectToShadowUV(float4 lightClip)
{
    float3 ndc = lightClip.xyz / max(lightClip.w, 0.0001f);
    return float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
}

bool IsValidShadowRect(float4 rect)
{
    return rect.z > 0.0f && rect.w > 0.0f;
}


float CalcShadowFromView(FLocalShadowInfo shadow, uint viewIndex, float3 worldPos, float3 N, float3 L)
{
    float4 rect = shadow.AtlasRect[viewIndex];
    if (!IsValidShadowRect(rect))
    {
        return 1.0f;
    }

    float4 lightClip = mul(float4(worldPos, 1.0f), shadow.LightViewProj[viewIndex]);
    if (lightClip.w <= 0.0f)
    {
        return 1.0f;
    }

    float3 ndc = lightClip.xyz / lightClip.w;
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    float2 localUV = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    uint atlasWidth;
    uint atlasHeight;
    ShadowMapAtlasTexture.GetDimensions(atlasWidth, atlasHeight);
    float2 tileSize = max(rect.zw * float2(atlasWidth, atlasHeight), float2(1.0f, 1.0f));
    float2 halfTexelInTile = 0.5f / tileSize;
    localUV = clamp(localUV, halfTexelInTile, 1.0f - halfTexelInTile);

    float2 atlasUV = rect.xy + localUV * rect.zw;

    //  Bias Calculate
    float slope = 1.0f - saturate(dot(normalize(N), normalize(L)));
    float bias = shadow.Bias + shadow.SlopeBias * slope;

    float shadowVisibility = 0.0f;
    if (ShadowFilterMode == SHADOW_FILTER_PCF_BOX)
    {
        shadowVisibility = SampleShadowAtlasPCFBox(rect, localUV, ndc.z, bias, tileSize);
    }
    else if (ShadowFilterMode == SHADOW_FILTER_PCF_POISSON)
    {
        shadowVisibility = SampleShadowAtlasPCFPoisson(rect, localUV, ndc.z, bias, tileSize);
    }
    else
    {
        float storedDepth = SampleShadowAtlas(atlasUV);

        shadowVisibility = (ndc.z + bias >= storedDepth) ? 1.0f : 0.0f;
    }

    return ApplyShadowSharpen(shadowVisibility, shadow.Sharpen);
}

uint SelectPointShadowFace(float3 lightToPixel)
{
    float3 a = abs(lightToPixel);
    if (a.x >= a.y && a.x >= a.z)
    {
        return lightToPixel.x >= 0.0f ? 0 : 1;
    }
    if (a.y >= a.x && a.y >= a.z)
    {
        return lightToPixel.y >= 0.0f ? 2 : 3;
    }
    return lightToPixel.z >= 0.0f ? 4 : 5;
}

float CalcLocalShadow(uint lightIndex, FLightInfo light, float3 worldPos, float3 N)
{
    FLocalShadowInfo shadow = LocalLights[lightIndex];
    if (shadow.CastShadow == 0)
    {
        return 1.0f;
    }

    float3 L = normalize(light.Position - worldPos);

    if (shadow.ShadowType == LIGHT_TYPE_SPOT)
    {
        return CalcShadowFromView(shadow, 0, worldPos, N, L);
    }

    if (shadow.ShadowType == LIGHT_TYPE_POINT)
    {
        uint faceIndex = SelectPointShadowFace(worldPos - light.Position);
        return CalcShadowFromView(shadow, faceIndex, worldPos, N, L);
    }

    return 1.0f;
}

float CalcAttenuation(float dist, float radius, float falloff)
{
    float ratio = saturate(dist / max(radius, 0.0001f));
    return pow(1.0f - ratio, falloff);
}

float3 CalcAmbient(float3 lightColor, float intensity)
{
    return lightColor * intensity;
}

float3 CalcDirectionalDiffuse(float3 lightColor, float3 lightDir, float intensity, float3 N)
{
    float NdotL = saturate(dot(N, -lightDir));
    return lightColor * intensity * NdotL;
}

float3 CalcDirectionalSpecular(float3 lightColor, float3 lightDir, float intensity,
                               float3 N, float3 V, float shininess)
{
    float3 H = normalize(-lightDir + V);
    float NdotH = saturate(dot(N, H));
    return lightColor * intensity * pow(NdotH, max(shininess, 1.0f));
}

float3 GetHeatmapColor(float value)
{
    float3 color;
    color.r = saturate(min(4.0 * value - 1.5, -4.0 * value + 4.5));
    color.g = saturate(min(4.0 * value - 0.5, -4.0 * value + 3.5));
    color.b = saturate(min(4.0 * value + 0.5, -4.0 * value + 2.5));
    return color;
}

uint DepthToClusterSlice(float viewDepth)
{
    float safeDepth = clamp(viewDepth, CullState.NearZ, CullState.FarZ);
    float logDepth = log(safeDepth / CullState.NearZ) / log(CullState.FarZ / CullState.NearZ);
    return min((uint)floor(logDepth * CullState.ClusterZ), CullState.ClusterZ - 1);
}

uint ComputeClusterIndex(float4 screenPos, float3 worldPos)
{
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    uint tileX = min((uint)(screenPos.x / CullState.ScreenWidth * CullState.ClusterX), CullState.ClusterX - 1);
    uint tileY = min((uint)(screenPos.y / CullState.ScreenHeight * CullState.ClusterY), CullState.ClusterY - 1);
    uint sliceZ = DepthToClusterSlice(abs(viewPos.z));

    return sliceZ * CullState.ClusterX * CullState.ClusterY
        + tileY * CullState.ClusterX
        + tileX;
}

float3 CalcLightDiffuse(FLightInfo light, float3 worldPos, float3 N)
{
    float3 L = light.Position - worldPos;
    float dist = length(L);
    L = normalize(L);

    float atten = CalcAttenuation(dist, light.AttenuationRadius, light.FalloffExponent);
    float NdotL = saturate(dot(N, L));

    float spotFactor = 1.0f;
    if (light.LightType == LIGHT_TYPE_SPOT)
    {
        float cosAngle = dot(-L, normalize(light.Direction));
        spotFactor = smoothstep(light.OuterConeCos, light.InnerConeCos, cosAngle);
    }

    return light.Color.rgb * light.Intensity * NdotL * atten * spotFactor;
}

float3 CalcLightSpecular(FLightInfo light, float3 worldPos, float3 N, float3 V, float shininess)
{
    float3 L = normalize(light.Position - worldPos);
    float dist = length(light.Position - worldPos);
    float atten = CalcAttenuation(dist, light.AttenuationRadius, light.FalloffExponent);

    float spotFactor = 1.0f;
    if (light.LightType == LIGHT_TYPE_SPOT)
    {
        float cosAngle = dot(-L, normalize(light.Direction));
        spotFactor = smoothstep(light.OuterConeCos, light.InnerConeCos, cosAngle);
    }

    float3 H = normalize(L + V);
    float NdotH = saturate(dot(N, H));
    return light.Color.rgb * light.Intensity * pow(NdotH, max(shininess, 1.0f)) * atten * spotFactor;
}

void AccumulatePointSpotDiffuse(float3 worldPos, float3 N, float4 screenPos, inout float3 result)
{
    if (LightCullingMode == LIGHT_CULLING_TILE && NumTilesX > 0 && NumTilesY > 0)
    {
        uint2 tileCoord = min(uint2(screenPos.xy) / TILE_SIZE, uint2(NumTilesX - 1, NumTilesY - 1));
        uint tileIdx = tileCoord.y * NumTilesX + tileCoord.x;
        uint2 gridData = TileLightGrid[tileIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = TileLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos, N);
        }
    }
    else if (LightCullingMode == LIGHT_CULLING_CLUSTER)
    {
        uint clusterIdx = ComputeClusterIndex(screenPos, worldPos);
        uint2 gridData = g_ClusterLightGrid[clusterIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = g_ClusterLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos, N);
        }
    }
    else
    {
        for (uint i = 0; i < NumActivePointLights + NumActiveSpotLights; ++i)
        {
            FLightInfo light = AllLights[i];
            result += CalcLightDiffuse(light, worldPos, N) * CalcLocalShadow(i, light, worldPos, N);
        }
    }
}

void AccumulatePointSpotSpecular(float3 worldPos, float3 N, float3 V, float shininess, float4 screenPos,
                                 inout float3 result)
{
    if (LightCullingMode == LIGHT_CULLING_TILE && NumTilesX > 0 && NumTilesY > 0)
    {
        uint2 tileCoord = min(uint2(screenPos.xy) / TILE_SIZE, uint2(NumTilesX - 1, NumTilesY - 1));
        uint tileIdx = tileCoord.y * NumTilesX + tileCoord.x;
        uint2 gridData = TileLightGrid[tileIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = TileLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightSpecular(light, worldPos, N, V, shininess) * CalcLocalShadow(
                lightIndex, light, worldPos, N);
        }
    }
    else if (LightCullingMode == LIGHT_CULLING_CLUSTER)
    {
        uint clusterIdx = ComputeClusterIndex(screenPos, worldPos);
        uint2 gridData = g_ClusterLightGrid[clusterIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = g_ClusterLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightSpecular(light, worldPos, N, V, shininess) * CalcLocalShadow(
                lightIndex, light, worldPos, N);
        }
    }
    else
    {
        for (uint i = 0; i < NumActivePointLights + NumActiveSpotLights; ++i)
        {
            FLightInfo light = AllLights[i];
            result += CalcLightSpecular(light, worldPos, N, V, shininess) * CalcLocalShadow(i, light, worldPos, N);
        }
    }
}

#if defined(LIGHTING_MODEL_TOON) && LIGHTING_MODEL_TOON
static const float g_ToonSteps = 4.0f;
static const float g_ToonDarknessFloor = 0.25f;
static const float g_ToonRimMin = 0.55f;
static const float g_ToonRimMax = 0.85f;
static const float g_ToonRimStrength = 0.25f;

float ToonStep(float NdotL)
{
    float x = saturate(NdotL);
    float stepped = smoothstep(g_ToonDarknessFloor, 1.0f, x * g_ToonSteps);
    stepped /= max(g_ToonSteps - 1.0f, 1.0f);
    return lerp(g_ToonDarknessFloor, 1.0f, saturate(stepped));
}

float3 CalcToonDirectionalDiffuse(float3 N)
{
    float band = ToonStep(saturate(dot(N, -DirectionalLight.Direction)));
    return DirectionalLight.Color.rgb * DirectionalLight.Intensity * band;
}

float3 CalcToonPointSpotDiffuse(FLightInfo light, float3 worldPos, float3 N)
{
    float3 L = light.Position - worldPos;
    float dist = length(L);
    L = normalize(L);

    float atten = CalcAttenuation(dist, light.AttenuationRadius, light.FalloffExponent);
    float band = ToonStep(saturate(dot(N, L)));

    float spotFactor = 1.0f;
    if (light.LightType == LIGHT_TYPE_SPOT)
    {
        float cosAngle = dot(-L, normalize(light.Direction));
        spotFactor = smoothstep(light.OuterConeCos, light.InnerConeCos, cosAngle);
    }

    return light.Color.rgb * light.Intensity * atten * spotFactor * band;
}

void AccumulateToonPointSpotDiffuse(float3 worldPos, float3 N, float4 screenPos, inout float3 result)
{
    if (LightCullingMode == LIGHT_CULLING_TILE && NumTilesX > 0 && NumTilesY > 0)
    {
        uint2 tileCoord = min(uint2(screenPos.xy) / TILE_SIZE, uint2(NumTilesX - 1, NumTilesY - 1));
        uint tileIdx = tileCoord.y * NumTilesX + tileCoord.x;
        uint2 gridData = TileLightGrid[tileIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = TileLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcToonPointSpotDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos, N);
        }
    }
    else if (LightCullingMode == LIGHT_CULLING_CLUSTER)
    {
        uint clusterIdx = ComputeClusterIndex(screenPos, worldPos);
        uint2 gridData = g_ClusterLightGrid[clusterIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = g_ClusterLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcToonPointSpotDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos, N);
        }
    }
    else
    {
        for (uint i = 0; i < NumActivePointLights + NumActiveSpotLights; ++i)
        {
            FLightInfo light = AllLights[i];
            result += CalcToonPointSpotDiffuse(light, worldPos, N) * CalcLocalShadow(i, light, worldPos, N);
        }
    }
}

float3 AccumulateToonDiffuse(float3 worldPos, float3 N, float4 screenPos)
{
    float3 result = CalcAmbient(AmbientLight.Color.rgb, AmbientLight.Intensity) * 0.15f;
    result += CalcToonDirectionalDiffuse(N);
    AccumulateToonPointSpotDiffuse(worldPos, N, screenPos, result);
    return result;
}

float CalcRimMask(float3 N, float3 V)
{
    float rimDot = 1.0f - saturate(dot(N, V));
    return smoothstep(g_ToonRimMin, g_ToonRimMax, rimDot);
}
#endif

float3 AccumulateDiffuse(float3 worldPos, float3 N, float4 screenPos)
{
    float3 result = float3(0, 0, 0);
    result += CalcAmbient(AmbientLight.Color.rgb, AmbientLight.Intensity);
    result += CalcDirectionalDiffuse(DirectionalLight.Color.rgb, DirectionalLight.Direction,
                                     DirectionalLight.Intensity, N);
    AccumulatePointSpotDiffuse(worldPos, N, screenPos, result);
    return result;
}

float3 AccumulateSpecular(float3 worldPos, float3 N, float3 V, float shininess, float4 screenPos)
{
    float3 result = float3(0, 0, 0);
    result += CalcDirectionalSpecular(DirectionalLight.Color.rgb, DirectionalLight.Direction,
                                      DirectionalLight.Intensity, N, V, shininess);
    AccumulatePointSpotSpecular(worldPos, N, V, shininess, screenPos, result);
    return result;
}

#endif // FORWARD_LIGHTING_HLSLI
