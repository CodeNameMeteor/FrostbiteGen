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
	for (int i = 0; *szMask; ++szMask, ++pData, ++bMask)
	{
		if (*szMask == 'x' && *pData != *bMask)
		{
			return false;
		}
	}
	return (*szMask) == NULL;
}

/// <summary>
/// Finds the pattern.
/// </summary>
DWORD_PTR FindPattern(DWORD_PTR dwAddress, DWORD_PTR dwLen, DWORD_PTR offset, bool deref, BYTE *bMask, char * szMask)
{
	for (DWORD_PTR i = 0; i < dwLen; i++)
	{
		if (DataCompare((BYTE*)(dwAddress + i), bMask, szMask))
		{
			if (deref)
			{
				DWORD_PTR dwOut;
				memcpy(&dwOut, *(void**)(dwAddress + i + offset), 4);
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
	_vsnprintf(buf, sizeof(buf), szText, va_alist);
	va_end(va_alist);

	fout.open(g_szLogFile, std::ios::app);

	if (fout.fail())
	{
		fout.close();
		return;
	}

	time_t rawtime;
	struct tm * ti;

	time(&rawtime);
	ti = localtime(&rawtime);

	char szTime[64];
	sprintf(szTime, "[%02d:%02d:%02d] ", ti->tm_hour, ti->tm_min, ti->tm_sec);

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
		if (GetModuleFileNameA(hinstDLL, g_szBaseDir, sizeof(g_szBaseDir)))
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
		sprintf(g_szLogFile, "%sfbgen.txt", g_szBaseDir);

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
			return FALSE;
		}

		Log("ClassInfo head: 0x%016llX", (uintptr_t)classInfo);

		ClassInfoManager manager(classInfo);
		manager.BuildClassList();
		manager.DumpClasses();

		Log("SDK generation complete!");
		MessageBox(0, "SDK generated successfully!\nCheck the SDK\\ folder for output.", "FrostbiteGen", MB_ICONINFORMATION);
	}

	return 0;
}
