#include "required.h"
#include "structs.h"
#include "classinfo.h"

char g_szLogFile[MAX_PATH];
char g_szBaseDir[MAX_PATH];

static std::ofstream g_logStream;

/// <summary>
/// Compares memory against a byte pattern; 'x' in the mask means "must match", anything else is a wildcard.
/// </summary>
bool DataCompare(const BYTE* pData, const BYTE* bMask, const char* szMask)
{
	for (; *szMask; ++szMask, ++pData, ++bMask)
	{
		if (*szMask == 'x' && *pData != *bMask)
		{
			return false;
		}
	}
	return *szMask == '\0';
}

/// <summary>
/// Scans one readable run for the pattern. Returns the match address or 0 (also on fault).
/// </summary>
static DWORD_PTR ScanRun(DWORD_PTR start, DWORD_PTR len, const BYTE* bMask, const char* szMask, size_t maskLen)
{
	__try
	{
		DWORD_PTR maxScan = len - maskLen;
		for (DWORD_PTR i = 0; i <= maxScan; i++)
		{
			if (DataCompare((BYTE*)(start + i), bMask, szMask))
				return start + i;
		}
	}
	__except (FBGEN_AV_FILTER) {}
	return 0;
}

/// <summary>
/// Finds the first match of a masked byte pattern in [dwAddress, dwAddress + dwLen).
/// Only committed, readable pages are scanned, so protected or packed images cannot crash the game.
/// </summary>
DWORD_PTR FindPattern(DWORD_PTR dwAddress, DWORD_PTR dwLen, DWORD_PTR offset, bool deref, BYTE *bMask, char * szMask)
{
	size_t maskLen = strlen(szMask);
	if (maskLen == 0 || dwLen < maskLen)
		return 0;

	for (const auto& run : GetReadableRuns(dwAddress, dwAddress + dwLen))
	{
		if (run.second - run.first < maskLen)
			continue;

		DWORD_PTR match = ScanRun(run.first, run.second - run.first, bMask, szMask, maskLen);
		if (!match)
			continue;

		if (deref)
		{
			void* derefAddr = SafeReadPointer(match + offset);
			if (!derefAddr)
				return 0;
			DWORD_PTR dwOut = 0;
			if (!SafeReadBytes((uintptr_t)derefAddr, &dwOut, 4))
				return 0;
			return dwOut;
		}
		return (DWORD_PTR)(match + offset);
	}
	return 0;
}

/// <summary>
/// Appends a timestamped line to the log file. The file is opened once and kept open.
/// </summary>
void Log(const char* szText, ...)
{
	va_list		va_alist;
	char		buf[1024];

	va_start(va_alist, szText);
	_vsnprintf(buf, sizeof(buf) - 1, szText, va_alist);
	buf[sizeof(buf) - 1] = '\0';
	va_end(va_alist);

	if (!g_logStream.is_open())
	{
		g_logStream.open(g_szLogFile, std::ios::app);
		if (!g_logStream.is_open())
			return;
	}

	time_t rawtime;
	struct tm ti;

	time(&rawtime);
	localtime_s(&ti, &rawtime);

	char szTime[64];
	snprintf(szTime, sizeof(szTime), "[%02d:%02d:%02d] ", ti.tm_hour, ti.tm_min, ti.tm_sec);

	// std::endl flushes, so the log survives if the game crashes mid-generation.
	g_logStream << szTime << buf << std::endl;
}

void CloseLog()
{
	if (g_logStream.is_open())
		g_logStream.close();
}

/// <summary>
/// Builds a path relative to the DLL's directory.
/// </summary>
void GetDirFile(const char* file, char* out, size_t len)
{
	snprintf(out, len, "%s%s", g_szBaseDir, file);
}

