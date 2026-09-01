#pragma once

class TypeInfo;
class FieldInfo;

DWORD_PTR FindPattern(DWORD_PTR dwAddress, DWORD_PTR dwLen, DWORD_PTR offset, bool deref, BYTE *bMask, char * szMask);

/// Safely copy N bytes from an address. Returns false on fault.
inline bool SafeReadBytes(uintptr_t addr, void* dst, size_t count)
{
	__try
	{
		memcpy(dst, (void*)addr, count);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ============================================================================
// Module & Pointer Helpers
// ============================================================================

inline void GetGameModuleInfo(uintptr_t& base, size_t& size)
{
	HMODULE hMod = GetModuleHandle(NULL);
	base = (uintptr_t)hMod;
	PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hMod;
	PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)(base + dosHeader->e_lfanew);
	size = ntHeaders->OptionalHeader.SizeOfImage;
}

inline bool IsValidPointer(void* ptr)
{
	return ptr != nullptr
		&& (uintptr_t)ptr > 0x10000
		&& (uintptr_t)ptr < 0x00007FFFFFFFFFFF;
}

// ============================================================================
// SEH-Safe Memory Read Helpers
// Standalone functions with no C++ objects, so MSVC allows __try/__except.
// ============================================================================

/// Safely read a pointer from the given address. Returns nullptr on fault.
inline void* SafeReadPointer(uintptr_t addr)
{
	__try
	{
		if (!IsValidPointer((void*)addr)) return nullptr;
		void* val = *(void**)addr;
		return val;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

/// Safely read a float from the given address. Returns 0.0f on fault.
inline float SafeReadFloat(uintptr_t addr)
{
	__try
	{
		if (!IsValidPointer((void*)addr)) return 0.0f;
		return *(float*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

/// Safely read a 32-bit int from the given address. Returns 0 on fault.
inline int SafeReadInt32(uintptr_t addr)
{
	__try
	{
		if (!IsValidPointer((void*)addr)) return 0;
		return *(int*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/// Safely read a 32-bit unsigned int. Returns 0 on fault.
inline unsigned int SafeReadUInt32(uintptr_t addr)
{
	__try
	{
		if (!IsValidPointer((void*)addr)) return 0;
		return *(unsigned int*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/// Safely read a bool. Returns false on fault.
inline bool SafeReadBool(uintptr_t addr)
{
	__try
	{
		if (!IsValidPointer((void*)addr)) return false;
		return *(bool*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

/// Safely read a double. Returns 0.0 on fault.
inline double SafeReadDouble(uintptr_t addr)
{
	__try
	{
		if (!IsValidPointer((void*)addr)) return 0.0;
		return *(double*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0.0; }
}

/// Check if a pointer targets the module's code/data region.
inline bool IsModulePointer(void* ptr, uintptr_t modBase, uintptr_t modEnd)
{
	uintptr_t addr = (uintptr_t)ptr;
	return addr >= modBase && addr < modEnd;
}

/// Safely count vtable entries starting from an object's vtable pointer.
/// Walks forward until a non-module pointer is hit (max 256 entries).
inline int SafeCountVTableEntries(void* instance, uintptr_t modBase, uintptr_t modEnd)
{
	__try
	{
		if (!IsValidPointer(instance)) return 0;
		void** vtable = *(void***)instance;
		if (!IsValidPointer(vtable)) return 0;

		int count = 0;
		while (count < 256)
		{
			void* entry = vtable[count];
			if (!IsValidPointer(entry)) break;
			uintptr_t addr = (uintptr_t)entry;
			if (addr < modBase || addr >= modEnd) break;
			count++;
		}
		return count;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/// Safely read a vtable entry at a given index. Returns 0 on fault.
inline uintptr_t SafeReadVTableEntry(void* instance, int index)
{
	__try
	{
		if (!IsValidPointer(instance)) return 0;
		void** vtable = *(void***)instance;
		if (!IsValidPointer(vtable)) return 0;
		void* entry = vtable[index];
		if (!IsValidPointer(entry)) return 0;
		return (uintptr_t)entry;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ============================================================================
// Frostbite Type Flags
// ============================================================================

enum kTypes
{
	kType_Pointer = 53,
	kType_Array = 65
};

// ============================================================================
// ClassInfo
// ============================================================================

class ClassInfo
{
public:
	static ClassInfo* GetInstance()
	{
		static ClassInfo** instance = NULL;
		if (!instance)
		{
			DWORD_PTR dwMatch = FindPattern((DWORD_PTR)GetModuleHandle(NULL), -1, 0, false, (BYTE*)"\x48\x8B\x05\x00\x00\x00\x00\x48\x89\x41\x08\x48\x89\x0D\x00\x00\x00\x00\xC3", "xxx????xxxxxxx????x");
			if (!dwMatch)
				return NULL;

			DWORD_PTR dwOffset = *(DWORD*)(dwMatch + 3);

			BYTE* first = (BYTE*)&dwOffset;
			if (first[3] == 0xFF)
				dwOffset = dwOffset + 0xFFFFFFFF00000000;

			DWORD_PTR dwOffset2 = (dwMatch + 7);

			instance = (ClassInfo**)(dwOffset + dwOffset2);
			Log("Instance found at 0x%016llX", instance);
		}
		return *instance;
	}

	TypeInfo* typeInfo; //0x0000 
	ClassInfo* next; //0x0008 
	unsigned short id; //0x0010 
	unsigned short isDataContainer; //0x0012 
	char pad_0x0014[0x4]; //0x0014
	ClassInfo* parent; //0x0018 
	char pad_0x0020[0x8]; //0x0020
	unsigned short id3; //0x0028 
	char pad_0x002C[0x94]; //0x002C

};//Size=0x00C0

// ============================================================================
// TypeInfo
// ============================================================================

class TypeInfo
{
public:
	char* name; //0x0000 
	unsigned short flags; //0x0008 
	unsigned short totalSize; //0x000A 
	char pad_0x000C[0x4];
	unsigned short flags2; //0x0010 
	char pad_0x0012[0x6]; //0x0012
	unsigned short alignment; //0x0018 
	unsigned short fieldCount; //0x001A 
	char pad_0x001C[0x4]; //0x001C
	FieldInfo* enumFields; //0x0020 
	FieldInfo* structFields; //0x0028 
	FieldInfo* fields; //0x0030 

};//Size=0x0038

// ============================================================================
// Member / Field descriptors
// ============================================================================

class MemberInfoFlags
{
public:
	unsigned short flagBits;
	enum
	{
		kMemberTypeMask = 0x3,
		kTypeCategoryShift = 0x2,
		kTypeCategoryMask = 0x3,
		kTypeCodeShift = 0x4,
		kTypeCodeMask = 0x1F,
		kMetadata = 0x800,
		kHomogeneous = 0x1000,
		kAlwaysPersist = 0x2000,
		kExposed = 0x2000,
		kLayoutImmutable = 0x4000,
		kBlittable = 0xFFFF8000
	};
};

enum TypeFlags
{
	kTypeFlag_Enum = 0xC000
};

class MemberTypeInfo
{
public:
	TypeInfo* typeInfo;	
	unsigned short flags;
	char pad[0x8];
};

class FieldInfo
{
public:
	int GetFieldSize()
	{
		if (!typeInfo)
			return 0;
		TypeInfo* ti = typeInfo->typeInfo;
		switch (ti->flags)
		{
		case kType_Pointer:
			return 8;
			
		case kType_Array:
			return 8;

		default:
			return ti->totalSize;
		}
	}

	char* name;
	MemberInfoFlags flags;
	unsigned short offset;
	char pad[0x4];
	MemberTypeInfo* typeInfo;
};

class FieldInfoEnum
{
public:
	char* name;
	MemberInfoFlags flags;
	unsigned short offset;
	char pad[0x4];
	__int32 value;
	char pad2[0x4];
};
inline bool SafeReadString(uintptr_t addr, char* dst, size_t maxLen)
{
	__try 
	{
		for (size_t i = 0; i < maxLen - 1; i++)
		{
			char c = *(char*)(addr + i);
			dst[i] = c;
			if (c == 0) return true;
		}
		dst[maxLen - 1] = 0;
		return true;
	}
	__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
