#pragma once

#include "Render/Resource/LocalShadowTypes.h"
#include "Render/Resource/ShadowAtlasResource.h"
#include <cstddef>

class FLocalShadowAtlasAllocator
{
public:
	void Reset(uint32 AtlasWidth, uint32 AtlasHeight);
	FAtlasResourceInfo Allocate(uint32 RequestWidth, uint32 RequestHeight);

private:
	static bool IsContainedIn(const FAtlasFreeRect& A, const FAtlasFreeRect& B);
	static void RemoveAtSwap(TArray<FAtlasFreeRect>& InFreeRects, size_t Index);
	void InsertFreeRect(uint32 OffsetX, uint32 OffsetY, uint32 Width, uint32 Height);

private:
	uint32 NextIndex = 0;
	TArray<FAtlasFreeRect> FreeRects;
};
