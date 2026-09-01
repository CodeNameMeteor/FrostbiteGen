#include "classinfo.h"

// ============================================================================
// Constructor
// ============================================================================

ClassInfoManager::ClassInfoManager(ClassInfo* info) :
	m_listHead(info),
	m_moduleBase((uintptr_t)GetModuleHandle(NULL)),
	m_moduleEnd(0),
	m_clientGameCtxInstance(0),
	m_clientGameCtxGlobalOffset(0)
{
	size_t moduleSize;
	GetGameModuleInfo(m_moduleBase, moduleSize);
	m_moduleEnd = m_moduleBase + moduleSize;
}

// ============================================================================
// Class List & Dump Entry Points
// ============================================================================

static bool CheckValidPtr(void* ptr) {
	return ptr != nullptr && (uintptr_t)ptr > 0x10000 && (uintptr_t)ptr < 0x00007FFFFFFFFFFF;
}

void ClassInfoManager::BuildClassList()
{
	ClassInfo* c = m_listHead;
	while (c != NULL)
	{
		if (c->typeInfo && c->typeInfo->name)
		{
			m_classMap[c->typeInfo->name] = c;
			// Build reverse map: TypeInfo address AND ClassInfo address -> ClassInfo
			m_typeInfoToClassMap[(uintptr_t)c->typeInfo] = c;
			m_typeInfoToClassMap[(uintptr_t)c] = c;
		}
		c = c->next;
	}
	Log("BuildClassList: %d classes, %d TypeInfo mappings",
		(int)m_classMap.size(), (int)m_typeInfoToClassMap.size());
}

void ClassInfoManager::DumpClasses()
{
	// Phase 0: Find ClientGameContext via pattern scan
	Log("=== Phase 0: Finding ClientGameContext ===");
	m_clientGameCtxInstance = FindClientGameContext();

	// Phase 1: Build traversal map from the context root
	if (m_clientGameCtxInstance)
	{
		Log("=== Phase 1: Building traversal map ===");

		// Try to identify the root class by name or by vtable
		ClassInfo* contextCI = nullptr;
		for (auto& tm : m_traversalMap)
		{
			if (tm.second.resolvedAddress == m_clientGameCtxInstance)
			{
				auto ci = m_classMap.find(tm.first);
				if (ci != m_classMap.end())
				{
					contextCI = ci->second;
					Log("  Root context class (by name): %s", tm.first.c_str());
					break;
				}
			}
		}

		// If not found by name, try vtable-based identification
		if (!contextCI)
		{
			contextCI = TryIdentifyClassByVTable(m_clientGameCtxInstance);
			if (contextCI && contextCI->typeInfo && contextCI->typeInfo->name)
			{
				Log("  Root context class (by vtable): %s", contextCI->typeInfo->name);
				// Update traversal map with the correct name
				std::string realName = contextCI->typeInfo->name;
				TraversalChain rootChain;
				rootChain.targetClass = realName;
				rootChain.resolvedAddress = m_clientGameCtxInstance;
				m_traversalMap.erase("UnknownRoot");
				m_traversalMap[realName] = rootChain;
			}
		}

		std::vector<TraversalStep> rootChain;
		std::set<uintptr_t> visited;
		visited.insert(m_clientGameCtxInstance);

		if (contextCI)
		{
			// Known class ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â use field-based traversal
			BuildTraversalMap(m_clientGameCtxInstance, contextCI, 0, rootChain, visited);
		}
		else if (m_clientGameCtxInstance)
		{
			Log("  Root class unknown by name. Attempting vtable identification...");
			ClassInfo* identifiedRoot = TryIdentifyClassByVTable(m_clientGameCtxInstance, true);
			
			std::vector<TraversalStep> chain;
			std::set<uintptr_t> visited;
			visited.insert(m_clientGameCtxInstance);

			if (identifiedRoot && identifiedRoot->typeInfo && identifiedRoot->typeInfo->name)
			{
				std::string targetName = identifiedRoot->typeInfo->name;
				Log("  -> Identified Root Class: %s! Switching to standard traversal.", targetName.c_str());

				TraversalChain rootChain;
				rootChain.targetClass = targetName;
				rootChain.steps = chain;
				rootChain.resolvedAddress = m_clientGameCtxInstance;
				m_traversalMap[targetName] = rootChain;

				BuildTraversalMap(m_clientGameCtxInstance, identifiedRoot, 1, chain, visited);
			}
			else
			{
				Log("  -> Root class still unknown, using blind traversal fallback...");
				BlindTraversalWalk(m_clientGameCtxInstance, 0, chain, visited);
			}
		}

		Log("Traversal map built: %d classes reachable", (int)m_traversalMap.size());
	}
	else
	{
		Log("WARNING: Root singleton not found ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â instance resolution will be limited");
	}

	// Phase 1.5: Scan globals for singletons
	ScanGlobalsForSingletons();

	// Phase 2: Build cross-reference map
	Log("=== Phase 2: Building cross-reference map ===");
	BuildCrossRefMap();

	// Phase 3: Generate utility headers
	Log("=== Phase 3: Generating utility headers ===");
	GenerateFBSDKTypes();

	// Phase 4: Dump all types
	Log("=== Phase 4: Dumping all types ===");
	for (auto it : m_classMap)
	{
		std::string name = it.first;
		ClassInfo* c = it.second;
		if (!c) continue;
		TypeInfo* ti = c->typeInfo;
		if (!ti || !ti->name) continue;

		if (ti->flags == 49289)
			DumpEnum(c);
		else if (ti->flags == 41 || ti->flags == 32809 || ti->flags == 53289)
			DumpStruct(c);
		else if (ti->flags == 13 || ti->flags == 49357 || ti->flags == 49453 || ti->flags == 49421 || ti->flags == 49389 || ti->flags == 16493 ||
			ti->flags == 49517 || ti->flags == 16765 || ti->flags == 49341 || ti->flags == 49437 || ti->flags == 49405 || ti->flags == 49373 || ti->flags == 49501 ||
			ti->flags == 49485 || ti->flags == 49469 || ti->flags == 16541 || ti->flags == 16509 || ti->flags == 49325)
			Log("Found data type: %s, size = %d", ti->name, ti->totalSize);
		else if (ti->flags == 69)
			DumpStruct(c);
		else if (ti->flags == 29)
		{ /* unknown */ }
		else
			DumpClass(c);
	}

	// Phase 5: Generate master includes and declarations
	Log("=== Phase 5: Generating master headers ===");
	GenerateForwardDeclarations();
	GenerateSDKMasterHeader();

	// Phase 6: Generate bonus outputs
	Log("=== Phase 6: Generating bonus outputs ===");
	GenerateBonusOutputs();
}

void ClassInfoManager::GenerateBonusOutputs()
{
	__try { GenerateHierarchyTree(); } __except(1) { Log("Crash in GenerateHierarchyTree"); }
	__try { GenerateJSONSchema(); } __except(1) { Log("Crash in GenerateJSONSchema"); }
	__try { GenerateIDAScript(); } __except(1) { Log("Crash in GenerateIDAScript"); }
	__try { GenerateGhidraScript(); } __except(1) { Log("Crash in GenerateGhidraScript"); }
	__try { GenerateCrossRefFile(); } __except(1) { Log("Crash in GenerateCrossRefFile"); }
	__try { DumpLiveInstances(); } __except(1) { Log("Crash in DumpLiveInstances"); }
}

// ============================================================================
// P0: ClientGameContext Resolution
// ============================================================================

