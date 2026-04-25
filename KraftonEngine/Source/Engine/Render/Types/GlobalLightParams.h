#pragma once
#include "Render/Pipeline/ForwardLightData.h"
#include "Render/Types/ShadowData.h"


struct FLightBaseParams
{
	float Intensity; //4
	FVector4 LightColor; //16 
	bool bVisible; // 4
};

struct FGlobalAmbientLightParams : FLightBaseParams
{
	
};

struct FGlobalDirectionalLightParams : FLightBaseParams
{
	FVector Direction;
	
	FDirectionalShadowData ShadowData;
};

struct FPointLightParams : FLightBaseParams
{
	FVector Position;
	float AttenuationRadius;
	float LightFalloffExponent;
	uint32 LightType;
	
	FPointShadowData ShadowData;

	virtual FLightInfo ToLightInfo() const
	{
		FLightInfo Info;
		Info.Position = Position;
		Info.AttenuationRadius = AttenuationRadius;

		Info.Color = LightColor;
		Info.Intensity = Intensity;

		Info.Direction = FVector(0.f, 0.f, 0.f);
		Info.FalloffExponent = LightFalloffExponent;

		Info.InnerConeCos = 0.f;
		Info.OuterConeCos = 0.f;
		Info.LightType = LightType;
		return Info;
	}
};

struct FSpotLightParams : FPointLightParams
{
	FVector Direction;
	float InnerConeCos;
	float OuterConeCos;
	
	FSpotShadowData ShadowData;

	virtual FLightInfo ToLightInfo() const override
	{
		FLightInfo Info = FPointLightParams::ToLightInfo();
		Info.Direction = Direction;
		Info.InnerConeCos = InnerConeCos;
		Info.OuterConeCos = OuterConeCos;
		return Info;
	}

};