# Rust Plugin Quickstart (Cargo + cdylib)

This guide walks through creating a new **Rust** emfe plugin. The result is
a Windows DLL (`cdylib`) with the MSVC C runtime statically linked, matching
the shape of the C++ plugins.

For the contract details, see [`plugin_developer_guide.md`](plugin_developer_guide.md).

Existing Rust plugin to copy from: `emfe_plugins/mc6809/`.

---

## 1. Directory scaffold

```
emfe_plugins/foo/
├── Cargo.toml
├── .cargo/
│   └── config.toml         # static MSVC CRT
├── README.md
├── docs/
│   ├── foo_reference.md
│   └── foo_reference_ja.md
├── src/
│   └── lib.rs              # all FFI + instance data
└── tests/
    └── smoke.rs            # integration tests via extern "C"
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
# cdylib: DLL; rlib: allow integration tests to link against the lib.
crate-type = ["cdylib", "rlib"]
path = "src/lib.rs"

[dependencies]
# your backend crate(s), e.g. a CPU core you wrap:
# em_foo = { path = "../../em_foo", default-features = false }

[profile.release]
opt-level = 3
lto = true
codegen-units = 1
panic = "abort"
```

## 3. `.cargo/config.toml` — static CRT

```toml
# Statically link MSVC C runtime. Matches C++ plugins' behaviour and avoids
# VCREDIST deployment.
[target.x86_64-pc-windows-msvc]
rustflags = ["-C", "target-feature=+crt-static"]
```

## 4. `src/lib.rs` — minimum skeleton

```rust
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(clippy::missing_safety_doc)]

use std::ffi::{c_char, c_void, CStr, CString};
use std::ptr;

// --- emfe_plugin.h mirrors ----------------------------------------------

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

// ... repeat for every struct/enum you use from emfe_plugin.h ...
// (See emfe_plugins/mc6809/src/lib.rs for a complete translation.)

pub type EmfeInstance = *mut c_void;

// --- Instance data -------------------------------------------------------

struct PluginInstance {
    pc: u16,
    memory: Box<[u8; 0x10000]>,
    // ... more fields ...
}

// SAFETY: raw pointers inside are opaque host handles we never dereference
// outside our own callbacks; the instance is accessed single-threaded.
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

// --- Static board info (lives for the DLL's lifetime) -------------------

static BOARD_NAME: &[u8] = b"FOO\0";
static CPU_NAME:   &[u8] = b"FOOCPU\0";
static DESC:       &[u8] = b"FOO plugin skeleton\0";
static VERSION:    &[u8] = b"0.1.0\0";

// --- FFI entry points ---------------------------------------------------

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

// ... add all the remaining emfe_* entry points ...
// The complete set is implemented in emfe_plugins/mc6809/src/lib.rs.
```

## 5. Build

```bash
cd emfe_plugins/foo
cargo build --release
```

Output: `target/release/emfe_plugin_foo.dll`.

Verify static CRT:

```bash
dumpbin /dependents target/release/emfe_plugin_foo.dll
```

You should see only `KERNEL32.dll`, `ntdll.dll`, `api-ms-win-core-*.dll`.
No `VCRUNTIME140.dll`.

## 6. Wire into frontends

Paths match the C++ plugins except the DLL lives under `target/release/`
instead of `build/bin/Release/`:

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

## 7. Integration tests

`tests/smoke.rs` can use the same `extern "C"` names directly because
`crate-type = ["cdylib", "rlib"]` also builds an rlib that can be linked
by the test binary:

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

Run with `cargo test --release`.

## 8. Rust-specific gotchas

### 8.1 `#[no_mangle]` on every FFI function

Without it the function name is mangled and won't appear in the exports
table. The C ABI demands `extern "C"` as well.

### 8.2 Panic safety

A Rust panic that unwinds across a C ABI boundary is **undefined
behaviour**. Two mitigations:

- Set `panic = "abort"` in `[profile.release]` (done in the template
  above) — the process exits instead of unwinding.
- For callback-heavy code, optionally wrap each entry point body in
  `std::panic::catch_unwind` and return `EMFE_ERR_INVALID` on panic.

### 8.3 Struct layout

Every struct crossing the ABI needs `#[repr(C)]`. Same for enums with
explicit discriminants. Rust's default layout is unspecified.

### 8.4 String ownership

`CString` / `String` owned by an instance's storage fields — hand out
`as_ptr()` (which points into that storage). Never `.into_raw()` unless
you also have a matching release function.

### 8.5 Send / Sync

Raw pointer fields (for user data) make structs `!Send + !Sync` by
default. Add `unsafe impl Send for T {}` / `unsafe impl Sync for T {}`
if your plugin genuinely uses the instance across threads (which it does
for the emulation worker).

### 8.6 Integration tests need `rlib`

Without `crate-type = ["cdylib", "rlib"]`, tests in `tests/*.rs` can't
`use emfe_plugin_foo::*;`. Add `"rlib"` to the crate-type array.

## 9. Next steps

- Read [`plugin_developer_guide.md`](plugin_developer_guide.md) for the
  contract details.
- Copy `emfe_plugins/mc6809/` — it's the reference Rust plugin with
  complete FFI coverage (~1200 LoC).
