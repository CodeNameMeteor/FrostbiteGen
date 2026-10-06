#!/usr/bin/env bash
# End-to-end test of the SDK generator, runnable on Linux (no game, no MSVC needed).
#
#  1. Builds the generator against tests/stubs/Windows.h and runs it on a fake engine
#     (tests/fake_engine.h) that exercises 64-bit fields, arrays, enums, structs, static
#     singletons, duplicate/hostile names, NaN snapshots and a 26-entry vtable.
#  2. Compiles the generated SDK with layout static_asserts enabled and runs runtime checks
#     against a fresh copy of the same fake engine.
#  3. Checks the generated IDA/Ghidra scripts parse and that hostile names stayed inside strings.
#
# Usage: tests/run_sdk_test.sh [build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/sdk-test}"
CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wno-unused-function -Wno-unknown-pragmas -I "$ROOT/tests/stubs")

rm -rf "$BUILD"
mkdir -p "$BUILD/out/SDK"

echo "== Stage 1: build generator and generate SDK"
"$CXX" "${CXXFLAGS[@]}" -o "$BUILD/generate_sdk" \
	"$ROOT/tests/generate_sdk.cpp" "$ROOT/classinfo.cpp" "$ROOT/main.cpp"
set +e
"$BUILD/generate_sdk" "$BUILD/out"
status=$?
set -e

# The generator writes "SDK\Name.h" (Windows separators); move them into SDK/.
shopt -s nullglob
for f in "$BUILD/out/SDK\\"*; do
	mv "$f" "$BUILD/out/SDK/${f##*\\}"
done
shopt -u nullglob

if [ $status -ne 0 ]; then
	echo "Generator reported errors; log:" >&2
	cat "$BUILD/out/fbgen.txt" >&2
	exit 1
fi

echo "== Stage 2: compile generated SDK with layout checks and run runtime checks"
"$CXX" "${CXXFLAGS[@]}" -Wno-invalid-offsetof -DFBGEN_VERIFY_LAYOUT -I "$BUILD/out/SDK" \
	-o "$BUILD/sdk_runtime_check" "$ROOT/tests/sdk_runtime_check.cpp"
"$BUILD/sdk_runtime_check"

echo "== Stage 3: generated scripts"
python3 - "$BUILD/out/SDK" <<'EOF'
import ast, sys, pathlib
sdk = pathlib.Path(sys.argv[1])
for name in ("ida_import.py", "ghidra_import.py"):
    tree = ast.parse((sdk / name).read_text())
    calls = [n.func.id for n in ast.walk(tree) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)]
    assert "__import__" not in calls, f"{name}: engine string escaped its literal"
    print(f"{name}: parses, hostile names stayed inside string literals")

import json, xml.dom.minidom
for name in ("sdk.json", "CVars.json", "LiveDump.json"):
    json.loads((sdk / name).read_text())
    print(f"{name}: valid JSON")
xml.dom.minidom.parse(str(sdk / "CheatEngineTable.CT"))
print("CheatEngineTable.CT: valid XML")
EOF

for f in "$BUILD/out/SDK/"*.h; do
	if grep -q "Evil" "$f" && ! grep -q "^//\|// " "$f"; then
		echo "Unexpected hostile name in $f" >&2
		exit 1
	fi
done
if ls "$BUILD/out/SDK" | grep -q "Evil"; then
	echo "A header was generated for a non-identifier type name" >&2
	exit 1
fi

echo "All SDK generation tests passed"