uintptr_t ClassInfoManager::FindClientGameContext()
{
	char modulePath[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA(NULL), modulePath, MAX_PATH);
	std::string modPathStr = modulePath;
	std::string exeName = modPathStr.substr(modPathStr.find_last_of("\\/") + 1);
	
	// Convert to lowercase for case-insensitive comparison
	std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::tolower);

	uintptr_t explicitGlobalOffset = 0;
	if (exeName == "mirrorsedgecatalyst.exe")
		explicitGlobalOffset = 0x2401CB0;
	else if (exeName == "bf4.exe")
		explicitGlobalOffset = 0x269F6B8; // Typical BF4 CTE/Retail offset
	else if (exeName == "bf1.exe")
		explicitGlobalOffset = 0x39384B0; // Typical BF1 offset
	else if (exeName == "starwarsbattlefrontii.exe")
		explicitGlobalOffset = 0x4648A78; // Typical SWBF2 offset
	
	if (explicitGlobalOffset != 0)
	{
		Log("  Found known game (%s). Checking explicit global 0x%llX...", exeName.c_str(), explicitGlobalOffset);
		uintptr_t explicitGlobal = m_moduleBase + explicitGlobalOffset;
		void* explicitPtr = SafeReadPointer(explicitGlobal);
		if (explicitPtr && IsValidPointer(explicitPtr))
		{
			Log("  Found instance at explicit global: 0x%016llX", (uintptr_t)explicitPtr);
			m_clientGameCtxGlobalOffset = explicitGlobalOffset;
			// Try to identify it by dereferencing
			ClassInfo* id = TryIdentifyClassByVTable((uintptr_t)explicitPtr, true);
			if (!id)
			{
				// Maybe the offset IS the instance (static object)?
				id = TryIdentifyClassByVTable(explicitGlobal, true);
				if (id) explicitPtr = (void*)explicitGlobal;
			}

			if (id && id->typeInfo && id->typeInfo->name)
			{
				TraversalChain rootChain;
				rootChain.targetClass = id->typeInfo->name;
				rootChain.resolvedAddress = (uintptr_t)explicitPtr;
				m_traversalMap[id->typeInfo->name] = rootChain;
				return (uintptr_t)explicitPtr;
			}
			
			// If it has no RTTI, return it anyway
			return (uintptr_t)explicitPtr;
		}
	}
	else
	{
		Log("  No explicit global offset known for %s. Falling back to dynamic heuristic scan...", exeName.c_str());
	}

	// ----------------------------------------------------------------
	// Step 1: Find the game-context class in our class map.
	//         Try many name variants ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â the root may not be called
	//         "ClientGameContext" in all FB3 games.
	// ----------------------------------------------------------------
	const char* candidateNames[] = {
		"ClientGameContext", "GameContext", "ServerGameContext",
		"ClientContext", "Context", "GameClient", "Client",
		"ClientLevel", "ClientWorld", "GameWorld", "WorldClient",
		"GameManager", "Main", "Application", "Engine",
		nullptr
	};

	ClassInfo* contextClass = nullptr;
	std::string contextClassName = "UnknownRoot";

	for (int i = 0; candidateNames[i]; i++)
	{
		auto it = m_classMap.find(candidateNames[i]);
		if (it != m_classMap.end())
		{
			if (!contextClass || it->second->typeInfo->totalSize > contextClass->typeInfo->totalSize)
			{
				contextClass = it->second;
				contextClassName = candidateNames[i];
			}
		}
	}

	// Log context-related class names
	Log("  Diagnostic: classes with 'Context', 'Client', 'Game', 'World', 'Manager':");
	for (auto& pair : m_classMap)
	{
		const std::string& n = pair.first;
		if (n.find("Context") != std::string::npos || n.find("Client") != std::string::npos ||
			n.find("GameWorld") != std::string::npos || n.find("GameManager") != std::string::npos)
		{
			ClassInfo* ci = pair.second;
			if (ci->typeInfo)
				Log("    %s  size=0x%X  isDC=%d  fields=%d",
					n.c_str(), ci->typeInfo->totalSize, ci->isDataContainer, ci->typeInfo->fieldCount);
		}
	}

	// Log top-20 largest classes (the root singleton is usually one of the biggest)
	Log("  Diagnostic: top 20 largest classes with pointer fields:");
	std::vector<std::pair<int, std::string>> classesBySize;
	for (auto& pair : m_classMap)
	{
		if (pair.second && pair.second->typeInfo)
			classesBySize.push_back({ pair.second->typeInfo->totalSize, pair.first });
	}
	std::sort(classesBySize.begin(), classesBySize.end(), std::greater<std::pair<int,std::string>>());
	for (int i = 0; i < 20 && i < (int)classesBySize.size(); i++)
	{
		auto ci = m_classMap[classesBySize[i].second];
		Log("    [%2d] %s  size=0x%X  fields=%d  isDC=%d",
			i + 1, classesBySize[i].second.c_str(), classesBySize[i].first,
			ci->typeInfo->fieldCount, ci->isDataContainer);
	}

	if (contextClass)
	{
		Log("  Matched context class: %s (size=0x%X, fields=%d)",
			contextClassName.c_str(), contextClass->typeInfo->totalSize,
			contextClass->typeInfo->fieldCount);
	}
	else
	{
		Log("  No exact name match ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â will use brute-force to find root singleton");
	}

	// Default scan range for sub-pointer validation
	int scanLimit = 0x200;
	if (contextClass && contextClass->typeInfo->totalSize > 0 && contextClass->typeInfo->totalSize < 0x400)
		scanLimit = contextClass->typeInfo->totalSize;

	// ----------------------------------------------------------------
	// Step 2: Try many pattern variants for singleton access.
	// ----------------------------------------------------------------
	struct PatternDef {
		const char* sig;
		const char* mask;
		int dispOff;
		int instrLen;
		const char* desc;
	};

	PatternDef patterns[] = {
		{ "\x48\x8B\x0D\x00\x00\x00\x00\x48\x85\xC9\x74", "xxx????xxxx", 3, 7, "mov rcx,[rip]; test; jz" },
		{ "\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x74", "xxx????xxxx", 3, 7, "mov rax,[rip]; test; jz" },
		{ "\x48\x8B\x0D\x00\x00\x00\x00\x48\x85\xC9\x0F\x84", "xxx????xxxxx", 3, 7, "mov rcx,[rip]; test; je long" },
		{ "\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x0F\x84", "xxx????xxxxx", 3, 7, "mov rax,[rip]; test; je long" },
		{ "\x48\x8B\x1D\x00\x00\x00\x00\x48\x85\xDB\x74", "xxx????xxxx", 3, 7, "mov rbx,[rip]; test; jz" },
		{ "\x48\x8B\x3D\x00\x00\x00\x00\x48\x85\xFF\x74", "xxx????xxxx", 3, 7, "mov rdi,[rip]; test; jz" },
		{ "\x48\x8B\x35\x00\x00\x00\x00\x48\x85\xF6\x74", "xxx????xxxx", 3, 7, "mov rsi,[rip]; test; jz" },
		{ "\x48\x8B\x0D\x00\x00\x00\x00\x48\x8B\x01", "xxx????xxx", 3, 7, "mov rcx,[rip]; mov rax,[rcx]" },
		{ "\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x08", "xxx????xxx", 3, 7, "mov rax,[rip]; mov rcx,[rax]" },
		{ "\x48\x8B\x05\x00\x00\x00\x00\xC3", "xxx????x", 3, 7, "mov rax,[rip]; ret" },
		{ "\x48\x8B\x0D\x00\x00\x00\x00\xC3", "xxx????x", 3, 7, "mov rcx,[rip]; ret" },
	};

	int numPatterns = sizeof(patterns) / sizeof(patterns[0]);

	// Helper: validate a candidate root singleton pointer
	auto validateCandidate = [&](uintptr_t globalAddr, void* objPtr, const char* desc) -> bool
	{
		if (!objPtr || !IsValidPointer(objPtr)) return false;

		void* vtable = SafeReadPointer((uintptr_t)objPtr);
		if (!vtable || !IsModulePointer(vtable, m_moduleBase, m_moduleEnd)) return false;

		int validPtrCount = 0;
		for (int off = 0x08; off < scanLimit; off += 8)
		{
			void* fieldPtr = SafeReadPointer((uintptr_t)objPtr + off);
			if (fieldPtr && IsValidPointer(fieldPtr))
			{
				void* fieldVt = SafeReadPointer((uintptr_t)fieldPtr);
				if (fieldVt && IsModulePointer(fieldVt, m_moduleBase, m_moduleEnd))
					validPtrCount++;
			}
		}

		if (validPtrCount >= 3)
		{
			Log("  MATCH via [%s]: obj=0x%016llX global=Module+0x%llX subPtrs=%d",
				desc, (uintptr_t)objPtr, globalAddr - m_moduleBase, validPtrCount);
			return true;
		}
		return false;
	};

	// Try each pattern
	for (int p = 0; p < numPatterns; p++)
	{
		DWORD_PTR scanAddr = m_moduleBase;
		int matchesChecked = 0;

		while (scanAddr < m_moduleEnd && matchesChecked < 200)
		{
			DWORD_PTR match = FindPattern(scanAddr, m_moduleEnd - scanAddr, 0, false,
				(BYTE*)patterns[p].sig, (char*)patterns[p].mask);

			if (!match) break;
			matchesChecked++;

			int32_t disp = *(int32_t*)(match + patterns[p].dispOff);
			uintptr_t globalPtr = match + patterns[p].instrLen + disp;

			void* objPtr = SafeReadPointer(globalPtr);

			if (validateCandidate(globalPtr, objPtr, patterns[p].desc))
			{
				m_clientGameCtxGlobalOffset = globalPtr - m_moduleBase;
				Log("Found %s at 0x%016llX (global @ Module+0x%llX)",
					contextClassName.c_str(), (uintptr_t)objPtr, m_clientGameCtxGlobalOffset);

				TraversalChain rootChain;
				rootChain.targetClass = contextClassName;
				rootChain.resolvedAddress = (uintptr_t)objPtr;
				m_traversalMap[contextClassName] = rootChain;

				return (uintptr_t)objPtr;
			}

			scanAddr = match + 1;
		}
	}

	// ----------------------------------------------------------------
	// Step 3: Brute-force ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â scan the .data section for global pointers
	//         that hold objects with many valid sub-pointers.
	// ----------------------------------------------------------------
	Log("  Pattern scan failed, trying brute-force global pointer scan...");

	// Find .data section
	PIMAGE_DOS_HEADER dosH = (PIMAGE_DOS_HEADER)m_moduleBase;
	PIMAGE_NT_HEADERS ntH = (PIMAGE_NT_HEADERS)(m_moduleBase + dosH->e_lfanew);
	PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(ntH);

	for (int s = 0; s < ntH->FileHeader.NumberOfSections; s++)
	{
		// Look for writable sections (.data, .bss)
		if (!(sec[s].Characteristics & IMAGE_SCN_MEM_WRITE))
			continue;

		uintptr_t secBase = m_moduleBase + sec[s].VirtualAddress;
		uintptr_t secEnd = secBase + sec[s].Misc.VirtualSize;

		Log("  Scanning section %.8s (0x%llX - 0x%llX)",
			sec[s].Name, secBase, secEnd);

		int bestScore = 0;
		uintptr_t bestGlobal = 0;
		void* bestObj = nullptr;

		for (uintptr_t addr = secBase; addr < secEnd - 8; addr += 8)
		{
			void* candidate = SafeReadPointer(addr);
			if (!candidate || !IsValidPointer(candidate))
				continue;

			// Must NOT be within the module (heap object)
			if (IsModulePointer(candidate, m_moduleBase, m_moduleEnd))
				continue;

			// Must have vtable in module
			void* vtable = SafeReadPointer((uintptr_t)candidate);
			if (!vtable || !IsModulePointer(vtable, m_moduleBase, m_moduleEnd))
				continue;

			// Count valid sub-object pointers
			int score = 0;
			for (int off = 0x08; off < 0x200; off += 8)
			{
				void* sub = SafeReadPointer((uintptr_t)candidate + off);
				if (sub && IsValidPointer(sub))
				{
					void* subVt = SafeReadPointer((uintptr_t)sub);
					if (subVt && IsModulePointer(subVt, m_moduleBase, m_moduleEnd))
						score++;
				}
			}

			if (score > bestScore)
			{
				bestScore = score;
				bestGlobal = addr;
				bestObj = candidate;
			}
		}

		// Accept if the best candidate has a decent number of sub-pointers
		if (bestScore >= 5)
		{
			m_clientGameCtxGlobalOffset = bestGlobal - m_moduleBase;
			Log("Found probable %s via brute-force: obj=0x%016llX global=Module+0x%llX (score=%d sub-ptrs)",
				contextClassName.c_str(), (uintptr_t)bestObj, m_clientGameCtxGlobalOffset, bestScore);

			TraversalChain rootChain;
			rootChain.targetClass = contextClassName;
			rootChain.resolvedAddress = (uintptr_t)bestObj;
			m_traversalMap[contextClassName] = rootChain;

			return (uintptr_t)bestObj;
		}
	}

	Log("ERROR: Could not find %s via any method", contextClassName.c_str());
	return 0;
}

// ============================================================================
// P0: Traversal Map Builder
// ============================================================================

void ClassInfoManager::BuildTraversalMap(uintptr_t instanceAddr, ClassInfo* classInfo,
	int depth, std::vector<TraversalStep>& currentChain, std::set<uintptr_t>& visited)
{
	if (depth > 5 || !classInfo || !classInfo->typeInfo)
		return;

	TypeInfo* ti = classInfo->typeInfo;
	if (!ti->fields || ti->fieldCount == 0)
		return;

	// Walk this class's pointer fields
	for (int i = 0; i < ti->fieldCount; ++i)
	{
		FieldInfo* fi = &ti->fields[i];
		if (!fi || !fi->typeInfo || !fi->typeInfo->typeInfo)
			continue;

		TypeInfo* fieldType = fi->typeInfo->typeInfo;

		// Only follow pointer fields
		if (fieldType->flags != kType_Pointer)
			continue;

		if (!fieldType->name)
			continue;

		std::string targetName = fieldType->name;

		// Read the live pointer value
		void* fieldPtr = SafeReadPointer(instanceAddr + fi->offset);
		if (!fieldPtr || !IsValidPointer(fieldPtr))
			continue;

		// Validate: the target should have a vtable in the module
		void* targetVtable = SafeReadPointer((uintptr_t)fieldPtr);
		if (!targetVtable || !IsModulePointer(targetVtable, m_moduleBase, m_moduleEnd))
			continue;

		// Prevent infinite loops
		if (visited.count((uintptr_t)fieldPtr))
			continue;

		// Build the chain to this target
		TraversalStep step;
		step.offset = fi->offset;
		step.fieldName = fi->name ? fi->name : "unknown";
		step.typeName = targetName;

		std::vector<TraversalStep> newChain = currentChain;
		newChain.push_back(step);

		// Store in traversal map (prefer shorter chains)
		auto existing = m_traversalMap.find(targetName);
		if (existing == m_traversalMap.end() || newChain.size() < existing->second.steps.size())
		{
			TraversalChain chain;
			chain.targetClass = targetName;
			chain.steps = newChain;
			chain.resolvedAddress = (uintptr_t)fieldPtr;
			m_traversalMap[targetName] = chain;

			Log("  Traversal: %s @ depth %d, offset chain length %d, addr 0x%016llX",
				targetName.c_str(), depth, (int)newChain.size(), (uintptr_t)fieldPtr);

			// Capture vtable info for this instance
			int vtableCount = SafeCountVTableEntries(fieldPtr, m_moduleBase, m_moduleEnd);
			if (vtableCount > 0)
			{
				VTableInfo vti;
				vti.entryCount = vtableCount;
				for (int v = 0; v < vtableCount; v++)
				{
					uintptr_t entry = SafeReadVTableEntry(fieldPtr, v);
					vti.entries.push_back(entry > 0 ? entry - m_moduleBase : 0);
				}
				m_vtableMap[targetName] = vti;
			}
		}

		// Recurse into this object's fields
		visited.insert((uintptr_t)fieldPtr);
		auto targetClassIt = m_classMap.find(targetName);
		if (targetClassIt != m_classMap.end())
		{
			BuildTraversalMap((uintptr_t)fieldPtr, targetClassIt->second,
				depth + 1, newChain, visited);
		}
	}

	// Also walk parent class fields
	if (classInfo->parent && classInfo->parent != classInfo)
	{
		BuildTraversalMap(instanceAddr, classInfo->parent, depth, currentChain, visited);
	}
}

