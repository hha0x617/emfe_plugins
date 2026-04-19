# Rust プラグイン クイックスタート (Cargo + cdylib)

**Rust** で emfe プラグインを新規作成する手順。成果物は Windows DLL
(`cdylib`)、MSVC C ランタイムは静的リンクで C++ プラグインと同じ形に揃える。

契約 (contract) の詳細は
[`plugin_developer_guide_ja.md`](plugin_developer_guide_ja.md) を参照。

参考にできる既存 Rust プラグイン: `emfe_plugins/mc6809/`

---

## 1. ディレクトリ構成

```
emfe_plugins/foo/
├── Cargo.toml
├── .cargo/
│   └── config.toml         # MSVC CRT 静的リンク
├── README.md
├── docs/
│   ├── foo_reference.md
│   └── foo_reference_ja.md
├── src/
│   └── lib.rs              # FFI + インスタンスデータ全部
└── tests/
    └── smoke.rs            # extern "C" 経由の integration test
```

## 2. `Cargo.toml`

```toml
[package]
name = "emfe_plugin_foo"
version = "0.1.0"
edition = "2021"
description = "FOO CPU plugin for emfe"
license = "MIT OR Apache-2.0"

[lib]
name = "emfe_plugin_foo"
# cdylib: DLL 本体
# rlib:   integration test から use できるように
crate-type = ["cdylib", "rlib"]
path = "src/lib.rs"

[dependencies]
# バックエンドクレート (ラップ対象の CPU コアなど):
# em_foo = { path = "../../em_foo", default-features = false }

[profile.release]
opt-level = 3
lto = true
codegen-units = 1
panic = "abort"
```

## 3. `.cargo/config.toml` — 静的 CRT

```toml
# MSVC C ランタイムを静的リンク。C++ プラグインと同じ扱いになり
# VCREDIST 配布不要。
[target.x86_64-pc-windows-msvc]
rustflags = ["-C", "target-feature=+crt-static"]
```

## 4. `src/lib.rs` — 最小スケルトン

```rust
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(clippy::missing_safety_doc)]

use std::ffi::{c_char, c_void, CStr, CString};
use std::ptr;

// --- emfe_plugin.h のミラー -----------------------------------------------

pub const EMFE_API_VERSION_MAJOR: u32 = 1;
pub const EMFE_API_VERSION_MINOR: u32 = 0;

#[repr(C)] #[derive(Copy, Clone)]
pub struct EmfeNegotiateInfo {
    pub api_version_major: u32,
    pub api_version_minor: u32,
    pub flags: u32,
}

#[repr(C)] #[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum EmfeResult {
    Ok = 0,
    ErrInvalid = -1,
    ErrState = -2,
    ErrNotFound = -3,
    ErrIo = -4,
    ErrMemory = -5,
    ErrUnsupported = -6,
}

#[repr(C)] #[derive(Copy, Clone)]
pub struct EmfeBoardInfo {
    pub board_name: *const c_char,
    pub cpu_name: *const c_char,
    pub description: *const c_char,
    pub version: *const c_char,
}

// ... 使う全 struct / enum を同様に翻訳する ...
// (完全な翻訳は emfe_plugins/mc6809/src/lib.rs を参照)

pub type EmfeInstance = *mut c_void;

// --- インスタンスデータ --------------------------------------------------

struct PluginInstance {
    pc: u16,
    memory: Box<[u8; 0x10000]>,
    // ... 他フィールド ...
}

// SAFETY: 内部の生ポインタは不透明ホストハンドルのみ。自前コールバック
// 外では dereference しない。シングルスレッドアクセス想定。
unsafe impl Send for PluginInstance {}
unsafe impl Sync for PluginInstance {}

impl PluginInstance {
    fn new() -> Self {
        PluginInstance {
            pc: 0,
            memory: Box::new([0u8; 0x10000]),
        }
    }
}

unsafe fn inst_mut<'a>(h: EmfeInstance) -> Option<&'a mut PluginInstance> {
    if h.is_null() { None } else { Some(&mut *(h as *mut PluginInstance)) }
}

// --- 静的 board info (DLL の生存期間中有効) --------------------------

static BOARD_NAME: &[u8] = b"FOO\0";
static CPU_NAME:   &[u8] = b"FOOCPU\0";
static DESC:       &[u8] = b"FOO plugin skeleton\0";
static VERSION:    &[u8] = b"0.1.0\0";

// --- FFI entry point ----------------------------------------------------

#[no_mangle]
pub extern "C" fn emfe_negotiate(info: *const EmfeNegotiateInfo) -> EmfeResult {
    if info.is_null() { return EmfeResult::ErrInvalid; }
    let info = unsafe { &*info };
    if info.api_version_major != EMFE_API_VERSION_MAJOR {
        return EmfeResult::ErrUnsupported;
    }
    EmfeResult::Ok
}

#[no_mangle]
pub extern "C" fn emfe_get_board_info(out: *mut EmfeBoardInfo) -> EmfeResult {
    if out.is_null() { return EmfeResult::ErrInvalid; }
    unsafe {
        (*out).board_name  = BOARD_NAME.as_ptr() as *const c_char;
        (*out).cpu_name    = CPU_NAME.as_ptr()   as *const c_char;
        (*out).description = DESC.as_ptr()       as *const c_char;
        (*out).version     = VERSION.as_ptr()    as *const c_char;
    }
    EmfeResult::Ok
}

#[no_mangle]
pub extern "C" fn emfe_create(out: *mut EmfeInstance) -> EmfeResult {
    if out.is_null() { return EmfeResult::ErrInvalid; }
    let b = Box::new(PluginInstance::new());
    unsafe { *out = Box::into_raw(b) as EmfeInstance; }
    EmfeResult::Ok
}

#[no_mangle]
pub extern "C" fn emfe_destroy(instance: EmfeInstance) -> EmfeResult {
    if instance.is_null() { return EmfeResult::ErrInvalid; }
    unsafe { let _ = Box::from_raw(instance as *mut PluginInstance); }
    EmfeResult::Ok
}

// ... 残りの emfe_* entry point を追加 ...
// 完全実装は emfe_plugins/mc6809/src/lib.rs にある。
```

