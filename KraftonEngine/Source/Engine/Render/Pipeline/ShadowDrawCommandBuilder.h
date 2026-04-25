#pragma once

#include "DrawCommand.h"
#include "Render/Device/D3DDevice.h"

class FConstantBuffer;
class FPrimitiveSceneProxy;
class FScene;
class FPassRenderStateTable;

/*
 *	기존의 DrawCommandBuilder와 다르게 Shadow를 위한 DrawCommand만을 생성합니다.
 *	기본적으로, Mesh에 대해서만 Draw하며, Material 등은 모두 무시합니다.
 */

struct FShadowDrawCommand
{
	FDrawCommandBuffer Buffer;
	FConstantBuffer * PerObjectCB = nullptr;
};

class FShadowDrawCommandBuilder
{
public:
	void Create(ID3D11Device * Device, ID3D11DeviceContext * Context);
	void Release();
	
	void BeginBuild(uint32 MaxProxyCount = 0);
	void BuildCommands(const FScene & Scene);
	
	const TArray<FShadowDrawCommand> & GetCommands() const { return Commands; }

private:
	bool CheckBuildForProxy(const FPrimitiveSceneProxy & Proxy) const;
	void BuildCommandForProxy(const FPrimitiveSceneProxy & Proxy);
	
	FConstantBuffer * GetPerObjectCBForProxy(const FPrimitiveSceneProxy & Proxy);
	void EnsurePerObjectCBPoolCapacity(uint32 RequestCount);
	
private:
	TArray<FShadowDrawCommand> Commands;
	TArray<FConstantBuffer> PerObjectCBPool;
	
	ID3D11Device * CachedDevice = nullptr;
	ID3D11DeviceContext * CachedContext = nullptr;
	
};