ClassInfo* ClassInfoManager::TryIdentifyClassByVTable(uintptr_t instanceAddr, bool verbose)
{
	void* vtable = SafeReadPointer(instanceAddr);
	if (!vtable || !IsModulePointer(vtable, m_moduleBase, m_moduleEnd))
		return nullptr;

	int vtableCount = SafeCountVTableEntries((void*)instanceAddr, m_moduleBase, m_moduleEnd);
	if (vtableCount > 50) vtableCount = 50;

	for (int i = 0; i < vtableCount; i++)
	{
		uintptr_t funcAddr = SafeReadVTableEntry((void*)instanceAddr, i);
		if (!funcAddr || !IsModulePointer((void*)funcAddr, m_moduleBase, m_moduleEnd))
			continue;

		unsigned char code[32];
		if (!SafeReadBytes(funcAddr, code, sizeof(code)))
			continue;

		for (int j = 0; j < sizeof(code) - 7; j++)
		{
			if ((code[j] == 0x48 && code[j+1] == 0x8D && code[j+2] == 0x05) || // lea rax
			    (code[j] == 0x48 && code[j+1] == 0x8B && code[j+2] == 0x05) || // mov rax
			    (code[j] == 0x48 && code[j+1] == 0x8D && code[j+2] == 0x0D))   // lea rcx
			{
				int32_t disp = *(int32_t*)&code[j+3];
				uintptr_t targetAddr = funcAddr + j + 7 + (int64_t)disp;

				// If it's a 'mov rax, [rip+disp]', it's loading a pointer from a global variable.
				// We need to dereference it to get the actual TypeInfo/ClassInfo address.
				if (code[j+1] == 0x8B)
				{
					void* deref = SafeReadPointer(targetAddr);
					if (deref) targetAddr = (uintptr_t)deref;
				}

				if (verbose)
				{
					Log("      [vtable %d] found RIP-relative at +%d: target 0x%llX", i, j, targetAddr);
				}

				auto it = m_typeInfoToClassMap.find(targetAddr);
				if (it != m_typeInfoToClassMap.end())
				{
					if (verbose) Log("      -> MATCHED ClassInfo: %s", it->second->typeInfo->name);
					return it->second;
				}
			}
		}
	}

	return nullptr;
}

void ClassInfoManager::BlindTraversalWalk(uintptr_t objectAddr, int depth,
	std::vector<TraversalStep>& currentChain,
	std::set<uintptr_t>& visited)
{
	if (depth > 4) return;
	bool isRoot = (depth == 0);

	if (isRoot) Log("  Starting BlindTraversalWalk on root 0x%llX", objectAddr);

	for (int off = 0x08; off < 0x200; off += 8)
	{
		void* ptr = SafeReadPointer(objectAddr + off);
		if (!ptr || !IsValidPointer(ptr)) continue;

		void* vtable = SafeReadPointer((uintptr_t)ptr);
		if (!vtable || !IsModulePointer(vtable, m_moduleBase, m_moduleEnd))
		{
			if (isRoot && IsValidPointer(ptr))
				Log("    [+%X] Ptr 0x%llX - Invalid vtable (0x%llX)", off, (uintptr_t)ptr, (uintptr_t)vtable);
			continue;
		}

		if (isRoot) Log("    [+%X] Ptr 0x%llX (vtable 0x%llX) - checking...", off, (uintptr_t)ptr, (uintptr_t)vtable);

		if (visited.count((uintptr_t)ptr))
		{
			if (isRoot) Log("      Already visited.");
			continue;
		}

		ClassInfo* identifiedClass = TryIdentifyClassByVTable((uintptr_t)ptr, isRoot);

		TraversalStep step;
		step.offset = off;
		char fieldBuf[32];
		sprintf(fieldBuf, "unk_0x%X", off);
		step.fieldName = fieldBuf;

		std::vector<TraversalStep> newChain = currentChain;

		if (identifiedClass && identifiedClass->typeInfo && identifiedClass->typeInfo->name)
		{
			std::string targetName = identifiedClass->typeInfo->name;
			step.typeName = targetName;
			newChain.push_back(step);

			auto existing = m_traversalMap.find(targetName);
			if (existing == m_traversalMap.end() || newChain.size() < existing->second.steps.size())
			{
				TraversalChain chain;
				chain.targetClass = targetName;
				chain.steps = newChain;
				chain.resolvedAddress = (uintptr_t)ptr;
				m_traversalMap[targetName] = chain;

				Log("  [!] Blind Traversal Found: %s @ offset 0x%X, depth %d", targetName.c_str(), off, depth);

				int vtableCount = SafeCountVTableEntries(ptr, m_moduleBase, m_moduleEnd);
				if (vtableCount > 0)
				{
					VTableInfo vti;
					vti.entryCount = vtableCount;
					for (int v = 0; v < vtableCount; v++)
					{
						uintptr_t entry = SafeReadVTableEntry(ptr, v);
						vti.entries.push_back(entry > 0 ? entry - m_moduleBase : 0);
					}
					m_vtableMap[targetName] = vti;
				}
			}

			visited.insert((uintptr_t)ptr);
			BuildTraversalMap((uintptr_t)ptr, identifiedClass, depth + 1, newChain, visited);
		}
		else
		{
			if (isRoot) Log("      Could not identify class from vtable.");
			step.typeName = "UnknownClass";
			newChain.push_back(step);

			visited.insert((uintptr_t)ptr);
			BlindTraversalWalk((uintptr_t)ptr, depth + 1, newChain, visited);
		}
	}
}

// ============================================================================
// Class Dumping (with all new features)
// ============================================================================

void ClassInfoManager::DumpClass(ClassInfo* c)
{
	TypeInfo* ti = c->typeInfo;
	if (!ti || strlen(ti->name) == 0)
		return;

	std::vector<ClassInfo*> parents = GetParents(c);

	std::string sanitizedName = GetSanitizedClassName(c->typeInfo->name);
	char headerFile[128], headerPath[MAX_PATH];
	sprintf(headerFile, "SDK\\%s.h", sanitizedName.c_str());
	GetDirFile(headerFile, headerPath, sizeof(headerPath));

	std::ofstream file;
	file.open(headerPath, std::ios::out | std::ios::trunc);
	if (!file.is_open())
		return;

	DumpHeader(file, headerFile);
	m_sdkFileNames.push_back(sanitizedName);
	m_forwardDecls.insert(std::string("class ") + c->typeInfo->name);

	file << std::endl << "#ifndef FBGEN_" << sanitizedName << "_H" << std::endl;
	file << "#define FBGEN_" << sanitizedName << "_H" << std::endl << std::endl;

	file << "#include \"FBSDKTypes.h\"" << std::endl;

	std::vector<FieldInfo*> members;
	ParseClassMembers(ti, members);
	ResolveHeaders(members, file);

	if (parents.size() > 0)
	{
		file << "#include \"" << parents.at(0)->typeInfo->name << ".h\"" << std::endl << std::endl;
		file << "class " << ti->name << " :" << std::endl;
		file << "\tpublic " << parents.at(0)->typeInfo->name << " // size = 0x" << std::hex << parents.at(0)->typeInfo->totalSize << std::endl;
	}
	else
	{
		file << "class " << ti->name << std::endl;
	}

	file << "{" << std::endl;
	file << "public:" << std::endl;

	// --- Traversal chain docs ---
	DumpTraversalChainComment(c, file);

	// --- Type Info (ASLR-safe) ---
	DumpTypeInfo(c, file);

	// --- Instance Resolver ---
	DumpInstanceResolver(c, file);

	// --- VTable info ---
	DumpVTable(c, file);

	// --- Offset Constants ---
	DumpOffsetConstants(file, members);

	// --- Default Value Snapshots ---
	DumpDefaultValues(c, file, members);

	// --- Member Declarations ---
	int totalSizeOfClass = ti->totalSize;
	int totalSizeOfParents = 0;
	if (parents.size() > 0)
		totalSizeOfParents += parents.at(0)->typeInfo->totalSize;

	int memberSize = DumpClassMembers(file, members, totalSizeOfParents);
	if (memberSize + totalSizeOfParents < totalSizeOfClass)
		file << "\tunsigned char _0x" << std::hex << (memberSize + totalSizeOfParents) << "[0x" << std::hex << (totalSizeOfClass - (memberSize + totalSizeOfParents)) << "];" << std::endl;

	// --- Getter/Setter Accessors ---
	file << std::endl;
	DumpGetterSetters(file, members, ti);

	// --- VMT Hook Helper ---
	DumpVMTHookHelper(c, file);

	file << "}; // size = 0x" << std::hex << totalSizeOfClass << std::endl << std::endl;
	file << "#endif // FBGEN_" << c->typeInfo->name << "_H" << std::endl;

	file.close();
}

// ============================================================================
// Struct Dumping (with new features)
// ============================================================================

void ClassInfoManager::DumpStruct(ClassInfo* c)
{
	TypeInfo* ti = c->typeInfo;
	if (!ti || strlen(ti->name) == 0)
		return;

	std::string sanitizedName = GetSanitizedClassName(c->typeInfo->name);
	char headerFile[128], headerPath[MAX_PATH];
	sprintf(headerFile, "SDK\\%s.h", sanitizedName.c_str());
	GetDirFile(headerFile, headerPath, sizeof(headerPath));

	std::ofstream file;
	file.open(headerPath, std::ios::out | std::ios::trunc);
	if (!file.is_open())
		return;

	DumpHeader(file, headerFile);
	m_sdkFileNames.push_back(sanitizedName);
	m_forwardDecls.insert(std::string("struct ") + c->typeInfo->name);

	file << std::endl << "#ifndef FBGEN_" << sanitizedName << "_H" << std::endl;
	file << "#define FBGEN_" << sanitizedName << "_H" << std::endl << std::endl;
	file << "#include \"FBSDKTypes.h\"" << std::endl;

	std::vector<FieldInfo*> members;
	ParseStructMembers(ti, members);
	ResolveHeaders(members, file);

	file << "struct " << ti->name << std::endl;
	file << "{" << std::endl;

	DumpTypeInfo(c, file);
	DumpOffsetConstants(file, members);

	int totalSizeOfClass = ti->totalSize;
	int memberSize = DumpClassMembers(file, members, 0);
	if (memberSize < totalSizeOfClass)
		file << "\tunsigned char _0x" << std::hex << memberSize << "[0x" << std::hex << (totalSizeOfClass - memberSize) << "];" << std::endl;

	file << std::endl;
	DumpGetterSetters(file, members, ti);

	file << "}; // size = 0x" << std::hex << totalSizeOfClass << std::endl << std::endl;
	file << "#endif // FBGEN_" << c->typeInfo->name << "_H" << std::endl;

	file.close();
}

