#include "required.h"
#include "structs.h"
#include "classinfo.h"

char g_szLogFile[MAX_PATH]; 
char g_szBaseDir[MAX_PATH];

/// <summary>
/// Datas the compare.
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
/// Finds the pattern.
/// </summary>
DWORD_PTR FindPattern(DWORD_PTR dwAddress, DWORD_PTR dwLen, DWORD_PTR offset, bool deref, BYTE *bMask, char * szMask)
{
	size_t maskLen = strlen(szMask);
	if (maskLen == 0 || dwLen < maskLen)
		return 0;

	DWORD_PTR maxScan = dwLen - maskLen;
	for (DWORD_PTR i = 0; i <= maxScan; i++)
	{
		if (DataCompare((BYTE*)(dwAddress + i), bMask, szMask))
		{
			if (deref)
			{
				void* derefAddr = *(void**)(dwAddress + i + offset);
				if (!derefAddr)
					return 0;
				DWORD_PTR dwOut = 0;
				memcpy(&dwOut, derefAddr, 4);
				return dwOut;
			}
			return (DWORD_PTR)(dwAddress + i + offset);
		}
	}
	return 0;
}

/// <summary>
/// Logs the specified sz text.
/// </summary>
void Log(const char* szText, ...)
{
	va_list		va_alist;
	std::ofstream	fout;
	char		buf[1024];

	va_start(va_alist, szText);
	_vsnprintf(buf, sizeof(buf) - 1, szText, va_alist);
	buf[sizeof(buf) - 1] = '\0';
	va_end(va_alist);

	fout.open(g_szLogFile, std::ios::app);

	if (fout.fail())
	{
		fout.close();
		return;
	}

	time_t rawtime;
	struct tm ti;

	time(&rawtime);
	localtime_s(&ti, &rawtime);

	char szTime[64];
	snprintf(szTime, sizeof(szTime), "[%02d:%02d:%02d] ", ti.tm_hour, ti.tm_min, ti.tm_sec);

	fout << szTime << buf << std::endl;
	fout.close();
}

/// <summary>
/// Gets the dir file.
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
	Log("Module base: 0x%016llX", (uintptr_t)GetModuleHandle(NULL));

	char sdkPath[MAX_PATH];
	GetDirFile("SDK\\", sdkPath, sizeof(sdkPath));
	DWORD dwAttr = GetFileAttributes(sdkPath);
	if (dwAttr == INVALID_FILE_ATTRIBUTES)
		CreateDirectory(sdkPath, NULL);

	ClassInfo* classInfo = ClassInfo::GetInstance();
	if (!classInfo)
	{
		Log("ERROR: Failed to find ClassInfo instance");
		MessageBox(0, "Failed to find ClassInfo", "FrostbiteGen", MB_ICONERROR);
		return 1;
	}

	Log("ClassInfo head: 0x%016llX", (uintptr_t)classInfo);

	ClassInfoManager manager(classInfo);
	manager.BuildClassList();
	manager.DumpClasses();

	Log("SDK generation complete!");
	MessageBox(0, "SDK generated successfully!\nCheck the SDK\\ folder for output.", "FrostbiteGen", MB_ICONINFORMATION);

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
