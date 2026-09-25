# SDK vs Project: Comprehensive Comparison

**Analysis Date:** September 22, 2026

---

## Executive Summary

The **SDK** and **Project** folders are **NOT mirrors or synchronized copies** of each other. They have a **parent-child dependency relationship**:

- **SDK (20,880 files)**: Foundation - Auto-generated Unreal Engine class definitions reverse-engineered from the game binary
- **Project (177 files)**: Application - Custom exploit/tool code that uses the SDK

---

## Directory Structure Breakdown

### SDK Directory (`F:\Test\ARCs\sdk`)

#### 1. **CppSDK/ (20,871 files)**
- **Core headers:**
  - `SDK.hpp` - Main include file that pulls in all Unreal Engine class definitions
  - `Assertions.inl` - Assertion utilities
  - `NameCollisions.h` - Collision handling for name conflicts
  - `PropertyFixup.h` - Property definition corrections
  - `UtfN.hpp` - UTF string handling

- **SDK/ subfolder (20,868 files)** - Auto-generated files following patterns:
  - `*_class.h` - Unreal Engine class definitions
  - `*_struct.h` - Unreal Engine struct definitions
  - `*_enums.h` - Unreal Engine enum definitions
  - `*_functions.h` - Unreal Engine function signatures
  - `Basic.hpp`, `Field_classes.hpp`, `Property_classes.hpp`, `Actor_classes.hpp`, etc.

  Example files:
  - `Actor_class.h` - AActor class definition
  - `Character_class.h` - ACharacter class definition
  - `Pawn_class.h` - APawn class definition
  - Thousands more covering entire Unreal Engine API

#### 2. **Dumpspace/ (5 files)** - Reverse Engineering Metadata
- `ClassesInfo.json` - Class information and hierarchy
- `EnumsInfo.json` - Enumeration definitions
- `FunctionsInfo.json` - Function offsets and signatures
- `OffsetsInfo.json` - Memory offsets and addresses
- `StructsInfo.json` - Structure layouts

#### 3. **IDAMappings/ (1 file)**
- `DMA-ARC Raider mappings file` - IDA Pro debugger symbol mappings

#### 4. **Mappings/ (1 file)**
- `DMA-ARC Raider mappings file` - Address offset mappings

#### 5. **SDK.txt**
- General documentation about the SDK

---

### Project Directory (`F:\Test\ARCs\Project`)

#### Core Components:

**1. Core/ (13 files)** - Game engine and math
```
- SDK.hpp - CUSTOM WRAPPER around sdk/CppSDK/SDK.hpp
- Engine.h - Application game engine class
- Memory.h - Memory interface definition
- ActorType.h, AgentLog.h - Game entity definitions
- AimMath.hpp, BoneMath.hpp, Vector.hpp - Mathematical utilities
- Cache.hpp, Reflection.hpp - Reflection and caching systems
- SessionLog.*, CrashHandler.* - Logging and error handling
- Offsets.h, Reflection.hpp - Offset definitions and reflection
```

**2. DMA/ (5 files)** - Direct Memory Access Module
```
- Memory.cpp/h - DMA memory interface implementation
- DmaKeyboard.cpp/h - Keyboard input via DMA
- DmaVmm.h - Virtual memory module wrapper
```

**3. Functions/ (17 files)** - Game Logic & Exploits
```
- Aimbot.cpp - Aiming assist functionality
- Esp.cpp - Enemy/item visualization
- EntityList.cpp - Game entity tracking
- RobotList.cpp/h - Enemy robot management
- ItemList.cpp - World item tracking
- ContainerList.cpp - Inventory container management
- EngineThreads.cpp - Game thread management
- CollisionMirror.cpp/h - Collision detection
- PositionRefreshPass.cpp - Position update logic
- World.cpp - World/map functions
- WorldScanCommon.cpp/h - Common scanning utilities
- Utils.cpp - General utilities
- Update.cpp - Main update loop
- BoneList.cpp.new - Skeletal animation/bones
```

**4. Hardware/ (8 files)** - Hardware Device Integration
```
- KmBox.cpp/h - KmBox input device driver
- KmBoxNet.cpp - KmBox network communication
- KmboxNet.hpp - KmBox network protocol
- kmbox_config.json - Device configuration
- Makcu/ (4 files) - Serial port controller implementation
```

**5. Input/ (8 files)** - Input Handling
```
- Controller.cpp/h - Game controller abstraction
- DmaGamepad.cpp/h - Gamepad input via DMA
- KeyBind.cpp/h - Keyboard binding system
- InputBind.cpp/h - Input binding framework
```

**6. Interface/ (13 files)** - GUI & Overlay System
```
- Overlay/ (8 files) - ImGui-based overlay
  - ImGuiKeybind.cpp/h
  - OverlayHandler.cpp/h
  - And more overlay UI components
- Render.cpp/h - Rendering backend
- Render/ - Additional rendering utilities
  - RenderCommon.cpp/h
- Utils/ - UI helper utilities
  - AutoClickerUI.cpp/h
  - ThemeUI.cpp/h
  - ValidationUI.cpp/h
  - VisualsUI.cpp/h
  - VisualsUI_Advanced.cpp/h
```

**7. Data/ (24 files)** - Game Data & Localization
```
- CSV files:
  - asset_index.csv - Asset mapping
  - asset_localizations.csv - Asset names in different languages

- JSON files:
  - items_meta.json - Item metadata
  - Bots_Items_Maps/en.json - Bot/item/map data

- Localization folder (loc/):
  - ST_Enemy.json - Enemy names
  - ST_ItemNames*.json - Item names (various categories)
  - ST_ItemNames_Firearms.json
  - ST_ItemNames_Armors.json
  - ST_ItemNames_Augments.json
  - ST_ItemNames_Consumables.json
  - ST_ItemNames_Gadgets.json
  - ST_ItemNames_Throwables.json
  - ST_ItemNames_Salvage.json
  - And more...
```

