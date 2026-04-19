# emfe

**emfe** (Emulator Frontend) プラグインアーキテクチャの共通ヘッダおよび
開発者向けドキュメント。

エミュレータバックエンドを DLL として切り離し、フロントエンド
(C++ WinUI3 / C# WPF) と C ABI で接続するためのインターフェイスを定義
します。

English: see [`README.md`](README.md).

## このリポジトリに含まれるもの

| ファイル / ディレクトリ | 内容 |
|---|---|
| `emfe_plugin.h` | 全 API の宣言、構造体、列挙型 (C/C++ ヘッダ) |
| [`docs/plugin_developer_guide.md`](docs/plugin_developer_guide.md) | プラグイン開発者ガイド (英語) |
| [`docs/plugin_developer_guide_ja.md`](docs/plugin_developer_guide_ja.md) | プラグイン開発者ガイド (日本語) |
| [`docs/ffi_reference.md`](docs/ffi_reference.md) | FFI 関数リファレンス (英語) |
| [`docs/ffi_reference_ja.md`](docs/ffi_reference_ja.md) | FFI 関数リファレンス (日本語) |
| [`docs/quickstart_cpp.md`](docs/quickstart_cpp.md) | C++ プラグイン クイックスタート (英語) |
| [`docs/quickstart_cpp_ja.md`](docs/quickstart_cpp_ja.md) | C++ プラグイン クイックスタート (日本語) |
| [`docs/quickstart_rust.md`](docs/quickstart_rust.md) | Rust プラグイン クイックスタート (英語) |
| [`docs/quickstart_rust_ja.md`](docs/quickstart_rust_ja.md) | Rust プラグイン クイックスタート (日本語) |

## 特徴

- C++ の `LoadLibrary` / `GetProcAddress`、C# の `P/Invoke` 両方から呼び
  出し可能な C ABI
- プラグインは C++ (MSVC DLL) と Rust (`cdylib`) のどちらでも実装可能
- データ駆動 UI: フロントエンドはプラグインが返す配列から レジスタパネル
  と設定ダイアログを構築
- 64 bit アドレス (`uint64_t`) で将来の 64 bit アーキテクチャに備える
- Opaque handle (`EmfeInstance`) で複数インスタンス対応

## 関連プロジェクト

### フロントエンド

| プロジェクト | 役割 |
|---|---|
| [`emfe_WinUI3Cpp/`](../../emfe_WinUI3Cpp/) | C++ WinUI3 フロントエンド |
| [`emfe_CsWPF/`](../../emfe_CsWPF/) | C# WPF フロントエンド |

### プラグイン

すべて [`emfe_plugins/`](../) 配下 (この `api/` の親ディレクトリ):

| プラグイン | CPU / システム | 実装言語 |
|---|---|---|
| [`mc68030/`](../mc68030/) | Motorola MC68030 (MVME147 ボード) | C++ (Em68030 をラップ) |
| [`em8/`](../em8/) | EM8 (自作 8-bit 学習用) | C++ (自前実装) |
| [`z8000/`](../z8000/) | Zilog Z8000 ファミリー (Z8001/Z8002/Z8003/Z8004) | C++ (自前実装) |
| [`mc6809/`](../mc6809/) | Motorola MC6809 + MC6850 ACIA | Rust (em6809 をラップ) |

ビルド後の DLL は各フロントエンドの `plugins\` サブディレクトリに
ビルドシステムが自動配置する。フロントエンドは
`plugins\emfe_plugin_*.dll` をスキャンして "Switch Plugin" ダイアログに
列挙する。

## API カテゴリ

詳細は [`docs/ffi_reference_ja.md`](docs/ffi_reference_ja.md) を参照。

| カテゴリ | 関数数 | 代表関数 |
|---|---|---|
| Discovery / Lifecycle | 4 | `emfe_negotiate`, `emfe_get_board_info`, `emfe_create`, `emfe_destroy` |
| コールバック | 3 | `emfe_set_console_char_callback`, `emfe_set_state_change_callback`, `emfe_set_diagnostic_callback` |
| レジスタ | 3 | `emfe_get_register_defs`, `emfe_get_registers`, `emfe_set_registers` |
| メモリ | 8 | `emfe_peek/poke_{byte,word,long}`, `emfe_peek_range`, `emfe_get_memory_size` |
| 逆アセンブル | 3 | `emfe_disassemble_one`, `emfe_disassemble_range`, `emfe_get_program_range` |
| 実行制御 | 10 | `emfe_step`, `emfe_run`, `emfe_stop`, `emfe_reset`, `emfe_get_state`, ... |
| ブレークポイント | 6 | `emfe_add/remove/enable/set_breakpoint_condition/clear_breakpoints/get_breakpoints` |
| ウォッチポイント | 6 | 同上 (watchpoint 系) |
| ファイルロード | 4 | `emfe_load_elf`, `emfe_load_binary`, `emfe_load_srec`, `emfe_get_last_error` |
| 設定 | 14 | `emfe_get_setting_defs`, `emfe_get/set_setting`, `emfe_apply_settings`, `emfe_get_applied_setting`, list 操作, save/load, `emfe_set_data_dir` |
| コンソール I/O | 2 | `emfe_send_char`, `emfe_send_string` |
| フレームバッファ (Phase 3) | 3 | `emfe_get_framebuffer_info`, `emfe_get_palette*` |
| 入力イベント (Phase 3) | 4 | `emfe_push_key`, `emfe_push_mouse_*` |
| コールスタック (Phase 3) | 1 | `emfe_get_call_stack` |
| 文字列ユーティリティ | 1 | `emfe_release_string` |

## ライセンス

MIT License
