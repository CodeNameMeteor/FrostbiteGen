// Compiles the generated SDK (with layout static_asserts) and instantiates its templates.
#include "SDK.h"

typedef void (*VFunc)();

int main()
{
	// Nothing here dereferences game memory: a null instance is rejected by the helpers.
	(void)fb::Read<int>(0);
	(void)fb::ReadChain(0, { 0x10 });
	(void)fb::HookVFunc_InPlace<VFunc>(nullptr, 0, nullptr);
	(void)ClientGameContext::HookVFunc_Shadow<VFunc>(nullptr, 0, nullptr);
	return 0;
}