// ============================================================================
// Enum Dumping (unchanged)
// ============================================================================

void ClassInfoManager::DumpEnum(ClassInfo* c)
{
	TypeInfo* ti = c->typeInfo;
	if (!ti || strlen(ti->name) == 0) return;

	std::string sanitizedName = GetSanitizedClassName(c->typeInfo->name);
	char headerFile[128], headerPath[MAX_PATH];
	sprintf(headerFile, "SDK\\%s.h", sanitizedName.c_str());
	GetDirFile(headerFile, headerPath, sizeof(headerPath));

	std::ofstream file;
	file.open(headerPath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) return;

	DumpHeader(file, headerFile);
	m_sdkFileNames.push_back(sanitizedName);
	m_forwardDecls.insert(std::string("enum ") + c->typeInfo->name);

	file << std::endl << "#ifndef FBGEN_" << sanitizedName << "_H" << std::endl;
	file << "#define FBGEN_" << sanitizedName << "_H" << std::endl << std::endl;
	file << "enum " << ti->name << std::endl;
	file << "{" << std::endl;
	DumpEnumMembers(file, ti);
	file << "};" << std::endl << std::endl;
	file << "#endif // FBGEN_" << c->typeInfo->name << "_H" << std::endl;

	file.close();
}

void ClassInfoManager::DumpEnumMembers(std::ofstream& file, TypeInfo* ti)
{
	for (int i = 0; i < ti->fieldCount; ++i)
	{
		FieldInfo* fi = &ti->enumFields[i];
		if (!fi) continue;
		FieldInfoEnum* fie = (FieldInfoEnum*)fi;
		file << "\t" << fie->name << " = 0x" << std::hex << fie->value << "," << std::endl;
	}
}

// ============================================================================
// Member Parsing & Dumping (unchanged)
// ============================================================================

void ClassInfoManager::ResolveHeaders(std::vector<FieldInfo*> members, std::ofstream& file)
{
	for (unsigned int i = 0; i < members.size(); ++i)
	{
		FieldInfo* v1 = members.at(i);
		for (unsigned int j = 0; j < members.size(); ++j)
		{
			FieldInfo* v2 = members.at(j);
			if (v1 == v2) continue;
			if (!strcmp(v1->typeInfo->typeInfo->name, v2->typeInfo->typeInfo->name))
			{
				members.erase(members.begin() + i);
				i--;
				break;
			}
		}
	}
	for (auto m : members)
	{
		const char* tn = m->typeInfo->typeInfo->name;
		if (!strcmp(tn, "Boolean") || !strcmp(tn, "Float32") || !strcmp(tn, "Float64") ||
			!strcmp(tn, "Int8") || !strcmp(tn, "Int16") || !strcmp(tn, "Int32") || !strcmp(tn, "Int64") ||
			!strcmp(tn, "Uint8") || !strcmp(tn, "Uint16") || !strcmp(tn, "Uint32") || !strcmp(tn, "Uint64") ||
			!strcmp(tn, "CString"))
			continue;
		if (m->typeInfo->typeInfo->flags == kType_Array)
		{
			file << "#include \"Array.h\"" << std::endl;
			continue;
		}
		file << "#include \"" << tn << ".h\"" << std::endl;
	}
}

char* ClassInfoManager::GetFixedClassName(const char* orig)
{
	if (!orig || !IsValidPointer((void*)orig)) return (char*)"unk";
	if (!strcmp(orig, "Boolean"))   return (char*)"bool";
	if (!strcmp(orig, "Float32"))   return (char*)"float";
	if (!strcmp(orig, "Float64"))   return (char*)"double";
	if (!strcmp(orig, "Int8"))      return "char";
	if (!strcmp(orig, "Int16"))     return "short";
	if (!strcmp(orig, "Int32"))     return "int";
	if (!strcmp(orig, "Int64"))     return "long";
	if (!strcmp(orig, "Uint8"))     return "unsigned char";
	if (!strcmp(orig, "Uint16"))    return "unsigned short";
	if (!strcmp(orig, "Uint32"))    return "unsigned int";
	if (!strcmp(orig, "Uint64"))    return "unsigned long";
	if (!strcmp(orig, "CString"))   return "const char*";
	return (char*)orig;
}

void ClassInfoManager::ParseClassMembers(TypeInfo* ti, std::vector<FieldInfo*>& members)
{
	for (int i = 0; i < ti->fieldCount; ++i)
	{
		FieldInfo* fi = &ti->fields[i];
		if (!fi || !fi->typeInfo || !fi->typeInfo->typeInfo) continue;
		members.push_back(fi);
	}
	auto cmp = [](const FieldInfo* a, const FieldInfo* b) { return a->offset < b->offset; };
	std::sort(members.begin(), members.end(), cmp);
}

void ClassInfoManager::ParseStructMembers(TypeInfo* ti, std::vector<FieldInfo*>& members)
{
	for (int i = 0; i < ti->fieldCount; ++i)
	{
		FieldInfo* fi = &ti->structFields[i];
		if (!fi || !fi->typeInfo || !fi->typeInfo->typeInfo) continue;
		members.push_back(fi);
	}
	auto cmp = [](const FieldInfo* a, const FieldInfo* b) { return a->offset < b->offset; };
	std::sort(members.begin(), members.end(), cmp);
}

int ClassInfoManager::DumpClassMembers(std::ofstream& file, std::vector<FieldInfo*>& members, int parentSize)
{
	int totalSize = 0;
	int lastOffset = parentSize;

	for (int i = 0; i < (int)members.size(); ++i)
	{
		FieldInfo* fi = members.at(i);
		if (fi->offset > lastOffset)
		{
			file << "\tunsigned char _0x" << std::hex << lastOffset << "[0x" << std::hex << (fi->offset - lastOffset) << "];" << std::endl;
			totalSize += fi->offset - lastOffset;
		}
		lastOffset = fi->offset + fi->GetFieldSize();

		MemberTypeInfo* fti = fi->typeInfo;
		TypeInfo* mti = fti->typeInfo;

		if (mti->flags == kType_Pointer)
			file << "\t" << GetFixedClassName(mti->name) << "* m_" << fi->name << "; // 0x" << std::hex << fi->offset << std::endl;
		else if (mti->flags == kType_Array)
		{
			TypeInfo* ati = *(TypeInfo**)mti->enumFields;
			if (ati->flags == kType_Pointer)
				file << "\tArray<" << GetFixedClassName(ati->name) << "*> m_" << fi->name << "; // 0x" << std::hex << fi->offset << std::endl;
			else
				file << "\tArray<" << GetFixedClassName(ati->name) << "> m_" << fi->name << "; // 0x" << std::hex << fi->offset << std::endl;
		}
		else
			file << "\t" << GetFixedClassName(fti->typeInfo->name) << " m_" << fi->name << "; // 0x" << std::hex << fi->offset << std::endl;

		totalSize += fi->GetFieldSize();
	}
	return totalSize;
}

std::vector<ClassInfo*> ClassInfoManager::GetParents(ClassInfo* c)
{
	std::vector<ClassInfo*> parents;
	ClassInfo* p = c->parent;
	ClassInfo* lastP = c;
	while (p)
	{
		if (p == lastP) break;
		parents.push_back(p);
		lastP = p;
		p = p->parent;
	}
	return parents;
}

// ============================================================================
// Type Info (ASLR-safe ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â unchanged)
// ============================================================================

void ClassInfoManager::DumpTypeInfo(ClassInfo* c, std::ofstream& file)
{
	uintptr_t relativeAddr = (uintptr_t)c - m_moduleBase;
	file << "\tstatic void* GetTypeInfo()" << std::endl;
	file << "\t{" << std::endl;
	file << "\t\treturn (void*)(fb::GetModuleBase() + 0x" << std::hex << relativeAddr << ");" << std::endl;
	file << "\t}" << std::endl;
}

// ============================================================================
// P0: Instance Resolver (rewritten ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ClientGameContext traversal)
// ============================================================================

void ClassInfoManager::DumpInstanceResolver(ClassInfo* c, std::ofstream& file)
{
	std::string className = c->typeInfo->name;

	file << "\tstatic " << className << "* GetInstance()" << std::endl;
	file << "\t{" << std::endl;

	if (m_globalInstances.find(className) != m_globalInstances.end())
	{
		file << "\t\t// Global singleton found dynamically" << std::endl;
		file << "\t\tuintptr_t ptr = fb::Read<uintptr_t>(fb::GetModuleBase() + 0x"
			<< std::hex << m_globalInstances[className] << ");" << std::endl;
		file << "\t\tif (!ptr) return nullptr;" << std::endl;
		file << "\t\treturn reinterpret_cast<" << className << "*>(ptr);" << std::endl;
	}
	else if (className == "ClientGameContext" && m_clientGameCtxGlobalOffset != 0)
	{
		// Special case: ClientGameContext is the root ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â resolve directly from global ptr
		file << "\t\t// Root singleton ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â resolved via pattern scan" << std::endl;
		file << "\t\tuintptr_t pCtx = fb::Read<uintptr_t>(fb::GetModuleBase() + 0x"
			<< std::hex << m_clientGameCtxGlobalOffset << ");" << std::endl;
		file << "\t\tif (!pCtx) return nullptr;" << std::endl;
		file << "\t\treturn reinterpret_cast<" << className << "*>(pCtx);" << std::endl;
	}
	else
	{
		auto it = m_traversalMap.find(className);
		if (it != m_traversalMap.end() && !it->second.steps.empty() && m_clientGameCtxGlobalOffset != 0)
		{
			const TraversalChain& chain = it->second;

			// Build the ReadChain offset list
			file << "\t\t// Resolved via ClientGameContext traversal:" << std::endl;
			file << "\t\t// ClientGameContext";
			for (auto& step : chain.steps)
				file << " -> +" << std::hex << "0x" << step.offset << " (" << step.fieldName << ")";
			file << std::endl;

			file << "\t\tuintptr_t ctx = fb::Read<uintptr_t>(fb::GetModuleBase() + 0x"
				<< std::hex << m_clientGameCtxGlobalOffset << ");" << std::endl;
			file << "\t\tif (!ctx) return nullptr;" << std::endl;

			// Chain through each step
			for (size_t i = 0; i < chain.steps.size(); i++)
			{
				file << "\t\tctx = fb::Read<uintptr_t>(ctx + 0x"
					<< std::hex << chain.steps[i].offset << "); // " << chain.steps[i].fieldName << std::endl;
				file << "\t\tif (!ctx) return nullptr;" << std::endl;
			}

			file << "\t\treturn reinterpret_cast<" << className << "*>(ctx);" << std::endl;
		}
		else
		{
			// Not found in traversal ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â emit stub
			file << "\t\t// WARNING: No traversal chain found from ClientGameContext." << std::endl;
			file << "\t\t// This class was not reachable during SDK generation." << std::endl;
			file << "\t\t// Use fb::PatternScan() or manual pointer chains to resolve." << std::endl;
			file << "\t\treturn nullptr;" << std::endl;
		}
	}

	file << "\t}" << std::endl;
}

