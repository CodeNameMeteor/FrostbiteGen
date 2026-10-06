// Minimal Windows API stand-in so FrostbiteGen and the SDK it generates can be compiled and
// exercised on Linux by tests/run_sdk_test.sh. It is NOT a Windows emulation:
//  - __try/__except become plain if/else, so memory faults are not caught (the test only
//    feeds valid memory);
//  - the "game module" is a fake PE image the test registers via fbgen_test::SetModule().
#pragma once

// libstdc++ uses its own __try/__catch macros internally, so every standard header the
// project (or the generated SDK) uses must be processed before the SEH macros below exist.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// ---- Structured exception handling ----
#undef __try
#undef __except
#define __try if (true)
#define __except(filter) else if (false)
#define GetExceptionCode() 0u
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define __fastcall

// ---- Basic types ----
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int BOOL;
typedef unsigned int UINT;
typedef uintptr_t DWORD_PTR;
typedef uintptr_t SIZE_T;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HWND;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef const char* LPCSTR;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef int32_t __int32;

#define WINAPI
#define _In_
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define DLL_PROCESS_ATTACH 1
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define MB_ICONERROR 0x10u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
#define MB_TOPMOST 0x40000u
#define MB_SETFOREGROUND 0x10000u

// ---- Memory ----
#define MEM_COMMIT 0x1000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100

struct MEMORY_BASIC_INFORMATION
{
	void* BaseAddress;
	void* AllocationBase;
	DWORD AllocationProtect;
	SIZE_T RegionSize;
	DWORD State;
	DWORD Protect;
	DWORD Type;
};

// ---- PE image ----
#define IMAGE_SCN_CNT_INITIALIZED_DATA 0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000

struct IMAGE_DOS_HEADER { WORD e_magic; BYTE pad[58]; int32_t e_lfanew; };
struct IMAGE_FILE_HEADER { WORD Machine; WORD NumberOfSections; DWORD TimeDateStamp; DWORD PointerToSymbolTable; DWORD NumberOfSymbols; WORD SizeOfOptionalHeader; WORD Characteristics; };
struct IMAGE_OPTIONAL_HEADER64 { BYTE pad[56]; DWORD SizeOfImage; BYTE pad2[240 - 60]; };
struct IMAGE_NT_HEADERS { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; };
struct IMAGE_SECTION_HEADER
{
	BYTE Name[8];
	union { DWORD PhysicalAddress; DWORD VirtualSize; } Misc;
	DWORD VirtualAddress;
	DWORD SizeOfRawData;
	DWORD PointerToRawData;
	DWORD PointerToRelocations;
	DWORD PointerToLinenumbers;
	WORD NumberOfRelocations;
	WORD NumberOfLinenumbers;
	DWORD Characteristics;
};
typedef IMAGE_DOS_HEADER* PIMAGE_DOS_HEADER;
typedef IMAGE_NT_HEADERS* PIMAGE_NT_HEADERS;
typedef IMAGE_SECTION_HEADER* PIMAGE_SECTION_HEADER;
#define IMAGE_FIRST_SECTION(nt) ((PIMAGE_SECTION_HEADER)((uintptr_t)&(nt)->OptionalHeader + (nt)->FileHeader.SizeOfOptionalHeader))

namespace fbgen_test
{
	inline void* g_module = nullptr;
	inline std::string g_moduleName = "test.exe";
	inline void SetModule(void* base, const char* name) { g_module = base; g_moduleName = name; }
}

// ---- API functions ----
inline HMODULE GetModuleHandle(const char*) { return fbgen_test::g_module; }
inline HMODULE GetModuleHandleA(const char*) { return fbgen_test::g_module; }
inline DWORD GetModuleFileNameA(HMODULE, char* out, DWORD size)
{
	int n = snprintf(out, size, "C:\\Games\\%s", fbgen_test::g_moduleName.c_str());
	return n > 0 ? (DWORD)n : 0;
}
inline SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION* mbi, SIZE_T)
{
	// Every page is reported as committed and readable.
	uintptr_t page = (uintptr_t)address & ~(uintptr_t)0xFFF;
	memset(mbi, 0, sizeof(*mbi));
	mbi->BaseAddress = (void*)page;
	mbi->RegionSize = 0x1000;
	mbi->State = MEM_COMMIT;
	mbi->Protect = PAGE_READWRITE;
	return sizeof(*mbi);
}
inline BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD* old) { if (old) *old = PAGE_READONLY; return TRUE; }
inline DWORD GetFileAttributes(const char*) { return FILE_ATTRIBUTE_DIRECTORY; }
inline BOOL CreateDirectory(const char*, void*) { return TRUE; }
inline int MessageBox(HWND, const char*, const char*, UINT) { return 0; }
inline BOOL MessageBeep(UINT) { return TRUE; }
inline void FreeLibraryAndExitThread(HMODULE, DWORD) {}
inline HANDLE CreateThread(void*, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD*) { return nullptr; }
inline BOOL CloseHandle(HANDLE) { return TRUE; }
inline BOOL DisableThreadLibraryCalls(HMODULE) { return TRUE; }

// ---- MSVC CRT ----
#define _vsnprintf vsnprintf
inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : 1; }
template <size_t N>
inline int sprintf_s(char (&buf)[N], const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(buf, N, fmt, args);
	va_end(args);
	return n;
}