**8. Tests/ (11 files)** - Unit Tests
```
- main.cpp - Test runner
- aim_math_tests.cpp - Aiming math tests
- bone_transform_tests.cpp - Skeletal transform tests
- collision_mirror_tests.cpp - Collision tests
- json_parser_tests.cpp - JSON parsing tests
- memory_interface_tests.cpp - Memory tests
- reflection_tests.cpp - Reflection system tests
- vector_tests.cpp - Vector math tests
- Test helper files
```

**9. ThirdParty/ (26 files)** - External Libraries
```
- doctest/ - Unit testing framework
- ImGui/ - Dear ImGui UI library (multiple header versions)
- nlohmann_json/ - JSON parser library
```

**10. lib/ (13 files)** - Precompiled Libraries
```
- DMA/
  - leechcore.lib - DMA memory access library
  - vmm.lib - Virtual memory module library

- include/
  - leechcore_wrapper.h
  - vmmdll.h

- Precompiled DLLs:
  - leechcore.dll
  - vmm.dll
  - vmm.lib
  - leechcore.lib
```

**11. Root Files**
```
- Project.cpp - Main application entry point
- Project.vcxproj - Visual Studio project file
- Project.vcxproj.filters - Project file filters
- Project.Tests.vcxproj - Tests project file
- app.rc - Application resources
- resource.h - Resource header
- icon.ico - Application icon
```

**12. x64/ - Runtime Binaries**
```
- DLLs: leechcore.dll, vmm.dll, dbghelp.dll, FTD3XX.dll, etc.
- Symbol files for debugging
```

---

## Key Differences & Relationships

### The One Shared File: SDK.hpp

| Aspect | sdk/CppSDK/SDK.hpp | Project/Core/SDK.hpp |
|--------|-------------------|----------------------|
| **Purpose** | Include all Unreal Engine class headers | Custom offset decoder for memory access |
| **Size** | ~200+ lines of includes | ~500 lines of custom crypto/decode functions |
| **Content** | `#include "SDK/Field_classes.hpp"` etc. | Game-specific offsets + decryption logic |
| **Differences** | Generic Unreal Engine classes | Game-specific GWORLD, GNAMES, property offsets |
| **Relationship** | Referenced by Project/Core/SDK.hpp | Uses sdk/CppSDK/SDK.hpp for game structs |

**Project/Core/SDK.hpp** is a **thin wrapper** that:
1. Defines game-specific offsets (GWORLD, GNAMES, etc.)
2. Implements memory decryption functions (rol32, rol64, clmul64, shuffle64)
3. Provides offset decoders for the game's obfuscated memory layout
4. Is adapted for MSVC (uses `_byteswap_ulong` instead of `__builtin_bswap32`)

---

## File Mapping: Where SDK Classes Meet Project Code

```
sdk/CppSDK/SDK.hpp + sdk/CppSDK/SDK/*_class.h
    ↓
Project/Core/SDK.hpp (decodes + wraps)
    ↓
Project/Core/Engine.h (creates game engine abstraction)
    ↓
Project/Functions/* (uses game classes to implement features)
    ├→ Aimbot.cpp (uses actor classes to aim)
    ├→ Esp.cpp (uses actor classes for visualization)
    ├→ EntityList.cpp (iterates actors)
    └→ RobotList.cpp (manages enemy actors)
```

---

## No Direct File Synchronization

### Files ONLY in SDK (20,868+):
- All of `sdk/CppSDK/SDK/*.h` - Auto-generated Unreal Engine headers
- `sdk/Dumpspace/*.json` - Reverse engineering metadata
- `sdk/IDAMappings/*` - IDA debug info

### Files ONLY in Project (177):
- All application-specific code (exploit features, rendering, etc.)
- All data files (localization, asset mappings)
- All tests and third-party libraries
- Hardware-specific code (KmBox driver)

### Common Files (1):
- **SDK.hpp** - But with completely different purposes and contents

---

## Update Pattern & Maintenance

When the game updates:

1. **SDK needs regeneration:**
   - Run Unreal Engine SDK dumper on new game binary
   - Generates new `sdk/CppSDK/SDK/` files
   - Updates `sdk/Dumpspace/*.json` with new offsets

2. **Project/Core/SDK.hpp needs updates:**
   - Update game offsets (GWORLD, GNAMES, etc.)
   - May need new decode functions if encryption changes
   - Update `Project/Data/*.json` with new localization

3. **Project code may need adjustments:**
   - If actor structure changed, update Functions/*.cpp
   - If hardware device protocol changed, update Hardware/*.cpp
   - If UI changed, update Interface/*.cpp

---

## Conclusion

**SDK and Project are NOT copies or mirrors.**

They represent **different abstraction layers**:
- **SDK**: Foundational layer (Unreal Engine definitions)
- **Project**: Application layer (game exploitation tool)

The relationship is **parent → child**, not **parallel synchronization**.

**When syncing after game updates:**
- Regenerate SDK from game binary
- Update specific offsets in Project/Core/SDK.hpp
- Fix any broken references in Project/Functions/*.cpp
- Update localization in Project/Data/*.json

The majority of Project code will continue working because it depends on Unreal Engine's stable API, not the game's internal memory layout.