// ============================================================================
// P1: Traversal Chain Comment
// ============================================================================

void ClassInfoManager::DumpTraversalChainComment(ClassInfo* c, std::ofstream& file)
{
	std::string className = c->typeInfo->name;
	auto it = m_traversalMap.find(className);

	if (it != m_traversalMap.end())
	{
		const TraversalChain& chain = it->second;
		file << "\t// -------------------------------------------------------" << std::endl;
		file << "\t// Traversal chain from ClientGameContext:" << std::endl;
		file << "\t//   ClientGameContext* (Module+0x" << std::hex << m_clientGameCtxGlobalOffset << ")" << std::endl;

		std::string indent = "\t//     ";
		for (auto& step : chain.steps)
		{
			file << indent << "-> +0x" << std::hex << step.offset
				<< " (" << step.fieldName << " : " << step.typeName << "*)" << std::endl;
		}

		file << "\t// Live address at generation: 0x" << std::hex << chain.resolvedAddress << std::endl;
		file << "\t// -------------------------------------------------------" << std::endl;
	}
}

// ============================================================================
// P1: Default Value Snapshots
// ============================================================================

void ClassInfoManager::DumpDefaultValues(ClassInfo* c, std::ofstream& file,
	std::vector<FieldInfo*>& members)
{
	std::string className = c->typeInfo->name;
	auto it = m_traversalMap.find(className);
	if (it == m_traversalMap.end())
		return; // no live instance ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â can't snapshot

	uintptr_t instanceAddr = it->second.resolvedAddress;
	if (!instanceAddr)
		return;

	file << "\tstruct Defaults {" << std::endl;

	for (auto fi : members)
	{
		MemberTypeInfo* fti = fi->typeInfo;
		TypeInfo* mti = fti->typeInfo;
		const char* fixedName = GetFixedClassName(mti->name);
		uintptr_t fieldAddr = instanceAddr + fi->offset;

		// Only snapshot simple value types
		if (!strcmp(fixedName, "bool"))
		{
			bool val = SafeReadBool(fieldAddr);
			file << "\t\tstatic constexpr bool " << fi->name << " = " << (val ? "true" : "false") << ";" << std::endl;
		}
		else if (!strcmp(fixedName, "float"))
		{
			float val = SafeReadFloat(fieldAddr);
			file << "\t\tstatic constexpr float " << fi->name << " = " << std::defaultfloat << val << "f;" << std::endl;
		}
		else if (!strcmp(fixedName, "int") || !strcmp(fixedName, "short") || !strcmp(fixedName, "char"))
		{
			int val = SafeReadInt32(fieldAddr);
			file << "\t\tstatic constexpr int " << fi->name << " = " << std::dec << val << ";" << std::endl;
		}
		else if (!strcmp(fixedName, "unsigned int") || !strcmp(fixedName, "unsigned short") || !strcmp(fixedName, "unsigned char"))
		{
			unsigned int val = SafeReadUInt32(fieldAddr);
			file << "\t\tstatic constexpr unsigned int " << fi->name << " = " << std::dec << val << ";" << std::endl;
		}
		else if (!strcmp(fixedName, "double"))
		{
			double val = SafeReadDouble(fieldAddr);
			file << "\t\tstatic constexpr double " << fi->name << " = " << std::defaultfloat << val << ";" << std::endl;
		}
		// Skip pointers, arrays, structs ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â can't constexpr those
	}

	file << "\t};" << std::endl;
}

// ============================================================================
// P2: VTable Dumping
// ============================================================================

void ClassInfoManager::DumpVTable(ClassInfo* c, std::ofstream& file)
{
	std::string className = c->typeInfo->name;
	auto it = m_vtableMap.find(className);
	if (it == m_vtableMap.end())
		return;

	const VTableInfo& vti = it->second;
	if (vti.entryCount == 0)
		return;

	file << "\tstruct VTable {" << std::endl;
	file << "\t\tstatic constexpr int EntryCount = " << std::dec << vti.entryCount << ";" << std::endl;

	for (int i = 0; i < vti.entryCount && i < (int)vti.entries.size(); i++)
	{
		file << "\t\tstatic constexpr uintptr_t Func_" << std::dec << i
			<< " = 0x" << std::hex << vti.entries[i] << "; // Module+0x"
			<< std::hex << vti.entries[i] << std::endl;
	}

	file << std::endl;
	file << "\t\tstatic void* GetEntry(void* instance, int index) {" << std::endl;
	file << "\t\t\treturn (*(void***)instance)[index];" << std::endl;
	file << "\t\t}" << std::endl;
	file << "\t};" << std::endl;
}

// ============================================================================
// P3: VMT Hook Helper
// ============================================================================

void ClassInfoManager::DumpVMTHookHelper(ClassInfo* c, std::ofstream& file)
{
	std::string className = c->typeInfo->name;
	auto it = m_vtableMap.find(className);
	if (it == m_vtableMap.end() || it->second.entryCount == 0)
		return;

	file << std::endl;
	file << "\t// --- VMT Hook Helper ---" << std::endl;
	file << "\ttemplate<typename T>" << std::endl;
	file << "\tstatic T HookVFunc(void* instance, int index, T newFunc) {" << std::endl;
	file << "\t\tvoid** vtable = *(void***)instance;" << std::endl;
	file << "\t\tDWORD oldProtect;" << std::endl;
	file << "\t\tVirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);" << std::endl;
	file << "\t\tT original = (T)vtable[index];" << std::endl;
	file << "\t\tvtable[index] = (void*)newFunc;" << std::endl;
	file << "\t\tVirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);" << std::endl;
	file << "\t\treturn original;" << std::endl;
	file << "\t}" << std::endl;
}

// ============================================================================
// Offset Constants (unchanged)
// ============================================================================

void ClassInfoManager::DumpOffsetConstants(std::ofstream& file, std::vector<FieldInfo*>& members)
{
	if (members.empty()) return;
	file << "\tstruct Offsets {" << std::endl;
	for (auto fi : members)
		file << "\t\tstatic constexpr size_t " << fi->name << " = 0x" << std::hex << fi->offset << ";" << std::endl;
	file << "\t};" << std::endl;
}

// ============================================================================
// Getter/Setter Generation (unchanged)
// ============================================================================

void ClassInfoManager::DumpGetterSetters(std::ofstream& file, std::vector<FieldInfo*>& members, TypeInfo* ti)
{
	if (members.empty()) return;
	file << "\t// --- Accessors ---" << std::endl;

	for (auto fi : members)
	{
		MemberTypeInfo* fti = fi->typeInfo;
		TypeInfo* mti = fti->typeInfo;
		const char* fn = GetFixedClassName(mti->name);

		if (mti->flags == kType_Pointer)
		{
			file << "\t" << fn << "* Get" << fi->name << "() const { return m_" << fi->name << "; }" << std::endl;
			file << "\tvoid Set" << fi->name << "(" << fn << "* value) { m_" << fi->name << " = value; }" << std::endl;
		}
		else if (mti->flags == kType_Array)
		{
			TypeInfo* ati = *(TypeInfo**)mti->enumFields;
			if (ati->flags == kType_Pointer)
				file << "\tArray<" << GetFixedClassName(ati->name) << "*>& Get" << fi->name << "() { return m_" << fi->name << "; }" << std::endl;
			else
				file << "\tArray<" << GetFixedClassName(ati->name) << ">& Get" << fi->name << "() { return m_" << fi->name << "; }" << std::endl;
		}
		else if (!strcmp(fn, "const char*"))
		{
			file << "\tconst char* Get" << fi->name << "() const { return m_" << fi->name << "; }" << std::endl;
			file << "\tvoid Set" << fi->name << "(const char* value) { m_" << fi->name << " = value; }" << std::endl;
		}
		else if (!strcmp(fn, "bool") || !strcmp(fn, "float") || !strcmp(fn, "double") ||
			!strcmp(fn, "int") || !strcmp(fn, "short") || !strcmp(fn, "long") || !strcmp(fn, "char") ||
			!strcmp(fn, "unsigned int") || !strcmp(fn, "unsigned short") || !strcmp(fn, "unsigned long") || !strcmp(fn, "unsigned char"))
		{
			file << "\t" << fn << " Get" << fi->name << "() const { return m_" << fi->name << "; }" << std::endl;
			file << "\tvoid Set" << fi->name << "(" << fn << " value) { m_" << fi->name << " = value; }" << std::endl;
		}
		else
		{
			file << "\t" << fn << "& Get" << fi->name << "() { return m_" << fi->name << "; }" << std::endl;
			file << "\tconst " << fn << "& Get" << fi->name << "() const { return m_" << fi->name << "; }" << std::endl;
			file << "\tvoid Set" << fi->name << "(const " << fn << "& value) { m_" << fi->name << " = value; }" << std::endl;
		}
	}
}

// ============================================================================
// Template Class Dumping (with new features)
// ============================================================================

void ClassInfoManager::DumpTemplateClass(ClassInfo* c)
{
	TypeInfo* ti = c->typeInfo;
	if (!ti || strlen(ti->name) == 0) return;

	std::string sanitizedName = GetSanitizedClassName(c->typeInfo->name);
	char headerFile[128], headerPath[MAX_PATH];
	sprintf(headerFile, "SDK\\%s.h", sanitizedName.c_str());
	GetDirFile(headerFile, headerPath, sizeof(headerPath));

	std::ofstream file;
	file.open(headerPath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) return;

	DumpHeader(file, headerFile);
	m_sdkFileNames.push_back(sanitizedName);
	m_forwardDecls.insert(std::string("class ") + c->typeInfo->name);

	file << std::endl << "#ifndef FBGEN_" << sanitizedName << "_H" << std::endl;
	file << "#define FBGEN_" << sanitizedName << "_H" << std::endl << std::endl;
	file << "#include \"FBSDKTypes.h\"" << std::endl;

	std::vector<FieldInfo*> members;
	ParseClassMembers(ti, members);
	ResolveHeaders(members, file);

	file << "class " << ti->name << std::endl;
	file << "{" << std::endl;
	file << "public:" << std::endl;

	DumpTypeInfo(c, file);
	DumpOffsetConstants(file, members);

	int totalSizeOfClass = ti->totalSize;
	int memberSize = DumpClassMembers(file, members, 0);
	if (memberSize < totalSizeOfClass)
		file << "\tunsigned char _0x" << std::hex << memberSize << "[0x" << std::hex << (totalSizeOfClass - memberSize) << "];" << std::endl;

	file << std::endl;
	DumpGetterSetters(file, members, ti);

	file << "}; // size = 0x" << std::hex << totalSizeOfClass << std::endl << std::endl;
	file << "#endif // FBGEN_" << c->typeInfo->name << "_H" << std::endl;

	file.close();
}

// ============================================================================
// Header Boilerplate
// ============================================================================

