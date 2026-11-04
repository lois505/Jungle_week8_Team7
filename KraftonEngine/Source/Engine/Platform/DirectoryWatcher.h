#pragma once

#include "Core/CoreTypes.h"
#include "Core/Singleton.h"

#include <Windows.h>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>

// ============================================================
// FDirectoryWatcher — 범용 파일 감시 싱글턴
//
// ReadDirectoryChangesW 기반. 별도 감시 스레드에서 변경 감지 후
// ProcessChanges()에서 메인 스레드 콜백 디스패치.
// ============================================================

using FWatchIDu32 = uint32;
using FSubscriptionIDu32 = uint32;
using FWatchCallback = std::function<void(const TSet<FString>&)>;

struct FWatchDir//감시 폴더 하나
{
	FWatchIDu32        ID = 0;
	std::wstring    DirPath;       // 감시 디렉토리 절대 경로
	FString         PathPrefix;    // 구독자에게 전달할 접두사 (e.g. "Shaders/")
	
	HANDLE          DirHandle = INVALID_HANDLE_VALUE;
	OVERLAPPED      Overlapped = {}; //비동기 작업의 진행 상황 적어두는곳 //작업이 끝나면 OVERLapped안의 hEvent 실행
	TArray<BYTE>    ChangedFileNames;        // FILE_NOTIFY_INFORMATION 수신 버퍼
	
	TSet<FString>   FilesWaitingForApply; // Changed됐지만 변경안된 파일
	DWORD           LastChangeTime = 0; // 

	static constexpr DWORD kBufferSize = 4096;
	static constexpr DWORD kDebounceMs = 200; // 파일 저장 완료 대기
};

struct FSubscription
{
	FSubscriptionIDu32 ID = 0;
	FWatchIDu32        WatchID = 0;   // 0 = 전체 구독
	FWatchCallback  Callback;
};

class FDirectoryWatcher : public TSingleton<FDirectoryWatcher>
{
	friend class TSingleton<FDirectoryWatcher>;

public:
	void Initialize();
	void Shutdown();

	FWatchIDu32        Watch(const std::wstring& DirPath, const FString& PathPrefix);
	void            Unwatch(FWatchIDu32 ID);

	FSubscriptionIDu32 Subscribe(FWatchIDu32 WatchID, FWatchCallback Callback);
	FSubscriptionIDu32 SubscribeAll(FWatchCallback Callback);
	void            Unsubscribe(FSubscriptionIDu32 ID);

	// 메인 스레드에서 매 프레임 호출. 변경된 파일을 구독자에게 디스패치.
	void ProcessChanges();

private:
	FDirectoryWatcher() = default;

	void WatchThreadFunc();
	void RequestChangeNotification(FWatchDir& Entry);

	std::mutex                          Mutex;
	TArray<std::unique_ptr<FWatchDir>> WatchingDirs;
	TArray<FSubscription>               Subscriptions;

	std::thread                         WatchThread;
	HANDLE                              StopEvent = nullptr;
	std::atomic<bool>                   bRunning{false};

	FWatchIDu32        NextWatchID = 1;
	FSubscriptionIDu32 NextSubID = 1;
};
