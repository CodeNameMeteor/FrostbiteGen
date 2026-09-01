#ifndef CLASSINFO_H
#define CLASSINFO_H

#include "required.h"
#include "structs.h"

// ============================================================================
// Traversal types â€” describes the path from ClientGameContext to an instance
// ============================================================================

/// One step in a traversal chain (one pointer dereference).
struct TraversalStep
{
	int offset;              // byte offset within the parent object
	std::string fieldName;   // field name (e.g., "PlayerManager")
	std::string typeName;    // declared target type name
};

/// Full chain from ClientGameContext to a specific class instance.
struct TraversalChain
{
	std::string targetClass;             // class we're reaching
	std::vector<TraversalStep> steps;    // steps from ClientGameContext
	uintptr_t resolvedAddress;           // live instance address (at generation time)
};

/// VTable information captured from a live instance.
struct VTableInfo
{
	int entryCount;
	std::vector<uintptr_t> entries;      // module-relative addresses of each entry
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

private:
	// --- Existing Methods (unchanged) ---
	std::vector<ClassInfo*> GetParents(ClassInfo* c);
	void	DumpClass(ClassInfo* c);
	int		DumpClassMembers(std::ofstream& file, std::vector<FieldInfo*>& members, int parentSize);
	void	ParseClassMembers(TypeInfo* ti, std::vector<FieldInfo*>& members);
	char*	GetFixedClassName(const char* orig);
	std::string GetSanitizedClassName(const char* orig);
	void	ResolveHeaders(std::vector<FieldInfo*> members, std::ofstream& file);

	void	DumpEnum(ClassInfo* c);
	void	DumpEnumMembers(std::ofstream& file, TypeInfo* ti);

	void	DumpStruct(ClassInfo* c);
	void	ParseStructMembers(TypeInfo* ti, std::vector<FieldInfo*>& members);

	void	DumpTemplateClass(ClassInfo* c);

	void	DumpTypeInfo(ClassInfo* c, std::ofstream& file);
	void	DumpHeader(std::ofstream& file, const char* fileName);
	void	DumpOffsetConstants(std::ofstream& file, std::vector<FieldInfo*>& members);
	void	DumpGetterSetters(std::ofstream& file, std::vector<FieldInfo*>& members, TypeInfo* ti);

	// --- P0: Instance Resolution (ClientGameContext traversal) ---

	/// Pattern-scans for the ClientGameContext singleton pointer.
	/// Tries multiple known FB3 patterns and validates each match.
	/// Returns the live ClientGameContext* address, or 0 on failure.
	void ScanGlobalsForSingletons();
	uintptr_t FindClientGameContext();

	/// Recursively walks pointer fields starting from a known object,
	/// building the traversal map (class name â†’ chain of offsets).
	void BuildTraversalMap(uintptr_t instanceAddr, ClassInfo* classInfo,
		int depth, std::vector<TraversalStep>& currentChain,
		std::set<uintptr_t>& visited);

	/// Walks every 8-byte offset in an object blindly (no ClassInfo needed).
	/// For each valid sub-pointer, tries to identify the class via vtable
	/// analysis, then recurses with normal BuildTraversalMap if identified.
	void BlindTraversalWalk(uintptr_t objectAddr, int depth,
		std::vector<TraversalStep>& currentChain,
		std::set<uintptr_t>& visited);

	/// Tries to identify a live object's class by analyzing its vtable.
	/// Checks vtable entries for GetType()-style functions that reference
	/// a known TypeInfo address. Returns the ClassInfo* if found, nullptr otherwise.
	ClassInfo* TryIdentifyClassByVTable(uintptr_t instanceAddr, bool verbose = false);

	/// Emits GetInstance() that uses fb::ReadChain to traverse from
	/// ClientGameContext. Replaces the old ClassInfo-probing approach.
	void DumpInstanceResolver(ClassInfo* c, std::ofstream& file);

	// --- P1: Pointer Chain Documentation ---

	/// Emits a comment block documenting the exact offset chain from
	/// ClientGameContext to this class's instance.
	void DumpTraversalChainComment(ClassInfo* c, std::ofstream& file);

	// --- P1: Default Value Snapshots ---

	/// Reads live member values from the instance found during traversal,
	/// and emits a Defaults struct with captured values.
	void DumpDefaultValues(ClassInfo* c, std::ofstream& file,
		std::vector<FieldInfo*>& members);

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

private:
	ClassInfo* m_listHead;
	std::map<std::string, ClassInfo*, std::greater<std::string>> m_classMap;

	/// Reverse map: TypeInfo address â†’ ClassInfo* (for vtable identification).
	std::map<uintptr_t, ClassInfo*> m_typeInfoToClassMap;

	/// Module base address of the game exe.
	uintptr_t m_moduleBase;
	uintptr_t m_moduleEnd;

	/// Resolved ClientGameContext instance address (0 if not found).
	uintptr_t m_clientGameCtxInstance;

	/// Module-relative offset of the ClientGameContext** global pointer.
	uintptr_t m_clientGameCtxGlobalOffset;

	/// Traversal map: class name â†’ chain from ClientGameContext.
	std::map<std::string, TraversalChain> m_traversalMap;
	std::map<std::string, uintptr_t> m_globalInstances;

	/// VTable map: class name â†’ captured vtable info.
	std::map<std::string, VTableInfo> m_vtableMap;

	/// Cross-reference map: class name â†’ list of (referencing class, field name).
	std::map<std::string, std::vector<std::pair<std::string, std::string>>> m_crossRefMap;

	/// Names of all generated SDK header files (for SDK.h).
	std::vector<std::string> m_sdkFileNames;

	/// Forward-declaration set.
	std::set<std::string> m_forwardDecls;
};

#endif
