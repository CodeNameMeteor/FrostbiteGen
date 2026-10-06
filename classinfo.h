#ifndef CLASSINFO_H
#define CLASSINFO_H

#include "required.h"
#include "structs.h"

// ============================================================================
// Traversal types - describes the path from ClientGameContext to an instance
// ============================================================================

/// One step in a traversal chain (one pointer dereference).
struct TraversalStep
{
	int offset;              // byte offset within the parent object
	std::string fieldName;   // field name (e.g., "PlayerManager")
	std::string typeName;    // declared target type name
};

/// Full chain from ClientGameContext or a global singleton to a specific class instance.
struct TraversalChain
{
	std::string targetClass;             // class we're reaching
	std::string rootClassName;           // root class name (e.g., "ClientGameContext" or "ResourceManager")
	uintptr_t rootGlobalOffset = 0;      // module-relative offset of the root singleton (pointer slot, or the object itself if rootIsStatic)
	bool rootIsStatic = false;           // true if the root object lives inside the module image (no dereference needed)
	std::vector<TraversalStep> steps;    // steps from root singleton
	uintptr_t resolvedAddress = 0;       // live instance address (at generation time)
};

/// Discovered virtual method metadata
struct VTableMethodInfo
{
	int index;
	uintptr_t rva;
	std::string name;        // e.g. "GetType", "Get_m_health", "Set_m_health", "Stub_Return0"
	std::string returnType;  // e.g. "TypeInfo*", "float", "void", "bool"
	std::string paramType;   // e.g. "", "float value", "bool value"
	std::string comment;     // e.g. "field offset 0x140", "empty stub"
};

/// VTable information captured from a live instance.
struct VTableInfo
{
	int entryCount;
	std::vector<uintptr_t> entries;          // module-relative addresses of each entry
	std::vector<VTableMethodInfo> methods;   // analyzed method signatures
};

/// Discovered engine console variable / command
struct ConsoleVariableInfo
{
	std::string name;
	std::string description;
	uintptr_t rva = 0;              // Module RVA of the value pointer / node (if static)
	std::string typeStr;            // "bool", "int", "float", "command", etc.
	std::string liveValue;          // e.g. "1", "60.0", "true"
	std::string settingsClass;      // e.g. "UISettings"
	uintptr_t globalOffset = 0;     // RVA of singleton pointer in module
	uintptr_t memberOffset = 0;     // offset inside instance
	bool isHeap = false;            // true if accessed via *(fb::GetModuleBase() + globalOffset)
};

/// How a reflected type is emitted.
enum class TypeKind
{
	Enum,
	Struct,
	Class,
	Primitive,  // engine data types (Boolean, Float32, ...) - logged, not emitted
	Skip        // known-unsupported kinds
};

/// One reflected member, resolved once and shared by every emitter so that member
/// declarations, offsets, accessors, snapshots and layout checks always agree.
struct MemberDesc
{
	enum Kind
	{
		Primitive,  // bool, float, int32_t, ...
		CString,    // const char*
		Pointer,    // T* (or void* when T is not emitted)
		Array,      // fb::Array<T>
		Value,      // by-value enum/struct/class that has its own header
		Unknown     // type not emitted - declared as raw bytes
	};

	FieldInfo* fi = nullptr;
	std::string rawName;      // engine field name (for comments)
	std::string name;         // unique, sanitized C++ identifier
	std::string accessorName; // name used for Get/Set accessors (avoids GetInstance/GetTypeInfo clashes)
	std::string cppType;      // C++ type used in the declaration
	std::string engineType;   // engine type name (for comments)
	std::string includeType;  // header needed for a by-value member ("" if none)
	Kind kind = Unknown;
	int offset = 0;
	int size = 0;
	bool declared = false;    // set by DumpClassMembers: false if the member could not be placed
};

// ============================================================================
// ClassInfoManager
// ============================================================================

class ClassInfoManager
{
public:
	ClassInfoManager(ClassInfo* info);

	void BuildClassList();
	void DumpClasses();
	void DumpLiveInstances();

	/// True if any output file could not be written or any type faulted while dumping.
	bool HasErrors() const { return m_filesFailed > 0 || m_typesFailed > 0; }
	/// One-line description of what was generated, for the log and the final dialog.
	std::string GetSummary() const;

