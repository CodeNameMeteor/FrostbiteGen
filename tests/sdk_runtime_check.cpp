// Stage 2 of tests/run_sdk_test.sh: compiles the generated SDK (with layout static_asserts
// enabled) and checks that its resolvers and accessors reach the right objects in a freshly
// built copy of the same fake engine.
#include "../required.h"
#include "../structs.h"
#include "fake_engine.h"
#include "SDK.h"

static int g_failures = 0;

#define CHECK(cond) \
	do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

typedef void (*VFunc)();
static void Replacement() {}

int main()
{
	fake::Engine e;
	e.Build();

	// GetInstance() resolvers: heap global, traversal, array traversal, static singleton
	CHECK((void*)ClientGameContext::GetInstance() == e.ctx);
	CHECK((void*)PlayerManager::GetInstance() == e.pm);
	CHECK((void*)TestLevel::GetInstance() == e.level);
	CHECK((void*)TestEntity::GetInstance() == e.entity0);
	CHECK((void*)StaticManager::GetInstance() == e.staticObject);
	CHECK((void*)TestChild::GetInstance() == e.child);
	CHECK((void*)TestSettings::GetInstance() == e.settings);

	// Layout: typed members land on the engine's bytes
	TestEntity* entity = TestEntity::GetInstance();
	if (entity)
	{
		CHECK(entity->GetHealth() == 100.0f);
		CHECK(entity->m_Counter == 0x1122334455667788ULL);
		CHECK((void*)entity->GetOwner() == e.pm);
		CHECK(entity->GetInstanceField() == 0);   // field named "Instance" does not clash with GetInstance()
	}
	PlayerManager* pm = PlayerManager::GetInstance();
	if (pm)
	{
		CHECK(pm->GetMaxPlayers() == 64);
		CHECK(pm->GetBig() == INT64_MIN);
		CHECK(pm->GetRatio() == 1.0f);
	}
	StaticManager* sm = StaticManager::GetInstance();
	if (sm)
		CHECK(sm->GetValue() == 7);

	// Snapshot values
	CHECK(PlayerManager::Defaults::Ratio == 1.0f);
	CHECK(PlayerManager::Defaults::Big == INT64_MIN);
	CHECK(TestLevel::Defaults::Gravity != TestLevel::Defaults::Gravity); // NaN

	// Offsets / enums
	CHECK(TestEntity::Offsets::Counter == 0x38);
	CHECK(sizeof(SmallEnum) == 1);
	CHECK((TE_A | TE_B) == TE_B);

	// Runtime helpers
	CHECK(fb::ReadChain((uintptr_t)(e.module + fake::kRootGlobal), { 0x10 }) == (uintptr_t)e.ctx + 0x10);
	CHECK(fb::ReadChain((uintptr_t)(e.module + fake::kRootGlobal), { 0x10, 0x10 }) == (uintptr_t)e.pm + 0x10);
	CHECK(fb::PatternScan("48 8B 0D ?? ?? ?? ?? 48 85 C9 74 05", 3, true) == (uintptr_t)(e.module + fake::kRootGlobal));
	CHECK(fb::PatternScan("DE AD BE EF 00 11 22 33 44") == 0);

	// Hook helpers: out-of-range index is rejected; the shadow table keeps the RTTI slot
	CHECK(ClientGameContext::VTable::EntryCount == 26);
	CHECK(fb::HookVFunc_Shadow((void*)e.ctx, 26, &Replacement, 26) == nullptr);
	void** before = *(void***)e.ctx;
	VFunc original = ClientGameContext::HookVFunc_Shadow((void*)e.ctx, 3, &Replacement);
	void** after = *(void***)e.ctx;
	CHECK((void*)original == before[3]);
	CHECK(after != before && after[3] == (void*)&Replacement && after[-1] == before[-1] && after[25] == before[25]);

	if (g_failures)
	{
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("All runtime checks passed\n");
	return 0;
}