/// <summary>
/// Background worker thread for SDK generation (prevents loader-lock deadlocks).
/// </summary>
DWORD WINAPI GeneratorThread(LPVOID lpParam)
{
	HMODULE hModule = (HMODULE)lpParam;
	const UINT boxFlags = MB_SETFOREGROUND | MB_TOPMOST;

	if (GetModuleFileNameA(hModule, g_szBaseDir, sizeof(g_szBaseDir)))
	{
		for (int i = (int)strlen(g_szBaseDir); i > 0; i--)
		{
			if (g_szBaseDir[i] == '\\')
			{
				g_szBaseDir[i + 1] = 0;
				break;
			}
		}
	}
	snprintf(g_szLogFile, sizeof(g_szLogFile), "%sfbgen.txt", g_szBaseDir);

	std::ofstream fout;
	fout.open(g_szLogFile, std::ios::trunc);
	fout.close();

	Log("FrostbiteGen SDK Generator starting...");
	Log("Module base: 0x%016llX", (unsigned long long)(uintptr_t)GetModuleHandle(NULL));

	char sdkPath[MAX_PATH];
	GetDirFile("SDK\\", sdkPath, sizeof(sdkPath));
	DWORD dwAttr = GetFileAttributes(sdkPath);
	if (dwAttr == INVALID_FILE_ATTRIBUTES)
	{
		if (!CreateDirectory(sdkPath, NULL))
		{
			Log("ERROR: Could not create output directory %s", sdkPath);
			char msg[MAX_PATH + 128];
			snprintf(msg, sizeof(msg), "Could not create the output folder:\n%s\n\nMove FrostbiteGen.dll to a writable folder and inject again.", sdkPath);
			MessageBox(0, msg, "FrostbiteGen", MB_ICONERROR | boxFlags);
			CloseLog();
			FreeLibraryAndExitThread(hModule, 1);
			return 1;
		}
	}
	else if (!(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
	{
		Log("ERROR: %s exists but is not a directory", sdkPath);
		MessageBox(0, "An 'SDK' file is in the way of the output folder next to FrostbiteGen.dll.", "FrostbiteGen", MB_ICONERROR | boxFlags);
		CloseLog();
		FreeLibraryAndExitThread(hModule, 1);
		return 1;
	}

	ClassInfo* classInfo = ClassInfo::GetInstance();
	if (!classInfo)
	{
		Log("ERROR: Failed to find ClassInfo instance");
		char msg[MAX_PATH + 160];
		snprintf(msg, sizeof(msg), "Failed to find the engine's type list (ClassInfo).\n\nWait until the game reaches the main menu and inject again.\nLog: %s", g_szLogFile);
		MessageBox(0, msg, "FrostbiteGen", MB_ICONERROR | boxFlags);
		CloseLog();
		FreeLibraryAndExitThread(hModule, 1);
		return 1;
	}

	Log("ClassInfo head: 0x%016llX", (unsigned long long)(uintptr_t)classInfo);

	bool hadErrors = false;
	std::string summary;
	{
		ClassInfoManager manager(classInfo);
		manager.BuildClassList();
		manager.DumpClasses();
		hadErrors = manager.HasErrors();
		summary = manager.GetSummary();
	}

	Log("SDK generation complete: %s", summary.c_str());
	CloseLog();

	std::string msg = std::string(hadErrors ? "SDK generated with errors.\n" : "SDK generated successfully!\n")
		+ summary + "\n\nOutput: " + g_szBaseDir + "SDK\\\nLog: " + g_szLogFile;
	MessageBeep(hadErrors ? MB_ICONWARNING : MB_ICONINFORMATION);
	MessageBox(0, msg.c_str(), "FrostbiteGen", (hadErrors ? MB_ICONWARNING : MB_ICONINFORMATION) | boxFlags);

	// Automatically unload the DLL to release disk locks and allow immediate recompilation
	FreeLibraryAndExitThread(hModule, 0);
	return 0;
}

/// <summary>
/// DLL entry point.
/// </summary>
BOOL WINAPI DllMain(
	_In_ HINSTANCE hinstDLL,
	_In_ DWORD     fdwReason,
	_In_ LPVOID    lpvReserved
	)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hinstDLL);
		HANDLE hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)GeneratorThread, (LPVOID)hinstDLL, 0, NULL);
		if (hThread)
			CloseHandle(hThread);
	}

	return TRUE;
}
