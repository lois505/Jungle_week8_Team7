#include "Engine/Runtime/EngineLoop.h"

#include "Profiling/StartupTrace.h"

#if IS_OBJ_VIEWER
#include "ObjViewer/ObjViewerEngine.h"
#elif WITH_EDITOR
#include "Editor/EditorEngine.h"
#endif

void FEngineLoop::CreateEngine()
{
#if IS_OBJ_VIEWER
	GEngine = UObjectManager::Get().CreateObject<UObjViewerEngine>();
#elif WITH_EDITOR
	GEngine = UObjectManager::Get().CreateObject<UEditorEngine>();
#else
	GEngine = UObjectManager::Get().CreateObject<UEngine>();
#endif
}

bool FEngineLoop::Init(HINSTANCE hInstance, int nShowCmd)
{
	STARTUP_TRACE_SCOPE("FEngineLoop::Init");

	{
		STARTUP_TRACE_SCOPE("Application.Init");
		if (!Application.Init(hInstance))
		{
			return false;
		}
	}

	{
		STARTUP_TRACE_SCOPE("Setup.WindowCallbacks");
		Application.SetOnSizingCallback([this]()
			{
				Timer.Tick();
				GEngine->Tick(Timer.GetDeltaTime());
			});

		Application.SetOnResizedCallback([](unsigned int Width, unsigned int Height)
			{
				if (GEngine)
				{
					GEngine->OnWindowResized(Width, Height);
				}
			});
	}

	{
		STARTUP_TRACE_SCOPE("CreateEngine");
		CreateEngine();
	}

	{
		STARTUP_TRACE_SCOPE("GEngine.Init");
		GEngine->Init(&Application.GetWindow());
	}

	{
		STARTUP_TRACE_SCOPE("GEngine.SetTimer");
		GEngine->SetTimer(&Timer);
	}

	{
		STARTUP_TRACE_SCOPE("GEngine.BeginPlay");
		GEngine->BeginPlay();
	}

	{
		STARTUP_TRACE_SCOPE("Timer.Initialize");
		Timer.Initialize();
	}

	return true;
}

int FEngineLoop::Run()
{
	while (!Application.IsExitRequested())
	{
		Application.PumpMessages();

		if (Application.IsExitRequested())
		{
			break;
		}

		Timer.Tick();
		GEngine->Tick(Timer.GetDeltaTime());
	}

	return 0;
}

void FEngineLoop::Shutdown()
{
	if (GEngine)
	{
		GEngine->Shutdown();
		UObjectManager::Get().DestroyObject(GEngine);
		GEngine = nullptr;
	}
}
