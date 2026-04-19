# C++ プラグイン クイックスタート (CMake + MSVC)

**C++** で emfe プラグインを新規作成する手順。ターゲットは Windows DLL
で MSVC C ランタイムを静的リンク。

契約 (contract) の詳細は
[`plugin_developer_guide_ja.md`](plugin_developer_guide_ja.md) を参照。

参考にできる既存プラグイン:
- `emfe_plugins/em8/` — 最小 (8 bit 学習用 CPU、約 1,400 行)
- `emfe_plugins/z8000/` — 中規模 (Z8000 ファミリー、自前実装)
- `emfe_plugins/mc68030/` — 大規模 (外部 Em68030 を C++ でラップ)

---

## 1. ディレクトリ構成

```
emfe_plugins/foo/
├── CMakeLists.txt          # トップレベル
├── README.md
├── docs/
│   ├── foo_reference.md
│   └── foo_reference_ja.md
├── src/
│   ├── CMakeLists.txt      # ターゲット定義
│   ├── pch.h               # MSVC pch
│   ├── pch.cpp             #   (pch.h を include)
│   ├── plugin_foo.cpp      # 全 emfe_* entry point
│   └── plugin_foo.def      # DLL エクスポート
└── tests/
    ├── CMakeLists.txt
    └── test_plugin.cpp
```

## 2. トップレベル `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(emfe_plugin_foo
    VERSION 0.1.0
    DESCRIPTION "FOO CPU plugin for emfe"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 共通 emfe ヘッダ (emfe_plugin.h) の配置パス。兄弟ディレクトリ
# `emfe_plugins/api/` にある。
set(EMFE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../api"
    CACHE PATH "Path to the emfe shared header directory")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src)

