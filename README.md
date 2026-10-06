# FrostbiteGen SDK Generator

A C++ SDK generator for Frostbite 3 engine games (Mirror's Edge Catalyst). Injects into the running game, reflects the engine's type system, and produces ready-to-use C++ headers with **automatic memory address resolution** — no manual reverse engineering required.

---

## Table of Contents

1. [Building FrostbiteGen](#building-frostbitegen)
2. [Generating the SDK](#generating-the-sdk)
3. [What Gets Generated](#what-gets-generated)
4. [Using the SDK in Your Project](#using-the-sdk-in-your-project)
5. [Examples](#examples)
   - [God Mode](#example-1-god-mode)
   - [Unlimited Ammo](#example-2-unlimited-ammo)
   - [Reading Game Settings](#example-3-reading-game-settings)
   - [Walking Pointer Chains](#example-4-walking-pointer-chains)
   - [Custom Pattern Scanning](#example-5-custom-pattern-scanning)
   - [Using Offset Constants](#example-6-using-offset-constants-for-manual-access)
6. [SDK Architecture](#sdk-architecture)
7. [Troubleshooting](#troubleshooting)
8. [Running the Tests](#running-the-tests)

---

## Building FrostbiteGen

### Requirements

- **Visual Studio 2022** (or Build Tools with MSVC v143+)
- **CMake 3.15+**
- **Windows 10/11 x64**

### Build Steps

```powershell
# Clone or copy the source files into a directory
cd FrostbiteGen

# Configure (x64 is required — Frostbite 3 is 64-bit)
cmake -B build -A x64

# Build Release
cmake --build build --config Release
```

The output DLL is at `build/Release/FrostbiteGen.dll`.

---

## Generating the SDK

> **Warning: offline / single-player use only.** FrostbiteGen also recognises Battlefield 4, Battlefield 1 and Star Wars Battlefront II, which are online games with anti-cheat. Injecting any DLL into them can get your account banned. Use it at your own risk.

### Step 1: Launch Mirror's Edge Catalyst

Start the game and wait until you reach the main menu (or are in-game). The type system must be fully initialized before injection.

### Step 2: Inject the DLL

Use any DLL injector to load `FrostbiteGen.dll` into the game process:

- [System Informer](https://systeminformer.sourceforge.io/) (formerly Process Hacker)
- [Xenos Injector](https://github.com/DarthTon/Xenos)
- Or any x64-compatible injector

```text
Target Process: MirrorsEdgeCatalyst.exe
DLL Path:       <your_path>\build\Release\FrostbiteGen.dll
```

### Step 3: Wait for Generation

A message box appears in front of the game when generation finishes, with a sound. It shows how many files were written, whether anything failed, and where the output and the log are. If anything failed, the title reads **SDK generated with errors** and `fbgen.txt` lists each problem.

### Step 4: Collect the Output

The generated SDK is written to an `SDK` folder next to the DLL, and the log to `fbgen.txt` beside it. The folder contains:

- `FBSDKTypes.h`: runtime utilities in namespace `fb` (always include this)
- `FBClasses.h`: forward declarations of every generated type
- `SDK.h`: master include (includes everything)
- One header per class, struct and enum, for example `GameSettings.h` or `ClientGameContext.h`
- `ConsoleVariables.h` and `CVars.json`: console variables discovered from `*Settings` classes
- `sdk.json` (all types as JSON) and `LiveDump.json` (live singleton addresses)
- `ida_import.py` and `ghidra_import.py`: import types and labels into IDA Pro or Ghidra
- `CheatEngineTable.CT`: Cheat Engine structure definitions
- `ClassHierarchy.h` and `CrossReferences.h`: reference comments

```text
build/Release/
├── FrostbiteGen.dll
├── fbgen.txt              ← generation log
└── SDK/
    ├── FBSDKTypes.h        ← runtime utilities (always include this)
    ├── FBClasses.h         ← forward declarations of all types
    ├── SDK.h               ← master include (includes everything)
    ├── GameSettings.h      ← example: game settings class
    ├── ClientGameContext.h
    ├── ...                 ← hundreds of generated headers
    └── ida_import.py, ghidra_import.py, sdk.json, CheatEngineTable.CT, ...
```

`fbgen.txt` shows how the root object and singletons were found:

```text
[14:25:28] FrostbiteGen SDK Generator starting...
[14:25:28] Module base: 0x0000000140000000
[14:25:28] ClassInfo head: 0x00000001428109E0
[14:25:28]   Root context class (verified by vtable): ClientGameContext
[14:25:29]   Found global singleton (heap): GameSettings at Module+0x2401D10
[14:25:30] SDK generation complete: 1903 files written, 0 files failed, 0 types skipped after a fault
```

---

## What Gets Generated

Every generated class header includes these features:

### 1. ASLR-Safe Type Info

```cpp
static void* GetTypeInfo()
{
    // Module-relative address of the type's ClassInfo record - works across game restarts
    return (void*)(fb::GetModuleBase() + 0x28109E0);
}
```

### 2. Automatic Instance Resolution

For every class that was reachable while the SDK was generated, `GetInstance()` replays how it was found, either from a global singleton or by following pointers from the root context:

```cpp
static GameSettings* GetInstance()
{
    // Global singleton found dynamically
    uintptr_t ctx = fb::Read<uintptr_t>(fb::GetModuleBase() + 0x2401D10); // global GameSettings
    if (!ctx) return nullptr;
    return reinterpret_cast<GameSettings*>(ctx);
}
```

Classes that were not reachable get a `GetInstance()` that returns `nullptr`, with a comment explaining why.

### 3. Named Offset Constants

```cpp
struct Offsets {
    static constexpr size_t MaxPlayerCount = 0x20;
    static constexpr size_t IsGodMode = 0xde;
    static constexpr size_t IsJesusMode = 0xdf;
    // ...
};
```

### 4. Typed Getter/Setter Accessors

```cpp
bool GetIsGodMode() const { return m_IsGodMode; }
void SetIsGodMode(bool value) { m_IsGodMode = value; }
```

Engine integer types map to fixed-width types (`Int64` → `int64_t`, `Uint8` → `uint8_t`, …). Members whose type is not in the SDK are declared as raw bytes (`unsigned char m_X[size]`) so the layout stays correct. A field named `Instance` or `TypeInfo` gets `GetInstanceField()` / `GetTypeInfoField()` so it doesn't clash with the static functions.

### 5. Memory Layout Matching the Engine

Members are laid out at the offsets the engine reports, with explicit padding, so you can cast a raw pointer to the class type and access members directly. Each header also contains `static_assert`s that check every member offset and the class size against the engine's values. Turn them on by defining `FBGEN_VERIFY_LAYOUT` in your project; any mismatch then fails the build instead of corrupting game memory.

### 6. Snapshots, VTables and Hook Helpers

- `Defaults`: values read from the live instance **at generation time** (not engine defaults).
- `VTable`: module-relative addresses of virtual functions, with names for recognised getters and setters.
- `HookVFunc_InPlace` / `HookVFunc_Shadow`: thin wrappers around `fb::HookVFunc_InPlace` / `fb::HookVFunc_Shadow` that pass this class's vtable size.

---

## Using the SDK in Your Project

### Step 1: Create a New DLL Project

Create a new Visual Studio DLL project (or use CMake):

```
MyMECMod/
├── CMakeLists.txt
├── SDK/                  ← copy the entire generated SDK folder here
│   ├── FBSDKTypes.h
│   ├── SDK.h
│   ├── GameSettings.h
│   └── ...
└── main.cpp
```

### Step 2: CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.15)
project(MyMECMod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(MyMECMod SHARED main.cpp)

# Include the SDK directory
target_include_directories(MyMECMod PRIVATE ${CMAKE_SOURCE_DIR}/SDK)

if(MSVC)
    target_compile_definitions(MyMECMod PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    target_compile_options(MyMECMod PRIVATE /EHa)  # run destructors when the SDK's __try/__except catches a fault
endif()
```

### Step 3: Include the SDK

You have two options:

**Option A — Include everything:**
```cpp
#include "SDK.h"  // pulls in ALL generated headers
```

**Option B — Include only what you need (faster compilation):**
```cpp
#include "FBSDKTypes.h"      // always required
#include "GameSettings.h"     // specific class you want
```

### Step 4: Write Your Mod

```cpp
#include <Windows.h>
#include "SDK.h"

DWORD WINAPI MainThread(LPVOID)
{
    // Wait for the game to fully initialize
    Sleep(5000);

    while (true)
    {
        // Get the GameSettings singleton
        auto* settings = GameSettings::GetInstance();
        if (settings)
        {
            settings->SetIsGodMode(true);
        }

        Sleep(100);
    }

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
```

### Step 5: Build and Inject

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Inject `MyMECMod.dll` into Mirror's Edge Catalyst.

---

## Examples

### Example 1: God Mode

```cpp
#include "GameSettings.h"

void EnableGodMode()
{
    auto* settings = GameSettings::GetInstance();
    if (!settings) return;

    settings->SetIsGodMode(true);
    settings->SetIsJesusMode(true);     // invincible but can still take hits
    settings->SetHasUnlimitedAmmo(true);
    settings->SetHasUnlimitedMags(true);
}
```

### Example 2: Unlimited Ammo

```cpp
#include "GameSettings.h"

void ToggleUnlimitedAmmo(bool enable)
{
    auto* settings = GameSettings::GetInstance();
    if (!settings) return;

    settings->m_HasUnlimitedAmmo = enable;
    settings->m_HasUnlimitedMags = enable;
}
```

### Example 3: Reading Game Settings

```cpp
#include "GameSettings.h"
#include <cstdio>

void PrintGameInfo()
{
    auto* settings = GameSettings::GetInstance();
    if (!settings) return;

    printf("Max Players:    %u\n",   settings->GetMaxPlayerCount());
    printf("God Mode:       %s\n",   settings->GetIsGodMode() ? "ON" : "OFF");
    printf("Difficulty:     %d\n",   settings->m_DifficultyIndex);
    printf("Level:          %s\n",   settings->m_Level ? settings->m_Level : "null");
    printf("Start Point:    %s\n",   settings->m_StartPoint ? settings->m_StartPoint : "null");
}
```

### Example 4: Walking Pointer Chains

Use `fb::ReadChain` to follow multi-level pointer paths:

```cpp
#include "FBSDKTypes.h"

void ReadPlayerHealth()
{
    // Example: follow a pointer chain from a known global pointer
    // Base -> +0x28 (PlayerManager) -> +0x10 (LocalPlayer) -> +0x20 (Health)
    // ReadChain computes [[[base] + 0x28] + 0x10] + 0x20 and returns that address (0 on a null pointer).
    uintptr_t base = fb::GetModuleBase() + 0x1234567;  // address of the global pointer

    uintptr_t health_addr = fb::ReadChain(base, { 0x28, 0x10, 0x20 });
    if (health_addr)
    {
        float health = fb::Read<float>(health_addr);
        printf("Player health: %.1f\n", health);

        // Set health to max
        fb::Write<float>(health_addr, 100.0f);
    }
}
```

### Example 5: Custom Pattern Scanning

Find any function or global at runtime using `fb::PatternScan`:

```cpp
#include "FBSDKTypes.h"

// Find the ClientGameContext singleton pointer
uintptr_t FindClientGameContext()
{
    // IDA-style pattern with ?? wildcards
    // This scans the game module for the byte sequence and resolves
    // the RIP-relative address at offset 3
    uintptr_t addr = fb::PatternScan(
        "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01",
        3,      // offset to the 4-byte displacement
        true    // resolve as RIP-relative (adds displacement + 4 + match_addr)
    );

    if (addr)
    {
        printf("ClientGameContext* at 0x%llX\n", addr);
        void* ctx = *(void**)addr;
        printf("ClientGameContext instance: 0x%llX\n", (uintptr_t)ctx);
    }

    return addr;
}
```

**Pattern syntax:**
| Token | Meaning |
|---|---|
| `48` | Match exact byte 0x48 |
| `??` | Wildcard — match any byte |
| `?` | Also a wildcard (single char) |

**Parameters:**
| Param | Description |
|---|---|
| `offset` | Byte offset from match start to the value you want |
| `relative` | If `true`, reads a 4-byte `int32` at `match+offset` and resolves it as a RIP-relative address: `match + offset + 4 + displacement` |

### Example 6: Using Offset Constants for Manual Access

When you have a raw pointer and want to read a specific field without casting:

```cpp
#include "FBSDKTypes.h"
#include "GameSettings.h"

void ManualAccess(void* raw_settings_ptr)
{
    uintptr_t base = (uintptr_t)raw_settings_ptr;

    // Read using named offset constant
    bool godMode = fb::Read<bool>(base + GameSettings::Offsets::IsGodMode);
    printf("God mode: %s\n", godMode ? "ON" : "OFF");

    // Write using named offset constant
    fb::Write<bool>(base + GameSettings::Offsets::IsGodMode, true);

    // You can also just cast and access directly — the layout is correct:
    auto* settings = static_cast<GameSettings*>(raw_settings_ptr);
    settings->m_IsGodMode = true;  // same effect
}
```

---

## SDK Architecture

Your DLL includes `SDK.h`, which pulls in three kinds of header:

- `FBSDKTypes.h`, the `fb::` runtime utilities:
  - `fb::GetModuleBase()`: cached game base address
  - `fb::IsValidPtr()`: pointer range check
  - `fb::Read<T>()` / `fb::Write<T>()`: fault-safe memory access
  - `fb::ReadChain()`: follow Cheat Engine-style pointer paths
  - `fb::PatternScan()`: IDA-style byte pattern scanner over the game module
  - `fb::HookVFunc_InPlace()` / `fb::HookVFunc_Shadow()`: virtual function hooks
  - `fb::Array<T>`, `fb::WeakPtr<T>`, `fb::String`: engine container stubs
- `FBClasses.h`: forward declarations of every generated type.
- One header per class, struct and enum, each with:
  - `GetTypeInfo()`: ASLR-safe ClassInfo address
  - `GetInstance()`: instance resolver (global singleton or pointer chain)
  - `Offsets::FieldName`: compile-time offset constants
  - `m_FieldName`: direct member access at the engine's offsets
  - `Get/SetFieldName()`: typed accessors

```text
Your DLL
  │
  ├── #include "SDK.h"           ← master include
  │     │
  │     ├── FBSDKTypes.h         ← fb:: namespace utilities
  │     │     ├── fb::GetModuleBase()    — cached game base address
  │     │     ├── fb::IsValidPtr()       — pointer validation
  │     │     ├── fb::Read<T>()          — safe memory read
  │     │     ├── fb::Write<T>()         — safe memory write
  │     │     ├── fb::ReadChain()        — follow pointer chains
  │     │     ├── fb::PatternScan()      — AOB pattern scanner
  │     │     ├── fb::HookVFunc_*()      — vtable hooks
  │     │     └── fb::Array<T>           — Frostbite array stub
  │     │
  │     ├── FBClasses.h          ← forward declarations
  │     │
  │     └── [ClassName].h ...    ← one header per class/struct/enum
  │           ├── GetTypeInfo()          — ASLR-safe ClassInfo address
  │           ├── GetInstance()          — instance resolver
  │           ├── Offsets::FieldName     — compile-time offset constants
  │           ├── m_FieldName            — direct member access (layout-correct)
  │           └── Get/SetFieldName()     — typed accessors
  │
  ▼
  Game Process (MirrorsEdgeCatalyst.exe)
```

### How Instance Resolution Works

While generating, FrostbiteGen:

1. Finds the root game context through a known per-game offset or a code pattern, and **confirms its class from its vtable** (the `GetType()` virtual function). If the class can't be confirmed, it explores the object without assuming a layout, and the generated code carries a warning.
2. Follows pointer fields (and the first element of pointer arrays) from that root, recording the chain of offsets that reaches each class.
3. Scans the game's writable data for global singletons: pointers to heap objects, and objects stored directly in the executable.

`GetInstance()` then replays the shortest chain it recorded:

```cpp
uintptr_t ctx = fb::Read<uintptr_t>(fb::GetModuleBase() + 0x2401CB0); // ClientGameContext
if (!ctx) return nullptr;
ctx = fb::Read<uintptr_t>(ctx + 0x60); // PlayerManager
if (!ctx) return nullptr;
return reinterpret_cast<ClientPlayerManager*>(ctx);
```

For a class with many live instances, `GetInstance()` returns the one that was reached during generation (for example element 0 of an array), not a unique singleton.

### Three Ways to Access a Member

| Method | Code | When to Use |
|---|---|---|
| **Direct member** | `settings->m_IsGodMode = true;` | Simplest — when you have a typed pointer |
| **Accessor** | `settings->SetIsGodMode(true);` | Cleaner API, same as direct |
| **Manual offset** | `fb::Write<bool>(ptr + Offsets::IsGodMode, true);` | When working with raw `uintptr_t` |

---

## Troubleshooting

### "Failed to find ClassInfo"

The pattern scan for the ClassInfo linked list head failed. This can happen if:
- The game hasn't fully initialized yet — wait until you're at the main menu
- The game version doesn't match the hardcoded pattern — update the pattern in `structs.h` → `ClassInfo::GetInstance()`

### Root context not found or not verified

Check `fbgen.txt` for:
```text
WARNING: Root singleton not found - instance resolution will be limited
WARNING: the root object's class could not be confirmed from its vtable; using blind traversal
```

Possible causes:
- The game was at a loading screen (objects not yet allocated). Inject while in-game or at the main menu.
- The game version is not one of the known executables, or a patch moved the root pointer. Add the offset for your executable to `FindClientGameContext()` in `classinfo.cpp`.

Global singletons are still found independently, so many classes keep a working `GetInstance()` either way.

### GetInstance() returns nullptr

- The class may not have a singleton instance (not all DataContainers are instantiated)
- The game may be in a loading state — try again after the level loads
- For non-DataContainer classes, resolve through the game context hierarchy:

```cpp
// Example chain: GameContext → PlayerManager → LocalPlayer
auto* ctx = ClientGameContext::GetInstance();
if (ctx) {
    auto* pm = ctx->GetPlayerManager();
    // ... navigate to what you need
}
```

### ASLR: Addresses change every restart

This is handled automatically. All generated addresses use `fb::GetModuleBase() + offset`, which recalculates the base address at runtime. You don't need to update any offsets after a game restart.

### Game crashes on injection

- Make sure you're building for **x64** (`-A x64` in CMake)
- Make sure `/EHa` is enabled, so C++ destructors run when a `__try/__except` in the SDK catches a fault
- Avoid calling `GetInstance()` too early — add a `Sleep()` in `DllMain` or use a separate thread

### "SDK generated with errors"

`fbgen.txt` has an `ERROR:` line for each problem: a file that could not be written (read-only folder, path too long, two type names that differ only by case) or a type that was skipped after a memory fault. Everything else was still generated.

---

## Running the Tests

`tests/run_sdk_test.sh` checks the generator end to end on Linux, without the game or MSVC. It runs the generator against a small fake engine (`tests/fake_engine.h`), compiles the generated SDK with `FBGEN_VERIFY_LAYOUT` enabled, checks that every `GetInstance()` returns the right object, and checks that the IDA/Ghidra scripts, JSON and Cheat Engine outputs parse.

```bash
tests/run_sdk_test.sh          # needs g++ (C++17) and python3
```

CI runs this on every pull request, builds `FrostbiteGen.dll` with MSVC, and compiles the generated SDK with MSVC.

---

## License

FrostbiteGen is provided for educational and research purposes.