void ClassInfoManager::DumpHeader(std::ofstream& file, const char* fileName)
{
	std::time_t result = std::time(0);
	file << "//" << std::endl;
	file << "// Generated with FrostbiteGen" << std::endl;
	file << "// Extended with Memory Address Resolution v2" << std::endl;
	file << "// File: " << fileName << std::endl;
	file << "// Created: " << std::asctime(std::localtime(&result)) << "//" << std::endl;
}

// ============================================================================
// P3: Cross-Reference Map
// ============================================================================

void ClassInfoManager::BuildCrossRefMap()
{
	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		if (!c || !c->typeInfo) continue;
		TypeInfo* ti = c->typeInfo;
		if (!ti->fields || ti->fieldCount <= 0 || ti->fieldCount > 10000) continue;
		std::string ownerName = ti->name ? ti->name : "";

		// Walk class fields
		for (int i = 0; i < ti->fieldCount; ++i)
		{
			FieldInfo fiCopy;
			if (!SafeReadBytes((uintptr_t)&ti->fields[i], &fiCopy, sizeof(FieldInfo))) continue;

			if (!fiCopy.typeInfo) continue;
			
			MemberTypeInfo mtiCopy;
			if (!SafeReadBytes((uintptr_t)fiCopy.typeInfo, &mtiCopy, sizeof(MemberTypeInfo))) continue;

			if (!mtiCopy.typeInfo) continue;

			TypeInfo fieldTypeCopy;
			if (!SafeReadBytes((uintptr_t)mtiCopy.typeInfo, &fieldTypeCopy, sizeof(TypeInfo))) continue;

			if (fieldTypeCopy.flags == kType_Pointer && fieldTypeCopy.name)
			{
				char targetNameBuf[128] = { 0 };
				char fieldNameBuf[128] = { 0 };
				
				if (!SafeReadString((uintptr_t)fieldTypeCopy.name, targetNameBuf, sizeof(targetNameBuf))) continue;
				if (fiCopy.name && !SafeReadString((uintptr_t)fiCopy.name, fieldNameBuf, sizeof(fieldNameBuf))) continue;

				std::string targetName = targetNameBuf;
				std::string fieldName = fiCopy.name ? fieldNameBuf : "unknown";
				m_crossRefMap[targetName].push_back({ ownerName, fieldName });
			}
		}
	}
	Log("Cross-reference map: %d target types", (int)m_crossRefMap.size());
}

void ClassInfoManager::GenerateCrossRefFile()
{
	char filePath[MAX_PATH];
	GetDirFile("SDK\\CrossReferences.h", filePath, sizeof(filePath));

	std::ofstream file;
	file.open(filePath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) { Log("Failed to open CrossReferences.h"); return; }

	std::time_t result = std::time(0);
	file << "//" << std::endl;
	file << "// FrostbiteGen SDK ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â Cross-Reference Map" << std::endl;
	file << "// Shows which classes hold pointers to each type." << std::endl;
	file << "// Created: " << std::asctime(std::localtime(&result)) << "//" << std::endl << std::endl;

	for (auto& pair : m_crossRefMap)
	{
		file << "// " << pair.first << " is referenced by:" << std::endl;
		for (auto& ref : pair.second)
			file << "//   " << ref.first << "::" << ref.second << std::endl;
		file << "//" << std::endl;
	}

	file.close();
	Log("Generated CrossReferences.h");
}

// ============================================================================
// P2: Class Hierarchy Tree
// ============================================================================

void ClassInfoManager::GenerateHierarchyTree()
{
	char filePath[MAX_PATH];
	GetDirFile("SDK\\ClassHierarchy.h", filePath, sizeof(filePath));

	std::ofstream file;
	file.open(filePath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) { Log("Failed to open ClassHierarchy.h"); return; }

	std::time_t result = std::time(0);
	file << "//" << std::endl;
	file << "// FrostbiteGen SDK ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â Class Hierarchy" << std::endl;
	file << "// Full inheritance tree of all dumped classes." << std::endl;
	file << "// Created: " << std::asctime(std::localtime(&result)) << "//" << std::endl << std::endl;

	// Group classes by their root parent
	std::map<std::string, std::vector<std::string>> childMap;
	std::set<std::string> hasParent;

	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		if (!c || !IsValidPointer(c) || !IsValidPointer(c->typeInfo) || !c->typeInfo || !IsValidPointer((void*)c->typeInfo->name) || !c->typeInfo->name) continue;

		std::string name = c->typeInfo->name;
		if (c->parent && IsValidPointer(c->parent) && c->parent != c && IsValidPointer(c->parent->typeInfo) && c->parent->typeInfo && IsValidPointer((void*)c->parent->typeInfo->name) && c->parent->typeInfo->name)
		{
			std::string parentName = c->parent->typeInfo->name;
			childMap[parentName].push_back(name);
			hasParent.insert(name);
		}
	}

	// Print roots (classes with no parent) and their children
	std::set<std::string> visited;
	std::function<void(const std::string&, int)> printTree;
	printTree = [&](const std::string& name, int indent) {
		if (visited.count(name)) return;
		visited.insert(name);
		for (int i = 0; i < indent; i++) file << "  ";
		file << "// " << name;

		auto it = m_classMap.find(name);
		if (it != m_classMap.end() && it->second->typeInfo)
			file << " (size=0x" << std::hex << it->second->typeInfo->totalSize << ")";

		auto travIt = m_traversalMap.find(name);
		if (travIt != m_traversalMap.end())
			file << " [INSTANCE FOUND]";

		file << std::endl;

		auto childIt = childMap.find(name);
		if (childIt != childMap.end())
		{
			auto& children = childIt->second;
			std::sort(children.begin(), children.end());
			for (auto& child : children)
				printTree(child, indent + 1);
		}
	};

	for (auto& pair : m_classMap)
	{
		std::string name = pair.first;
		if (hasParent.count(name) == 0)
			printTree(name, 0);
	}

	file.close();
	Log("Generated ClassHierarchy.h");
}

// ============================================================================
// P2: JSON Schema Export
// ============================================================================

struct SafeFieldData {
	bool valid;
	const char* name;
	const char* typeName;
	int offset;
	int size;
	short flags;
	bool isArrayPointer; // Used for formatting array fields
};

static SafeFieldData GetSafeFieldData(FieldInfo* fi)
{
	SafeFieldData data = { false, nullptr, nullptr, 0, 0, 0, false };
	__try {
		if (!fi || !IsValidPointer(fi)) return data;
		if (!fi->typeInfo || !IsValidPointer(fi->typeInfo)) return data;
		
		TypeInfo* mti = fi->typeInfo->typeInfo;
		if (!mti || !IsValidPointer(mti)) return data;
		
		data.name = (fi->name && IsValidPointer((void*)fi->name)) ? fi->name : "unk";
		data.offset = fi->offset;
		data.size = fi->GetFieldSize();
		data.flags = mti->flags;
		
		if (mti->flags == kType_Array)
		{
			TypeInfo* ati = *(TypeInfo**)mti->enumFields;
			if (ati && IsValidPointer(ati) && ati->name && IsValidPointer((void*)ati->name))
			{
				data.typeName = ati->name;
				data.isArrayPointer = (ati->flags == kType_Pointer);
			}
			else
			{
				data.typeName = "unk";
			}
		}
		else
		{
			if (!mti->name || !IsValidPointer((void*)mti->name)) return data;
			data.typeName = mti->name;
		}
		
		volatile char c1 = data.name[0];
		volatile char c2 = data.typeName[0];
		
		data.valid = true;
	} __except(1) {
		data.valid = false;
	}
	return data;
}

static FieldInfo* GetFieldsArray(TypeInfo* ti)
{
	if (!ti) return nullptr;
	if (ti->flags == 41 || ti->flags == 32809 || ti->flags == 53289 || ti->flags == 69)
		return ti->structFields;
	return ti->fields;
}

void ClassInfoManager::GenerateJSONSchema()
{
	char filePath[MAX_PATH];
	GetDirFile("SDK\\sdk.json", filePath, sizeof(filePath));

	std::ofstream file;
	file.open(filePath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) { Log("Failed to open sdk.json"); return; }

	file << "{" << std::endl;
	file << "  \"generator\": \"FrostbiteGen v2\"," << std::endl;
	file << "  \"moduleBase\": \"0x" << std::hex << m_moduleBase << "\"," << std::endl;
	file << "  \"clientGameContext\": \"0x" << std::hex << m_clientGameCtxGlobalOffset << "\"," << std::endl;
	file << "  \"classes\": {" << std::endl;

	bool firstClass = true;
	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		if (!c || !IsValidPointer(c) || !IsValidPointer(c->typeInfo) || !c->typeInfo || !IsValidPointer((void*)c->typeInfo->name) || !c->typeInfo->name) continue;
		TypeInfo* ti = c->typeInfo;

		if (!firstClass) file << "," << std::endl;
		firstClass = false;

		file << "    \"" << ti->name << "\": {" << std::endl;
		file << "      \"size\": " << std::dec << ti->totalSize << "," << std::endl;
		file << "      \"typeInfoOffset\": \"0x" << std::hex << ((uintptr_t)c - m_moduleBase) << "\"," << std::endl;
		file << "      \"isDataContainer\": " << (c->isDataContainer ? "true" : "false") << "," << std::endl;

		// Parent
		if (c->parent && IsValidPointer(c->parent) && c->parent != c && IsValidPointer(c->parent->typeInfo) && c->parent->typeInfo && IsValidPointer((void*)c->parent->typeInfo->name) && c->parent->typeInfo->name)
			file << "      \"parent\": \"" << c->parent->typeInfo->name << "\"," << std::endl;
		else
			file << "      \"parent\": null," << std::endl;

		// Traversal chain
		auto travIt = m_traversalMap.find(std::string(ti->name));
		if (travIt != m_traversalMap.end() && !travIt->second.steps.empty())
		{
			file << "      \"traversalChain\": [";
			for (size_t i = 0; i < travIt->second.steps.size(); i++)
			{
				if (i > 0) file << ", ";
				file << "{\"offset\": " << std::dec << travIt->second.steps[i].offset
					<< ", \"field\": \"" << travIt->second.steps[i].fieldName << "\"}";
			}
			file << "]," << std::endl;
		}
		else
		{
			file << "      \"traversalChain\": null," << std::endl;
		}

		// VTable
		auto vtIt = m_vtableMap.find(std::string(ti->name));
		if (vtIt != m_vtableMap.end())
			file << "      \"vtableEntries\": " << std::dec << vtIt->second.entryCount << "," << std::endl;
		else
			file << "      \"vtableEntries\": 0," << std::endl;

		if (ti->flags == 49289)
		{
			file << "      \"isEnum\": true," << std::endl;
			file << "      \"members\": [";
			bool firstMember = true;
			if (ti->enumFields && IsValidPointer(ti->enumFields) && ti->fieldCount > 0)
			{
				for (int i = 0; i < ti->fieldCount; ++i)
				{
					FieldInfoEnum* fie = (FieldInfoEnum*)&ti->enumFields[i];
					if (!fie || !IsValidPointer(fie)) continue;
					
					if (!firstMember) file << ", ";
					firstMember = false;
					
					file << "{\"name\": \"" << (fie->name && IsValidPointer(fie->name) ? fie->name : "unk") << "\""
						<< ", \"value\": " << std::dec << fie->value << "}";
				}
			}
			file << "]" << std::endl;
		}
		else
		{
			file << "      \"isEnum\": false," << std::endl;
			// Members
			file << "      \"members\": [";
			bool firstMember = true;

			FieldInfo* targetFields = GetFieldsArray(ti);
			if (targetFields && IsValidPointer(targetFields) && ti->fieldCount > 0)
			{
				for (int i = 0; i < ti->fieldCount; ++i)
				{
					SafeFieldData fd = GetSafeFieldData(&targetFields[i]);
					if (!fd.valid) continue;

					if (!firstMember) file << ", ";
					firstMember = false;

					std::string typeStr = GetFixedClassName(fd.typeName);
					if (fd.flags == kType_Array)
					{
						if (fd.isArrayPointer)
							typeStr = "Array<" + typeStr + "*>";
						else
							typeStr = "Array<" + typeStr + ">";
					}

					file << "{\"name\": \"" << fd.name << "\""
						<< ", \"offset\": " << std::dec << fd.offset
						<< ", \"type\": \"" << typeStr << "\""
						<< ", \"size\": " << std::dec << fd.size
						<< "}";
				}
			}
			file << "]" << std::endl;
		}
		file << "    }";
	}

	file << std::endl << "  }" << std::endl;
	file << "}" << std::endl;

	file.close();
	Log("Generated sdk.json");
}

