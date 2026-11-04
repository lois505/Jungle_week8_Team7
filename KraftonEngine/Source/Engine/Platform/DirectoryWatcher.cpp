#include "Engine/Platform/DirectoryWatcher.h"
#include "Platform/Paths.h"
#include "Core/Log.h"
#include <algorithm>

// ============================================================
// Initialize / Shutdown
// ============================================================
void FDirectoryWatcher::Initialize()
{
	if (bRunning) return;

	StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	bRunning = true;
	WatchThread = std::thread(&FDirectoryWatcher::WatchThreadFunc, this);
}

void FDirectoryWatcher::Shutdown()
{
	if (!bRunning) return;
	bRunning = false;

	if (StopEvent)
	{
		SetEvent(StopEvent);
	}

	if (WatchThread.joinable())
	{
		WatchThread.join();
	}

	// 감시 핸들 정리
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		for (auto& Entry : WatchingDirs)
		{
			if (Entry->DirHandle != INVALID_HANDLE_VALUE)
			{
				CancelIo(Entry->DirHandle);
				CloseHandle(Entry->Overlapped.hEvent);
				CloseHandle(Entry->DirHandle);
			}
		}
		WatchingDirs.clear();
		Subscriptions.clear();
	}

	if (StopEvent)
	{
		CloseHandle(StopEvent);
		StopEvent = nullptr;
	}
}

// ============================================================
// Watch / Unwatch
// ============================================================
FWatchIDu32 FDirectoryWatcher::Watch(const std::wstring& DirPath, const FString& PathPrefix)
{
	//폴더 열기
	HANDLE hDir = CreateFileW(
		DirPath.c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
		nullptr
	);

	if (hDir == INVALID_HANDLE_VALUE)
	{
		UE_LOG("[DirectoryWatcher] Failed to open directory: %s",
			FPaths::ToUtf8(DirPath).c_str());
		return 0;
	}
	//벨 만들기
	auto WatchDir = std::make_unique<FWatchDir>();
	WatchDir->DirPath = DirPath;
	WatchDir->PathPrefix = PathPrefix;
	WatchDir->DirHandle = hDir;
	WatchDir->ChangedFileNames.resize(FWatchDir::kBufferSize);
	WatchDir->Overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

	//OS한테 벨 울리면 깨워달라고 함
	FWatchIDu32 ID;
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		ID = NextWatchID++;
		WatchDir->ID = ID;
		RequestChangeNotification(*WatchDir);
		WatchingDirs.push_back(std::move(WatchDir));
	}

	UE_LOG("[DirectoryWatcher] Watching: %s (prefix=\"%s\", id=%u)",
		FPaths::ToUtf8(DirPath).c_str(), PathPrefix.c_str(), ID);
	return ID;
}

void FDirectoryWatcher::Unwatch(FWatchIDu32 ID)
{
	std::lock_guard<std::mutex> Lock(Mutex);
	auto It = std::find_if(WatchingDirs.begin(), WatchingDirs.end(),
		[ID](const auto& E) { return E->ID == ID; });

	if (It != WatchingDirs.end())
	{
		CancelIo((*It)->DirHandle);
		CloseHandle((*It)->Overlapped.hEvent);
		CloseHandle((*It)->DirHandle);
		WatchingDirs.erase(It);
	}
}

// ============================================================
// Subscribe / Unsubscribe
// ============================================================
FSubscriptionIDu32 FDirectoryWatcher::Subscribe(FWatchIDu32 WatchID, FWatchCallback Callback)
{
	std::lock_guard<std::mutex> Lock(Mutex);
	FSubscriptionIDu32 ID = NextSubID++;
	Subscriptions.push_back({ ID, WatchID, std::move(Callback) });
	return ID;
}

FSubscriptionIDu32 FDirectoryWatcher::SubscribeAll(FWatchCallback Callback)
{
	return Subscribe(0, std::move(Callback));
}

void FDirectoryWatcher::Unsubscribe(FSubscriptionIDu32 ID)
{
	std::lock_guard<std::mutex> Lock(Mutex);
	auto It = std::find_if(Subscriptions.begin(), Subscriptions.end(),
		[ID](const FSubscription& S) { return S.ID == ID; });
	if (It != Subscriptions.end())
	{
		Subscriptions.erase(It);
	}
}

// ============================================================
// ProcessChanges — 메인 스레드에서 호출
// 디바운스: 마지막 변경 후 kDebounceMs 경과해야 디스패치
// ============================================================
void FDirectoryWatcher::ProcessChanges()
{
	DWORD Now = GetTickCount();

	// 각 Watch별 변경 사항 수집 (lock 최소화)
	TArray<TPair<FWatchIDu32, TSet<FString>>> Collected;

	{
		std::lock_guard<std::mutex> Lock(Mutex);
		for (auto& WatchingDir : WatchingDirs)
		{
			if (!WatchingDir->FilesWaitingForApply.empty()
				&& (Now - WatchingDir->LastChangeTime) >= FWatchDir::kDebounceMs) //대기중인 파일 있으면
			{
				TSet<FString> Changes;
				std::swap(Changes, WatchingDir->FilesWaitingForApply); //꺼내오고
				Collected.push_back({ WatchingDir->ID, std::move(Changes) });
			}
		}
	}

	// 콜백 디스패치 (lock 밖에서)
	for (auto& [WatchID, Changes] : Collected)
	{
		// lock 안에서 구독 목록 복사 (콜백 중 구독 변경 가능)
		TArray<FSubscription> SubsCopy;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			SubsCopy = Subscriptions;
		}

		for (const auto& Sub : SubsCopy)
		{
			if (Sub.WatchID == 0 || Sub.WatchID == WatchID)
			{
				Sub.Callback(Changes);
			}
		}
	}
}

