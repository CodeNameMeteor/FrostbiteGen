// A small, deterministic fake "game" for tests/run_sdk_test.sh.
//
// It builds a PE-like image in memory containing Frostbite-style reflection records
// (ClassInfo / TypeInfo / FieldInfo), vtables with real GetType() code stubs, a root
// pointer the pattern scanner can find, a static singleton, and heap objects.
// Everything the generator needs to find lives at fixed offsets inside the image, so a
// second process that rebuilds the engine can check the generated GetInstance() resolvers.
#pragma once

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fake
{
	constexpr uintptr_t kModuleSize = 0x40000;
	constexpr uintptr_t kNtHeaders = 0x80;
	constexpr uintptr_t kDataVA = 0x1000;        // the only (writable) section
	constexpr uintptr_t kDataSize = 0x1000;
	constexpr uintptr_t kRootGlobal = 0x1000;    // ClientGameContext* (heap object)
	constexpr uintptr_t kSettingsGlobal = 0x1010;// TestSettings* (heap object)
	constexpr uintptr_t kStaticObject = 0x1100;  // StaticManager object stored inside the image
	constexpr uintptr_t kCode = 0x4000;
	constexpr uintptr_t kRootPattern = 0x4800;   // mov rcx,[rip+x]; test rcx,rcx; jz
	constexpr uintptr_t kVTables = 0x6000;
	constexpr uintptr_t kTypeData = 0x10000;

	constexpr unsigned short kClassFlags = 0x0001;
	constexpr unsigned short kStructFlags = 41;
	constexpr unsigned short kEnumFlags = 49289;
	constexpr unsigned short kPrimitiveFlags = 13;

	struct FieldSpec
	{
		const char* name;
		TypeInfo* type;
		unsigned short offset;
	};

	struct Engine
	{
		uint8_t* module = nullptr;
		uintptr_t base = 0;
		uintptr_t typeCursor = kTypeData;
		uintptr_t codeCursor = kCode;
		uintptr_t vtableCursor = kVTables;
		ClassInfo* head = nullptr;
		std::map<std::string, TypeInfo*> prim;
		std::map<std::string, ClassInfo*> classes;

		// Live objects (what GetInstance() should return)
		uint8_t* ctx = nullptr;
		uint8_t* pm = nullptr;
		uint8_t* level = nullptr;
		uint8_t* entity0 = nullptr;
		uint8_t* entity1 = nullptr;
		uint8_t* child = nullptr;
		uint8_t* mystery = nullptr;
		uint8_t* settings = nullptr;
		uint8_t* staticObject = nullptr;

		template <typename T>
		T* New(size_t count = 1)
		{
			typeCursor = (typeCursor + 15) & ~(uintptr_t)15;
			T* p = (T*)(base + typeCursor);
			typeCursor += sizeof(T) * count;
			return p;
		}

		char* Str(const char* s)
		{
			char* p = New<char>(strlen(s) + 1);
			strcpy(p, s);
			return p;
		}

		TypeInfo* Type(const char* name, unsigned short flags, unsigned short size)
		{
			TypeInfo* t = New<TypeInfo>();
			t->name = Str(name);
			t->flags = flags;
			t->totalSize = size;
			return t;
		}

		TypeInfo* Ptr(const char* pointee) { return Type(pointee, 53, 8); }

		TypeInfo* Arr(TypeInfo* elem)
		{
			TypeInfo* t = Type((std::string(elem->name) + "-Array").c_str(), 65, 32);
			TypeInfo** slot = New<TypeInfo*>();
			*slot = elem;
			t->enumFields = (FieldInfo*)slot;
			return t;
		}

		FieldInfo* Fields(const std::vector<FieldSpec>& specs)
		{
			FieldInfo* fields = New<FieldInfo>(specs.size());
			for (size_t i = 0; i < specs.size(); i++)
			{
				MemberTypeInfo* mti = New<MemberTypeInfo>();
				mti->typeInfo = specs[i].type;
				fields[i].name = Str(specs[i].name);
				fields[i].offset = specs[i].offset;
				fields[i].typeInfo = mti;
			}
			return fields;
		}

		ClassInfo* Class(const char* name, unsigned short flags, unsigned short size, ClassInfo* parent,
			const std::vector<FieldSpec>& fields)
		{
			TypeInfo* t = Type(name, flags, size);
			t->fieldCount = (unsigned short)fields.size();
			if (!fields.empty())
			{
				if (flags == kStructFlags)
					t->structFields = Fields(fields);
				else
					t->fields = Fields(fields);
			}
			ClassInfo* c = New<ClassInfo>();
			c->typeInfo = t;
			c->parent = parent;
			c->next = head;
			head = c;
			classes[name] = c;
			return c;
		}

		ClassInfo* Enum(const char* name, unsigned short size, const std::vector<std::pair<const char*, int32_t>>& values)
		{
			TypeInfo* t = Type(name, kEnumFlags, size);
			t->fieldCount = (unsigned short)values.size();
			FieldInfoEnum* fields = New<FieldInfoEnum>(values.size());
			for (size_t i = 0; i < values.size(); i++)
			{
				fields[i].name = Str(values[i].first);
				fields[i].value = values[i].second;
			}
			t->enumFields = (FieldInfo*)fields;
			ClassInfo* c = New<ClassInfo>();
			c->typeInfo = t;
			c->next = head;
			head = c;
			classes[name] = c;
			return c;
		}

		uintptr_t Code(const std::vector<uint8_t>& bytes)
		{
			uintptr_t at = base + codeCursor;
			memcpy((void*)at, bytes.data(), bytes.size());
			codeCursor += (bytes.size() + 15) & ~(size_t)15;
			return at;
		}

		/// lea rax, [rip + (target - next)]; ret
		uintptr_t GetTypeStub(void* target)
		{
			uintptr_t at = base + codeCursor;
			int32_t disp = (int32_t)((uintptr_t)target - (at + 7));
			std::vector<uint8_t> code = { 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3 };
			memcpy(&code[3], &disp, 4);
			return Code(code);
		}

		/// vtable[-1] is left as a (zero) RTTI slot; the table is 0-terminated.
		void** VTable(const std::vector<uintptr_t>& entries)
		{
			vtableCursor += 8;
			void** vt = (void**)(base + vtableCursor);
			for (size_t i = 0; i < entries.size(); i++)
				vt[i] = (void*)entries[i];
			vt[entries.size()] = nullptr;
			vtableCursor += (entries.size() + 1) * 8;
			return vt;
		}

		uint8_t* Obj(size_t size, void** vtable)
		{
			uint8_t* o = (uint8_t*)calloc(1, size + 64);
			*(void***)o = vtable;
			return o;
		}

		template <typename T>
		static void Set(uint8_t* obj, size_t offset, T value) { memcpy(obj + offset, &value, sizeof(T)); }

		void Build()
		{
			module = (uint8_t*)aligned_alloc(0x10000, kModuleSize);
			memset(module, 0, kModuleSize);
			base = (uintptr_t)module;

			// --- PE headers ---
			auto dos = (IMAGE_DOS_HEADER*)module;
			dos->e_magic = 0x5A4D;
			dos->e_lfanew = (int32_t)kNtHeaders;
			auto nt = (IMAGE_NT_HEADERS*)(base + kNtHeaders);
			nt->Signature = 0x4550;
			nt->FileHeader.NumberOfSections = 1;
			nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
			nt->OptionalHeader.SizeOfImage = (DWORD)kModuleSize;
			auto sec = IMAGE_FIRST_SECTION(nt);
			memcpy(sec->Name, ".data", 5);
			sec->VirtualAddress = (DWORD)kDataVA;
			sec->Misc.VirtualSize = (DWORD)kDataSize;
			sec->Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

			// --- Primitive types ---
			const char* prims[][2] = {
				{ "Boolean", "1" }, { "Float32", "4" }, { "Float64", "8" }, { "Int8", "1" }, { "Int16", "2" },
				{ "Int32", "4" }, { "Int64", "8" }, { "Uint8", "1" }, { "Uint16", "2" }, { "Uint32", "4" },
				{ "Uint64", "8" }, { "CString", "8" }
			};
			for (auto& p : prims)
				prim[p[0]] = Type(p[0], kPrimitiveFlags, (unsigned short)atoi(p[1]));

			// --- Types ---
			ClassInfo* testEnum = Enum("TestEnum", 4, {
				{ "TE_A", 0 }, { "TE_B", 1 }, { "TE_Neg", -1 }, { "TE_B", 5 }, { "class", 6 },
				{ "Evil');__import__('os').system('echo pwned')#", 7 } });
			ClassInfo* smallEnum = Enum("SmallEnum", 1, { { "SE_Zero", 0 }, { "SE_Big", 200 } });

			ClassInfo* vec3 = Class("Vec3", kStructFlags, 0x10, nullptr, {
				{ "x", prim["Float32"], 0x0 }, { "y", prim["Float32"], 0x4 }, { "z", prim["Float32"], 0x8 } });

			TypeInfo* mysteryBlob = Type("MysteryBlob", kStructFlags, 8); // referenced, but not in the class list

			ClassInfo* testBase = Class("TestBase", kClassFlags, 0x10, nullptr, {
				{ "Id", prim["Int32"], 0x8 }, { "Flags", prim["Uint32"], 0xC } });

			ClassInfo* playerManager = Class("PlayerManager", kClassFlags, 0x20, nullptr, {
				{ "LocalPlayerId", prim["Int32"], 0x8 }, { "MaxPlayers", prim["Uint8"], 0xC },
				{ "Ratio", prim["Float32"], 0x10 }, { "Big", prim["Int64"], 0x18 } });

			ClassInfo* testLevel = Class("TestLevel", kClassFlags, 0x18, nullptr, {
				{ "Name", prim["CString"], 0x8 }, { "Gravity", prim["Float64"], 0x10 } });

			ClassInfo* testEntity = Class("TestEntity", kClassFlags, 0x68, testBase, {
				{ "Health", prim["Float32"], 0x10 },
				{ "Instance", prim["Uint8"], 0x14 },
				{ "Instance", prim["Uint8"], 0x15 },
				{ "Position", vec3->typeInfo, 0x18 },
				{ "Mode", testEnum->typeInfo, 0x28 },
				{ "Small", smallEnum->typeInfo, 0x2C },
				{ "TypeInfo", prim["Uint8"], 0x2D },
				{ "Blob", mysteryBlob, 0x30 },
				{ "Counter", prim["Uint64"], 0x38 },
				{ "OverlapField", prim["Int32"], 0x3C },
				{ "Owner", Ptr("PlayerManager"), 0x40 },
				{ "Tags", Arr(prim["Int32"]), 0x48 } });

			ClassInfo* clientGameContext = Class("ClientGameContext", kClassFlags, 0x48, testBase, {
				{ "PlayerManager", Ptr("PlayerManager"), 0x10 },
				{ "Level", Ptr("TestLevel"), 0x18 },
				{ "Entities", Arr(Ptr("TestEntity")), 0x20 },
				{ "Mystery", Ptr("Mystery"), 0x40 } });

			ClassInfo* testChild = Class("TestChild", kClassFlags, 0x10, nullptr, {
				{ "Speed", prim["Float32"], 0x8 } });

			ClassInfo* staticManager = Class("StaticManager", kClassFlags, 0x18, nullptr, {
				{ "Child", Ptr("TestChild"), 0x8 }, { "Value", prim["Int32"], 0x10 } });

			ClassInfo* testSettings = Class("TestSettings", kClassFlags, 0x18, nullptr, {
				{ "DrawEnable", prim["Boolean"], 0x8 }, { "Quirk\\", prim["Uint8"], 0x9 },
				{ "Quality", testEnum->typeInfo, 0xC }, { "BigValue", prim["Int64"], 0x10 } });

			// A type whose name must never reach generated code verbatim
			Class("Evil'Name", kClassFlags, 0x10, nullptr, {
				{ "x');__import__('os').system('echo pwned')#", prim["Int32"], 0x8 } });

			// --- Code & vtables ---
			uintptr_t ret = Code({ 0xC3 });
			uintptr_t getPlayerManager = Code({ 0x48, 0x8B, 0x41, 0x10, 0xC3 });       // mov rax,[rcx+0x10]; ret
			uintptr_t getId = Code({ 0xF3, 0x0F, 0x10, 0x41, 0x08, 0xC3 });            // movss xmm0,[rcx+8]; ret
			uintptr_t getPosition = Code({ 0x48, 0x8B, 0x41, 0x18, 0xC3 });           // mov rax,[rcx+0x18]; ret (struct field)

			// 26 entries: exercises hex formatting of the entry count and duplicate method names.
			std::vector<uintptr_t> ctxEntries = { GetTypeStub(clientGameContext), getPlayerManager, getPlayerManager, getId };
			while (ctxEntries.size() < 26)
				ctxEntries.push_back(ret);
			void** ctxVt = VTable(ctxEntries);
			void** pmVt = VTable({ GetTypeStub(playerManager), ret });
			void** levelVt = VTable({ GetTypeStub(testLevel), ret });
			void** entityVt = VTable({ GetTypeStub(testEntity), getPosition, ret });
			void** childVt = VTable({ GetTypeStub(testChild), ret });
			void** staticVt = VTable({ GetTypeStub(staticManager), ret });
			void** settingsVt = VTable({ GetTypeStub(testSettings), ret });
			void** mysteryVt = VTable({ ret, ret });

			// Root access pattern: mov rcx,[rip+disp]; test rcx,rcx; jz +5
			{
				uintptr_t at = base + kRootPattern;
				int32_t disp = (int32_t)((base + kRootGlobal) - (at + 7));
				uint8_t code[] = { 0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x48, 0x85, 0xC9, 0x74, 0x05 };
				memcpy(&code[3], &disp, 4);
				memcpy((void*)at, code, sizeof(code));
			}

			// --- Live objects ---
			pm = Obj(0x20, pmVt);
			Set<int32_t>(pm, 0x8, 3);
			Set<uint8_t>(pm, 0xC, 64);
			Set<float>(pm, 0x10, 1.0f);
			Set<int64_t>(pm, 0x18, INT64_MIN);

			level = Obj(0x18, levelVt);
			Set<const char*>(level, 0x8, "TestMap");
			Set<double>(level, 0x10, std::numeric_limits<double>::quiet_NaN());

			entity0 = Obj(0x68, entityVt);
			entity1 = Obj(0x68, entityVt);
			Set<float>(entity0, 0x10, 100.0f);
			Set<uint64_t>(entity0, 0x38, 0x1122334455667788ULL);
			Set<uint8_t*>(entity0, 0x40, pm);

			mystery = Obj(0x10, mysteryVt);

			ctx = Obj(0x48, ctxVt);
			Set<int32_t>(ctx, 0x8, 42);
			Set<uint8_t*>(ctx, 0x10, pm);
			Set<uint8_t*>(ctx, 0x18, level);
			uint8_t** storage = (uint8_t**)calloc(4, sizeof(void*));
			storage[0] = entity0;
			storage[1] = entity1;
			Set<uint8_t**>(ctx, 0x20, storage);      // first
			Set<uint8_t**>(ctx, 0x28, storage + 2);  // last
			Set<uint8_t**>(ctx, 0x30, storage + 4);  // bound
			Set<uint8_t*>(ctx, 0x40, mystery);

			child = Obj(0x10, childVt);
			Set<float>(child, 0x8, 2.5f);

			settings = Obj(0x18, settingsVt);
			Set<bool>(settings, 0x8, true);
			Set<int64_t>(settings, 0x10, 1234567890123LL);

			staticObject = module + kStaticObject;
			*(void***)staticObject = staticVt;
			Set<uint8_t*>(staticObject, 0x8, child);
			Set<int32_t>(staticObject, 0x10, 7);

			Set<uint8_t*>(module, kRootGlobal, ctx);
			Set<uint8_t*>(module, kSettingsGlobal, settings);

			fbgen_test::SetModule(module, "test.exe");
		}
	};
}