	static std::string SanitizeMemberName(const std::string& orig);
	static bool IsCppKeyword(const std::string& s);
	static bool IsValidTypeName(const std::string& s);
	static std::string EscapeXml(const std::string& s);
	static std::string EscapeJson(const std::string& s);
	static std::string PyStr(const std::string& s);
	static std::string CommentSafe(const std::string& s);
	static std::string FormatFloatLiteral(double v, bool isFloat);
	static const char* MapPrimitiveName(const char* engineName);
	static TypeKind GetTypeKind(unsigned short flags);

private:
	typedef void (ClassInfoManager::*DumpFn)(ClassInfo*);

	// --- Existing Methods ---
	std::vector<ClassInfo*> GetParents(ClassInfo* c);
	void	DumpClass(ClassInfo* c);
	int		DumpClassMembers(std::ofstream& file, std::vector<MemberDesc>& members, int parentSize);
	void	ParseClassMembers(TypeInfo* ti, std::vector<FieldInfo*>& members);
	const char*	GetFixedClassName(const char* orig);
	std::string GetSanitizedClassName(const char* orig);
	void	ResolveHeaders(const std::vector<MemberDesc>& members, std::ofstream& file);
	std::vector<MemberDesc> BuildMemberDescs(const std::vector<FieldInfo*>& members);
	bool	ResolveMemberType(TypeInfo* mti, MemberDesc& d);

	void	DumpEnum(ClassInfo* c);
	void	DumpEnumMembers(std::ofstream& file, TypeInfo* ti);
	std::string GetEnumUnderlyingType(TypeInfo* ti);

	void	DumpStruct(ClassInfo* c);
	void	ParseStructMembers(TypeInfo* ti, std::vector<FieldInfo*>& members);

	void	DumpTypeInfo(ClassInfo* c, std::ofstream& file);
	void	DumpHeader(std::ofstream& file, const char* fileName);
	void	DumpOffsetConstants(std::ofstream& file, const std::vector<MemberDesc>& members);
	void	DumpGetterSetters(std::ofstream& file, const std::vector<MemberDesc>& members);
	void	DumpLayoutChecks(std::ofstream& file, const std::string& className, int totalSize, const std::vector<MemberDesc>& members);

	/// Runs one per-type dump function, turning an access violation into a skipped type instead of a game crash.
	bool	GuardedDump(DumpFn fn, ClassInfo* c);
	/// Opens an output file under the DLL directory, tracking success/failure counts.
	bool	OpenOutput(std::ofstream& file, const std::string& relPath);
	/// Reads a type's name safely; returns "" if unreadable.
	static std::string ReadTypeName(TypeInfo* ti);
	/// Records every type that will get a header, with its forward declaration.
	void	BuildDeclaredTypes();
	bool	IsDeclaredType(const std::string& name) const { return m_declaredTypes.count(name) != 0; }

	// --- P0: Instance Resolution (ClientGameContext & singleton traversal) ---

	/// Pattern-scans for the ClientGameContext singleton pointer.
	/// Tries multiple known FB3 patterns and validates each match.
	/// Returns the live ClientGameContext* address, or 0 on failure.
	void ScanGlobalsForSingletons();
	uintptr_t FindClientGameContext();

	/// Recursively walks pointer fields starting from a known object,
	/// building the traversal map (class name -> chain of offsets).
	void BuildTraversalMap(uintptr_t instanceAddr, ClassInfo* classInfo,
		int depth, std::vector<TraversalStep>& currentChain,
		std::set<uintptr_t>& visited,
		uintptr_t rootGlobalOffset = 0, const std::string& rootClassName = "ClientGameContext",
		bool rootIsStatic = false);

	/// Walks every 8-byte offset in an object blindly (no ClassInfo needed).
	/// For each valid sub-pointer, tries to identify the class via vtable
	/// analysis, then recurses with normal BuildTraversalMap if identified.
	void BlindTraversalWalk(uintptr_t objectAddr, int depth,
		std::vector<TraversalStep>& currentChain,
		std::set<uintptr_t>& visited,
		uintptr_t rootGlobalOffset = 0, const std::string& rootClassName = "ClientGameContext",
		bool rootIsStatic = false);

	/// Tries to identify a live object's class by analyzing its vtable.
	/// Prefers an exact GetType() shape (lea/mov rax,[rip+x]; ret); falls back to
	/// any RIP-relative reference to a known TypeInfo. Results are cached per vtable.
	ClassInfo* TryIdentifyClassByVTable(uintptr_t instanceAddr, bool verbose = false);

	/// Records vtable entries + analyzed method names for a live instance.
	void CaptureVTable(const std::string& className, void* instance, ClassInfo* ci);

