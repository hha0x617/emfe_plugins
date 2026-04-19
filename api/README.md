# emfe

Shared header and developer documentation for the **emfe** (Emulator
Frontend) plugin architecture.

Emulator backends are built as DLLs and connected to the frontends
(C++ WinUI3, C# WPF) through a C ABI.

日本語版は [`README_ja.md`](README_ja.md) を参照してください。

## What this repo contains

| File / directory | Contents |
|---|---|
| `emfe_plugin.h` | The complete C/C++ header: every function declaration, struct, and enum. |
| [`docs/plugin_developer_guide.md`](docs/plugin_developer_guide.md) | Plugin developer guide (English) |
| [`docs/plugin_developer_guide_ja.md`](docs/plugin_developer_guide_ja.md) | Plugin developer guide (Japanese) |
| [`docs/ffi_reference.md`](docs/ffi_reference.md) | Per-function FFI reference (English) |
| [`docs/ffi_reference_ja.md`](docs/ffi_reference_ja.md) | Per-function FFI reference (Japanese) |
| [`docs/quickstart_cpp.md`](docs/quickstart_cpp.md) | C++ plugin quickstart (English) |
| [`docs/quickstart_cpp_ja.md`](docs/quickstart_cpp_ja.md) | C++ plugin quickstart (Japanese) |
| [`docs/quickstart_rust.md`](docs/quickstart_rust.md) | Rust plugin quickstart (English) |
| [`docs/quickstart_rust_ja.md`](docs/quickstart_rust_ja.md) | Rust plugin quickstart (Japanese) |

## Highlights

- C ABI callable from both C++ (`LoadLibrary` / `GetProcAddress`) and
  C# (`P/Invoke`).
- Plugins can be implemented in either C++ (MSVC DLL) or Rust (`cdylib`).
- Data-driven UI: the frontend builds its register panel and settings
  dialog from arrays the plugin returns.
- 64-bit addresses (`uint64_t`) to accommodate future 64-bit targets.
- Opaque handle (`EmfeInstance`) for multi-instance support.

## Related projects

### Frontends

| Project | Role |
|---|---|
| [`emfe_WinUI3Cpp/`](../../emfe_WinUI3Cpp/) | C++ WinUI3 frontend |
| [`emfe_CsWPF/`](../../emfe_CsWPF/) | C# WPF frontend |

### Plugins

All under [`emfe_plugins/`](../) (the parent of this `api/` directory):

| Plugin | CPU / system | Implementation language |
|---|---|---|
| [`mc68030/`](../mc68030/) | Motorola MC68030 (MVME147 board) | C++ (wraps Em68030) |
| [`em8/`](../em8/) | EM8 (toy 8-bit) | C++ (self-contained) |
| [`z8000/`](../z8000/) | Zilog Z8000 family (Z8001/Z8002/Z8003/Z8004) | C++ (self-contained) |
| [`mc6809/`](../mc6809/) | Motorola MC6809 + MC6850 ACIA | Rust (wraps em6809) |

Built DLLs are copied into the frontends' `plugins\` subdirectory
automatically by the build system. Frontends scan `plugins\emfe_plugin_*.dll`
to populate the "Switch Plugin" dialog.

## API categories

See [`docs/ffi_reference.md`](docs/ffi_reference.md) for full details.

| Category | # | Representative functions |
|---|---|---|
| Discovery / Lifecycle | 4 | `emfe_negotiate`, `emfe_get_board_info`, `emfe_create`, `emfe_destroy` |
| Callbacks | 3 | `emfe_set_console_char_callback`, `emfe_set_state_change_callback`, `emfe_set_diagnostic_callback` |
| Registers | 3 | `emfe_get_register_defs`, `emfe_get_registers`, `emfe_set_registers` |
| Memory | 8 | `emfe_peek/poke_{byte,word,long}`, `emfe_peek_range`, `emfe_get_memory_size` |
| Disassembly | 3 | `emfe_disassemble_one`, `emfe_disassemble_range`, `emfe_get_program_range` |
| Execution | 10 | `emfe_step`, `emfe_run`, `emfe_stop`, `emfe_reset`, `emfe_get_state`, ... |
| Breakpoints | 6 | `emfe_add/remove/enable/set_breakpoint_condition/clear_breakpoints/get_breakpoints` |
| Watchpoints | 6 | same shape as breakpoints |
| File loading | 4 | `emfe_load_elf`, `emfe_load_binary`, `emfe_load_srec`, `emfe_get_last_error` |
| Settings | 14 | `emfe_get_setting_defs`, `emfe_get/set_setting`, `emfe_apply_settings`, `emfe_get_applied_setting`, list ops, save/load, `emfe_set_data_dir` |
| Console I/O | 2 | `emfe_send_char`, `emfe_send_string` |
| Framebuffer (Phase 3) | 3 | `emfe_get_framebuffer_info`, `emfe_get_palette*` |
| Input events (Phase 3) | 4 | `emfe_push_key`, `emfe_push_mouse_*` |
| Call stack (Phase 3) | 1 | `emfe_get_call_stack` |
| String utilities | 1 | `emfe_release_string` |

## License

MIT License