option(EMFE_PLUGIN_BUILD_TESTS "Build tests" ON)
if(EMFE_PLUGIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

## 3. `src/CMakeLists.txt`

```cmake
add_library(emfe_plugin_foo SHARED
    plugin_foo.cpp
    pch.cpp
)

target_include_directories(emfe_plugin_foo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EMFE_HEADER_DIR}
)

target_compile_definitions(emfe_plugin_foo PRIVATE EMFE_PLUGIN_EXPORTS)

if(MSVC)
    target_compile_options(emfe_plugin_foo PRIVATE /W4 /O2 /GL)
    target_link_options(emfe_plugin_foo PRIVATE /LTCG)
    # MSVC ランタイムを静的リンク (VCREDIST 配布不要に)
    set_target_properties(emfe_plugin_foo PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set(MODULE_DEF "${CMAKE_CURRENT_SOURCE_DIR}/plugin_foo.def")
    if(EXISTS "${MODULE_DEF}")
        target_sources(emfe_plugin_foo PRIVATE "${MODULE_DEF}")
    endif()
endif()
```

## 4. `src/plugin_foo.def`

`.def` ファイルは全エクスポート関数を列挙する。ヘッダの `emfe_*` 関数
すべてを記述する必要がある。最短動作セットは
`emfe_plugins/em8/src/plugin_em8.def` にある。コピーして `LIBRARY` 行
だけ DLL 名に合わせて変更すればよい。

```
LIBRARY emfe_plugin_foo
EXPORTS
    emfe_negotiate
    emfe_get_board_info
    emfe_create
    emfe_destroy
    ; ... 実装した emfe_* 関数を全て列挙 ...
```

## 5. `src/pch.h`

```cpp
#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef GetObject
#undef GetObject
#endif
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
```

## 6. `src/plugin_foo.cpp` — 最小スケルトン

```cpp
#include "pch.h"
#include "emfe_plugin.h"

// ============================================================================
// インスタンス
// ============================================================================

struct EmfeInstanceData {
    // CPU / メモリ / ペリフェラル
    uint8_t memory[65536] = {};
    uint16_t pc = 0;
    std::atomic<EmfeState> state{EMFE_STATE_STOPPED};

    // コールバック
    EmfeConsoleCharCallback console_cb = nullptr;
    void* console_user = nullptr;

    // UI 駆動用ストレージ
    std::vector<EmfeRegisterDef> reg_defs;

    void BuildRegisterDefs() {
        reg_defs.push_back({0, "PC", "CPU", EMFE_REG_INT, 16, EMFE_REG_FLAG_PC});
        // ... レジスタを追加 ...
    }
};

// ============================================================================
// Board info (static — DLL の生存期間中有効)
// ============================================================================

static EmfeBoardInfo s_board_info = {
    "FOO",          // board_name
    "FOOCPU",       // cpu_name
    "FOO CPU plugin (16-bit toy)",   // description
    "0.1.0",        // version
};

// ============================================================================
// Discovery & lifecycle
// ============================================================================

extern "C" EmfeResult EMFE_CALL emfe_negotiate(const EmfeNegotiateInfo* info) {
    if (!info) return EMFE_ERR_INVALID;
    if (info->api_version_major != EMFE_API_VERSION_MAJOR)
        return EMFE_ERR_UNSUPPORTED;
    return EMFE_OK;
}

extern "C" EmfeResult EMFE_CALL emfe_get_board_info(EmfeBoardInfo* out) {
    if (!out) return EMFE_ERR_INVALID;
    *out = s_board_info;
    return EMFE_OK;
}

extern "C" EmfeResult EMFE_CALL emfe_create(EmfeInstance* out_instance) {
    if (!out_instance) return EMFE_ERR_INVALID;
    auto inst = new (std::nothrow) EmfeInstanceData();
    if (!inst) return EMFE_ERR_MEMORY;
    inst->BuildRegisterDefs();
    *out_instance = reinterpret_cast<EmfeInstance>(inst);
    return EMFE_OK;
}

extern "C" EmfeResult EMFE_CALL emfe_destroy(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    // 本来はここでワーカースレッドを stop + join (このサンプルでは省略)。
    delete inst;
    return EMFE_OK;
}

// ============================================================================
// レジスタ
// ============================================================================

extern "C" int32_t EMFE_CALL emfe_get_register_defs(
    EmfeInstance instance, const EmfeRegisterDef** out) {
    if (!instance || !out) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    *out = inst->reg_defs.data();
    return static_cast<int32_t>(inst->reg_defs.size());
}

extern "C" EmfeResult EMFE_CALL emfe_get_registers(
    EmfeInstance instance, EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    for (int32_t i = 0; i < count; i++) {
        auto& v = values[i];
        switch (v.reg_id) {
        case 0: v.value.u64 = inst->pc; break;
        default: v.value.u64 = 0;
        }
    }
    return EMFE_OK;
}

// ... 残りの entry point — 完全なセットは em8 / z8000 プラグイン参照 ...

// ============================================================================
// DLL entry point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
```

## 7. ビルド

```bash
cd emfe_plugins/foo
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

出力: `build/bin/Release/emfe_plugin_foo.dll`

`VCRUNTIME140.dll` などに依存していないか確認:

```bash
dumpbin /dependents build/bin/Release/emfe_plugin_foo.dll
```

## 8. フロントエンドへの配線

`emfe_WinUI3Cpp/emfe/emfe.vcxproj` に `<Content>` エントリ、
`emfe_CsWPF/emfe/emfe.csproj` に `<None>` エントリを追加:

```xml
<Content Include="$(ProjectDir)..\..\emfe_plugins\foo\build\bin\Release\emfe_plugin_foo.dll"
         Condition="Exists('$(ProjectDir)..\..\emfe_plugins\foo\build\bin\Release\emfe_plugin_foo.dll')">
  <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
  <DeploymentContent>true</DeploymentContent>
  <TargetPath>plugins\%(Filename)%(Extension)</TargetPath>
</Content>
```

フロントエンドを再ビルドすると DLL は exe 隣接の `plugins\` に配置され、
起動時に自動認識される。

## 9. テストハーネス (任意だが推奨)

`emfe_plugins/em8/tests/test_em8.cpp` が参考例。C ABI 経由で DLL を
ロードし、全 entry point を網羅的に叩く自己完結したテスト実行ファイル。

## 10. 次のステップ

- 契約詳細は
  [`plugin_developer_guide_ja.md`](plugin_developer_guide_ja.md)
  (スレッド、文字列所有権、ステートマシンなど)
- 類似プラグインをコピーして改変: 単純な toy CPU なら em8、自前実装
  ISA なら z8000、外部エンジンのラップなら mc68030
