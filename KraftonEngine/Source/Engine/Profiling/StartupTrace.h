#pragma once

#include "Profiling/PlatformTime.h"
#include "Core/Log.h"
#include "Platform/Paths.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

// Temporary startup-time tracing toggle.
// Set to 0 to disable all startup trace logs at compile time.
#ifndef ENABLE_STARTUP_TRACE
#define ENABLE_STARTUP_TRACE 0
#endif

#if ENABLE_STARTUP_TRACE
class FStartupTraceScope
{
public:
	explicit FStartupTraceScope(const char* InName)
		: Name(InName)
		, StartCycles(FPlatformTime::Cycles64())
		, ScopeDepth(GetDepth())
	{
		LogPhase("BEGIN");
		++GetDepth();
	}

	~FStartupTraceScope()
	{
		const uint64 EndCycles = FPlatformTime::Cycles64();
		if (GetDepth() > 0)
		{
			--GetDepth();
		}

		const double ElapsedMs = FPlatformTime::ToMilliseconds(EndCycles - StartCycles);
		LogPhase("END", ElapsedMs);
	}

	static void Logf(const char* Fmt, ...)
	{
		char Message[1024] = {};
		va_list Args;
		va_start(Args, Fmt);
		vsnprintf_s(Message, sizeof(Message), _TRUNCATE, Fmt, Args);
		va_end(Args);

		char Final[1152] = {};
		snprintf(Final, sizeof(Final), "[StartupTrace] %s", Message);
		UE_LOG("%s", Final);
		WriteLineToFile(Final);
	}

private:
	static int32& GetDepth()
	{
		static thread_local int32 Depth = 0;
		return Depth;
	}

	void LogPhase(const char* Phase, double ElapsedMs = -1.0) const
	{
		char Indent[64] = {};
		int32 IndentChars = ScopeDepth * 2;
		if (IndentChars > 60) IndentChars = 60;
		for (int32 i = 0; i < IndentChars; ++i)
		{
			Indent[i] = ' ';
		}
		Indent[IndentChars] = '\0';

		if (ElapsedMs >= 0.0)
		{
			char Final[1152] = {};
			snprintf(Final, sizeof(Final), "[StartupTrace] %s%s %s (%.3f ms)", Indent, Phase, Name, ElapsedMs);
			UE_LOG("%s", Final);
			WriteLineToFile(Final);
		}
		else
		{
			char Final[1152] = {};
			snprintf(Final, sizeof(Final), "[StartupTrace] %s%s %s", Indent, Phase, Name);
			UE_LOG("%s", Final);
			WriteLineToFile(Final);
		}
	}

	static void WriteLineToFile(const char* Line)
	{
		std::lock_guard<std::mutex> Lock(FileMutex);
		const std::filesystem::path TracePath = GetTraceFilePath();

		if (!bFileInitialized)
		{
			std::filesystem::create_directories(TracePath.parent_path());
			std::ofstream ClearFile(TracePath, std::ios::out | std::ios::trunc);
			bFileInitialized = true;
		}

		std::ofstream AppendFile(TracePath, std::ios::out | std::ios::app);
		if (AppendFile.is_open())
		{
			AppendFile << Line << '\n';
		}
	}

	static std::filesystem::path GetTraceFilePath()
	{
		return std::filesystem::path(FPaths::LogDir()) / L"StartupTrace_Temp.txt";
	}

private:
	const char* Name = "Unknown";
	uint64 StartCycles = 0;
	int32 ScopeDepth = 0;
	inline static std::mutex FileMutex;
	inline static bool bFileInitialized = false;
};

#define STARTUP_TRACE_CONCAT2(a, b) a##b
#define STARTUP_TRACE_CONCAT(a, b) STARTUP_TRACE_CONCAT2(a, b)
#define STARTUP_TRACE_SCOPE(Name) FStartupTraceScope STARTUP_TRACE_CONCAT(_StartupTraceScope_, __COUNTER__)(Name)
#define STARTUP_TRACE_LOG(Format, ...) FStartupTraceScope::Logf(Format, ##__VA_ARGS__)

#else

#define STARTUP_TRACE_SCOPE(Name) ((void)0)
#define STARTUP_TRACE_LOG(Format, ...) ((void)0)

#endif