// ============================================================================
// P3: IDA Pro Script
// ============================================================================

void ClassInfoManager::GenerateIDAScript()
{
	char filePath[MAX_PATH];
	GetDirFile("SDK\\ida_import.py", filePath, sizeof(filePath));

	std::ofstream file;
	file.open(filePath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) { Log("Failed to open ida_import.py"); return; }

	file << "# FrostbiteGen SDK - IDA Pro Import Script" << std::endl;
	file << "# Run this in IDA's Python console to import all SDK types." << std::endl;
	file << "#" << std::endl;
	file << "# Usage: File -> Script File -> select this file" << std::endl;
	file << "#" << std::endl;
	file << "import idaapi, idc, ida_struct, ida_name" << std::endl;
	file << std::endl;

	file << "def create_struct(name, size, fields):" << std::endl;
	file << "    sid = ida_struct.get_struc_id(name)" << std::endl;
	file << "    if sid != idc.BADADDR:" << std::endl;
	file << "        ida_struct.del_struc(ida_struct.get_struc(sid))" << std::endl;
	file << "    sid = idc.add_struc(-1, name, 0)" << std::endl;
	file << "    if sid == idc.BADADDR:" << std::endl;
	file << "        print(f'Failed to create struct {name}')" << std::endl;
	file << "        return" << std::endl;
	file << "    sptr = ida_struct.get_struc(sid)" << std::endl;
	file << "    for fname, foffset, fsize, fflags in fields:" << std::endl;
	file << "        idc.add_struc_member(sid, fname, foffset, fflags, -1, fsize)" << std::endl;
	file << "    # Pad to full size" << std::endl;
	file << "    if ida_struct.get_struc_size(sptr) < size:" << std::endl;
	file << "        idc.add_struc_member(sid, '__pad_end', size - 1, idc.FF_BYTE, -1, 1)" << std::endl;
	file << "    print(f'Created struct {name} (0x{size:X} bytes, {len(fields)} fields)')" << std::endl;
	file << std::endl;

	file << "def create_enum(name, members):" << std::endl;
	file << "    eid = idc.get_enum(name)" << std::endl;
	file << "    if eid != idc.BADADDR:" << std::endl;
	file << "        idc.del_enum(eid)" << std::endl;
	file << "    eid = idc.add_enum(-1, name, 0x1100000) # hex flag" << std::endl;
	file << "    if eid == idc.BADADDR:" << std::endl;
	file << "        print(f'Failed to create enum {name}')" << std::endl;
	file << "        return" << std::endl;
	file << "    for mname, mval in members:" << std::endl;
	file << "        idc.add_enum_member(eid, mname, mval, -1)" << std::endl;
	file << "    print(f'Created enum {name} ({len(members)} members)')" << std::endl;
	file << std::endl;
	
	file << "def main():" << std::endl;

	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		if (!c || !IsValidPointer(c) || !IsValidPointer(c->typeInfo) || !c->typeInfo || !IsValidPointer((void*)c->typeInfo->name) || !c->typeInfo->name) continue;
		TypeInfo* ti = c->typeInfo;

		if (ti->flags == 49289)
		{
			file << "    create_enum('" << ti->name << "', [" << std::endl;
			if (ti->enumFields && IsValidPointer(ti->enumFields) && ti->fieldCount > 0)
			{
				for (int i = 0; i < ti->fieldCount; ++i)
				{
					FieldInfoEnum* fie = (FieldInfoEnum*)&ti->enumFields[i];
					if (!fie || !IsValidPointer(fie)) continue;
					file << "        ('" << (fie->name && IsValidPointer(fie->name) ? fie->name : "unk") << "', " << std::dec << fie->value << ")," << std::endl;
				}
			}
			file << "    ])" << std::endl;
		}
		else
		{
			FieldInfo* targetFields = GetFieldsArray(ti);
			if (ti->fieldCount <= 0 || !targetFields)
				continue;

			file << "    create_struct('" << ti->name << "', 0x" << std::hex << ti->totalSize << ", [" << std::endl;

			if (targetFields && IsValidPointer(targetFields) && ti->fieldCount > 0)
			{
				for (int i = 0; i < ti->fieldCount; ++i)
				{
					SafeFieldData fd = GetSafeFieldData(&targetFields[i]);
					if (!fd.valid) continue;

					const char* fn = GetFixedClassName(fd.typeName);

					// Map to IDA flags
					std::string idaFlags = "idc.FF_BYTE";
					if (!strcmp(fn, "float"))
						idaFlags = "idc.FF_FLOAT";
					else if (!strcmp(fn, "double"))
						idaFlags = "idc.FF_DOUBLE";
					else if (!strcmp(fn, "int") || !strcmp(fn, "unsigned int"))
						idaFlags = "idc.FF_DWORD";
					else if (!strcmp(fn, "short") || !strcmp(fn, "unsigned short"))
						idaFlags = "idc.FF_WORD";
					else if (fd.flags == kType_Pointer || fd.flags == kType_Array || !strcmp(fn, "const char*"))
						idaFlags = "idc.FF_QWORD";
					else if (!strcmp(fn, "bool") || !strcmp(fn, "char") || !strcmp(fn, "unsigned char"))
						idaFlags = "idc.FF_BYTE";

					file << "        ('m_" << fd.name << "', 0x"
						<< std::hex << fd.offset << ", " << std::dec << fd.size << ", " << idaFlags << ")," << std::endl;
				}
			}

			file << "    ])" << std::endl;
		}

		// Label the TypeInfo address
		uintptr_t relAddr = (uintptr_t)c - m_moduleBase;
		file << "    ida_name.set_name(idaapi.get_imagebase() + 0x" << std::hex << relAddr
			<< ", 'g_" << ti->name << "_TypeInfo', ida_name.SN_NOCHECK)" << std::endl;
		
		auto vtIt = m_vtableMap.find(std::string(ti->name));
		if (vtIt != m_vtableMap.end() && vtIt->second.entryCount > 0)
		{
			for (int v = 0; v < vtIt->second.entryCount; ++v)
			{
				uintptr_t vfuncRva = vtIt->second.entries[v];
				if (vfuncRva > 0)
				{
					file << "    ida_name.set_name(idaapi.get_imagebase() + 0x" << std::hex << vfuncRva
						<< ", '" << ti->name << "_vfunc_" << std::dec << v << "', ida_name.SN_NOCHECK)" << std::endl;
				}
			}
		}
		
		file << std::endl;
	}

	file << "    print('SDK import complete!')" << std::endl;
	file << std::endl;
	file << "main()" << std::endl;

	file.close();
	Log("Generated ida_import.py");
}

// ============================================================================
// P3: Ghidra Script
// ============================================================================

void ClassInfoManager::GenerateGhidraScript()
{
	char filePath[MAX_PATH];
	GetDirFile("SDK\\ghidra_import.py", filePath, sizeof(filePath));

	std::ofstream file;
	file.open(filePath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) { Log("Failed to open ghidra_import.py"); return; }

	file << "# FrostbiteGen SDK Ghidra Import Script" << std::endl;
	file << "# Run via Ghidra's Script Manager (Python)" << std::endl;
	file << "#" << std::endl;
	file << "# @category FrostbiteGen" << std::endl;
	file << "# @author FrostbiteGen" << std::endl;
	file << std::endl;
	file << "from ghidra.program.model.data import *" << std::endl;
	file << "from ghidra.app.cmd.data import CreateStructureCmd" << std::endl;
	file << std::endl;

	file << "dtm = currentProgram.getDataTypeManager()" << std::endl;
	file << "cat = CategoryPath('/FrostbiteSDK')" << std::endl;
	file << std::endl;

	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		if (!c || !IsValidPointer(c) || !IsValidPointer(c->typeInfo) || !c->typeInfo || !IsValidPointer((void*)c->typeInfo->name) || !c->typeInfo->name) continue;
		TypeInfo* ti = c->typeInfo;
		if (ti->flags == 49289)
		{
			file << "# Enum " << ti->name << std::endl;
			file << "e = EnumDataType(cat, '" << ti->name << "', " << std::dec << (ti->totalSize > 0 ? ti->totalSize : 4) << ")" << std::endl;
			if (ti->enumFields && IsValidPointer(ti->enumFields) && ti->fieldCount > 0)
			{
				for (int i = 0; i < ti->fieldCount; ++i)
				{
					FieldInfoEnum* fie = (FieldInfoEnum*)&ti->enumFields[i];
					if (!fie || !IsValidPointer(fie)) continue;
					file << "e.add('" << (fie->name && IsValidPointer(fie->name) ? fie->name : "unk") << "', " << std::dec << fie->value << ")" << std::endl;
				}
			}
			file << "dtm.addDataType(e, DataTypeConflictHandler.REPLACE_HANDLER)" << std::endl;
			file << std::endl;
		}
		else
		{
			FieldInfo* targetFields = GetFieldsArray(ti);
			if (ti->fieldCount <= 0 || !targetFields) continue;

			file << "# " << ti->name << " (0x" << std::hex << ti->totalSize << " bytes)" << std::endl;
			file << "s = StructureDataType(cat, '" << ti->name << "', 0x" << std::hex << ti->totalSize << ")" << std::endl;

			if (targetFields && IsValidPointer(targetFields) && ti->fieldCount > 0)
			{
				for (int i = 0; i < ti->fieldCount; ++i)
				{
					SafeFieldData fd = GetSafeFieldData(&targetFields[i]);
					if (!fd.valid) continue;

					const char* fn = GetFixedClassName(fd.typeName);

					std::string ghidraType = "ByteDataType.dataType";
					if (!strcmp(fn, "float")) ghidraType = "FloatDataType.dataType";
					else if (!strcmp(fn, "double")) ghidraType = "DoubleDataType.dataType";
					else if (!strcmp(fn, "int")) ghidraType = "IntegerDataType.dataType";
					else if (!strcmp(fn, "unsigned int")) ghidraType = "UnsignedIntegerDataType.dataType";
					else if (!strcmp(fn, "short")) ghidraType = "ShortDataType.dataType";
					else if (!strcmp(fn, "bool")) ghidraType = "BooleanDataType.dataType";
					else if (fd.flags == kType_Pointer || fd.flags == kType_Array || !strcmp(fn, "const char*"))
						ghidraType = "Pointer64DataType.dataType";

					file << "s.replaceAtOffset(0x" << std::hex << fd.offset << ", " << ghidraType
						<< ", " << std::dec << fd.size << ", 'm_" << fd.name << "', '')" << std::endl;
				}
			}

			file << "dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER)" << std::endl;
			file << std::endl;
		}
	}

	file << "from ghidra.program.model.symbol import SourceType" << std::endl;
	file << "base_addr = currentProgram.getImageBase()" << std::endl;
	file << "sym_table = currentProgram.getSymbolTable()" << std::endl;
	file << std::endl;
	
	for (auto& pair : m_globalInstances)
	{
		uintptr_t offset = pair.second; // m_globalInstances already stores the offset relative to m_moduleBase!
		std::string name = GetSanitizedClassName(pair.first.c_str()) + "_Singleton";
		file << "addr = base_addr.add(0x" << std::hex << offset << ")" << std::endl;
		file << "sym_table.createLabel(addr, '" << name << "', SourceType.USER_DEFINED)" << std::endl;
	}
	
	file << "print('Applying VTable function labels...')" << std::endl;
	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		if (!c || !IsValidPointer(c) || !IsValidPointer(c->typeInfo) || !c->typeInfo || !IsValidPointer((void*)c->typeInfo->name) || !c->typeInfo->name) continue;
		
		auto vtIt = m_vtableMap.find(std::string(c->typeInfo->name));
		if (vtIt != m_vtableMap.end() && vtIt->second.entryCount > 0)
		{
			for (int v = 0; v < vtIt->second.entryCount; ++v)
			{
				uintptr_t vfuncRva = vtIt->second.entries[v];
				if (vfuncRva > 0)
				{
					file << "addr = base_addr.add(0x" << std::hex << vfuncRva << ")" << std::endl;
					file << "sym_table.createLabel(addr, '" << c->typeInfo->name << "_vfunc_" << std::dec << v << "', SourceType.USER_DEFINED)" << std::endl;
				}
			}
		}
	}
	
	file << "println('FrostbiteGen SDK imported: ' + str(" << std::dec << m_classMap.size() << ") + ' types, ' + str(" << m_globalInstances.size() << ") + ' singletons labeled')" << std::endl;

	file.close();
	Log("Generated ghidra_import.py");
}

