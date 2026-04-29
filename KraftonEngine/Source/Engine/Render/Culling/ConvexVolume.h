#pragma once

#include "Engine/Core/CoreTypes.h"
#include "Engine/Math/Vector.h"
#include "Engine/Math/Matrix.h"

struct FBoundingBox;

enum class EAABBFrustumClassify : int
{
	Outside,
	Intersects,
	Contains,
};

struct FConvexVolume
{
public:
	void UpdateFromMatrix(const FMatrix& InViewProjectionMatrix);
	void UpdateAsOBB(const FMatrix& InWorldMatrix);
	bool IntersectAABB(const FBoundingBox& Box) const;
	// Returns true if the AABB is completely inside all 6 frustum planes
	bool ContainsAABB(const FBoundingBox& Box) const;
	
	EAABBFrustumClassify ClassifyAABB(const FBoundingBox& Box) const;
	TStaticArray<FVector, 8> GetFrustumCorners() const;
	FVector GetFrustumCenter() const;
private:
	static FVector IntersectPlanes(const FVector4& PlaneA, const FVector4& PlaneB, const FVector4& PlaneC);

	FVector4 Planes[6];
};
