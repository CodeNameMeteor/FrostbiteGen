#include "classinfo.h"

// ============================================================================
// Constructor
// ============================================================================

ClassInfoManager::ClassInfoManager(ClassInfo* info) :
	m_listHead(info),
	m_moduleBase((uintptr_t)GetModuleHandle(NULL)),
	m_moduleEnd(0),
	m_clientGameCtxInstance(0),
	m_clientGameCtxGlobalOffset(0),
	m_rootIsStatic(false),
	m_rootVerified(false),
	m_cvarHeaderWritten(false),
	m_filesWritten(0),
	m_filesFailed(0),
	m_typesFailed(0),
	m_loggedArraySize(false)
{
	size_t moduleSize;
	GetGameModuleInfo(m_moduleBase, moduleSize);
	m_moduleEnd = m_moduleBase + moduleSize;
}

std::string ClassInfoManager::GetSummary() const
{
	char buf[256];
	snprintf(buf, sizeof(buf), "%d files written, %d files failed, %d types skipped after a fault",
		m_filesWritten, m_filesFailed, m_typesFailed);
	return buf;
}

// ============================================================================
// Sanitization and Escaping Helpers
// ============================================================================

bool ClassInfoManager::IsCppKeyword(const std::string& s)
{
	// C++ keywords plus names that would clash with the SDK's own declarations
	static const std::set<std::string> keywords = {
		"alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
		"atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
		"char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const",
		"consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
		"co_return", "co_yield", "decltype", "default", "delete", "do", "double",
		"dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
		"for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
		"new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
		"private", "protected", "public", "reflexpr", "register", "reinterpret_cast",
		"requires", "return", "short", "signed", "sizeof", "static", "static_assert",
		"static_cast", "struct", "switch", "synchronized", "template", "this",
		"thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
		"union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
		"while", "xor", "xor_eq",
		"int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
		"uintptr_t", "intptr_t", "size_t", "fb"
	};
	return keywords.count(s) != 0;
}

std::string ClassInfoManager::SanitizeMemberName(const std::string& orig)
{
	if (orig.empty()) return "unk";

	std::string s = orig;
	for (char& c : s)
	{
		if (!isalnum((unsigned char)c) && c != '_')
			c = '_';
	}
	if (!s.empty() && isdigit((unsigned char)s[0]))
		s = "_" + s;

	if (IsCppKeyword(s))
		s += "_";

	return s;
}

bool ClassInfoManager::IsValidTypeName(const std::string& s)
{
	// Type names are written verbatim into C++ declarations, #includes and scripts,
	// so only plain identifiers are accepted.
	return IsIdentifierChars(s) && !IsCppKeyword(s);
}

std::string ClassInfoManager::PyStr(const std::string& s)
{
	// Single-quoted Python literal; engine strings are never trusted to be quote-free.
	std::string out = "'";
	for (char c : s)
	{
		unsigned char uc = (unsigned char)c;
		if (c == '\\') out += "\\\\";
		else if (c == '\'') out += "\\'";
		else if (uc < 0x20 || uc >= 0x7F)
		{
			char buf[8];
			snprintf(buf, sizeof(buf), "\\x%02x", uc);
			out += buf;
		}
		else out += c;
	}
	out += "'";
	return out;
}

std::string ClassInfoManager::CommentSafe(const std::string& s)
{
	// A trailing backslash would splice the next generated line into a // comment.
	std::string out = s;
	for (char& c : out)
	{
		if (c == '\\') c = '/';
		else if (c == '\r' || c == '\n') c = ' ';
	}
	return out;
}

std::string ClassInfoManager::FormatFloatLiteral(double v, bool isFloat)
{
	if (std::isnan(v))
		return isFloat ? "std::numeric_limits<float>::quiet_NaN()" : "std::numeric_limits<double>::quiet_NaN()";
	if (std::isinf(v))
		return std::string(v < 0 ? "-" : "") + (isFloat ? "std::numeric_limits<float>::infinity()" : "std::numeric_limits<double>::infinity()");

	char buf[64];
	snprintf(buf, sizeof(buf), isFloat ? "%.9g" : "%.17g", v);
	std::string s = buf;
	// "1" or "60" are not floating literals; "1.0f" is.
	if (s.find_first_of(".eE") == std::string::npos)
		s += ".0";
	if (isFloat)
		s += "f";
	return s;
}

const char* ClassInfoManager::MapPrimitiveName(const char* engineName)
{
	if (!engineName) return nullptr;
	if (!strcmp(engineName, "Boolean"))   return "bool";
	if (!strcmp(engineName, "Float32"))   return "float";
	if (!strcmp(engineName, "Float64"))   return "double";
	if (!strcmp(engineName, "Int8"))      return "int8_t";
	if (!strcmp(engineName, "Int16"))     return "int16_t";
	if (!strcmp(engineName, "Int32"))     return "int32_t";
	if (!strcmp(engineName, "Int64"))     return "int64_t";
	if (!strcmp(engineName, "Uint8"))     return "uint8_t";
	if (!strcmp(engineName, "Uint16"))    return "uint16_t";
	if (!strcmp(engineName, "Uint32"))    return "uint32_t";
	if (!strcmp(engineName, "Uint64"))    return "uint64_t";
	if (!strcmp(engineName, "CString"))   return "const char*";
	return nullptr;
}

TypeKind ClassInfoManager::GetTypeKind(unsigned short flags)
{
	// TODO: decode the type-code bits (MemberInfoFlags::kTypeCodeShift/Mask) instead of
	// matching whole flag values; these values were collected from Mirror's Edge Catalyst.
	switch (flags)
	{
	case 49289:
		return TypeKind::Enum;
	case 41: case 32809: case 53289: case 69:
		return TypeKind::Struct;
	case 13: case 49357: case 49453: case 49421: case 49389: case 16493:
	case 49517: case 16765: case 49341: case 49437: case 49405: case 49373: case 49501:
	case 49485: case 49469: case 16541: case 16509: case 49325:
		return TypeKind::Primitive;
	case 29:
		return TypeKind::Skip;
	default:
		return TypeKind::Class;
	}
}

std::string ClassInfoManager::ReadTypeName(TypeInfo* ti)
{
	if (!IsValidPointer(ti)) return "";
	void* namePtr = SafeReadPointer((uintptr_t)&ti->name);
	char buf[256] = { 0 };
	if (!namePtr || !SafeReadString((uintptr_t)namePtr, buf, sizeof(buf)))
		return "";
	return buf;
}

bool ClassInfoManager::OpenOutput(std::ofstream& file, const std::string& relPath)
{
	std::string fullPath = std::string(g_szBaseDir) + relPath;
	if (fullPath.size() >= MAX_PATH)
	{
		Log("ERROR: output path too long, skipped: %s", relPath.c_str());
		m_filesFailed++;
		return false;
	}

	// Windows file names are case-insensitive and some names are reserved devices.
	std::string stem = relPath.substr(relPath.find_last_of("\\/") + 1);
	stem = stem.substr(0, stem.find('.'));
	std::string upperStem = stem;
	std::transform(upperStem.begin(), upperStem.end(), upperStem.begin(), ::toupper);
	static const std::set<std::string> reserved = {
		"CON", "PRN", "AUX", "NUL",
		"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
		"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
	};
	if (reserved.count(upperStem))
	{
		Log("ERROR: '%s' is a reserved Windows device name, skipped", relPath.c_str());
		m_filesFailed++;
		return false;
	}

	std::string lowerPath = relPath;
	std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
	if (!m_writtenPaths.insert(lowerPath).second)
	{
		Log("ERROR: '%s' collides with another output file (names differ only by case), skipped", relPath.c_str());
		m_filesFailed++;
		return false;
	}

	file.open(fullPath, std::ios::out | std::ios::trunc);
	if (!file.is_open())
	{
		Log("ERROR: could not open %s for writing", fullPath.c_str());
		m_filesFailed++;
		return false;
	}
	m_filesWritten++;
	return true;
}

bool ClassInfoManager::GuardedDump(DumpFn fn, ClassInfo* c)
{
	__try
	{
		(this->*fn)(c);
		return true;
	}
	__except (FBGEN_AV_FILTER)
	{
		return false;
	}
}

std::string ClassInfoManager::EscapeXml(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 16);
	for (char c : s)
	{
		switch (c)
		{
		case '&':  out += "&amp;";  break;
		case '\"': out += "&quot;"; break;
		case '\'': out += "&apos;"; break;
		case '<':  out += "&lt;";   break;
		case '>':  out += "&gt;";   break;
		default:   out += c;        break;
		}
	}
	return out;
}

std::string ClassInfoManager::EscapeJson(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 16);
	for (char c : s)
	{
		switch (c)
		{
		case '\"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b";  break;
		case '\f': out += "\\f";  break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if ((unsigned char)c < 0x20)
			{
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
				out += buf;
			}
			else
			{
				out += c;
			}
			break;
		}
	}
	return out;
}

// ============================================================================
// Class List & Dump Entry Points
// ============================================================================

void ClassInfoManager::BuildClassList()
{
	ClassInfo* c = m_listHead;
	std::set<ClassInfo*> visited;
	while (c != NULL && IsValidPointer(c))
	{
		if (visited.count(c))
			break; // cycle detected
		visited.insert(c);
		if (IsValidPointer(c->typeInfo) && IsValidPointer((void*)c->typeInfo->name))
		{
			char nameBuf[256] = { 0 };
			if (SafeReadString((uintptr_t)c->typeInfo->name, nameBuf, sizeof(nameBuf)) && nameBuf[0] != 0)
			{
				m_classMap[nameBuf] = c;
				// Build reverse map: TypeInfo address AND ClassInfo address -> ClassInfo
				m_typeInfoToClassMap[(uintptr_t)c->typeInfo] = c;
				m_typeInfoToClassMap[(uintptr_t)c] = c;
			}
		}
		c = (ClassInfo*)SafeReadPointer((uintptr_t)&c->next);
	}
	Log("BuildClassList: %d classes, %d TypeInfo mappings",
		(int)m_classMap.size(), (int)m_typeInfoToClassMap.size());
}

void ClassInfoManager::BuildDeclaredTypes()
{
	// Every type that will get its own header, decided up front so that member
	// declarations only ever reference types that actually exist in the SDK.
	// Names of the SDK's own files; a type with one of these names would overwrite them.
	static const std::set<std::string> reservedFileNames = {
		"fbsdktypes", "fbclasses", "sdk", "consolevariables", "crossreferences", "classhierarchy"
	};

	m_declaredTypes.clear();
	for (auto& pair : m_classMap)
	{
		const std::string& name = pair.first;
		if (!IsValidTypeName(name))
			continue;
		std::string lowerName = name;
		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
		if (reservedFileNames.count(lowerName))
		{
			Log("Skipping type %s: its header name is reserved for an SDK file", name.c_str());
			continue;
		}
		TypeInfo* ti = (TypeInfo*)SafeReadPointer((uintptr_t)&pair.second->typeInfo);
		if (!ti)
			continue;
		switch (GetTypeKind(SafeReadUInt16((uintptr_t)&ti->flags)))
		{
		case TypeKind::Enum:   m_declaredTypes[name] = "enum " + name + " : " + GetEnumUnderlyingType(ti); break;
		case TypeKind::Struct: m_declaredTypes[name] = "struct " + name; break;
		case TypeKind::Class:  m_declaredTypes[name] = "class " + name; break;
		default: break;
		}
	}
}