// ============================================================================
// Utility Header Generation (FBSDKTypes.h ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â unchanged from v1)
// ============================================================================

void ClassInfoManager::GenerateFBSDKTypes()
{
	char headerPath[MAX_PATH];
	GetDirFile("SDK\\FBSDKTypes.h", headerPath, sizeof(headerPath));

	std::ofstream file;
	file.open(headerPath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) return;

	std::time_t result = std::time(0);
	file << "//" << std::endl;
	file << "// FrostbiteGen SDK Runtime Utilities v2" << std::endl;
	file << "// Auto-generated" << std::endl;
	file << "// Created: " << std::asctime(std::localtime(&result)) << "//" << std::endl << std::endl;
	file << "#pragma once" << std::endl << std::endl;
	file << "#include <cstdint>" << std::endl;
	file << "#include <cstring>" << std::endl;
	file << "#include <initializer_list>" << std::endl;
	file << "#include <Windows.h>" << std::endl << std::endl;

	file << "namespace fb {" << std::endl << std::endl;

	file << "inline uintptr_t GetModuleBase() {" << std::endl;
	file << "\tstatic uintptr_t base = (uintptr_t)GetModuleHandle(NULL);" << std::endl;
	file << "\treturn base;" << std::endl;
	file << "}" << std::endl << std::endl;

	file << "template <typename T>" << std::endl;
	file << "inline T Read(uintptr_t address) {" << std::endl;
	file << "\tT buffer{};" << std::endl;
	file << "\t__try { memcpy(&buffer, (void*)address, sizeof(T)); } __except(EXCEPTION_EXECUTE_HANDLER) {}" << std::endl;
	file << "\treturn buffer;" << std::endl;
	file << "}" << std::endl << std::endl;

	file << "template <typename T>" << std::endl;
	file << "class Array {" << std::endl;
	file << "private:" << std::endl;
	file << "\tT* m_firstElement;" << std::endl;
	file << "\tT* m_lastElement;" << std::endl;
	file << "\tT* m_arrayBound;" << std::endl;
	file << "\tvoid* m_allocator;" << std::endl;
	file << "public:" << std::endl;
	file << "\tT& At(uint32_t i) { return m_firstElement[i]; }" << std::endl;
	file << "\tT& operator[](uint32_t i) { return m_firstElement[i]; }" << std::endl;
	file << "\tT* begin() { return m_firstElement; }" << std::endl;
	file << "\tT* end() { return m_lastElement; }" << std::endl;
	file << "\tuint32_t Count() const { return (uint32_t)(m_lastElement - m_firstElement); }" << std::endl;
	file << "};" << std::endl << std::endl;

	file << "template <typename T>" << std::endl;
	file << "class WeakPtr {" << std::endl;
	file << "private:" << std::endl;
	file << "\tT** m_ptr;" << std::endl;
	file << "public:" << std::endl;
	file << "\tT* Get() const { return m_ptr ? *m_ptr : nullptr; }" << std::endl;
	file << "\tT* operator->() const { return Get(); }" << std::endl;
	file << "\tbool IsValid() const { return m_ptr && *m_ptr; }" << std::endl;
	file << "};" << std::endl << std::endl;
	
	file << "class String {" << std::endl;
	file << "private:" << std::endl;
	file << "\tchar* m_str;" << std::endl;
	file << "public:" << std::endl;
	file << "\tconst char* c_str() const { return m_str ? m_str : \"\"; }" << std::endl;
	file << "};" << std::endl << std::endl;

	file << "inline bool IsValidPtr(void* ptr) {" << std::endl;
	file << "\treturn ptr != nullptr && (uintptr_t)ptr > 0x10000 && (uintptr_t)ptr < 0x00007FFFFFFFFFFF;" << std::endl;
	file << "}" << std::endl << std::endl;

	file << "template<typename T> inline void Write(uintptr_t address, T value) {" << std::endl;
	file << "\tif (!IsValidPtr((void*)address)) return;" << std::endl;
	file << "\t__try { *reinterpret_cast<T*>(address) = value; }" << std::endl;
	file << "\t__except(1) {}" << std::endl;
	file << "}" << std::endl << std::endl;

	file << "template<typename T> inline T ReadPtr(uintptr_t address) {" << std::endl;
	file << "\treturn Read<T>(address);" << std::endl;
	file << "}" << std::endl << std::endl;

	file << "}" << std::endl;
}


void ClassInfoManager::GenerateForwardDeclarations()
{
	char headerPath[MAX_PATH];
	GetDirFile("SDK\\FBClasses.h", headerPath, sizeof(headerPath));

	std::ofstream file;
	file.open(headerPath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) return;

	file << "#pragma once" << std::endl << std::endl;
	for (auto& decl : m_forwardDecls)
		file << decl << ";" << std::endl;
	file << std::endl;

	file.close();
	Log("Generated FBClasses.h (%d declarations)", (int)m_forwardDecls.size());
}

void ClassInfoManager::GenerateSDKMasterHeader()
{
	char headerPath[MAX_PATH];
	GetDirFile("SDK\\SDK.h", headerPath, sizeof(headerPath));

	std::ofstream file;
	file.open(headerPath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) return;

	file << "#pragma once" << std::endl << std::endl;
	file << "#include \"FBSDKTypes.h\"" << std::endl;
	file << "#include \"FBClasses.h\"" << std::endl << std::endl;

	std::vector<std::string> sorted(m_sdkFileNames.begin(), m_sdkFileNames.end());
	std::sort(sorted.begin(), sorted.end());
	for (auto& name : sorted)
		file << "#include \"" << name << ".h\"" << std::endl;

	file << std::endl;
	file.close();
	Log("Generated SDK.h (%d includes)", (int)m_sdkFileNames.size());
}
void ClassInfoManager::ScanGlobalsForSingletons()
{
    Log("=== Phase 1.5: Scanning for global singletons ===");
    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)m_moduleBase;
    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)(m_moduleBase + dosHeader->e_lfanew);
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(ntHeaders);

    int found = 0;
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++, section++)
    {
        bool isData = (section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) ||
                      (section->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA);
        if (!isData) continue;

        uintptr_t start = m_moduleBase + section->VirtualAddress;
        uintptr_t end = start + section->Misc.VirtualSize;

        for (uintptr_t ptr = start; ptr < end - 8; ptr += 8)
        {
            void* obj = SafeReadPointer(ptr);
            if (obj && !IsModulePointer(obj, m_moduleBase, m_moduleEnd))
            {
                ClassInfo* id = TryIdentifyClassByVTable((uintptr_t)obj, false);
                if (id && id->typeInfo && id->typeInfo->name)
                {
                    std::string name = id->typeInfo->name;
                    if (m_globalInstances.find(name) == m_globalInstances.end())
                    {
                        m_globalInstances[name] = ptr - m_moduleBase;
                        Log("  Found global singleton: %s at Module+0x%llX", name.c_str(), ptr - m_moduleBase);
                        found++;
                        
                        // Add to traversal map as a root!
                        if (m_traversalMap.find(name) == m_traversalMap.end())
                        {
                            TraversalChain rootChain;
                            rootChain.targetClass = name;
                            rootChain.resolvedAddress = (uintptr_t)obj;
                            m_traversalMap[name] = rootChain;
                            
                            std::vector<TraversalStep> chain;
                            std::set<uintptr_t> visited;
                            visited.insert((uintptr_t)obj);
                            BuildTraversalMap((uintptr_t)obj, id, 1, chain, visited);
                        }
                    }
                }
            }
        }
    }
    Log("Found %d global singletons", found);
}
std::string ClassInfoManager::GetSanitizedClassName(const char* name)
{
	std::string s = name;
	for (char& c : s)
	{
		if (c == '<' || c == '>' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '/' || c == '|' || c == '\\')
			c = '_';
	}
	return s;
}


inline bool LocalIsValidPtr(void* ptr) {
	return ptr != nullptr && (uintptr_t)ptr > 0x10000 && (uintptr_t)ptr < 0x00007FFFFFFFFFFF;
}
void ClassInfoManager::DumpLiveInstances()
{
	char filePath[MAX_PATH];
	GetDirFile("SDK\\LiveDump.json", filePath, sizeof(filePath));

	std::ofstream file;
	file.open(filePath, std::ios::out | std::ios::trunc);
	if (!file.is_open()) { Log("Failed to open LiveDump.json"); return; }

	file << "{" << std::endl;
	
	bool firstSingleton = true;
	for (auto& pair : m_globalInstances)
	{
		uintptr_t addr = pair.second + m_moduleBase; // pair.second is an offset!
		if (!LocalIsValidPtr((void*)addr)) continue;
		
		uintptr_t instance = 0;
		if (!SafeReadBytes(addr, &instance, sizeof(uintptr_t))) continue;
		if (!LocalIsValidPtr((void*)instance)) continue;
		
		if (!firstSingleton) file << "," << std::endl;
		file << "  \"" << pair.first << "\": \"0x" << std::hex << instance << "\"";
		firstSingleton = false;
	}
	
	file << std::endl << "}" << std::endl;
	file.close();
	Log("Generated LiveDump.json");
}
