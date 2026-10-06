// Stage 1 of tests/run_sdk_test.sh: runs the real generator against the fake engine.
#include "../required.h"
#include "../structs.h"
#include "../classinfo.h"
#include "fake_engine.h"

extern char g_szLogFile[MAX_PATH];

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
		return 2;
	}
	snprintf(g_szBaseDir, sizeof(g_szBaseDir), "%s/", argv[1]);
	snprintf(g_szLogFile, MAX_PATH, "%s/fbgen.txt", argv[1]);

	fake::Engine engine;
	engine.Build();

	bool hadErrors;
	{
		ClassInfoManager manager(engine.head);
		manager.BuildClassList();
		manager.DumpClasses();
		printf("%s\n", manager.GetSummary().c_str());
		hadErrors = manager.HasErrors();
	}
	CloseLog();
	return hadErrors ? 1 : 0;
}