## 5. ビルド

```bash
cd emfe_plugins/foo
cargo build --release
```

出力: `target/release/emfe_plugin_foo.dll`

静的 CRT リンクの確認:

```bash
dumpbin /dependents target/release/emfe_plugin_foo.dll
```

`KERNEL32.dll`, `ntdll.dll`, `api-ms-win-core-*.dll` のみが表示され、
`VCRUNTIME140.dll` が出てこなければ成功。

## 6. フロントエンドへの配線

C++ プラグインと同じだが、DLL のパスが `target/release/` になるだけ:

```xml
<!-- emfe_WinUI3Cpp/emfe/emfe.vcxproj -->
<Content Include="$(ProjectDir)..\..\emfe_plugins\foo\target\release\emfe_plugin_foo.dll"
         Condition="Exists('$(ProjectDir)..\..\emfe_plugins\foo\target\release\emfe_plugin_foo.dll')">
  <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
  <DeploymentContent>true</DeploymentContent>
  <TargetPath>plugins\%(Filename)%(Extension)</TargetPath>
</Content>
```

```xml
<!-- emfe_CsWPF/emfe/emfe.csproj -->
<None Include="..\..\emfe_plugins\foo\target\release\emfe_plugin_foo.dll"
      Condition="Exists('..\..\emfe_plugins\foo\target\release\emfe_plugin_foo.dll')">
  <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
  <Link>plugins\%(Filename)%(Extension)</Link>
  <Visible>false</Visible>
</None>
```

## 7. Integration テスト

`tests/smoke.rs` は `crate-type = ["cdylib", "rlib"]` の **rlib** 側と
リンクするので、extern "C" 関数を直接使える:

```rust
use emfe_plugin_foo::*;
use std::ptr;

#[test]
fn negotiate_ok() {
    let info = EmfeNegotiateInfo {
        api_version_major: EMFE_API_VERSION_MAJOR,
        api_version_minor: EMFE_API_VERSION_MINOR,
        flags: 0,
    };
    assert_eq!(emfe_negotiate(&info), EmfeResult::Ok);
}

#[test]
fn create_and_destroy() {
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    assert!(!h.is_null());
    assert_eq!(emfe_destroy(h), EmfeResult::Ok);
}
```

実行: `cargo test --release`

## 8. Rust 特有の落とし穴

### 8.1 `#[no_mangle]` を全 FFI 関数に

付け忘れると関数名が mangle されてエクスポートテーブルに出てこない。
同時に `extern "C"` も必須。

### 8.2 Panic セーフティ

Rust の panic が C ABI 境界を越えて unwind すると **undefined behaviour**。
2 つの対策:

- `[profile.release]` に `panic = "abort"` を設定 (上のテンプレで既に設定
  済み) — unwind せずにプロセス終了
- コールバックが多い場合は各 entry point 本体を `std::panic::catch_unwind`
  で囲み、panic 時に `EMFE_ERR_INVALID` を返す

### 8.3 構造体レイアウト

ABI を越える struct は全て `#[repr(C)]` 必須。明示的 discriminant を持つ
enum も同様。Rust 既定レイアウトは unspecified。

### 8.4 文字列所有権

`CString` / `String` をインスタンスのストレージフィールドに保持し、
`as_ptr()` を配る (ストレージ内部を指す)。`into_raw()` は対応する解放関数
を持たない限り使わない。

### 8.5 Send / Sync

生ポインタフィールド (user data 用) を持つ struct は既定で
`!Send + !Sync`。プラグインが本当にスレッドをまたいでインスタンスを
使う (emulation worker は使う) なら、`unsafe impl Send for T {}` /
`unsafe impl Sync for T {}` を追加する。

### 8.6 Integration test には `rlib` が必要

`crate-type = ["cdylib", "rlib"]` としないと、`tests/*.rs` から
`use emfe_plugin_foo::*;` できない。必ず `rlib` を加える。

## 9. 次のステップ

- 契約詳細は
  [`plugin_developer_guide_ja.md`](plugin_developer_guide_ja.md)
- `emfe_plugins/mc6809/` をコピーして改変するのが近道 — 完全な FFI
  カバレッジが ~1,200 行で揃っている Rust リファレンス実装