// ============================================================
// BeginRead — OVERLAPPED ReadDirectoryChangesW 시작
// ============================================================
void FDirectoryWatcher::RequestChangeNotification(FWatchDir& Entry)
{
	// OVERLAPPED 재초기화 (hEvent 보존, Internal/InternalHigh 클리어)
	HANDLE hEvent = Entry.Overlapped.hEvent;//벨만 남겨두고
	ZeroMemory(&Entry.Overlapped, sizeof(OVERLAPPED));//비우고
	Entry.Overlapped.hEvent = hEvent;//벨 다시 넣음
	ResetEvent(hEvent);//벨도 끔

	BOOL Ok = ReadDirectoryChangesW(
		Entry.DirHandle,
		Entry.ChangedFileNames.data(),
		static_cast<DWORD>(Entry.ChangedFileNames.size()),
		TRUE,  // bWatchSubtree
		FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
		nullptr,
		&Entry.Overlapped,
		nullptr
	);

	if (!Ok)
	{
		DWORD Err = GetLastError();
		if (Err != ERROR_IO_PENDING)
		{
			UE_LOG("[DirectoryWatcher] ReadDirectoryChangesW failed: %lu", Err);
		}
	}
}

// ============================================================
// WatchThreadFunc — 감시 스레드 메인 루프
// 폴더 변경 벨이 울릴 때까지 자다가, 울리면 바뀐 파일 이름을 메인 스레드가 가져갈 목록에 적어두기를 반복하는 함수입니다.
// ============================================================
void FDirectoryWatcher::WatchThreadFunc()
{
	while (bRunning)
	{
		// 대기 핸들 배열 구축
		TArray<HANDLE> hEvents; //벨 모음
		hEvents.push_back(StopEvent);

		TArray<FWatchDir*> LWatchingDirs;

		{
			std::lock_guard<std::mutex> Lock(Mutex);
			for (auto& Entry : WatchingDirs)
			{
				hEvents.push_back(Entry->Overlapped.hEvent); //벨 모음
				LWatchingDirs.push_back(Entry.get());//현재 
			}
		}
		int WatchingDirCnt = hEvents.size();
		if (WatchingDirCnt <= 1)
		{
			// 감시 대상 없음 — 짧게 sleep 후 재확인
			Sleep(100);
			continue;
		}

		//감시스레드들 재우기
		DWORD Result = WaitForMultipleObjects(
			static_cast<DWORD>(hEvents.size()),
			hEvents.data(),
			FALSE,
			500  // 500ms timeout — 새 Watch 추가 감지용
		);

		if (!bRunning) break;

		if (Result == WAIT_OBJECT_0) //StopEvent면 스레드 끝
		{
			// StopEvent 시그널
			break;
		}
		else if (Result == WAIT_TIMEOUT)//500ms 타임아웃
		{
			continue;
		}
		else if (Result >= WAIT_OBJECT_0 + 1 && Result < WAIT_OBJECT_0 + hEvents.size()) // 바뀐 폴더목록중 하나면
		{
			DWORD WatchingDirIndex = Result - WAIT_OBJECT_0 - 1; // -1: StopEvent 오프셋
			FWatchDir* Entry = LWatchingDirs[WatchingDirIndex];

			DWORD BytesTransferred = 0; //몇바이트 썼어?
			BOOL bGotResult = GetOverlappedResult(
				Entry->DirHandle, &Entry->Overlapped, &BytesTransferred, FALSE); //

			if (bGotResult && BytesTransferred > 0)
			{
				// FILE_NOTIFY_INFORMATION 파싱
				BYTE* Base = Entry->ChangedFileNames.data();
				FILE_NOTIFY_INFORMATION* Info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(Base);

				{
					std::lock_guard<std::mutex> Lock(Mutex);
					for (;;)
					{
						// ----wchar → UTF-8, 백슬래시 → 슬래시
						std::wstring FileName(Info->FileName, Info->FileNameLength / sizeof(WCHAR));
						for (auto& Ch : FileName)
						{
							if (Ch == L'\\') Ch = L'/';
						}
						// ----wchar → UTF-8, 백슬래시 → 슬래시
						
						FString NormalizedPath = Entry->PathPrefix + FPaths::ToUtf8(FileName);
						Entry->FilesWaitingForApply.insert(NormalizedPath);

						UE_LOG("[DirectoryWatcher] Detected: %s", NormalizedPath.c_str());

						//다른 변경된 파일 없으면 넘어가고
						if (Info->NextEntryOffset == 0) break;
						//아니면 오프셋만큼 움직이기
						Info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
							reinterpret_cast<BYTE*>(Info) + Info->NextEntryOffset);
					}

					Entry->LastChangeTime = GetTickCount();
				}
			}

			// 다음 읽기 시작 (성공/실패 무관하게 항상 재등록)
			{
				std::lock_guard<std::mutex> Lock(Mutex);
				RequestChangeNotification(*Entry);
			}
		}
	}
}
