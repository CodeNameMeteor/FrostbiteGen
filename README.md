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

### Step 1: Launch Mirror's Edge Catalyst

Start the game and wait until you reach the main menu (or are in-game). The type system must be fully initialized before injection.

### Step 2: Inject the DLL

Use any DLL injector to load `FrostbiteGen.dll` into the game process:

- [Process Hacker](https://processhacker.sourceforge.io/)
- [Xenos Injector](https://github.com/DarthTon/Xenos)
- Or any x64-compatible injector

```
Target Process: MirrorsEdgeCatalyst.exe
DLL Path:       <your_path>\build\Release\FrostbiteGen.dll
```

### Step 3: Wait for Generation

A message box will appear when generation is complete:

> **SDK generated successfully!**
> Check the SDK\ folder for output.

### Step 4: Collect the Output

The generated SDK is written next to the DLL:

```
build/Release/
├── FrostbiteGen.dll
├── fbgen.txt              ← generation log
└── SDK/
    ├── FBSDKTypes.h        ← runtime utilities (always include this)
    ├── FBClasses.h         ← forward declarations of all types
    ├── SDK.h               ← master include (includes everything)
    ├── GameSettings.h      ← example: game settings class
    ├── ClientGameContext.h
    ├── PlayerData.h
    ├── ...                 ← hundreds of generated headers
    └── Array.h             ← (if referenced by any class)
```

Check `fbgen.txt` for the discovered instance offset:

```
[14:25:28] FrostbiteGen SDK Generator starting...
[14:25:28] Module base: 0x0000000140000000
[14:25:28] ClassInfo head: 0x00000001428109E0
[14:25:28] Discovered instance offset in ClassInfo: 0x48 (127/1893 DataContainers)
[14:25:30] SDK generation complete!
```

---

## What Gets Generated

Every generated class header includes these features:

### 1. ASLR-Safe Type Info

```cpp
static void* GetTypeInfo()
{
    // Module-relative address — works across game restarts
    return (void*)(fb::GetModuleBase() + 0x28109E0);
}
```

### 2. Automatic Instance Resolution (DataContainer classes)

```cpp
static GameSettings* GetInstance()
{
    __try {
        uintptr_t classInfo = fb::GetModuleBase() + 0x28109E0;
        void* instance = *(void**)(classInfo + 0x48);
        if (!fb::IsValidPtr(instance)) return nullptr;
        return static_cast<GameSettings*>(instance);
    } __except(1) { return nullptr; }
}
```

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

### 5. Correct Memory Layout

The class member declarations are laid out to match the exact in-memory structure, including padding bytes. This means you can cast a raw pointer to the class type and access members directly.

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
    target_compile_options(MyMECMod PRIVATE /EHa)  # Required for __try/__except in SDK
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
    // Example: follow a pointer chain from a known base
    // Base -> +0x28 (PlayerManager) -> +0x10 (LocalPlayer) -> +0x20 (Health)
    uintptr_t base = fb::GetModuleBase() + 0x1234567;  // your known offset

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

```
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
  │     │     └── Array<T>               — Frostbite array stub
  │     │
  │     ├── FBClasses.h          ← forward declarations
  │     │
  │     └── [ClassName].h ...    ← one header per class/struct/enum
  │           ├── GetTypeInfo()          — ASLR-safe ClassInfo address
  │           ├── GetInstance()          — singleton resolver (DataContainer only)
  │           ├── Offsets::FieldName     — compile-time offset constants
  │           ├── m_FieldName            — direct member access (layout-correct)
  │           └── Get/SetFieldName()     — typed accessors
  │
  ▼
  Game Process (MirrorsEdgeCatalyst.exe)
```

### How Instance Resolution Works

```
1. GetInstance() is called
2. Computes ClassInfo address:  fb::GetModuleBase() + relative_offset
3. Reads instance pointer:      *(void**)(classInfo + discovered_offset)
4. Validates the pointer:       fb::IsValidPtr(instance)
5. Returns typed pointer:       static_cast<GameSettings*>(instance)
```

The `discovered_offset` is found automatically at SDK generation time by probing
the ClassInfo structures of all DataContainer classes. The generator looks for a
consistent offset that contains a valid heap pointer (with an in-module vtable)
across many DataContainer types.

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

### Instance offset not discovered

Check `fbgen.txt` for:
```
WARNING: Could not reliably discover instance offset.
```

This means the probing heuristic didn't find a consistent offset. Possible causes:
- The game was at a loading screen (instances not yet allocated)
- The engine version stores instances differently

**Fix:** Inject while in-game (not at a loading screen), or manually set `m_instanceOffset` in the `ClassInfoManager` constructor if you know the offset from reverse engineering.

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
- Make sure `/EHa` is enabled (required for `__try/__except`)
- Avoid calling `GetInstance()` too early — add a `Sleep()` in `DllMain` or use a separate thread

---

## License

FrostbiteGen is provided for educational and research purposes.
