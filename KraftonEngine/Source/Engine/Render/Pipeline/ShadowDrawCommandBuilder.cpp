#include "ShadowDrawCommandBuilder.h"

#include "Render/Proxy/FScene.h"
#include "Render/Proxy/PrimitiveSceneProxy.h"

void FShadowDrawCommandBuilder::Create(ID3D11Device* Device, ID3D11DeviceContext* Context)
{
	CachedDevice = Device;
	CachedContext = Context;
}

void FShadowDrawCommandBuilder::Release()
{
	for (FConstantBuffer & CB : PerObjectCBPool)
	{
		CB.Release();
	}
	
	PerObjectCBPool.clear();
	Commands.clear();
}

void FShadowDrawCommandBuilder::BeginBuild(uint32 MaxProxyCount)
{
	Commands.clear();
	
	if (MaxProxyCount > 0)
	{
		EnsurePerObjectCBPoolCapacity(MaxProxyCount);
	}
}

void FShadowDrawCommandBuilder::BuildCommands(const FScene& Scene)
{
	for (FPrimitiveSceneProxy * Proxy : Scene.GetAllProxies())
	{
		if (!Proxy)
		{
			continue;
		}
		
		if (CheckBuildForProxy(*Proxy))
		{
			BuildCommandForProxy(*Proxy);
		}
	}
}

bool FShadowDrawCommandBuilder::CheckBuildForProxy(const FPrimitiveSceneProxy& Proxy) const
{
	if (!Proxy.IsVisible())
	{
		return false;
	}
		
	if (Proxy.HasProxyFlag(EPrimitiveProxyFlags::EditorOnly | EPrimitiveProxyFlags::Decal | EPrimitiveProxyFlags::FontBatched))
	{
		return false;
	}
		
	if (!Proxy.GetMeshBuffer())
	{
		return false;
	}
		
	if (Proxy.GetSectionDraws().empty())
	{
		return false;
	}
		
	return true;
}

void FShadowDrawCommandBuilder::BuildCommandForProxy(const FPrimitiveSceneProxy& Proxy)
{
	if (!Proxy.GetMeshBuffer() || !Proxy.GetMeshBuffer()->IsValid())
	{
		return;
	}
	
	FConstantBuffer * PerObjectCB = GetPerObjectCBForProxy(Proxy);
	if (PerObjectCB && Proxy.NeedsPerObjectCBUpload())
	{
		PerObjectCB->Update(CachedContext, &Proxy.GetPerObjectConstants(), sizeof(FPerObjectConstants));
		Proxy.ClearPerObjectCBDirty();
	}
	
	FShadowDrawCommand & Cmd = Commands.emplace_back();
	Cmd.PerObjectCB = PerObjectCB;
	
	Cmd.Buffer.VB = Proxy.GetMeshBuffer()->GetVertexBuffer().GetBuffer();
	Cmd.Buffer.VBStride = Proxy.GetMeshBuffer()->GetVertexBuffer().GetStride();
	Cmd.Buffer.IB = Proxy.GetMeshBuffer()->GetIndexBuffer().GetBuffer();
	Cmd.Buffer.FirstIndex = 0;
	Cmd.Buffer.IndexCount = Proxy.GetMeshBuffer()->GetIndexBuffer().GetIndexCount();
	Cmd.Buffer.BaseVertex = 0;
	
}

FConstantBuffer* FShadowDrawCommandBuilder::GetPerObjectCBForProxy(const FPrimitiveSceneProxy& Proxy)
{
	if (Proxy.GetProxyId() == UINT32_MAX)
	{
		return nullptr;
	}
	
	EnsurePerObjectCBPoolCapacity(Proxy.GetProxyId() + 1);
	return &PerObjectCBPool[Proxy.GetProxyId()];
}

void FShadowDrawCommandBuilder::EnsurePerObjectCBPoolCapacity(uint32 RequestCount)
{
	if (PerObjectCBPool.size() >= RequestCount)
	{
		return;
	}
	
	const size_t OldCount = PerObjectCBPool.size();
	PerObjectCBPool.resize(RequestCount);
	
	for (size_t Index = OldCount; Index < PerObjectCBPool.size(); ++Index)
	{
		PerObjectCBPool[Index].Create(CachedDevice, sizeof(FPerObjectConstants));
	}
}