	/// Emits GetInstance() that uses fb::Read to traverse from
	/// ClientGameContext or discovered global singleton.
	void DumpInstanceResolver(ClassInfo* c, std::ofstream& file);

	// --- P1: Pointer Chain Documentation ---

	/// Emits a comment block documenting the exact offset chain from
	/// the root singleton to this class's instance.
	void DumpTraversalChainComment(ClassInfo* c, std::ofstream& file);

	// --- P1: Default Value Snapshots ---

	/// Reads live member values from the instance found during traversal,
	/// and emits a Defaults struct with captured values.
	void DumpDefaultValues(ClassInfo* c, std::ofstream& file,
		const std::vector<MemberDesc>& members);

	// --- P2: VTable Dumping ---

	/// Captures vtable entries from a live instance and emits a VTable struct.
	void DumpVTable(ClassInfo* c, std::ofstream& file);

	// --- P2: Class Hierarchy Tree ---
	void GenerateHierarchyTree();

	// --- P2: JSON Schema Export ---
	void GenerateJSONSchema();

	// --- P3: IDA Pro Script ---
	void GenerateIDAScript();

	// --- P3: Ghidra Script ---
	void GenerateGhidraScript();

	// --- P3: VMT Hook Helpers ---
	void DumpVMTHookHelper(ClassInfo* c, std::ofstream& file);

	// --- P3: Cross-Reference Map ---
	void BuildCrossRefMap();
	void GenerateCrossRefFile();

	// --- Utility Header Generation ---
	void GenerateFBSDKTypes();
	void GenerateSDKMasterHeader();
	void GenerateForwardDeclarations();
	void GenerateBonusOutputs();

	// --- P4: Additional Bonus Outputs ---
	void GenerateCheatEngineTable();
	void DumpConsoleVariables();

	// --- Micro-Disassembly Function Analysis ---
	VTableMethodInfo AnalyzeVTableMethod(ClassInfo* c, int index, uintptr_t funcAddr);

private:
	ClassInfo* m_listHead;
	std::map<std::string, ClassInfo*, std::greater<std::string>> m_classMap;

	/// Reverse map: TypeInfo address -> ClassInfo* (for vtable identification).
	std::map<uintptr_t, ClassInfo*> m_typeInfoToClassMap;

	/// Cache: vtable address -> identified class (nullptr if unidentifiable).
	std::map<uintptr_t, ClassInfo*> m_vtableIdCache;

	/// Module base address of the game exe.
	uintptr_t m_moduleBase;
	uintptr_t m_moduleEnd;

	/// Resolved ClientGameContext instance address (0 if not found).
	uintptr_t m_clientGameCtxInstance;

	/// Module-relative offset of the ClientGameContext** global pointer.
	uintptr_t m_clientGameCtxGlobalOffset;

	/// True if the root object is stored inside the module (offset is the object, not a pointer to it).
	bool m_rootIsStatic;

	/// True if the root object's class was confirmed from its vtable (not just guessed by name).
	bool m_rootVerified;

	/// Traversal map: class name -> chain from ClientGameContext.
	std::map<std::string, TraversalChain> m_traversalMap;

	/// Global singletons: class name -> module-relative offset.
	std::map<std::string, uintptr_t> m_globalInstances;
	/// Names in m_globalInstances whose offset is the object itself (static) rather than a pointer slot.
	std::set<std::string> m_staticGlobals;

	/// VTable map: class name -> captured vtable info.
	std::map<std::string, VTableInfo> m_vtableMap;

	/// Discovered console variables and commands.
	std::vector<ConsoleVariableInfo> m_cvarList;
	bool m_cvarHeaderWritten;

	/// Cross-reference map: class name -> list of (referencing class, field name).
	std::map<std::string, std::vector<std::pair<std::string, std::string>>> m_crossRefMap;

	/// Names of all generated SDK header files (for SDK.h).
	std::vector<std::string> m_sdkFileNames;

	/// Types that get a header: name -> forward declaration (e.g. "class Foo", "enum Bar : int32_t").
	std::map<std::string, std::string> m_declaredTypes;

	/// Lower-cased output paths already written (Windows file names are case-insensitive).
	std::set<std::string> m_writtenPaths;

	/// Bookkeeping for the final summary.
	int m_filesWritten;
	int m_filesFailed;
	int m_typesFailed;
	bool m_loggedArraySize;
};

#endif
