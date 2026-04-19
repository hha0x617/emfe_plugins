# emfe_plugin_mc68030

MC68030 エミュレータの emfe プラグイン DLL。[em68030](../em68030_WinUI3Cpp/) のコアエンジン (Core/IO/Config) を C ABI でラップします。

## 機能

- MVME147 ボード全デバイス配線 (PCC, SCC, WD33C93 SCSI, LANCE Ethernet, MK48T02 RTC, UART16550)
- TRAP #15 ハンドラ (147Bug プロトコル + Generic ConsoleDevice)
- NetBSD / Linux カーネルのブートスタブ自動設定
- 設定の永続化 (`EmulatorConfig` をラップ)
- ELF / S-Record / Binary ファイルのロード

## ディレクトリ構造

```
emfe_plugin_mc68030/
├── CMakeLists.txt       トップレベル (project宣言, 依存パス)
├── README.md
├── src/
│   ├── CMakeLists.txt   DLL ターゲット定義
│   ├── plugin_mc68030.cpp
│   ├── plugin_mc68030.def (エクスポート定義)
│   ├── pch.h / pch.cpp
├── tests/
│   ├── CMakeLists.txt   テスト実行ファイル定義
│   └── test_plugin.cpp  スモークテスト
└── build/               ビルド成果物 (out-of-source)
```

## 依存関係

| 依存先 | 想定パス | 内容 |
|-------|---------|-----|
| `em68030_WinUI3Cpp/Em68030` | `../../em68030_WinUI3Cpp/Em68030` | Core/, IO/, Config/ のソース参照 |
| `emfe_plugins/api` | `../api` | `emfe_plugin.h` |

パスは `-DEM68030_ROOT=...` / `-DEMFE_HEADER_DIR=...` で上書き可能。

### システム要件

- Windows (x64)
- Visual Studio 2026 (v145 toolset) または Build Tools
- CMake 3.20+
- `ws2_32.lib` (Slirp ネットワークハンドラ用, Windows SDK に付属)

## ビルド

```bash
# Configure
cmake -B build -S .

# Build
cmake --build build --config Release

# Test
ctest --test-dir build -C Release
```

出力:

- `build/bin/Release/emfe_plugin_mc68030.dll` — プラグイン DLL
- `build/bin/Release/test_plugin.exe` — スモークテスト

## CMake オプション

| オプション | デフォルト | 説明 |
|-----------|----------|------|
| `EM68030_ROOT` | `../em68030_WinUI3Cpp/Em68030` | Em68030 ソースツリーのパス |
| `EMFE_HEADER_DIR` | `../api` | `emfe_plugin.h` のあるディレクトリ |
| `EMFE_PLUGIN_BUILD_TESTS` | `ON` | テストハーネスをビルド |

## テスト

`test_plugin.exe` は以下を検証:

- DLL ロード + 全関数解決
- API バージョンネゴシエーション
- インスタンス生成 / 破棄
- 44 レジスタ定義の取得
- メモリ poke + 逆アセンブリ + 3命令ステップ実行 (D0=42 確認)
- Phase 2 設定 API (get_setting_defs, set_setting, apply)
- `MOVE.L (4,A7),D7` 回帰テスト
- Generic モードで hello.s19 ロード + 実行 → コンソール出力確認
- MVME147 モードで TRAP #15 → Handle147BugCall → コンソール出力確認

## 使用例 (C++)

```cpp
#include <windows.h>
#include "emfe_plugin.h"

HMODULE hDll = LoadLibraryW(L"emfe_plugin_mc68030.dll");
auto create = reinterpret_cast<decltype(emfe_create)*>(GetProcAddress(hDll, "emfe_create"));
auto step = reinterpret_cast<decltype(emfe_step)*>(GetProcAddress(hDll, "emfe_step"));

EmfeInstance inst = nullptr;
create(&inst);
step(inst);
```

C# 側は [emfe_CsWPF/emfe/PluginInterop.cs](../emfe_CsWPF/emfe/PluginInterop.cs) を参照。

## ライセンス

MIT License