void ClassInfoManager::DumpClasses()
{
	BuildDeclaredTypes();
	Log("Types that will be emitted: %d", (int)m_declaredTypes.size());

	// Phase 0: Find ClientGameContext via pattern scan
	Log("=== Phase 0: Finding ClientGameContext ===");
	m_clientGameCtxInstance = FindClientGameContext();

	// Phase 1: Build traversal map from the context root
	if (m_clientGameCtxInstance)
	{
		Log("=== Phase 1: Building traversal map ===");

		// Only trust the root's class if its vtable confirms it. Walking an object with a
		// guessed class's field layout bakes wrong chains into every GetInstance().
		ClassInfo* contextCI = TryIdentifyClassByVTable(m_clientGameCtxInstance, true);
		std::string rootName = contextCI ? ReadTypeName(contextCI->typeInfo) : "";

		// Drop root entries FindClientGameContext labelled by guess; the verified one is re-added below.
		for (auto it = m_traversalMap.begin(); it != m_traversalMap.end(); )
		{
			if (it->second.resolvedAddress == m_clientGameCtxInstance && it->second.steps.empty() && it->first != rootName)
				it = m_traversalMap.erase(it);
			else
				++it;
		}

		std::vector<TraversalStep> rootChain;
		std::set<uintptr_t> visited;
		visited.insert(m_clientGameCtxInstance);

		if (!rootName.empty())
		{
			m_rootVerified = true;
			Log("  Root context class (verified by vtable): %s", rootName.c_str());

			TraversalChain chain;
			chain.targetClass = rootName;
			chain.rootClassName = rootName;
			chain.rootGlobalOffset = m_clientGameCtxGlobalOffset;
			chain.rootIsStatic = m_rootIsStatic;
			chain.resolvedAddress = m_clientGameCtxInstance;
			m_traversalMap[rootName] = chain;
			CaptureVTable(rootName, (void*)m_clientGameCtxInstance, contextCI);

			BuildTraversalMap(m_clientGameCtxInstance, contextCI, 0, rootChain, visited,
				m_clientGameCtxGlobalOffset, rootName, m_rootIsStatic);
		}
		else
		{
			m_rootVerified = false;
			Log("  WARNING: the root object's class could not be confirmed from its vtable; using blind traversal");
			BlindTraversalWalk(m_clientGameCtxInstance, 0, rootChain, visited,
				m_clientGameCtxGlobalOffset, "ClientGameContext", m_rootIsStatic);
		}

		Log("Traversal map built: %d classes reachable", (int)m_traversalMap.size());
	}
	else
	{
		Log("WARNING: Root singleton not found - instance resolution will be limited");
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
	for (auto& it : m_classMap)
	{
		const std::string& name = it.first;
		ClassInfo* c = it.second;
		TypeInfo* ti = c ? (TypeInfo*)SafeReadPointer((uintptr_t)&c->typeInfo) : nullptr;
		if (!ti) continue;

		TypeKind kind = GetTypeKind(SafeReadUInt16((uintptr_t)&ti->flags));
		if (kind == TypeKind::Primitive)
		{
			Log("Found data type: %s, size = %d", name.c_str(), (int)SafeReadUInt16((uintptr_t)&ti->totalSize));
			continue;
		}
		if (kind == TypeKind::Skip)
			continue;
		if (!IsDeclaredType(name))
		{
			Log("Skipping type whose name is not a valid C++ identifier: %s", name.c_str());
			continue;
		}

		DumpFn fn = kind == TypeKind::Enum ? &ClassInfoManager::DumpEnum
			: kind == TypeKind::Struct ? &ClassInfoManager::DumpStruct
			: &ClassInfoManager::DumpClass;
		if (!GuardedDump(fn, c))
		{
			m_typesFailed++;
			Log("ERROR: memory fault while dumping %s; type skipped", name.c_str());
		}
	}

	// Phase 5: Forward declarations
	Log("=== Phase 5: Generating forward declarations ===");
	GenerateForwardDeclarations();

	// Phase 6: Generate bonus outputs
	Log("=== Phase 6: Generating bonus outputs ===");
	GenerateBonusOutputs();

	// Phase 7: Master include, last so it only references headers that were actually written
	Log("=== Phase 7: Generating master header ===");
	GenerateSDKMasterHeader();
}

void ClassInfoManager::GenerateBonusOutputs()
{
	__try { GenerateHierarchyTree(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in GenerateHierarchyTree"); }
	__try { GenerateJSONSchema(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in GenerateJSONSchema"); }
	__try { GenerateIDAScript(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in GenerateIDAScript"); }
	__try { GenerateGhidraScript(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in GenerateGhidraScript"); }
	__try { GenerateCrossRefFile(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in GenerateCrossRefFile"); }
	__try { DumpLiveInstances(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in DumpLiveInstances"); }
	__try { GenerateCheatEngineTable(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in GenerateCheatEngineTable"); }
	__try { DumpConsoleVariables(); } __except(FBGEN_AV_FILTER) { m_typesFailed++; Log("Crash in DumpConsoleVariables"); }
}

// ============================================================================
// P0: ClientGameContext Resolution
// ============================================================================

uintptr_t ClassInfoManager::FindClientGameContext()
{
	char modulePath[MAX_PATH] = { 0 };
	if (!GetModuleFileNameA(GetModuleHandleA(NULL), modulePath, MAX_PATH))
		Log("  WARNING: GetModuleFileNameA failed; known-game offsets will not be used");
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
			// Try to identify it by dereferencing
			ClassInfo* id = TryIdentifyClassByVTable((uintptr_t)explicitPtr, true);
			bool isStatic = false;
			if (!id)
			{
				// Maybe the offset IS the instance (static object)?
				id = TryIdentifyClassByVTable(explicitGlobal, true);
				if (id)
				{
					explicitPtr = (void*)explicitGlobal;
					isStatic = true;
				}
			}

			if (id && id->typeInfo && id->typeInfo->name)
			{
				m_clientGameCtxGlobalOffset = explicitGlobalOffset;
				m_rootIsStatic = isStatic;
				Log("  Found instance at explicit global: 0x%016llX (%s%s)", (uintptr_t)explicitPtr, id->typeInfo->name, isStatic ? ", static object" : "");
				TraversalChain rootChain;
				rootChain.targetClass = id->typeInfo->name;
				rootChain.rootClassName = id->typeInfo->name;
				rootChain.rootGlobalOffset = explicitGlobalOffset;
				rootChain.rootIsStatic = isStatic;
				rootChain.resolvedAddress = (uintptr_t)explicitPtr;
				m_traversalMap[id->typeInfo->name] = rootChain;
				return (uintptr_t)explicitPtr;
			}
			
			// Validate vtable in module before accepting generic instance
			void* vt = SafeReadPointer((uintptr_t)explicitPtr);
			if (vt && IsModulePointer(vt, m_moduleBase, m_moduleEnd))
			{
				m_clientGameCtxGlobalOffset = explicitGlobalOffset;
				Log("  Found instance at explicit global with valid vtable: 0x%016llX", (uintptr_t)explicitPtr);
				TraversalChain rootChain;
				rootChain.targetClass = "ClientGameContext";
				rootChain.rootClassName = "ClientGameContext";
				rootChain.rootGlobalOffset = explicitGlobalOffset;
				rootChain.resolvedAddress = (uintptr_t)explicitPtr;
				m_traversalMap["ClientGameContext"] = rootChain;
				return (uintptr_t)explicitPtr;
			}
			Log("  Explicit pointer 0x%016llX lacks valid module vtable. Falling back to dynamic heuristic scan...", (uintptr_t)explicitPtr);
		}
	}
	else
	{
		Log("  No explicit global offset known for %s. Falling back to dynamic heuristic scan...", exeName.c_str());
	}

	// ----------------------------------------------------------------
	// Step 1: Find the game-context class in our class map.
	//         Try many name variants - the root may not be called
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
		Log("  No exact name match - will use brute-force to find root singleton");
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
				rootChain.rootClassName = contextClassName;
				rootChain.rootGlobalOffset = m_clientGameCtxGlobalOffset;
				rootChain.resolvedAddress = (uintptr_t)objPtr;
				m_traversalMap[contextClassName] = rootChain;

				return (uintptr_t)objPtr;
			}

			scanAddr = match + 1;
		}
	}

	// ----------------------------------------------------------------
	// Step 3: Brute-force - scan the .data section for global pointers
	//         that hold objects with many valid sub-pointers.
	// ----------------------------------------------------------------
	Log("  Pattern scan failed, trying brute-force global pointer scan...");

	// Find .data section
	PIMAGE_DOS_HEADER dosH = (PIMAGE_DOS_HEADER)m_moduleBase;
	PIMAGE_NT_HEADERS ntH = (PIMAGE_NT_HEADERS)(m_moduleBase + dosH->e_lfanew);
	PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(ntH);

	int bestScore = 0;
	uintptr_t bestGlobal = 0;
	void* bestObj = nullptr;

	for (int s = 0; s < ntH->FileHeader.NumberOfSections; s++)
	{
		// Look for writable sections (.data, .bss)
		if (!(sec[s].Characteristics & IMAGE_SCN_MEM_WRITE))
			continue;

		uintptr_t secBase = m_moduleBase + sec[s].VirtualAddress;
		uintptr_t secEnd = secBase + sec[s].Misc.VirtualSize;

		Log("  Scanning section %.8s (0x%llX - 0x%llX)",
			sec[s].Name, secBase, secEnd);

		for (uintptr_t addr = secBase; addr + 8 <= secEnd; addr += 8)
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
	}

	// Accept the best candidate across all sections if it has a decent number of sub-pointers
	if (bestScore >= 5)
	{
		m_clientGameCtxGlobalOffset = bestGlobal - m_moduleBase;
		Log("Found probable %s via brute-force: obj=0x%016llX global=Module+0x%llX (score=%d sub-ptrs)",
			contextClassName.c_str(), (uintptr_t)bestObj, m_clientGameCtxGlobalOffset, bestScore);

		TraversalChain rootChain;
		rootChain.targetClass = contextClassName;
		rootChain.rootClassName = contextClassName;
		rootChain.rootGlobalOffset = m_clientGameCtxGlobalOffset;
		rootChain.resolvedAddress = (uintptr_t)bestObj;
		m_traversalMap[contextClassName] = rootChain;

		return (uintptr_t)bestObj;
	}

	Log("ERROR: Could not find %s via any method", contextClassName.c_str());
	return 0;
}

// ============================================================================
// P0: Traversal Map Builder
// ============================================================================

void ClassInfoManager::CaptureVTable(const std::string& className, void* instance, ClassInfo* ci)
{
	int vtableCount = SafeCountVTableEntries(instance, m_moduleBase, m_moduleEnd);
	if (vtableCount <= 0)
		return;

	VTableInfo vti;
	vti.entryCount = vtableCount;
	std::set<std::string> usedNames;
	for (int v = 0; v < vtableCount; v++)
	{
		uintptr_t entry = SafeReadVTableEntry(instance, v);
		vti.entries.push_back(entry > 0 ? entry - m_moduleBase : 0);

		// Always push one entry per slot so methods[i] stays aligned with entries[i]
		VTableMethodInfo mi;
		if (entry > 0)
			mi = AnalyzeVTableMethod(ci, v, entry);
		else
			mi = { v, 0, "Func_" + std::to_string(v), "void*", "", "" };

		// Method names become static constexpr members, so they must be unique
		std::string baseName = mi.name;
		for (int n = 1; !usedNames.insert(mi.name).second; n++)
			mi.name = baseName + "_" + std::to_string(v) + (n > 1 ? "_" + std::to_string(n) : "");

		vti.methods.push_back(mi);
	}
	m_vtableMap[className] = vti;
}

void ClassInfoManager::BuildTraversalMap(uintptr_t instanceAddr, ClassInfo* classInfo,
	int depth, std::vector<TraversalStep>& currentChain, std::set<uintptr_t>& visited,
	uintptr_t rootGlobalOffset, const std::string& rootClassName, bool rootIsStatic)
{
	if (depth > 5 || !classInfo || !IsValidPointer(classInfo))
		return;

	uintptr_t effectiveRootOffset = rootGlobalOffset != 0 ? rootGlobalOffset : m_clientGameCtxGlobalOffset;
	bool effectiveRootIsStatic = rootGlobalOffset != 0 ? rootIsStatic : m_rootIsStatic;
	std::string effectiveRootClass = rootClassName.empty() ? "ClientGameContext" : rootClassName;

	// Records `chain` for `targetName` if it is new or shorter than the known one.
	auto recordChain = [&](const std::string& targetName, const std::vector<TraversalStep>& chainSteps, void* instance, const char* via)
	{
		auto existing = m_traversalMap.find(targetName);
		if (existing != m_traversalMap.end() && chainSteps.size() >= existing->second.steps.size())
			return;

		TraversalChain chain;
		chain.targetClass = targetName;
		chain.rootClassName = effectiveRootClass;
		chain.rootGlobalOffset = effectiveRootOffset;
		chain.rootIsStatic = effectiveRootIsStatic;
		chain.steps = chainSteps;
		chain.resolvedAddress = (uintptr_t)instance;
		m_traversalMap[targetName] = chain;

		Log("  Traversal%s: %s @ depth %d, offset chain length %d, addr 0x%016llX (via %s)",
			via, targetName.c_str(), depth, (int)chainSteps.size(), (uintptr_t)instance, effectiveRootClass.c_str());

		auto targetClassIt = m_classMap.find(targetName);
		CaptureVTable(targetName, instance, targetClassIt != m_classMap.end() ? targetClassIt->second : nullptr);
	};

	// Iteratively collect this class and its parents with cycle detection (prevents stack overflow)
	std::vector<ClassInfo*> classesToWalk;
	classesToWalk.push_back(classInfo);
	std::vector<ClassInfo*> parents = GetParents(classInfo);
	classesToWalk.insert(classesToWalk.end(), parents.begin(), parents.end());

	for (ClassInfo* currCI : classesToWalk)
	{
		TypeInfo* ti = (TypeInfo*)SafeReadPointer((uintptr_t)&currCI->typeInfo);
		if (!ti)
			continue;

		FieldInfo* fields = (FieldInfo*)SafeReadPointer((uintptr_t)&ti->fields);
		if (!fields)
			continue;

		int fieldCount = SafeReadUInt16((uintptr_t)&ti->fieldCount);
		if (fieldCount <= 0 || fieldCount > 1000)
			continue;

		// Walk this class's fields
		for (int i = 0; i < fieldCount; ++i)
		{
			FieldInfo* fi = &fields[i];
			MemberTypeInfo* mti = (MemberTypeInfo*)SafeReadPointer((uintptr_t)&fi->typeInfo);
			TypeInfo* fieldType = mti ? (TypeInfo*)SafeReadPointer((uintptr_t)&mti->typeInfo) : nullptr;
			if (!fieldType)
				continue;

			unsigned short fieldFlags = SafeReadUInt16((uintptr_t)&fieldType->flags);
			int fieldOffset = SafeReadUInt16((uintptr_t)&fi->offset);

			char fiNameBuf[128] = { 0 };
			void* fiNamePtr = SafeReadPointer((uintptr_t)&fi->name);
			std::string fieldNameStr = (fiNamePtr && SafeReadString((uintptr_t)fiNamePtr, fiNameBuf, sizeof(fiNameBuf))) ? fiNameBuf : "unknown";

			// Follow arrays of pointers (fb::Array<T*>) through their first element
			if (fieldFlags == kType_Array)
			{
				void* elemTypeSlot = SafeReadPointer((uintptr_t)&fieldType->enumFields);
				TypeInfo* ati = elemTypeSlot ? (TypeInfo*)SafeReadPointer((uintptr_t)elemTypeSlot) : nullptr;
				if (!ati || SafeReadUInt16((uintptr_t)&ati->flags) != kType_Pointer)
					continue;

				std::string elemTypeName = ReadTypeName(ati);
				if (elemTypeName.empty())
					continue;

				// Assumes the {first, last, ...} array layout used by fb::Array (see GetFieldSize).
				uintptr_t arrayAddr = instanceAddr + fieldOffset;
				void* firstElem = SafeReadPointer(arrayAddr);
				void* lastElem = SafeReadPointer(arrayAddr + 8);
				if (!firstElem || !lastElem || (uintptr_t)lastElem < (uintptr_t)firstElem)
					continue;

				size_t elemCount = ((uintptr_t)lastElem - (uintptr_t)firstElem) / sizeof(void*);
				if (elemCount == 0 || elemCount >= 256)
					continue;

				void* elemPtr = SafeReadPointer((uintptr_t)firstElem);
				if (!elemPtr)
					continue;
				void* elemVtable = SafeReadPointer((uintptr_t)elemPtr);
				if (!elemVtable || !IsModulePointer(elemVtable, m_moduleBase, m_moduleEnd))
					continue;
				if (visited.count((uintptr_t)elemPtr))
					continue;

				// Two dereferences: the array's data pointer, then element 0.
				TraversalStep arrayStep;
				arrayStep.offset = fieldOffset;
				arrayStep.fieldName = fieldNameStr;
				arrayStep.typeName = "Array<" + elemTypeName + "*>";

				TraversalStep elemStep;
				elemStep.offset = 0;
				elemStep.fieldName = fieldNameStr + "[0]";
				elemStep.typeName = elemTypeName;

				std::vector<TraversalStep> newChain = currentChain;
				newChain.push_back(arrayStep);
				newChain.push_back(elemStep);

				recordChain(elemTypeName, newChain, elemPtr, " (via array)");

				visited.insert((uintptr_t)elemPtr);
				auto targetElemClassIt = m_classMap.find(elemTypeName);
				if (targetElemClassIt != m_classMap.end())
				{
					BuildTraversalMap((uintptr_t)elemPtr, targetElemClassIt->second,
						depth + 1, newChain, visited, effectiveRootOffset, effectiveRootClass, effectiveRootIsStatic);
				}
				continue;
			}

			// Only follow pointer fields
			if (fieldFlags != kType_Pointer)
				continue;

			std::string targetName = ReadTypeName(fieldType);
			if (targetName.empty())
				continue;

			// Read the live pointer value
			void* fieldPtr = SafeReadPointer(instanceAddr + fieldOffset);
			if (!fieldPtr)
				continue;

			// Validate: the target should have a vtable in the module
			void* targetVtable = SafeReadPointer((uintptr_t)fieldPtr);
			if (!targetVtable || !IsModulePointer(targetVtable, m_moduleBase, m_moduleEnd))
				continue;

			// Prevent infinite loops
			if (visited.count((uintptr_t)fieldPtr))
				continue;

			TraversalStep step;
			step.offset = fieldOffset;
			step.fieldName = fieldNameStr;
			step.typeName = targetName;

			std::vector<TraversalStep> newChain = currentChain;
			newChain.push_back(step);

			recordChain(targetName, newChain, fieldPtr, "");

			// Recurse into this object's fields
			visited.insert((uintptr_t)fieldPtr);
			auto targetClassIt = m_classMap.find(targetName);
			if (targetClassIt != m_classMap.end())
			{
				BuildTraversalMap((uintptr_t)fieldPtr, targetClassIt->second,
					depth + 1, newChain, visited, effectiveRootOffset, effectiveRootClass, effectiveRootIsStatic);
			}
		}
	}
}

ClassInfo* ClassInfoManager::TryIdentifyClassByVTable(uintptr_t instanceAddr, bool verbose)
{
	void* vtable = SafeReadPointer(instanceAddr);
	if (!vtable || !IsModulePointer(vtable, m_moduleBase, m_moduleEnd))
		return nullptr;

	// Identification only depends on the vtable, so each vtable is analyzed once.
	auto cached = m_vtableIdCache.find((uintptr_t)vtable);
	if (cached != m_vtableIdCache.end())
		return cached->second;

	int vtableCount = SafeCountVTableEntries((void*)instanceAddr, m_moduleBase, m_moduleEnd);
	if (vtableCount > 50) vtableCount = 50;

	auto lookup = [&](uintptr_t funcAddr, int j, bool isMov, int32_t disp, int index) -> ClassInfo*
	{
		uintptr_t targetAddr = funcAddr + j + 7 + (int64_t)disp;

		// A 'mov rax, [rip+disp]' loads a pointer from a global variable, so dereference it
		// to get the actual TypeInfo/ClassInfo address.
		if (isMov)
		{
			void* deref = SafeReadPointer(targetAddr);
			if (deref) targetAddr = (uintptr_t)deref;
		}

		if (verbose)
			Log("      [vtable %d] found RIP-relative at +%d: target 0x%llX", index, j, targetAddr);

		auto it = m_typeInfoToClassMap.find(targetAddr);
		return it != m_typeInfoToClassMap.end() ? it->second : nullptr;
	};

	ClassInfo* result = nullptr;

	// Pass 1: an exact GetType() shape - 'lea/mov rax, [rip+disp32]; ret'. This is the
	// class's own type, unlike references found inside larger functions.
	for (int i = 0; i < vtableCount && !result; i++)
	{
		uintptr_t funcAddr = SafeReadVTableEntry((void*)instanceAddr, i);
		if (!funcAddr || !IsModulePointer((void*)funcAddr, m_moduleBase, m_moduleEnd))
			continue;

		unsigned char code[8];
		if (!SafeReadBytes(funcAddr, code, sizeof(code)))
			continue;

		if (code[0] == 0x48 && (code[1] == 0x8D || code[1] == 0x8B) && code[2] == 0x05 && code[7] == 0xC3)
		{
			int32_t disp;
			memcpy(&disp, &code[3], sizeof(disp));
			result = lookup(funcAddr, 0, code[1] == 0x8B, disp, i);
		}
	}

	// Pass 2: any RIP-relative lea/mov near the start of a virtual function.
	for (int i = 0; i < vtableCount && !result; i++)
	{
		uintptr_t funcAddr = SafeReadVTableEntry((void*)instanceAddr, i);
		if (!funcAddr || !IsModulePointer((void*)funcAddr, m_moduleBase, m_moduleEnd))
			continue;

		unsigned char code[32];
		if (!SafeReadBytes(funcAddr, code, sizeof(code)))
			continue;

		for (int j = 0; j < (int)sizeof(code) - 7 && !result; j++)
		{
			// If we hit RET (0xC3) or INT3 (0xCC), stop scanning this function to avoid bleeding into adjacent functions
			if (code[j] == 0xC3 || code[j] == 0xCC)
				break;

			if ((code[j] == 0x48 && code[j+1] == 0x8D && code[j+2] == 0x05) || // lea rax
			    (code[j] == 0x48 && code[j+1] == 0x8B && code[j+2] == 0x05) || // mov rax
			    (code[j] == 0x48 && code[j+1] == 0x8D && code[j+2] == 0x0D))   // lea rcx
			{
				int32_t disp;
				memcpy(&disp, &code[j + 3], sizeof(disp));
				result = lookup(funcAddr, j, code[j+1] == 0x8B, disp, i);
			}
		}
	}

	if (result && verbose)
		Log("      -> MATCHED ClassInfo: %s", ReadTypeName(result->typeInfo).c_str());

	m_vtableIdCache[(uintptr_t)vtable] = result;
	return result;
}

void ClassInfoManager::BlindTraversalWalk(uintptr_t objectAddr, int depth,
	std::vector<TraversalStep>& currentChain,
	std::set<uintptr_t>& visited,
	uintptr_t rootGlobalOffset, const std::string& rootClassName, bool rootIsStatic)
{
	if (depth > 4) return;
	bool isRoot = (depth == 0);

	uintptr_t effectiveRootOffset = rootGlobalOffset != 0 ? rootGlobalOffset : m_clientGameCtxGlobalOffset;
	bool effectiveRootIsStatic = rootGlobalOffset != 0 ? rootIsStatic : m_rootIsStatic;
	std::string effectiveRootClass = rootClassName.empty() ? "ClientGameContext" : rootClassName;

	if (isRoot) Log("  Starting BlindTraversalWalk on root 0x%llX (%s)", objectAddr, effectiveRootClass.c_str());

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
		std::string targetName = identifiedClass ? ReadTypeName(identifiedClass->typeInfo) : "";

		TraversalStep step;
		step.offset = off;
		char fieldBuf[32];
		snprintf(fieldBuf, sizeof(fieldBuf), "unk_0x%X", off);
		step.fieldName = fieldBuf;

		std::vector<TraversalStep> newChain = currentChain;

		if (!targetName.empty())
		{
			step.typeName = targetName;
			newChain.push_back(step);

			auto existing = m_traversalMap.find(targetName);
			if (existing == m_traversalMap.end() || newChain.size() < existing->second.steps.size())
			{
				TraversalChain chain;
				chain.targetClass = targetName;
				chain.rootClassName = effectiveRootClass;
				chain.rootGlobalOffset = effectiveRootOffset;
				chain.rootIsStatic = effectiveRootIsStatic;
				chain.steps = newChain;
				chain.resolvedAddress = (uintptr_t)ptr;
				m_traversalMap[targetName] = chain;

				Log("  [!] Blind Traversal Found: %s @ offset 0x%X, depth %d (via %s)",
					targetName.c_str(), off, depth, effectiveRootClass.c_str());

				CaptureVTable(targetName, ptr, identifiedClass);
			}

			visited.insert((uintptr_t)ptr);
			BuildTraversalMap((uintptr_t)ptr, identifiedClass, depth + 1, newChain, visited,
				effectiveRootOffset, effectiveRootClass, effectiveRootIsStatic);
		}
		else
		{
			if (isRoot) Log("      Could not identify class from vtable.");
			step.typeName = "UnknownClass";
			newChain.push_back(step);

			visited.insert((uintptr_t)ptr);
			BlindTraversalWalk((uintptr_t)ptr, depth + 1, newChain, visited,
				effectiveRootOffset, effectiveRootClass, effectiveRootIsStatic);
		}
	}
}

// ============================================================================
// Class Dumping (with all new features)
// ============================================================================

void ClassInfoManager::DumpClass(ClassInfo* c)
{
	TypeInfo* ti = c->typeInfo;
	std::string name = ReadTypeName(ti);
	if (!IsDeclaredType(name))
		return;

	// Only inherit from a parent that gets its own header; otherwise its bytes become padding.
	std::vector<ClassInfo*> parents = GetParents(c);
	std::string parentName;
	int parentSize = 0;
	if (!parents.empty())
	{
		std::string candidate = ReadTypeName(parents.at(0)->typeInfo);
		auto decl = m_declaredTypes.find(candidate);
		if (decl != m_declaredTypes.end() && decl->second.rfind("enum ", 0) != 0)
		{
			parentName = candidate;
			parentSize = SafeReadUInt16((uintptr_t)&parents.at(0)->typeInfo->totalSize);
		}
	}

	std::string headerFile = "SDK\\" + name + ".h";
	std::ofstream file;
	if (!OpenOutput(file, headerFile))
		return;

	DumpHeader(file, headerFile.c_str());

	file << std::endl << "#ifndef FBGEN_" << name << "_H" << std::endl;
	file << "#define FBGEN_" << name << "_H" << std::endl << std::endl;

	file << "#include \"FBSDKTypes.h\"" << std::endl;
	file << "#include \"FBClasses.h\"" << std::endl;

	std::vector<FieldInfo*> fields;
	ParseClassMembers(ti, fields);
	std::vector<MemberDesc> members = BuildMemberDescs(fields);
	ResolveHeaders(members, file);

	if (!parentName.empty())
	{
		file << "#include \"" << parentName << ".h\"" << std::endl << std::endl;
		file << "class " << name << " :" << std::endl;
		file << "\tpublic " << parentName << " // size = 0x" << std::hex << parentSize << std::dec << std::endl;
	}
	else
	{
		file << std::endl << "class " << name << std::endl;
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
	int totalSizeOfClass = SafeReadUInt16((uintptr_t)&ti->totalSize);
	int memberSize = DumpClassMembers(file, members, parentSize);
	if (memberSize + parentSize < totalSizeOfClass)
		file << "\tunsigned char _0x" << std::hex << (memberSize + parentSize) << "[0x" << (totalSizeOfClass - (memberSize + parentSize)) << "];" << std::dec << std::endl;

	// --- Getter/Setter Accessors ---
	file << std::endl;
	DumpGetterSetters(file, members);

	// --- VMT Hook Helper ---
	DumpVMTHookHelper(c, file);

	file << "}; // size = 0x" << std::hex << totalSizeOfClass << std::dec << std::endl << std::endl;
	DumpLayoutChecks(file, name, totalSizeOfClass, members);
	file << "#endif // FBGEN_" << name << "_H" << std::endl;

	file.close();
	m_sdkFileNames.push_back(name);
}

// ============================================================================
// Struct Dumping (with new features)
// ============================================================================

void ClassInfoManager::DumpStruct(ClassInfo* c)
{
	TypeInfo* ti = c->typeInfo;
	std::string name = ReadTypeName(ti);
	if (!IsDeclaredType(name))
		return;

	std::string headerFile = "SDK\\" + name + ".h";
	std::ofstream file;
	if (!OpenOutput(file, headerFile))
		return;

	DumpHeader(file, headerFile.c_str());

	file << std::endl << "#ifndef FBGEN_" << name << "_H" << std::endl;
	file << "#define FBGEN_" << name << "_H" << std::endl << std::endl;
	file << "#include \"FBSDKTypes.h\"" << std::endl;
	file << "#include \"FBClasses.h\"" << std::endl;

	std::vector<FieldInfo*> fields;
	ParseStructMembers(ti, fields);
	std::vector<MemberDesc> members = BuildMemberDescs(fields);
	ResolveHeaders(members, file);

	file << std::endl << "struct " << name << std::endl;
	file << "{" << std::endl;

	DumpTypeInfo(c, file);
	DumpOffsetConstants(file, members);

	int totalSizeOfClass = SafeReadUInt16((uintptr_t)&ti->totalSize);
	int memberSize = DumpClassMembers(file, members, 0);
	if (memberSize < totalSizeOfClass)
		file << "\tunsigned char _0x" << std::hex << memberSize << "[0x" << (totalSizeOfClass - memberSize) << "];" << std::dec << std::endl;

	file << std::endl;
	DumpGetterSetters(file, members);

	file << "}; // size = 0x" << std::hex << totalSizeOfClass << std::dec << std::endl << std::endl;
	DumpLayoutChecks(file, name, totalSizeOfClass, members);
	file << "#endif // FBGEN_" << name << "_H" << std::endl;

	file.close();
	m_sdkFileNames.push_back(name);
}

// ============================================================================
// Enum Dumping
// ============================================================================

std::string ClassInfoManager::GetEnumUnderlyingType(TypeInfo* ti)
{
	// A fixed underlying type keeps forward declarations legal C++ and the size equal to the engine's.
	int size = SafeReadUInt16((uintptr_t)&ti->totalSize);
	FieldInfoEnum* fields = (FieldInfoEnum*)SafeReadPointer((uintptr_t)&ti->enumFields);
	int count = SafeReadUInt16((uintptr_t)&ti->fieldCount);

	long long minValue = 0, maxValue = 0;
	if (fields && count > 0 && count <= 10000)
	{
		for (int i = 0; i < count; ++i)
		{
			FieldInfoEnum e;
			if (!SafeReadBytes((uintptr_t)&fields[i], &e, sizeof(e)))
				continue;
			if (e.value < minValue) minValue = e.value;
			if (e.value > maxValue) maxValue = e.value;
		}
	}

	auto fits = [&](long long lo, long long hi) { return minValue >= lo && maxValue <= hi; };
	switch (size)
	{
	case 1:
		if (fits(INT8_MIN, INT8_MAX)) return "int8_t";
		if (fits(0, UINT8_MAX)) return "uint8_t";
		break;
	case 2:
		if (fits(INT16_MIN, INT16_MAX)) return "int16_t";
		if (fits(0, UINT16_MAX)) return "uint16_t";
		break;
	case 8:
		return "int64_t";
	}
	return "int32_t";
}

void ClassInfoManager::DumpEnum(ClassInfo* c)
{
	TypeInfo* ti = c->typeInfo;
	std::string name = ReadTypeName(ti);
	if (!IsDeclaredType(name))
		return;

	std::string headerFile = "SDK\\" + name + ".h";
	std::ofstream file;
	if (!OpenOutput(file, headerFile))
		return;

	DumpHeader(file, headerFile.c_str());

	const std::string underlying = "std::underlying_type_t<" + name + ">";

	file << std::endl << "#ifndef FBGEN_" << name << "_H" << std::endl;
	file << "#define FBGEN_" << name << "_H" << std::endl << std::endl;
	file << "#include <cstdint>" << std::endl;
	file << "#include <type_traits>" << std::endl << std::endl;
	file << "enum " << name << " : " << GetEnumUnderlyingType(ti) << std::endl;
	file << "{" << std::endl;
	DumpEnumMembers(file, ti);
	file << "};" << std::endl << std::endl;

	// Bitwise operator overloads for flags / masks
	file << "// Bitwise operators for " << name << std::endl;
	file << "inline constexpr " << name << " operator|(" << name << " a, " << name << " b) {" << std::endl;
	file << "\treturn static_cast<" << name << ">(static_cast<" << underlying << ">(a) | static_cast<" << underlying << ">(b));" << std::endl;
	file << "}" << std::endl;
	file << "inline constexpr " << name << " operator&(" << name << " a, " << name << " b) {" << std::endl;
	file << "\treturn static_cast<" << name << ">(static_cast<" << underlying << ">(a) & static_cast<" << underlying << ">(b));" << std::endl;
	file << "}" << std::endl;
	file << "inline constexpr " << name << " operator^(" << name << " a, " << name << " b) {" << std::endl;
	file << "\treturn static_cast<" << name << ">(static_cast<" << underlying << ">(a) ^ static_cast<" << underlying << ">(b));" << std::endl;
	file << "}" << std::endl;
	file << "inline constexpr " << name << " operator~(" << name << " a) {" << std::endl;
	file << "\treturn static_cast<" << name << ">(~static_cast<" << underlying << ">(a));" << std::endl;
	file << "}" << std::endl;
	file << "inline " << name << "& operator|=(" << name << "& a, " << name << " b) {" << std::endl;
	file << "\treturn a = a | b;" << std::endl;
	file << "}" << std::endl;
	file << "inline " << name << "& operator&=(" << name << "& a, " << name << " b) {" << std::endl;
	file << "\treturn a = a & b;" << std::endl;
	file << "}" << std::endl;
	file << "inline " << name << "& operator^=(" << name << "& a, " << name << " b) {" << std::endl;
	file << "\treturn a = a ^ b;" << std::endl;
	file << "}" << std::endl << std::endl;

	file << "#endif // FBGEN_" << name << "_H" << std::endl;

	file.close();
	m_sdkFileNames.push_back(name);
}

void ClassInfoManager::DumpEnumMembers(std::ofstream& file, TypeInfo* ti)
{
	FieldInfoEnum* fields = (FieldInfoEnum*)SafeReadPointer((uintptr_t)&ti->enumFields);
	int count = SafeReadUInt16((uintptr_t)&ti->fieldCount);
	if (!fields || count <= 0 || count > 10000)
		return;

	std::set<std::string> used;
	for (int i = 0; i < count; ++i)
	{
		FieldInfoEnum e;
		if (!SafeReadBytes((uintptr_t)&fields[i], &e, sizeof(e)))
			continue;

		char nameBuf[256] = { 0 };
		std::string rawName = (e.name && SafeReadString((uintptr_t)e.name, nameBuf, sizeof(nameBuf))) ? nameBuf : "unk_" + std::to_string(i);
		std::string baseName = SanitizeMemberName(rawName);
		std::string memberName = baseName;
		for (int k = 2; !used.insert(memberName).second; k++)
			memberName = baseName + "_" + std::to_string(k);

		file << "\t" << memberName << " = " << std::dec << e.value << ",";
		if (memberName != rawName)
			file << " // " << CommentSafe(rawName);
		file << std::endl;
	}
}

// ============================================================================
// Member Parsing & Dumping
// ============================================================================

void ClassInfoManager::ResolveHeaders(const std::vector<MemberDesc>& members, std::ofstream& file)
{
	// Only by-value members need a full definition; pointers and arrays use FBClasses.h.
	std::set<std::string> emitted;
	for (const auto& d : members)
	{
		if (d.kind == MemberDesc::Value && !d.includeType.empty() && emitted.insert(d.includeType).second)
			file << "#include \"" << d.includeType << ".h\"" << std::endl;
	}
}

const char* ClassInfoManager::GetFixedClassName(const char* orig)
{
	if (!orig || !IsValidPointer((void*)orig)) return "unk";
	const char* prim = MapPrimitiveName(orig);
	return prim ? prim : orig;
}

bool SafeValidateFieldInfo(FieldInfo* fi)
{
	__try {
		if (!fi || !fi->typeInfo || !fi->typeInfo->typeInfo) return false;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void ClassInfoManager::ParseClassMembers(TypeInfo* ti, std::vector<FieldInfo*>& members)
{
	if (!IsValidPointer(ti)) return;
	FieldInfo* fields = (FieldInfo*)SafeReadPointer((uintptr_t)&ti->fields);
	if (!fields) return;
	int fieldCount = SafeReadUInt16((uintptr_t)&ti->fieldCount);
	if (fieldCount <= 0 || fieldCount > 10000) return;
	for (int i = 0; i < fieldCount; ++i)
	{
		FieldInfo* fi = &fields[i];
		if (SafeValidateFieldInfo(fi))
			members.push_back(fi);
	}
	auto cmp = [](const FieldInfo* a, const FieldInfo* b) { return a->offset < b->offset; };
	std::stable_sort(members.begin(), members.end(), cmp);
}

void ClassInfoManager::ParseStructMembers(TypeInfo* ti, std::vector<FieldInfo*>& members)
{
	if (!IsValidPointer(ti)) return;
	FieldInfo* fields = (FieldInfo*)SafeReadPointer((uintptr_t)&ti->structFields);
	if (!fields) return;
	int fieldCount = SafeReadUInt16((uintptr_t)&ti->fieldCount);
	if (fieldCount <= 0 || fieldCount > 10000) return;
	for (int i = 0; i < fieldCount; ++i)
	{
		FieldInfo* fi = &fields[i];
		if (SafeValidateFieldInfo(fi))
			members.push_back(fi);
	}
	auto cmp = [](const FieldInfo* a, const FieldInfo* b) { return a->offset < b->offset; };
	std::stable_sort(members.begin(), members.end(), cmp);
}

bool ClassInfoManager::ResolveMemberType(TypeInfo* mti, MemberDesc& d)
{
	if (!mti)
	{
		d.kind = MemberDesc::Unknown;
		d.engineType = "?";
		return false;
	}

	std::string typeName = ReadTypeName(mti);
	d.engineType = typeName;
	unsigned short flags = SafeReadUInt16((uintptr_t)&mti->flags);

	if (flags == kType_Pointer)
	{
		// A pointer TypeInfo is named after its pointee.
		const char* prim = MapPrimitiveName(typeName.c_str());
		d.kind = MemberDesc::Pointer;
		if (prim && strcmp(prim, "const char*"))
			d.cppType = std::string(prim) + "*";
		else if (IsDeclaredType(typeName))
			d.cppType = typeName + "*";
		else
			d.cppType = "void*"; // pointee type is not in the SDK
		return true;
	}

	if (flags == kType_Array)
	{
		int engineSize = SafeReadUInt16((uintptr_t)&mti->totalSize);
		if (!m_loggedArraySize && engineSize != 0 && engineSize != 32)
		{
			Log("WARNING: engine reports array type %s as %d bytes but the SDK assumes 32; array layouts may be wrong",
				typeName.c_str(), engineSize);
			m_loggedArraySize = true;
		}

		void* elemTypeSlot = SafeReadPointer((uintptr_t)&mti->enumFields);
		TypeInfo* ati = elemTypeSlot ? (TypeInfo*)SafeReadPointer((uintptr_t)elemTypeSlot) : nullptr;
		std::string elem = "uint8_t";
		if (ati)
		{
			std::string elemName = ReadTypeName(ati);
			bool elemIsPointer = SafeReadUInt16((uintptr_t)&ati->flags) == kType_Pointer;
			const char* prim = MapPrimitiveName(elemName.c_str());
			std::string base = prim ? prim : (IsDeclaredType(elemName) ? elemName : (elemIsPointer ? "void" : "uint8_t"));
			elem = elemIsPointer ? base + "*" : base;
			d.engineType = "Array<" + elemName + (elemIsPointer ? "*" : "") + ">";
		}
		d.kind = MemberDesc::Array;
		d.cppType = "fb::Array<" + elem + ">";
		return true;
	}

	const char* prim = MapPrimitiveName(typeName.c_str());
	if (prim)
	{
		d.kind = strcmp(prim, "const char*") ? MemberDesc::Primitive : MemberDesc::CString;
		d.cppType = prim;
		return true;
	}

	if (IsDeclaredType(typeName))
	{
		d.kind = MemberDesc::Value;
		d.cppType = typeName;
		d.includeType = typeName;
		return true;
	}

	d.kind = MemberDesc::Unknown;
	return false;
}

std::vector<MemberDesc> ClassInfoManager::BuildMemberDescs(const std::vector<FieldInfo*>& fields)
{
	std::vector<MemberDesc> out;
	std::set<std::string> usedNames;

	for (FieldInfo* fi : fields)
	{
		MemberDesc d;
		d.fi = fi;
		d.offset = SafeReadUInt16((uintptr_t)&fi->offset);
		d.size = fi->GetFieldSize();

		char nameBuf[256] = { 0 };
		void* namePtr = SafeReadPointer((uintptr_t)&fi->name);
		d.rawName = (namePtr && SafeReadString((uintptr_t)namePtr, nameBuf, sizeof(nameBuf))) ? nameBuf : "";

		char fallback[32];
		snprintf(fallback, sizeof(fallback), "unk_0x%X", d.offset);
		std::string baseName = SanitizeMemberName(d.rawName.empty() ? fallback : d.rawName);
		d.name = baseName;
		for (int k = 2; !usedNames.insert(d.name).second; k++)
			d.name = baseName + "_" + std::to_string(k);

		MemberTypeInfo* mti = (MemberTypeInfo*)SafeReadPointer((uintptr_t)&fi->typeInfo);
		TypeInfo* fieldType = mti ? (TypeInfo*)SafeReadPointer((uintptr_t)&mti->typeInfo) : nullptr;
		ResolveMemberType(fieldType, d);

		out.push_back(d);
	}

	// Accessors are Get<Name>/Set<Name>; keep them clear of the static GetInstance()/GetTypeInfo().
	std::set<std::string> usedAccessors = { "Instance", "TypeInfo" };
	for (auto& d : out)
	{
		std::string accessor = d.name;
		for (int k = 2; !usedAccessors.insert(accessor).second; k++)
			accessor = d.name + "Field" + (k > 2 ? std::to_string(k) : "");
		d.accessorName = accessor;
	}
	return out;
}

int ClassInfoManager::DumpClassMembers(std::ofstream& file, std::vector<MemberDesc>& members, int parentSize)
{
	// Every member either occupies exactly [offset, offset + size) or is reported in a comment,
	// so one bad field can never shift the members after it.
	int lastOffset = parentSize;

	for (auto& d : members)
	{
		d.declared = false;

		if (d.offset < lastOffset)
		{
			file << "\t// 0x" << std::hex << d.offset << std::dec << ": " << CommentSafe(d.rawName)
				<< " overlaps the previous member and is not declared" << std::endl;
			continue;
		}

		if (d.offset > lastOffset)
			file << "\tunsigned char _0x" << std::hex << lastOffset << "[0x" << (d.offset - lastOffset) << "];" << std::dec << std::endl;

		if (d.kind == MemberDesc::Unknown)
		{
			if (d.size <= 0)
			{
				file << "\t// 0x" << std::hex << d.offset << std::dec << ": " << CommentSafe(d.rawName)
					<< " (" << CommentSafe(d.engineType) << ") has unknown size and is not declared" << std::endl;
				lastOffset = d.offset;
				continue;
			}
			file << "\tunsigned char m_" << d.name << "[0x" << std::hex << d.size << "]; // 0x" << d.offset << std::dec
				<< " (" << CommentSafe(d.engineType) << ", type not in SDK)" << std::endl;
		}
		else
		{
			file << "\t" << d.cppType << " m_" << d.name << "; // 0x" << std::hex << d.offset << std::dec << std::endl;
		}

		d.declared = true;
		lastOffset = d.offset + d.size;
	}
	return lastOffset - parentSize;
}

void ClassInfoManager::DumpLayoutChecks(std::ofstream& file, const std::string& className, int totalSize, const std::vector<MemberDesc>& members)
{
	// Opt-in compile-time proof that the emitted layout matches the engine's.
	file << "#ifdef FBGEN_VERIFY_LAYOUT" << std::endl;
	if (totalSize > 0)
		file << "static_assert(sizeof(" << className << ") == 0x" << std::hex << totalSize << std::dec
			<< ", \"" << className << ": size does not match the engine\");" << std::endl;
	for (const auto& d : members)
	{
		if (!d.declared)
			continue;
		file << "static_assert(offsetof(" << className << ", m_" << d.name << ") == 0x" << std::hex << d.offset << std::dec
			<< ", \"" << className << "::m_" << d.name << ": offset does not match the engine\");" << std::endl;
	}
	file << "#endif" << std::endl << std::endl;
}

std::vector<ClassInfo*> ClassInfoManager::GetParents(ClassInfo* c)
{
	std::vector<ClassInfo*> parents;
	std::set<ClassInfo*> visited;
	visited.insert(c);
	ClassInfo* p = (ClassInfo*)SafeReadPointer((uintptr_t)&c->parent);
	while (p)
	{
		if (visited.count(p)) break; // cycle detected
		visited.insert(p);
		parents.push_back(p);
		p = (ClassInfo*)SafeReadPointer((uintptr_t)&p->parent);
	}
	return parents;
}

// ============================================================================
// Type Info (ASLR-safe)
// ============================================================================

void ClassInfoManager::DumpTypeInfo(ClassInfo* c, std::ofstream& file)
{
	// Note: this is the address of the type's ClassInfo record (which points to its TypeInfo).
	uintptr_t relativeAddr = (uintptr_t)c - m_moduleBase;
	file << "\tstatic void* GetTypeInfo()" << std::endl;
	file << "\t{" << std::endl;
	file << "\t\treturn (void*)(fb::GetModuleBase() + 0x" << std::hex << relativeAddr << std::dec << ");" << std::endl;
	file << "\t}" << std::endl;
}

// ============================================================================
// P0: Instance Resolver (ClientGameContext / singleton traversal)
// ============================================================================

void ClassInfoManager::DumpInstanceResolver(ClassInfo* c, std::ofstream& file)
{
	std::string className = ReadTypeName(c->typeInfo);

	// Emits code that leaves the root object's address in `ctx`. A static root lives inside
	// the module image, so its address is computed rather than read from a pointer slot.
	auto emitRoot = [&](uintptr_t offset, bool isStatic, const std::string& label)
	{
		if (isStatic)
		{
			file << "\t\tuintptr_t ctx = fb::GetModuleBase() + 0x" << std::hex << offset << std::dec
				<< "; // " << CommentSafe(label) << " (static object in module)" << std::endl;
		}
		else
		{
			file << "\t\tuintptr_t ctx = fb::Read<uintptr_t>(fb::GetModuleBase() + 0x" << std::hex << offset << std::dec
				<< "); // " << CommentSafe(label) << std::endl;
			file << "\t\tif (!ctx) return nullptr;" << std::endl;
		}
	};

	file << "\tstatic " << className << "* GetInstance()" << std::endl;
	file << "\t{" << std::endl;

	auto global = m_globalInstances.find(className);
	auto chainIt = m_traversalMap.find(className);

	if (global != m_globalInstances.end())
	{
		file << "\t\t// Global singleton found dynamically" << std::endl;
		emitRoot(global->second, m_staticGlobals.count(className) != 0, "global " + className);
		file << "\t\treturn reinterpret_cast<" << className << "*>(ctx);" << std::endl;
	}
	else if (chainIt != m_traversalMap.end() && chainIt->second.rootGlobalOffset != 0)
	{
		const TraversalChain& chain = chainIt->second;
		std::string rootName = chain.rootClassName.empty() ? "ClientGameContext" : chain.rootClassName;

		if (chain.rootGlobalOffset == m_clientGameCtxGlobalOffset && !m_rootVerified)
			file << "\t\t// WARNING: the root object was found heuristically and its class was not verified." << std::endl;

		if (!chain.steps.empty())
		{
			file << "\t\t// Resolved via " << CommentSafe(rootName) << " traversal:" << std::endl;
			file << "\t\t// " << CommentSafe(rootName);
			for (auto& step : chain.steps)
				file << " -> +0x" << std::hex << step.offset << std::dec << " (" << CommentSafe(step.fieldName) << ")";
			file << std::endl;
			file << "\t\t// Returns the instance reached at generation time; for non-singleton types this is one arbitrary instance." << std::endl;
		}

		emitRoot(chain.rootGlobalOffset, chain.rootIsStatic, rootName);

		for (const auto& step : chain.steps)
		{
			file << "\t\tctx = fb::Read<uintptr_t>(ctx + 0x" << std::hex << step.offset << std::dec
				<< "); // " << CommentSafe(step.fieldName) << std::endl;
			file << "\t\tif (!ctx) return nullptr;" << std::endl;
		}

		file << "\t\treturn reinterpret_cast<" << className << "*>(ctx);" << std::endl;
	}
	else if (className == "ClientGameContext" && m_clientGameCtxGlobalOffset != 0)
	{
		file << "\t\t// Root singleton - resolved via pattern scan" << std::endl;
		if (!m_rootVerified)
			file << "\t\t// WARNING: found heuristically; the object's class was not verified." << std::endl;
		emitRoot(m_clientGameCtxGlobalOffset, m_rootIsStatic, "ClientGameContext");
		file << "\t\treturn reinterpret_cast<" << className << "*>(ctx);" << std::endl;
	}
	else
	{
		// Not found in traversal - emit stub
		file << "\t\t// WARNING: No traversal chain found from ClientGameContext or singletons." << std::endl;
		file << "\t\t// This class was not reachable during SDK generation." << std::endl;
		file << "\t\t// Use fb::PatternScan() or fb::ReadChain() to resolve it manually." << std::endl;
		file << "\t\treturn nullptr;" << std::endl;
	}

	file << "\t}" << std::endl;
}

// ============================================================================
// P1: Traversal Chain Comment
// ============================================================================

void ClassInfoManager::DumpTraversalChainComment(ClassInfo* c, std::ofstream& file)
{
	std::string className = ReadTypeName(c->typeInfo);
	auto it = m_traversalMap.find(className);

	if (it != m_traversalMap.end() && it->second.rootGlobalOffset != 0)
	{
		const TraversalChain& chain = it->second;
		std::string rootName = CommentSafe(chain.rootClassName.empty() ? "ClientGameContext" : chain.rootClassName);
		file << "\t// -------------------------------------------------------" << std::endl;
		file << "\t// Traversal chain from " << rootName << ":" << std::endl;
		file << "\t//   " << rootName << (chain.rootIsStatic ? " (static object at Module+0x" : "* (Module+0x")
			<< std::hex << chain.rootGlobalOffset << std::dec << ")" << std::endl;

		std::string indent = "\t//     ";
		for (auto& step : chain.steps)
		{
			file << indent << "-> +0x" << std::hex << step.offset << std::dec
				<< " (" << CommentSafe(step.fieldName) << " : " << CommentSafe(step.typeName) << "*)" << std::endl;
		}

		file << "\t// Live address at generation: 0x" << std::hex << chain.resolvedAddress << std::dec << std::endl;
		file << "\t// -------------------------------------------------------" << std::endl;
	}
}

// ============================================================================
// P1: Default Value Snapshots
// ============================================================================

void ClassInfoManager::DumpDefaultValues(ClassInfo* c, std::ofstream& file,
	const std::vector<MemberDesc>& members)
{
	std::string className = ReadTypeName(c->typeInfo);
	auto it = m_traversalMap.find(className);
	if (it == m_traversalMap.end())
		return; // no live instance - can't snapshot

	uintptr_t instanceAddr = it->second.resolvedAddress;
	if (!instanceAddr)
		return;

	file << "\t// Values read from the live instance when the SDK was generated (not engine defaults)." << std::endl;
	file << "\tstruct Defaults {" << std::endl;

	for (const auto& d : members)
	{
		// Only snapshot simple value types
		if (d.kind != MemberDesc::Primitive)
			continue;

		uintptr_t fieldAddr = instanceAddr + d.offset;
		std::string literal;
		const std::string& t = d.cppType;

		if (t == "bool")
		{
			uint8_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = v ? "true" : "false";
		}
		else if (t == "float")
		{
			float v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = FormatFloatLiteral(v, true);
		}
		else if (t == "double")
		{
			double v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = FormatFloatLiteral(v, false);
		}
		else if (t == "int8_t")
		{
			int8_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = std::to_string((int)v);
		}
		else if (t == "uint8_t")
		{
			uint8_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = std::to_string((unsigned)v);
		}
		else if (t == "int16_t")
		{
			int16_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = std::to_string(v);
		}
		else if (t == "uint16_t")
		{
			uint16_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = std::to_string(v);
		}
		else if (t == "int32_t")
		{
			int32_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = std::to_string(v);
		}
		else if (t == "uint32_t")
		{
			uint32_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = std::to_string(v) + "u";
		}
		else if (t == "int64_t")
		{
			int64_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v)))
				literal = v == INT64_MIN ? "(-9223372036854775807LL - 1)" : std::to_string(v) + "LL";
		}
		else if (t == "uint64_t")
		{
			uint64_t v;
			if (SafeReadBytes(fieldAddr, &v, sizeof(v))) literal = std::to_string(v) + "ULL";
		}

		if (!literal.empty())
			file << "\t\tstatic constexpr " << t << " " << d.name << " = " << literal << ";" << std::endl;
	}

	file << "\t};" << std::endl;
}

// ============================================================================
// P2: VTable Dumping & Micro-Disassembly Analysis
// ============================================================================

static FieldInfo* FindClassFieldByOffset(ClassInfo* c, int offset)
{
	ClassInfo* curr = c;
	std::set<ClassInfo*> visited;
	while (curr && IsValidPointer(curr) && visited.count(curr) == 0 && visited.size() < 32)
	{
		visited.insert(curr);
		TypeInfo* ti = (TypeInfo*)SafeReadPointer((uintptr_t)&curr->typeInfo);
		if (!IsValidPointer(ti)) break;

		FieldInfo* fields = (FieldInfo*)SafeReadPointer((uintptr_t)&ti->fields);
		if (fields && IsValidPointer(fields))
		{
			int count = SafeReadUInt16((uintptr_t)&ti->fieldCount);
			if (count > 0 && count < 1000)
			{
				for (int i = 0; i < count; i++)
				{
					FieldInfo* fi = &fields[i];
					if (!IsValidPointer(fi)) break;
					unsigned short fOffset = SafeReadUInt16((uintptr_t)&fi->offset);
					if (fOffset == offset)
						return fi;
				}
			}
		}
		curr = (ClassInfo*)SafeReadPointer((uintptr_t)&curr->parent);
	}
	return nullptr;
}

VTableMethodInfo ClassInfoManager::AnalyzeVTableMethod(ClassInfo* c, int index, uintptr_t funcAddr)
{
	VTableMethodInfo info;
	info.index = index;
	info.rva = (funcAddr >= m_moduleBase && funcAddr < m_moduleEnd) ? (funcAddr - m_moduleBase) : 0;
	info.name = "Func_" + std::to_string(index);
	info.returnType = "void*";
	info.paramType = "";
	info.comment = "";

	if (!funcAddr || !IsModulePointer((void*)funcAddr, m_moduleBase, m_moduleEnd))
		return info;

	unsigned char code[16] = {0};
	if (!SafeReadBytes(funcAddr, code, sizeof(code)))
		return info;

	// Pattern 1: GetType()
	// lea rax, [rip+disp32]; ret (48 8D 05 XX XX XX XX C3)
	// mov rax, [rip+disp32]; ret (48 8B 05 XX XX XX XX C3)
	if ((code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05 && code[7] == 0xC3) ||
	    (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x05 && code[7] == 0xC3))
	{
		int32_t disp = *(int32_t*)&code[3];
		uintptr_t targetAddr = funcAddr + 7 + (int64_t)disp;
		if (code[1] == 0x8B)
		{
			void* deref = SafeReadPointer(targetAddr);
			if (deref) targetAddr = (uintptr_t)deref;
		}
		auto it = m_typeInfoToClassMap.find(targetAddr);
		if (it != m_typeInfoToClassMap.end() && it->second && it->second->typeInfo && it->second->typeInfo->name)
		{
			info.name = "GetType";
			info.returnType = "TypeInfo*";
			info.comment = std::string("Returns TypeInfo for ") + it->second->typeInfo->name;
			return info;
		}
	}

	// Pattern 2: Null / Constant stubs
	if (code[0] == 0xC3) // ret
	{
		info.name = "Stub_Empty_" + std::to_string(index);
		info.returnType = "void";
		info.comment = "empty stub";
		return info;
	}
	if ((code[0] == 0x31 && code[1] == 0xC0 && code[2] == 0xC3) || // xor eax, eax; ret
	    (code[0] == 0x33 && code[1] == 0xC0 && code[2] == 0xC3))
	{
		info.name = "Stub_Return0_" + std::to_string(index);
		info.returnType = "uint32_t";
		info.comment = "returns 0";
		return info;
	}
	if (code[0] == 0xB8 && *(int32_t*)&code[1] == 1 && code[5] == 0xC3) // mov eax, 1; ret
	{
		info.name = "Stub_Return1_" + std::to_string(index);
		info.returnType = "uint32_t";
		info.comment = "returns 1";
		return info;
	}

	// Pattern 3: Getters & Setters
	int fieldOffset = -1;
	bool isGetter = false;
	bool isSetter = false;
	std::string detectedType = "uintptr_t";

	// mov rax, [rcx] ; ret
	if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xC3)
	{
		fieldOffset = 0; isGetter = true; detectedType = "void*";
	}
	// mov rax, [rcx + disp8] ; ret
	else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x41 && code[4] == 0xC3)
	{
		fieldOffset = (int)(int8_t)code[3]; isGetter = true; detectedType = "void*";
	}
	// mov rax, [rcx + disp32] ; ret
	else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x81 && code[7] == 0xC3)
	{
		fieldOffset = *(int32_t*)&code[3]; isGetter = true; detectedType = "void*";
	}
	// mov eax, [rcx] ; ret
	else if (code[0] == 0x8B && code[1] == 0x01 && code[2] == 0xC3)
	{
		fieldOffset = 0; isGetter = true; detectedType = "int32_t";
	}
	// mov eax, [rcx + disp8] ; ret
	else if (code[0] == 0x8B && code[1] == 0x41 && code[3] == 0xC3)
	{
		fieldOffset = (int)(int8_t)code[2]; isGetter = true; detectedType = "int32_t";
	}
	// mov eax, [rcx + disp32] ; ret
	else if (code[0] == 0x8B && code[1] == 0x81 && code[6] == 0xC3)
	{
		fieldOffset = *(int32_t*)&code[2]; isGetter = true; detectedType = "int32_t";
	}
	// movss xmm0, [rcx + disp8] ; ret
	else if (code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x10 && code[3] == 0x41 && code[5] == 0xC3)
	{
		fieldOffset = (int)(int8_t)code[4]; isGetter = true; detectedType = "float";
	}
	// movss xmm0, [rcx + disp32] ; ret
	else if (code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x10 && code[3] == 0x81 && code[8] == 0xC3)
	{
		fieldOffset = *(int32_t*)&code[4]; isGetter = true; detectedType = "float";
	}
	// movzx eax, byte ptr [rcx + disp8] ; ret
	else if (code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x41 && code[4] == 0xC3)
	{
		fieldOffset = (int)(int8_t)code[3]; isGetter = true; detectedType = "bool";
	}
	// movzx eax, byte ptr [rcx + disp32] ; ret
	else if (code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x81 && code[7] == 0xC3)
	{
		fieldOffset = *(int32_t*)&code[3]; isGetter = true; detectedType = "bool";
	}
	// mov [rcx + disp8], rdx ; ret
	else if (code[0] == 0x48 && code[1] == 0x89 && code[2] == 0x51 && code[4] == 0xC3)
	{
		fieldOffset = (int)(int8_t)code[3]; isSetter = true; detectedType = "void*";
	}
	// mov [rcx + disp32], rdx ; ret
	else if (code[0] == 0x48 && code[1] == 0x89 && code[2] == 0x91 && code[7] == 0xC3)
	{
		fieldOffset = *(int32_t*)&code[3]; isSetter = true; detectedType = "void*";
	}
	// mov [rcx + disp8], edx ; ret
	else if (code[0] == 0x89 && code[1] == 0x51 && code[3] == 0xC3)
	{
		fieldOffset = (int)(int8_t)code[2]; isSetter = true; detectedType = "int32_t";
	}
	// mov [rcx + disp32], edx ; ret
	else if (code[0] == 0x89 && code[1] == 0x91 && code[6] == 0xC3)
	{
		fieldOffset = *(int32_t*)&code[2]; isSetter = true; detectedType = "int32_t";
	}

	if (fieldOffset >= 0)
	{
		FieldInfo* fi = c ? FindClassFieldByOffset(c, fieldOffset) : nullptr;
		char offsetHex[16];
		snprintf(offsetHex, sizeof(offsetHex), "0x%X", fieldOffset);

		char rawNameBuf[128] = {0};
		if (fi && SafeReadString((uintptr_t)fi->name, rawNameBuf, sizeof(rawNameBuf)) && rawNameBuf[0] != 0)
		{
			std::string rawName = rawNameBuf;
			std::string safeName = SanitizeMemberName(rawName);

			MemberTypeInfo* mti = (MemberTypeInfo*)SafeReadPointer((uintptr_t)&fi->typeInfo);
			if (IsValidPointer(mti))
			{
				TypeInfo* fti = (TypeInfo*)SafeReadPointer((uintptr_t)&mti->typeInfo);
				if (IsValidPointer(fti))
				{
					// Use the field's type only when it is returned in a register (scalar or pointer).
					// For structs, keep the register-sized type implied by the instruction: declaring a
					// struct return would make callers pass a hidden return buffer (wrong ABI).
					MemberDesc fieldDesc;
					if (ResolveMemberType(fti, fieldDesc) &&
						(fieldDesc.kind == MemberDesc::Primitive || fieldDesc.kind == MemberDesc::CString || fieldDesc.kind == MemberDesc::Pointer))
						detectedType = fieldDesc.cppType;
				}
			}

			if (isGetter)
			{
				info.name = "Get_" + safeName;
				info.returnType = detectedType;
				info.comment = "getter for " + rawName + " (offset " + offsetHex + ")";
				return info;
			}
			else if (isSetter)
			{
				info.name = "Set_" + safeName;
				info.returnType = "void";
				info.paramType = detectedType + " value";
				info.comment = "setter for " + rawName + " (offset " + offsetHex + ")";
				return info;
			}
		}
		else
		{
			if (isGetter)
			{
				info.name = "Get_field_" + std::string(offsetHex);
				info.returnType = detectedType;
				info.comment = "getter for offset " + std::string(offsetHex);
				return info;
			}
			else if (isSetter)
			{
				info.name = "Set_field_" + std::string(offsetHex);
				info.returnType = "void";
				info.paramType = detectedType + " value";
				info.comment = "setter for offset " + std::string(offsetHex);
				return info;
			}
		}
	}

	return info;
}

static bool IsRegisterReturnType(const std::string& t)
{
	static const std::set<std::string> scalars = {
		"bool", "float", "double", "int8_t", "int16_t", "int32_t", "int64_t",
		"uint8_t", "uint16_t", "uint32_t", "uint64_t", "const char*"
	};
	return scalars.count(t) != 0 || (!t.empty() && t.back() == '*');
}

void ClassInfoManager::DumpVTable(ClassInfo* c, std::ofstream& file)
{
	std::string className = ReadTypeName(c->typeInfo);
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
		std::string methodName = (i < (int)vti.methods.size() && !vti.methods[i].name.empty()) ? vti.methods[i].name : ("Func_" + std::to_string(i));
		std::string comment = (i < (int)vti.methods.size()) ? vti.methods[i].comment : "";

		file << "\t\tstatic constexpr uintptr_t " << methodName
			<< " = 0x" << std::hex << vti.entries[i] << "; // Module+0x"
			<< vti.entries[i] << std::dec;
		if (!comment.empty()) file << " (" << CommentSafe(comment) << ")";
		file << std::endl;
	}

	file << std::endl;
	file << "\t\tstatic void* GetEntry(void* instance, int index) {" << std::endl;
	file << "\t\t\treturn (*(void***)instance)[index];" << std::endl;
	file << "\t\t}" << std::endl;

	// Emit typed callers for discovered getters (register-returned types only, see AnalyzeVTableMethod)
	bool emittedCallers = false;
	for (int i = 0; i < vti.entryCount && i < (int)vti.methods.size(); i++)
	{
		const auto& mi = vti.methods[i];
		if (mi.name.rfind("Get_", 0) == 0 && mi.returnType != "void*" && IsRegisterReturnType(mi.returnType))
		{
			if (!emittedCallers) { file << std::endl << "\t\t// --- Discovered Virtual Callers ---" << std::endl; emittedCallers = true; }
			file << "\t\tstatic " << mi.returnType << " Call_" << mi.name << "(void* instance) {" << std::endl;
			file << "\t\t\ttypedef " << mi.returnType << "(__fastcall* tFunc)(void*);" << std::endl;
			file << "\t\t\treturn ((tFunc)GetEntry(instance, " << std::dec << i << "))(instance);" << std::endl;
			file << "\t\t}" << std::endl;
		}
	}

	file << "\t};" << std::endl;
}

// ============================================================================
// P3: VMT Hook Helper
// ============================================================================

void ClassInfoManager::DumpVMTHookHelper(ClassInfo* c, std::ofstream& file)
{
	std::string className = ReadTypeName(c->typeInfo);
	auto it = m_vtableMap.find(className);
	if (it == m_vtableMap.end() || it->second.entryCount == 0)
		return;

	// The implementations live once in FBSDKTypes.h; these only supply this class's vtable size.
	file << std::endl;
	file << "\t// --- VMT Hook Helpers (see fb::HookVFunc_InPlace / fb::HookVFunc_Shadow) ---" << std::endl;
	file << "\ttemplate<typename T>" << std::endl;
	file << "\tstatic T HookVFunc_InPlace(void* instance, int index, T newFunc) {" << std::endl;
	file << "\t\treturn fb::HookVFunc_InPlace(instance, index, newFunc);" << std::endl;
	file << "\t}" << std::endl << std::endl;

	file << "\ttemplate<typename T>" << std::endl;
	file << "\tstatic T HookVFunc_Shadow(void* instance, int index, T newFunc, int totalMethods = " << std::dec << it->second.entryCount << ") {" << std::endl;
	file << "\t\treturn fb::HookVFunc_Shadow(instance, index, newFunc, totalMethods);" << std::endl;
	file << "\t}" << std::endl << std::endl;

	file << "\ttemplate<typename T>" << std::endl;
	file << "\tstatic T HookVFunc(void* instance, int index, T newFunc) {" << std::endl;
	file << "\t\treturn fb::HookVFunc_InPlace(instance, index, newFunc);" << std::endl;
	file << "\t}" << std::endl;
}

// ============================================================================
// Offset Constants
// ============================================================================

void ClassInfoManager::DumpOffsetConstants(std::ofstream& file, const std::vector<MemberDesc>& members)
{
	if (members.empty()) return;
	file << "\tstruct Offsets {" << std::endl;
	for (const auto& d : members)
		file << "\t\tstatic constexpr size_t " << d.name << " = 0x" << std::hex << d.offset << std::dec << ";" << std::endl;
	file << "\t};" << std::endl;
}

// ============================================================================
// Getter/Setter Generation
// ============================================================================

void ClassInfoManager::DumpGetterSetters(std::ofstream& file, const std::vector<MemberDesc>& members)
{
	if (members.empty()) return;
	file << "\t// --- Accessors ---" << std::endl;

	for (const auto& d : members)
	{
		if (!d.declared)
			continue;

		const std::string& t = d.cppType;
		const std::string& m = d.name;
		const std::string& a = d.accessorName;

		switch (d.kind)
		{
		case MemberDesc::CString:
			file << "\t// The engine owns this string: a pointer passed to Set" << a << " must outlive its use by the game." << std::endl;
			[[fallthrough]];
		case MemberDesc::Primitive:
		case MemberDesc::Pointer:
			file << "\t" << t << " Get" << a << "() const { return m_" << m << "; }" << std::endl;
			file << "\tvoid Set" << a << "(" << t << " value) { m_" << m << " = value; }" << std::endl;
			break;
		case MemberDesc::Array:
			file << "\t" << t << "& Get" << a << "() { return m_" << m << "; }" << std::endl;
			file << "\tconst " << t << "& Get" << a << "() const { return m_" << m << "; }" << std::endl;
			break;
		case MemberDesc::Value:
			file << "\t" << t << "& Get" << a << "() { return m_" << m << "; }" << std::endl;
			file << "\tconst " << t << "& Get" << a << "() const { return m_" << m << "; }" << std::endl;
			file << "\tvoid Set" << a << "(const " << t << "& value) { m_" << m << " = value; }" << std::endl;
			break;
		case MemberDesc::Unknown:
			break; // raw bytes; use Offsets / fb::Read instead
		}
	}
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
		TypeInfo* ti = c ? (TypeInfo*)SafeReadPointer((uintptr_t)&c->typeInfo) : nullptr;
		if (!ti) continue;
		FieldInfo* fields = (FieldInfo*)SafeReadPointer((uintptr_t)&ti->fields);
		int fieldCount = SafeReadUInt16((uintptr_t)&ti->fieldCount);
		if (!fields || fieldCount <= 0 || fieldCount > 10000) continue;
		const std::string& ownerName = pair.first;

		// Walk class fields
		for (int i = 0; i < fieldCount; ++i)
		{
			FieldInfo fiCopy;
			if (!SafeReadBytes((uintptr_t)&fields[i], &fiCopy, sizeof(FieldInfo))) continue;

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
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\CrossReferences.h")) return;

	std::time_t result = std::time(0);
	file << "//" << std::endl;
	file << "// FrostbiteGen SDK - Cross-Reference Map" << std::endl;
	file << "// Shows which classes hold pointers to each type." << std::endl;
	file << "// Created: " << std::asctime(std::localtime(&result)) << "//" << std::endl << std::endl;

	for (auto& pair : m_crossRefMap)
	{
		file << "// " << CommentSafe(pair.first) << " is referenced by:" << std::endl;
		for (auto& ref : pair.second)
			file << "//   " << CommentSafe(ref.first) << "::" << CommentSafe(ref.second) << std::endl;
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
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\ClassHierarchy.h")) return;

	std::time_t result = std::time(0);
	file << "//" << std::endl;
	file << "// FrostbiteGen SDK - Class Hierarchy" << std::endl;
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
		file << "// " << CommentSafe(name);

		auto it = m_classMap.find(name);
		if (it != m_classMap.end() && it->second->typeInfo)
			file << " (size=0x" << std::hex << it->second->typeInfo->totalSize << std::dec << ")";

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
			TypeInfo* ati = (mti->enumFields && IsValidPointer(mti->enumFields)) ? *(TypeInfo**)mti->enumFields : nullptr;
			if (!IsValidPointer(ati)) return data;
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
		
		// Touch both strings inside the __try so unreadable names are rejected here.
		(void)*(volatile const char*)data.name;
		(void)*(volatile const char*)data.typeName;

		data.valid = true;
	} __except(1) {
		data.valid = false;
	}
	return data;
}

static FieldInfo* GetFieldsArray(TypeInfo* ti)
{
	if (!ti) return nullptr;
	if (ClassInfoManager::GetTypeKind(SafeReadUInt16((uintptr_t)&ti->flags)) == TypeKind::Struct)
		return (FieldInfo*)SafeReadPointer((uintptr_t)&ti->structFields);
	return (FieldInfo*)SafeReadPointer((uintptr_t)&ti->fields);
}

void ClassInfoManager::GenerateJSONSchema()
{
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\sdk.json")) return;

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

		file << "    \"" << EscapeJson(ti->name) << "\": {" << std::endl;
		file << "      \"size\": " << std::dec << ti->totalSize << "," << std::endl;
		file << "      \"typeInfoOffset\": \"0x" << std::hex << ((uintptr_t)c - m_moduleBase) << "\"," << std::endl;
		file << "      \"isDataContainer\": " << (c->isDataContainer ? "true" : "false") << "," << std::endl;

		// Parent
		if (c->parent && IsValidPointer(c->parent) && c->parent != c && IsValidPointer(c->parent->typeInfo) && c->parent->typeInfo && IsValidPointer((void*)c->parent->typeInfo->name) && c->parent->typeInfo->name)
			file << "      \"parent\": \"" << EscapeJson(c->parent->typeInfo->name) << "\"," << std::endl;
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
					<< ", \"field\": \"" << EscapeJson(travIt->second.steps[i].fieldName) << "\"}";
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

		if (GetTypeKind(ti->flags) == TypeKind::Enum)
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
					
					const char* mName = (fie->name && IsValidPointer(fie->name) ? fie->name : "unk");
					file << "{\"name\": \"" << EscapeJson(mName) << "\""
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

					file << "{\"name\": \"" << EscapeJson(fd.name) << "\""
						<< ", \"offset\": " << std::dec << fd.offset
						<< ", \"type\": \"" << EscapeJson(typeStr) << "\""
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

/// Reads a field's or enumerator's name for script output ("unk" if unreadable).
static std::string SafeName(const char* p, const char* fallback = "unk")
{
	char buf[256] = { 0 };
	if (p && SafeReadString((uintptr_t)p, buf, sizeof(buf)))
		return buf;
	return fallback;
}

void ClassInfoManager::GenerateIDAScript()
{
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\ida_import.py")) return;

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

	// Every string taken from game memory is emitted through PyStr(), never pasted raw.
	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		TypeInfo* ti = c ? (TypeInfo*)SafeReadPointer((uintptr_t)&c->typeInfo) : nullptr;
		if (!ti) continue;
		const std::string& typeName = pair.first;
		int fieldCount = SafeReadUInt16((uintptr_t)&ti->fieldCount);

		if (GetTypeKind(SafeReadUInt16((uintptr_t)&ti->flags)) == TypeKind::Enum)
		{
			file << "    create_enum(" << PyStr(typeName) << ", [" << std::endl;
			FieldInfoEnum* fields = (FieldInfoEnum*)SafeReadPointer((uintptr_t)&ti->enumFields);
			if (fields && fieldCount > 0 && fieldCount <= 10000)
			{
				for (int i = 0; i < fieldCount; ++i)
				{
					FieldInfoEnum e;
					if (!SafeReadBytes((uintptr_t)&fields[i], &e, sizeof(e))) continue;
					file << "        (" << PyStr(SafeName(e.name)) << ", " << std::dec << e.value << ")," << std::endl;
				}
			}
			file << "    ])" << std::endl;
		}
		else
		{
			FieldInfo* targetFields = GetFieldsArray(ti);
			if (fieldCount <= 0 || fieldCount > 10000 || !targetFields)
				continue;

			file << "    create_struct(" << PyStr(typeName) << ", 0x" << std::hex << SafeReadUInt16((uintptr_t)&ti->totalSize) << std::dec << ", [" << std::endl;

			for (int i = 0; i < fieldCount; ++i)
			{
				SafeFieldData fd = GetSafeFieldData(&targetFields[i]);
				if (!fd.valid) continue;

				std::string fn = GetFixedClassName(SafeName(fd.typeName).c_str());

				// Map to IDA flags
				std::string idaFlags = "idc.FF_BYTE";
				if (fn == "float")
					idaFlags = "idc.FF_FLOAT";
				else if (fn == "double")
					idaFlags = "idc.FF_DOUBLE";
				else if (fn == "int32_t" || fn == "uint32_t")
					idaFlags = "idc.FF_DWORD";
				else if (fn == "int16_t" || fn == "uint16_t")
					idaFlags = "idc.FF_WORD";
				else if (fn == "int64_t" || fn == "uint64_t" || fd.flags == kType_Pointer || fd.flags == kType_Array || fn == "const char*")
					idaFlags = "idc.FF_QWORD";

				file << "        (" << PyStr("m_" + SafeName(fd.name)) << ", 0x"
					<< std::hex << fd.offset << std::dec << ", " << fd.size << ", " << idaFlags << ")," << std::endl;
			}

			file << "    ])" << std::endl;
		}

		// Label the ClassInfo address
		uintptr_t relAddr = (uintptr_t)c - m_moduleBase;
		file << "    ida_name.set_name(idaapi.get_imagebase() + 0x" << std::hex << relAddr << std::dec
			<< ", " << PyStr("g_" + typeName + "_TypeInfo") << ", ida_name.SN_NOCHECK)" << std::endl;
		
		auto vtIt = m_vtableMap.find(typeName);
		if (vtIt != m_vtableMap.end() && vtIt->second.entryCount > 0)
		{
			for (int v = 0; v < vtIt->second.entryCount && v < (int)vtIt->second.entries.size(); ++v)
			{
				uintptr_t vfuncRva = vtIt->second.entries[v];
				if (vfuncRva > 0)
				{
					std::string methodName = (v < (int)vtIt->second.methods.size() && !vtIt->second.methods[v].name.empty()) ?
						vtIt->second.methods[v].name : ("vfunc_" + std::to_string(v));
					file << "    ida_name.set_name(idaapi.get_imagebase() + 0x" << std::hex << vfuncRva << std::dec
						<< ", " << PyStr(typeName + "__" + methodName) << ", ida_name.SN_NOCHECK)" << std::endl;
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
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\ghidra_import.py")) return;

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

	static const std::map<std::string, std::string> ghidraTypes = {
		{ "float", "FloatDataType.dataType" },
		{ "double", "DoubleDataType.dataType" },
		{ "bool", "BooleanDataType.dataType" },
		{ "int8_t", "SignedByteDataType.dataType" },
		{ "uint8_t", "ByteDataType.dataType" },
		{ "int16_t", "ShortDataType.dataType" },
		{ "uint16_t", "UnsignedShortDataType.dataType" },
		{ "int32_t", "IntegerDataType.dataType" },
		{ "uint32_t", "UnsignedIntegerDataType.dataType" },
		{ "int64_t", "LongLongDataType.dataType" },
		{ "uint64_t", "UnsignedLongLongDataType.dataType" },
	};

	// Every string taken from game memory is emitted through PyStr(), never pasted raw.
	for (auto& pair : m_classMap)
	{
		ClassInfo* c = pair.second;
		TypeInfo* ti = c ? (TypeInfo*)SafeReadPointer((uintptr_t)&c->typeInfo) : nullptr;
		if (!ti) continue;
		const std::string& typeName = pair.first;
		int fieldCount = SafeReadUInt16((uintptr_t)&ti->fieldCount);
		int totalSize = SafeReadUInt16((uintptr_t)&ti->totalSize);

		if (GetTypeKind(SafeReadUInt16((uintptr_t)&ti->flags)) == TypeKind::Enum)
		{
			file << "# Enum " << CommentSafe(typeName) << std::endl;
			file << "e = EnumDataType(cat, " << PyStr(typeName) << ", " << std::dec << (totalSize > 0 ? totalSize : 4) << ")" << std::endl;
			FieldInfoEnum* fields = (FieldInfoEnum*)SafeReadPointer((uintptr_t)&ti->enumFields);
			if (fields && fieldCount > 0 && fieldCount <= 10000)
			{
				for (int i = 0; i < fieldCount; ++i)
				{
					FieldInfoEnum e;
					if (!SafeReadBytes((uintptr_t)&fields[i], &e, sizeof(e))) continue;
					file << "e.add(" << PyStr(SafeName(e.name)) << ", " << std::dec << e.value << ")" << std::endl;
				}
			}
			file << "dtm.addDataType(e, DataTypeConflictHandler.REPLACE_HANDLER)" << std::endl;
			file << std::endl;
		}
		else
		{
			FieldInfo* targetFields = GetFieldsArray(ti);
			if (fieldCount <= 0 || fieldCount > 10000 || !targetFields) continue;

			file << "# " << CommentSafe(typeName) << " (0x" << std::hex << totalSize << " bytes)" << std::dec << std::endl;
			file << "s = StructureDataType(cat, " << PyStr(typeName) << ", 0x" << std::hex << totalSize << std::dec << ")" << std::endl;

			for (int i = 0; i < fieldCount; ++i)
			{
				SafeFieldData fd = GetSafeFieldData(&targetFields[i]);
				if (!fd.valid) continue;

				std::string fn = GetFixedClassName(SafeName(fd.typeName).c_str());

				std::string ghidraType = "ByteDataType.dataType";
				auto mapped = ghidraTypes.find(fn);
				if (fd.flags == kType_Pointer || fd.flags == kType_Array || fn == "const char*")
					ghidraType = "Pointer64DataType.dataType";
				else if (mapped != ghidraTypes.end())
					ghidraType = mapped->second;

				file << "s.replaceAtOffset(0x" << std::hex << fd.offset << std::dec << ", " << ghidraType
					<< ", " << fd.size << ", " << PyStr("m_" + SafeName(fd.name)) << ", '')" << std::endl;
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
		file << "addr = base_addr.add(0x" << std::hex << offset << std::dec << ")" << std::endl;
		file << "sym_table.createLabel(addr, " << PyStr(name) << ", SourceType.USER_DEFINED)" << std::endl;
	}
	
	file << "print('Applying VTable function labels...')" << std::endl;
	for (auto& pair : m_classMap)
	{
		auto vtIt = m_vtableMap.find(pair.first);
		if (vtIt != m_vtableMap.end() && vtIt->second.entryCount > 0)
		{
			for (int v = 0; v < vtIt->second.entryCount && v < (int)vtIt->second.entries.size(); ++v)
			{
				uintptr_t vfuncRva = vtIt->second.entries[v];
				if (vfuncRva > 0)
				{
					std::string methodName = (v < (int)vtIt->second.methods.size() && !vtIt->second.methods[v].name.empty()) ?
						vtIt->second.methods[v].name : ("vfunc_" + std::to_string(v));
					file << "addr = base_addr.add(0x" << std::hex << vfuncRva << std::dec << ")" << std::endl;
					file << "sym_table.createLabel(addr, " << PyStr(pair.first + "_" + methodName) << ", SourceType.USER_DEFINED)" << std::endl;
				}
			}
		}
	}
	
	file << "print('FrostbiteGen SDK imported: ' + str(" << std::dec << m_classMap.size() << ") + ' types, ' + str(" << m_globalInstances.size() << ") + ' singletons labeled')" << std::endl;

	file.close();
	Log("Generated ghidra_import.py");
}

// ============================================================================
// Utility Header Generation (FBSDKTypes.h)
// ============================================================================

void ClassInfoManager::GenerateFBSDKTypes()
{
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\FBSDKTypes.h")) return;

	std::time_t result = std::time(0);
	file << "//" << std::endl;
	file << "// FrostbiteGen SDK Runtime Utilities v2" << std::endl;
	file << "// Auto-generated" << std::endl;
	file << "// Created: " << std::asctime(std::localtime(&result)) << "//" << std::endl << std::endl;
	file << "#pragma once" << std::endl << std::endl;
	file << "#include <cstddef>" << std::endl;
	file << "#include <cstdint>" << std::endl;
	file << "#include <cstring>" << std::endl;
	file << "#include <limits>" << std::endl;
	file << "#include <type_traits>" << std::endl;
	file << "#include <initializer_list>" << std::endl;
	file << "#include <vector>" << std::endl;
	file << "#include <Windows.h>" << std::endl << std::endl;

	file << R"FBSDK(namespace fb {

inline uintptr_t GetModuleBase() {
	static uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
	return base;
}

inline bool IsValidPtr(void* ptr) {
	return ptr != nullptr && (uintptr_t)ptr > 0x10000 && (uintptr_t)ptr < 0x00007FFFFFFFFFFF;
}

/// Reads a T from game memory. Returns a value-initialized T if the address is unreadable.
template <typename T>
inline T Read(uintptr_t address) {
	static_assert(std::is_trivially_copyable_v<T>, "fb::Read<T> requires trivially copyable T");
	T buffer{};
	__try { memcpy(&buffer, (void*)address, sizeof(T)); } __except(EXCEPTION_EXECUTE_HANDLER) {}
	return buffer;
}

/// Writes a T to game memory. Silently does nothing if the address is invalid or unwritable.
template<typename T> inline void Write(uintptr_t address, const T& value) {
	static_assert(std::is_trivially_copyable_v<T>, "fb::Write<T> requires trivially copyable T");
	if (!IsValidPtr((void*)address)) return;
	__try { memcpy((void*)address, &value, sizeof(T)); }
	__except(EXCEPTION_EXECUTE_HANDLER) {}
}

template<typename T> inline T ReadPtr(uintptr_t address) {
	return Read<T>(address);
}

/// Follows a Cheat Engine-style pointer path and returns the final address (not dereferenced).
/// ReadChain(base, { a, b, c }) == [[[base] + a] + b] + c. Returns 0 if any pointer on the way is invalid.
inline uintptr_t ReadChain(uintptr_t base, std::initializer_list<uintptr_t> offsets) {
	uintptr_t address = base;
	for (uintptr_t offset : offsets) {
		address = Read<uintptr_t>(address);
		if (!IsValidPtr((void*)address)) return 0;
		address += offset;
	}
	return address;
}

namespace detail {
	// pattern[i] < 0 is a wildcard. Returns the first match in [start, end), or 0 (also on fault).
	inline uintptr_t ScanRegion(const int* pattern, size_t count, uintptr_t start, uintptr_t end) {
		if (end - start < count) return 0;
		__try {
			for (uintptr_t p = start; p <= end - count; p++) {
				size_t i = 0;
				while (i < count && (pattern[i] < 0 || ((const unsigned char*)p)[i] == (unsigned char)pattern[i])) i++;
				if (i == count) return p;
			}
		} __except(EXCEPTION_EXECUTE_HANDLER) {}
		return 0;
	}
}

/// Scans the game module for an IDA-style pattern ("48 8B 0D ?? ?? ?? ??").
/// offset:   byte offset from the match start to the value you want.
/// relative: if true, reads an int32 displacement at match+offset and returns match + offset + 4 + displacement.
/// Returns 0 if not found. Only committed, readable pages are scanned.
inline uintptr_t PatternScan(const char* pattern, int offset = 0, bool relative = false) {
	std::vector<int> bytes;
	for (const char* p = pattern; *p; ) {
		if (*p == ' ') { p++; continue; }
		if (*p == '?') { bytes.push_back(-1); p++; if (*p == '?') p++; continue; }
		char hex[3] = { p[0], p[1] ? p[1] : '\0', '\0' };
		bytes.push_back((int)strtoul(hex, nullptr, 16));
		p += p[1] ? 2 : 1;
	}
	if (bytes.empty()) return 0;

	uintptr_t base = GetModuleBase();
	auto dos = (const IMAGE_DOS_HEADER*)base;
	auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	uintptr_t end = base + nt->OptionalHeader.SizeOfImage;

	for (uintptr_t address = base; address < end; ) {
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi))) break;
		uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (regionEnd <= address) break;
		bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
			(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
		if (readable) {
			uintptr_t match = detail::ScanRegion(bytes.data(), bytes.size(), address, regionEnd < end ? regionEnd : end);
			if (match) {
				uintptr_t target = match + offset;
				return relative ? target + 4 + Read<int32_t>(target) : target;
			}
		}
		address = regionEnd;
	}
	return 0;
}

/// Swaps one entry of a class's shared vtable (affects every instance of that class).
/// Returns the original function, or a null T if the vtable could not be made writable.
template<typename T>
inline T HookVFunc_InPlace(void* instance, int index, T newFunc) {
	if (!instance || index < 0) return T{};
	void** vtable = *(void***)instance;
	DWORD oldProtect;
	if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) return T{};
	T original = reinterpret_cast<T>(vtable[index]);
	vtable[index] = reinterpret_cast<void*>(newFunc);
	VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
	return original;
}

/// Gives one object its own copy of its vtable with entry `index` replaced.
/// totalMethods must be at least the real number of virtual functions; the RTTI slot before the
/// table is copied too. The copy is intentionally never freed (the object keeps using it).
/// Returns the original function, or a null T if index is out of range.
template<typename T>
inline T HookVFunc_Shadow(void* instance, int index, T newFunc, int totalMethods) {
	if (!instance || totalMethods <= 0 || index < 0 || index >= totalMethods) return T{};
	void** originalVTable = *(void***)instance;
	void** block = new void*[totalMethods + 1];
	memcpy(block, originalVTable - 1, sizeof(void*) * (totalMethods + 1));
	void** shadowVTable = block + 1;
	T original = reinterpret_cast<T>(originalVTable[index]);
	shadowVTable[index] = reinterpret_cast<void*>(newFunc);
	*(void***)instance = shadowVTable;
	return original;
}

template <typename T>
class Array {
private:
	T* m_firstElement;
	T* m_lastElement;
	T* m_arrayBound;
	void* m_allocator;
public:
	T& At(uint32_t i) { return m_firstElement[i]; }
	T& operator[](uint32_t i) { return m_firstElement[i]; }
	T* begin() { return m_firstElement; }
	T* end() { return m_lastElement; }
	uint32_t Count() const { return (uint32_t)(m_lastElement - m_firstElement); }
};

template <typename T>
class WeakPtr {
private:
	T** m_ptr;
public:
	T* Get() const { return m_ptr ? *m_ptr : nullptr; }
	T* operator->() const { return Get(); }
	bool IsValid() const { return m_ptr && *m_ptr; }
};

class String {
private:
	char* m_str;
public:
	const char* c_str() const { return m_str ? m_str : ""; }
};

} // namespace fb
)FBSDK";

	file.close();
}


void ClassInfoManager::GenerateForwardDeclarations()
{
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\FBClasses.h")) return;

	file << "#pragma once" << std::endl << std::endl;
	file << "#include <cstdint>" << std::endl << std::endl;
	for (auto& decl : m_declaredTypes)
		file << decl.second << ";" << std::endl;
	file << std::endl;

	file.close();
	Log("Generated FBClasses.h (%d declarations)", (int)m_declaredTypes.size());
}

void ClassInfoManager::GenerateSDKMasterHeader()
{
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\SDK.h")) return;

	file << "#pragma once" << std::endl << std::endl;
	file << "#include \"FBSDKTypes.h\"" << std::endl;
	file << "#include \"FBClasses.h\"" << std::endl;
	if (m_cvarHeaderWritten)
		file << "#include \"ConsoleVariables.h\"" << std::endl;
	file << std::endl;

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
        // Singleton pointers and static singletons are written at runtime, so only
        // writable sections can hold them (skips .rdata, which is mostly vtables and strings).
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE))
            continue;

        uintptr_t start = m_moduleBase + section->VirtualAddress;
        uintptr_t end = start + section->Misc.VirtualSize;

        for (uintptr_t ptr = start; ptr + 8 <= end; ptr += 8)
        {
            void* obj = SafeReadPointer(ptr);
            if (obj && !IsModulePointer(obj, m_moduleBase, m_moduleEnd))
            {
                ClassInfo* id = TryIdentifyClassByVTable((uintptr_t)obj, false);
                std::string name = id ? ReadTypeName(id->typeInfo) : "";
                if (!name.empty() && m_globalInstances.find(name) == m_globalInstances.end())
                {
                    m_globalInstances[name] = ptr - m_moduleBase;
                    Log("  Found global singleton (heap): %s at Module+0x%llX", name.c_str(), ptr - m_moduleBase);
                    found++;
                    if (!m_vtableMap.count(name))
                        CaptureVTable(name, obj, id);
                    
                    // Add to traversal map as a root!
                    if (m_traversalMap.find(name) == m_traversalMap.end())
                    {
                        TraversalChain rootChain;
                        rootChain.targetClass = name;
                        rootChain.resolvedAddress = (uintptr_t)obj;
                        rootChain.rootClassName = name;
                        rootChain.rootGlobalOffset = ptr - m_moduleBase;
                        m_traversalMap[name] = rootChain;
                        
                        std::vector<TraversalStep> chain;
                        std::set<uintptr_t> visited;
                        visited.insert((uintptr_t)obj);
                        BuildTraversalMap((uintptr_t)obj, id, 1, chain, visited, ptr - m_moduleBase, name, false);
                    }
                }
            }
            else if (obj && IsModulePointer(obj, m_moduleBase, m_moduleEnd))
            {
                // ptr itself might be an embedded static singleton in .data whose vtable is obj
                ClassInfo* id = TryIdentifyClassByVTable(ptr, false);
                std::string name = id ? ReadTypeName(id->typeInfo) : "";
                if (!name.empty() && m_globalInstances.find(name) == m_globalInstances.end())
                {
                    m_globalInstances[name] = ptr - m_moduleBase;
                    m_staticGlobals.insert(name);
                    Log("  Found static global singleton: %s at Module+0x%llX", name.c_str(), ptr - m_moduleBase);
                    found++;
                    if (!m_vtableMap.count(name))
                        CaptureVTable(name, (void*)ptr, id);
                    
                    if (m_traversalMap.find(name) == m_traversalMap.end())
                    {
                        TraversalChain rootChain;
                        rootChain.targetClass = name;
                        rootChain.resolvedAddress = ptr;
                        rootChain.rootClassName = name;
                        rootChain.rootGlobalOffset = ptr - m_moduleBase;
                        rootChain.rootIsStatic = true;
                        m_traversalMap[name] = rootChain;
                        
                        std::vector<TraversalStep> chain;
                        std::set<uintptr_t> visited;
                        visited.insert(ptr);
                        BuildTraversalMap(ptr, id, 1, chain, visited, ptr - m_moduleBase, name, true);
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



void ClassInfoManager::DumpLiveInstances()
{
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\LiveDump.json")) return;

	file << "{" << std::endl;
	
	bool firstSingleton = true;
	for (auto& pair : m_globalInstances)
	{
		uintptr_t addr = pair.second + m_moduleBase; // pair.second is an offset!
		if (!IsValidPointer((void*)addr)) continue;
		
		// A static singleton is the object itself; otherwise the global holds a pointer to it.
		uintptr_t instance = addr;
		if (!m_staticGlobals.count(pair.first))
		{
			if (!SafeReadBytes(addr, &instance, sizeof(uintptr_t))) continue;
			if (!IsValidPointer((void*)instance)) continue;
		}
		
		if (!firstSingleton) file << "," << std::endl;
		file << "  \"" << EscapeJson(pair.first) << "\": \"0x" << std::hex << instance << "\"";
		firstSingleton = false;
	}
	
	file << std::endl << "}" << std::endl;
	file.close();
	Log("Generated LiveDump.json");
}

void ClassInfoManager::GenerateCheatEngineTable()
{
	std::ofstream file;
	if (!OpenOutput(file, "SDK\\CheatEngineTable.CT")) return;

	file << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
	file << "<CheatTable CheatEngineTableVersion=\"45\">\n";
	file << "  <CheatEntries/>\n";
	file << "  <UserdefinedSymbols/>\n";
	file << "  <Structures StructVersion=\"2\">\n";

	for (auto it = m_classMap.rbegin(); it != m_classMap.rend(); ++it)
	{
		ClassInfo* c = it->second;
		if (!c || !IsValidPointer(c) || !IsValidPointer(c->typeInfo)) continue;
		TypeInfo* ti = c->typeInfo;
		
		char nameBuf[256] = {0};
		if (!ti || !IsValidPointer((void*)ti->name) || !SafeReadString((uintptr_t)ti->name, nameBuf, sizeof(nameBuf)) || nameBuf[0] == 0) continue;

		std::string className = EscapeXml(GetSanitizedClassName(nameBuf));
		file << "    <Structure Name=\"" << className << "\" AutoFill=\"0\" AutoCreate=\"1\" DefaultHex=\"0\" AutoDestroy=\"0\" DoNotSaveLocal=\"0\" RLECompression=\"1\" AutoCreateStructsize=\"4096\">\n";
		file << "      <Elements>\n";

		std::vector<FieldInfo*> members;
		FieldInfo* fields = GetFieldsArray(ti);
		if (IsValidPointer(fields))
		{
			int fieldCount = SafeReadUInt16((uintptr_t)&ti->fieldCount);
			if (fieldCount > 0 && fieldCount <= 10000)
			{
				for (int i = 0; i < fieldCount; ++i)
				{
					FieldInfo* fi = &fields[i];
					if (SafeValidateFieldInfo(fi))
						members.push_back(fi);
				}
				auto cmp = [](const FieldInfo* a, const FieldInfo* b) { return a->offset < b->offset; };
				std::sort(members.begin(), members.end(), cmp);
			}
		}
		
		for (FieldInfo* field : members)
		{
			if (!IsValidPointer(field)) continue;

			unsigned short fieldOffset = SafeReadUInt16((uintptr_t)&field->offset);

			char fieldBuf[256] = {0};
			std::string rawFieldName = (IsValidPointer((void*)field->name) && SafeReadString((uintptr_t)field->name, fieldBuf, sizeof(fieldBuf)) && fieldBuf[0] != 0) ? fieldBuf : ("unk_" + std::to_string(fieldOffset));
			std::string fieldName = EscapeXml(rawFieldName);

			int size = field->GetFieldSize();
			std::string ceType = "4 Bytes";
			if (size == 1) ceType = "Byte";
			else if (size == 2) ceType = "2 Bytes";
			else if (size == 4) ceType = "4 Bytes";
			else if (size == 8) ceType = "8 Bytes";
			else ceType = "Array of byte";

			if (IsValidPointer(field->typeInfo) && IsValidPointer(field->typeInfo->typeInfo))
			{
				unsigned short flags = SafeReadUInt16((uintptr_t)&field->typeInfo->typeInfo->flags);
				std::string fieldTypeName = ReadTypeName(field->typeInfo->typeInfo);
				if (flags == kType_Pointer)
					ceType = "Pointer";
				else if (fieldTypeName == "Float32")
					ceType = "Float";
				else if (fieldTypeName == "Float64")
					ceType = "Double";
			}

			// Floats would be shown as meaningless integers with an integer display method
			const char* displayMethod = (ceType == "Float" || ceType == "Double") ? "" : " DisplayMethod=\"Unsigned Integer\"";
			file << "        <Element Offset=\"" << fieldOffset << "\" Vartype=\"" << ceType << "\" Bytesize=\"" << size << "\" OffsetHex=\"" << std::hex << std::uppercase << fieldOffset << std::nouppercase << std::dec << "\" Description=\"" << fieldName << "\"" << displayMethod << "/>\n";
		}

		file << "      </Elements>\n";
		file << "    </Structure>\n";
	}

	file << "  </Structures>\n";
	file << "</CheatTable>\n";
	file.close();

	Log("Generated CheatEngineTable.CT");
}

void ClassInfoManager::DumpConsoleVariables()
{
	Log("=== Phase 6.1: Scanning and dumping Console Variables (CVars) ===");

	m_cvarList.clear();
	std::map<std::string, ConsoleVariableInfo> uniqueCVars;

	// ------------------------------------------------------------------------
	// Source 1: Frostbite Settings Singletons & Reflection
	// In Frostbite, console variables are structured under Settings classes
	// (e.g. UISettings -> UI.DrawEnable, WorldRenderSettings -> WorldRender.SkyEnable).
	// With 160+ settings classes and 140+ live singletons, this provides thousands
	// of verified, typed CVars with live runtime memory values.
	// ------------------------------------------------------------------------
	int settingsCVarCount = 0;
	for (auto& pair : m_classMap)
	{
		std::string className = pair.first;
		ClassInfo* c = pair.second;
		if (!c || !c->typeInfo) continue;

		// Filter for settings classes
		if (className.find("Settings") == std::string::npos) continue;

		// Extract CVar prefix: "UISettings" -> "UI", "WorldRenderSettings" -> "WorldRender"
		std::string prefix = className;
		if (prefix.length() > 8 && prefix.substr(prefix.length() - 8) == "Settings")
		{
			prefix = prefix.substr(0, prefix.length() - 8);
		}
		if (prefix.length() > 4 && prefix.substr(prefix.length() - 4) == "Base")
		{
			prefix = prefix.substr(0, prefix.length() - 4);
		}
		if (prefix.empty()) prefix = className;

		// Check for live singleton instance
		uintptr_t instanceAddr = 0;
		uintptr_t globalOffset = 0;
		bool isHeap = false;

		auto git = m_globalInstances.find(className);
		if (git != m_globalInstances.end())
		{
			globalOffset = git->second;
			uintptr_t gAddr = m_moduleBase + globalOffset;
			void* ptrVal = SafeReadPointer(gAddr);
			if (ptrVal && !IsModulePointer(ptrVal, m_moduleBase, m_moduleEnd))
			{
				instanceAddr = (uintptr_t)ptrVal;
				isHeap = true;
			}
			else
			{
				instanceAddr = gAddr;
				isHeap = false;
			}
		}
		else
		{
			auto tit = m_traversalMap.find(className);
			if (tit != m_traversalMap.end())
			{
				instanceAddr = tit->second.resolvedAddress;
			}
		}

		// Gather members (including inheritance)
		std::vector<ClassInfo*> parents = GetParents(c);
		std::vector<FieldInfo*> allMembers;
		for (ClassInfo* p : parents)
		{
			if (p && p->typeInfo)
				ParseClassMembers(p->typeInfo, allMembers);
		}
		ParseClassMembers(c->typeInfo, allMembers);

		for (FieldInfo* field : allMembers)
		{
			if (!IsValidPointer(field)) continue;

			unsigned short fieldOffset = SafeReadUInt16((uintptr_t)&field->offset);
			char nameBuf[128] = {0};
			if (!IsValidPointer((void*)field->name) || !SafeReadString((uintptr_t)field->name, nameBuf, sizeof(nameBuf)) || !nameBuf[0])
				continue;

			std::string fieldName = nameBuf;
			// Strip 'm_' prefix if present
			std::string cvarField = fieldName;
			if (cvarField.rfind("m_", 0) == 0 && cvarField.length() > 2)
				cvarField = cvarField.substr(2);
			else if (cvarField.rfind("m", 0) == 0 && cvarField.length() > 1 && isupper((unsigned char)cvarField[1]))
				cvarField = cvarField.substr(1);

			std::string cvarName = prefix + "." + cvarField;

			// Determine C++ type string. ConsoleVariables.h only sees FBSDKTypes.h, so anything
			// that is not a primitive is exposed as untyped memory (the engine type goes in the comment).
			std::string typeStr = "void";
			std::string rawType;
			MemberTypeInfo* fieldMti = (MemberTypeInfo*)SafeReadPointer((uintptr_t)&field->typeInfo);
			TypeInfo* fieldType = fieldMti ? (TypeInfo*)SafeReadPointer((uintptr_t)&fieldMti->typeInfo) : nullptr;
			if (fieldType)
			{
				rawType = ReadTypeName(fieldType);
				const char* prim = MapPrimitiveName(rawType.c_str());
				if (SafeReadUInt16((uintptr_t)&fieldType->flags) == kType_Pointer)
					typeStr = "void*"; // the member holds a pointer
				else if (prim)
					typeStr = prim;
				else if (rawType == "String")
					typeStr = "fb::String";
			}

			// Read live value if instance is available
			std::string liveVal = "";
			if (instanceAddr && IsValidPointer((void*)instanceAddr))
			{
				uintptr_t fieldAddr = instanceAddr + fieldOffset;
				if (typeStr == "bool")
				{
					uint8_t bv = 0;
					if (SafeReadBytes(fieldAddr, &bv, 1))
						liveVal = (bv != 0) ? "true" : "false";
				}
				else if (typeStr == "float")
				{
					float fv = 0.0f;
					if (SafeReadBytes(fieldAddr, &fv, 4))
					{
						char fBuf[32];
						sprintf_s(fBuf, "%g", fv);
						liveVal = fBuf;
					}
				}
				else if (typeStr == "double")
				{
					double dv = 0.0;
					if (SafeReadBytes(fieldAddr, &dv, 8))
					{
						char dBuf[32];
						sprintf_s(dBuf, "%g", dv);
						liveVal = dBuf;
					}
				}
				else if (typeStr == "int32_t")
				{
					int32_t iv = 0;
					if (SafeReadBytes(fieldAddr, &iv, 4))
						liveVal = std::to_string(iv);
				}
				else if (typeStr == "int64_t")
				{
					int64_t iv = 0;
					if (SafeReadBytes(fieldAddr, &iv, 8))
						liveVal = std::to_string(iv);
				}
				else if (typeStr == "uint64_t")
				{
					uint64_t uv = 0;
					if (SafeReadBytes(fieldAddr, &uv, 8))
						liveVal = std::to_string(uv);
				}
				else if (typeStr == "uint32_t")
				{
					uint32_t uv = 0;
					if (SafeReadBytes(fieldAddr, &uv, 4))
						liveVal = std::to_string(uv);
				}
				else if (typeStr == "int16_t")
				{
					int16_t iv = 0;
					if (SafeReadBytes(fieldAddr, &iv, 2))
						liveVal = std::to_string(iv);
				}
				else if (typeStr == "uint16_t")
				{
					uint16_t uv = 0;
					if (SafeReadBytes(fieldAddr, &uv, 2))
						liveVal = std::to_string(uv);
				}
				else if (typeStr == "int8_t")
				{
					int8_t iv = 0;
					if (SafeReadBytes(fieldAddr, &iv, 1))
						liveVal = std::to_string((int)iv);
				}
				else if (typeStr == "uint8_t")
				{
					uint8_t uv = 0;
					if (SafeReadBytes(fieldAddr, &uv, 1))
						liveVal = std::to_string((unsigned int)uv);
				}
				else if (typeStr == "const char*")
				{
					void* strP = SafeReadPointer(fieldAddr);
					if (strP && IsValidPointer(strP))
					{
						char sBuf[128] = {0};
						if (SafeReadString((uintptr_t)strP, sBuf, sizeof(sBuf)))
							liveVal = sBuf;
					}
				}
			}

			ConsoleVariableInfo cvi;
			cvi.name = cvarName;
			cvi.description = "Member of " + className + " (" + fieldName + (rawType.empty() ? "" : " : " + rawType) + ")";
			cvi.settingsClass = className;
			cvi.memberOffset = fieldOffset;
			cvi.globalOffset = globalOffset;
			cvi.isHeap = isHeap;
			cvi.typeStr = typeStr;
			cvi.liveValue = liveVal;
			cvi.rva = (!isHeap && globalOffset > 0) ? (globalOffset + fieldOffset) : 0;

			uniqueCVars[cvarName] = cvi;
			settingsCVarCount++;
		}
	}
	Log("Extracted %d Console Variables from Settings reflection (%d unique)", settingsCVarCount, (int)uniqueCVars.size());

	// ------------------------------------------------------------------------
	// Source 2: Dynamic Engine Command & CVar Linked-List Nodes Scan
	// Scan memory sections for registered commands and anchor nodes.
	// ------------------------------------------------------------------------
	PIMAGE_DOS_HEADER dosH = (PIMAGE_DOS_HEADER)m_moduleBase;
	PIMAGE_NT_HEADERS ntH = (PIMAGE_NT_HEADERS)(m_moduleBase + dosH->e_lfanew);
	PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(ntH);

	std::vector<std::pair<uintptr_t, uintptr_t>> dataSections;
	std::vector<std::pair<uintptr_t, uintptr_t>> readableSections;

	for (WORD i = 0; i < ntH->FileHeader.NumberOfSections; i++, sec++)
	{
		uintptr_t start = m_moduleBase + sec->VirtualAddress;
		uintptr_t end = start + sec->Misc.VirtualSize;
		if (end <= start) continue;

		bool isData = (sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) ||
		              (sec->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA);

		if (isData)
			dataSections.push_back({start, end});

		if (sec->Characteristics & IMAGE_SCN_MEM_READ)
			readableSections.push_back({start, end});
	}

	// Verified Frostbite anchor strings for commands/cvars
	const char* anchors[] = {
		"UI.DrawEnable",
		"WorldRender.SkyEnable",
		"DebugCam.ToggleToFreeCamera",
		"demo.pausePlayback",
		"demo.playPlayback",
		"demo.rewindPlayback",
		"demo.showFrameNumber",
		"Debug.DrawScreenCenterHelper",
		"EmitterSystem.QuadEnableRendering",
		"EmitterSystem.MeshRenderingEnable",
		nullptr
	};

	std::set<uintptr_t> visitedNodes;
	auto extractNode = [&](uintptr_t node, int nameOff, int descOff, int dataOff, int nextOff, int prevOff) {
		std::vector<uintptr_t> queue;
		queue.push_back(node);

		while (!queue.empty() && visitedNodes.size() < 4000)
		{
			uintptr_t curr = queue.back();
			queue.pop_back();

			if (!curr || visitedNodes.count(curr)) continue;
			visitedNodes.insert(curr);

			// Read name
			void* namePtr = SafeReadPointer(curr + nameOff);
			if (!namePtr || !IsValidPointer(namePtr)) continue;
			char nameBuf[128] = {0};
			if (!SafeReadString((uintptr_t)namePtr, nameBuf, sizeof(nameBuf)) || strlen(nameBuf) < 2) continue;

			// Filter out non-printable names
			bool hasAlpha = false;
			for (int c = 0; nameBuf[c]; c++) {
				if (isalpha((unsigned char)nameBuf[c])) { hasAlpha = true; break; }
			}
			if (!hasAlpha) continue;

			// Read description
			char descBuf[256] = {0};
			if (descOff >= 0)
			{
				void* descPtr = SafeReadPointer(curr + descOff);
				if (descPtr && IsValidPointer(descPtr))
				{
					SafeReadString((uintptr_t)descPtr, descBuf, sizeof(descBuf));
				}
			}

			// Read data pointer
			void* dataPtr = (dataOff >= 0) ? SafeReadPointer(curr + dataOff) : nullptr;
			uintptr_t dataAddr = (uintptr_t)dataPtr;

			ConsoleVariableInfo cvi;
			cvi.name = nameBuf;
			cvi.description = descBuf;
			cvi.rva = (dataAddr >= m_moduleBase && dataAddr < m_moduleEnd) ? (dataAddr - m_moduleBase) : 0;
			cvi.typeStr = (dataAddr != 0) ? "bool" : "command";
			cvi.liveValue = "";

			if (dataAddr && IsValidPointer((void*)dataAddr))
			{
				uint8_t bVal = 0;
				if (SafeReadBytes(dataAddr, &bVal, 1))
				{
					cvi.liveValue = (bVal != 0) ? "1" : "0";
					cvi.typeStr = (bVal <= 1) ? "bool" : "int32_t";
				}
			}

			// If not already discovered via settings reflection or has more info, add it
			if (uniqueCVars.find(cvi.name) == uniqueCVars.end())
			{
				uniqueCVars[cvi.name] = cvi;
			}

			// Enqueue next
			if (nextOff >= 0)
			{
				void* nextPtr = SafeReadPointer(curr + nextOff);
				if (nextPtr && IsValidPointer(nextPtr) && !visitedNodes.count((uintptr_t)nextPtr))
					queue.push_back((uintptr_t)nextPtr);
			}

			// Enqueue prev
			if (prevOff >= 0)
			{
				void* prevPtr = SafeReadPointer(curr + prevOff);
				if (prevPtr && IsValidPointer(prevPtr) && !visitedNodes.count((uintptr_t)prevPtr))
					queue.push_back((uintptr_t)prevPtr);
			}
		}
	};

	// Try each anchor string in readable sections
	for (int a = 0; anchors[a]; a++)
	{
		const char* target = anchors[a];
		size_t targetLen = strlen(target) + 1;

		// Search for the string including its terminator, only in committed readable pages.
		uintptr_t strAddr = 0;
		for (auto& secPair : readableSections)
		{
			for (const auto& run : GetReadableRuns(secPair.first, secPair.second))
			{
				strAddr = SafeFindBytes(run.first, run.second, target, targetLen);
				if (strAddr) break;
			}
			if (strAddr) break;
		}

		if (!strAddr) continue;

		// Scan data sections for pointers to this anchor string
		for (auto& secPair : dataSections)
		{
			uintptr_t dStart = secPair.first;
			uintptr_t dEnd = secPair.second;

			for (uintptr_t p = dStart; p + 8 <= dEnd; p += 8)
			{
				uintptr_t val = (uintptr_t)SafeReadPointer(p);
				if (val == strAddr)
				{
					// Layout 1: next @ 0x00, prev @ 0x08, name @ 0x10, desc @ 0x18, data @ 0x20
					if (p >= dStart + 0x10)
						extractNode(p - 0x10, 0x10, 0x18, 0x20, 0x00, 0x08);

					// Layout 2: next @ 0x00, name @ 0x08, desc @ 0x10, data @ 0x18
					if (p >= dStart + 0x08)
						extractNode(p - 0x08, 0x08, 0x10, 0x18, 0x00, -1);

					// Layout 3: name @ 0x00, next @ 0x08, desc @ 0x10, data @ 0x18
					extractNode(p, 0x00, 0x10, 0x18, 0x08, -1);
				}
			}
		}
	}

	for (auto& pair : uniqueCVars)
	{
		m_cvarList.push_back(pair.second);
	}

	Log("Total Discovered Console Variables / Commands: %d", (int)m_cvarList.size());

	// ------------------------------------------------------------------------
	// Output 1: Generate SDK/ConsoleVariables.h
	// ------------------------------------------------------------------------
	std::ofstream hFile;
	if (OpenOutput(hFile, "SDK\\ConsoleVariables.h"))
	{
		std::set<std::string> usedIdentifiers;
		hFile << "// FrostbiteGen SDK - Discovered Engine Console Variables (CVars)\n";
		hFile << "#pragma once\n";
		hFile << "#include <cstdint>\n";
		hFile << "#include \"FBSDKTypes.h\"\n\n";
		hFile << "namespace fb {\n";
		hFile << "namespace CVars {\n\n";

		for (auto& cvar : m_cvarList)
		{
			std::string baseIdentifier = cvar.name;
			for (char& ch : baseIdentifier)
			{
				if (!isalnum((unsigned char)ch)) ch = '_';
			}
			if (baseIdentifier.empty() || isdigit((unsigned char)baseIdentifier[0])) baseIdentifier = "_" + baseIdentifier;

			// "A.B" and "A_B" sanitize to the same identifier; keep each one unique.
			std::string sanitized = baseIdentifier;
			for (int k = 2; !usedIdentifiers.insert(sanitized).second; k++)
				sanitized = baseIdentifier + "_" + std::to_string(k);

			hFile << "\t// " << CommentSafe(cvar.name);
			if (!cvar.description.empty())
				hFile << " - " << CommentSafe(cvar.description);
			if (!cvar.liveValue.empty())
				hFile << " [Live Value: " << CommentSafe(cvar.liveValue) << "]";
			hFile << "\n";

			if (cvar.globalOffset > 0)
			{
				if (cvar.isHeap)
				{
					hFile << "\tinline " << cvar.typeStr << "* GetPtr_" << sanitized << "() {\n";
					hFile << "\t\tuintptr_t inst = fb::Read<uintptr_t>(fb::GetModuleBase() + 0x" << std::hex << cvar.globalOffset << ");\n";
					hFile << "\t\tif (!inst) return nullptr;\n";
					hFile << "\t\treturn (" << cvar.typeStr << "*)(inst + 0x" << std::hex << cvar.memberOffset << ");\n";
					hFile << "\t}\n\n";
				}
				else
				{
					hFile << "\tinline " << cvar.typeStr << "* GetPtr_" << sanitized << "() {\n";
					hFile << "\t\treturn (" << cvar.typeStr << "*)(fb::GetModuleBase() + 0x" << std::hex << (cvar.globalOffset + cvar.memberOffset) << ");\n";
					hFile << "\t}\n\n";
				}
			}
			else if (cvar.rva > 0)
			{
				hFile << "\tstatic constexpr uintptr_t " << sanitized << "_RVA = 0x" << std::hex << cvar.rva << ";\n";
				hFile << "\tinline " << cvar.typeStr << "* GetPtr_" << sanitized << "() {\n";
				hFile << "\t\treturn (" << cvar.typeStr << "*)(fb::GetModuleBase() + " << sanitized << "_RVA);\n";
				hFile << "\t}\n\n";
			}
			else
			{
				hFile << "\t// (no static global pointer found)\n\n";
			}
		}

		hFile << "} // namespace CVars\n";
		hFile << "} // namespace fb\n";
		hFile.close();
		m_cvarHeaderWritten = true;
		Log("Generated ConsoleVariables.h");
	}

	// ------------------------------------------------------------------------
	// Output 2: Generate SDK/CVars.json
	// ------------------------------------------------------------------------
	std::ofstream jFile;
	if (OpenOutput(jFile, "SDK\\CVars.json"))
	{
		jFile << "{\n";
		bool first = true;
		for (auto& cvar : m_cvarList)
		{
			if (!first) jFile << ",\n";
			jFile << "  \"" << EscapeJson(cvar.name) << "\": {\n";
			jFile << "    \"description\": \"" << EscapeJson(cvar.description) << "\",\n";
			jFile << "    \"type\": \"" << EscapeJson(cvar.typeStr) << "\",\n";
			jFile << "    \"liveValue\": \"" << EscapeJson(cvar.liveValue) << "\",\n";
			jFile << "    \"settingsClass\": \"" << EscapeJson(cvar.settingsClass) << "\",\n";
			jFile << "    \"memberOffset\": \"0x" << std::hex << cvar.memberOffset << "\",\n";
			jFile << "    \"globalOffset\": \"0x" << std::hex << cvar.globalOffset << "\",\n";
			jFile << "    \"isHeap\": " << (cvar.isHeap ? "true" : "false") << ",\n";
			jFile << "    \"rva\": \"0x" << std::hex << cvar.rva << "\"\n";
			jFile << "  }";
			first = false;
		}
		jFile << "\n}\n";
		jFile.close();
		Log("Generated CVars.json");
	}
}

