# emfe FFI Reference

Companion to [`plugin_developer_guide.md`](plugin_developer_guide.md). Each
entry gives the signature, semantic summary, and threading notes for a
single FFI function. Signatures are shown in C form; Rust plugins use the
same names via `#[no_mangle] pub extern "C"`.

Throughout this document:

- **UI** = the function is called from the frontend UI thread while the
  plugin is NOT running.
- **any** = may be called from any thread (the plugin must serialize
  internally).
- **worker** = the callback fires from the plugin's emulation thread.

For how the frontend orchestrates these calls, see
[`plugin_developer_guide.md`](plugin_developer_guide.md).

---

## Enumerations (quick recap)

```c
EmfeResult { OK=0, ERR_INVALID=-1, ERR_STATE=-2, ERR_NOTFOUND=-3,
             ERR_IO=-4, ERR_MEMORY=-5, ERR_UNSUPPORTED=-6 }

EmfeState { STOPPED=0, RUNNING=1, HALTED=2, STEPPING=3 }

EmfeStopReason { NONE=0, USER=1, BREAKPOINT=2, WATCHPOINT=3, STEP=4,
                 HALT=5, EXCEPTION=6 }

EmfeRegFlag bits: NONE, READONLY, PC, SP, FLAGS, FPU, MMU, HIDDEN
EmfeSettingType: INT, STRING, BOOL, COMBO, FILE, LIST
EmfeWatchpointType: READ, WRITE, READWRITE
EmfeBreakpointType: EXEC, READ, WRITE, RW
```

---

## 1. Discovery & Lifecycle

### `emfe_negotiate(const EmfeNegotiateInfo* info) -> EmfeResult`
Thread: **UI**.
First call after `LoadLibrary`. Returns `OK` if the caller's
`api_version_major` matches `EMFE_API_VERSION_MAJOR`, else
`ERR_UNSUPPORTED`.

### `emfe_get_board_info(EmfeBoardInfo* out) -> EmfeResult`
Thread: **UI**.
Fills in plugin-owned strings. Valid for DLL lifetime. Called before
`emfe_create` to populate the "Switch Plugin" dialog.

#### `EmfeBoardInfo::capabilities`

Bitwise OR of `EMFE_CAP_*` flags declaring which optional features the
plugin implements. The frontend uses these to enable/disable menu items,
toolbar buttons, and panels. A plugin that **sets** a flag must implement
the corresponding API; a plugin that **clears** a flag may leave the
entry point as a stub returning `EMFE_ERR_UNSUPPORTED`.

| Flag                       | Value       | API gated                              |
| -------------------------- | ----------- | -------------------------------------- |
| `EMFE_CAP_LOAD_ELF`        | `1 <<  0`   | `emfe_load_elf`                        |
| `EMFE_CAP_LOAD_SREC`       | `1 <<  1`   | `emfe_load_srec`                       |
| `EMFE_CAP_LOAD_BINARY`     | `1 <<  2`   | `emfe_load_binary`                     |
| `EMFE_CAP_STEP_OVER`       | `1 <<  3`   | `emfe_step_over`                       |
| `EMFE_CAP_STEP_OUT`        | `1 <<  4`   | `emfe_step_out`                        |
| `EMFE_CAP_CALL_STACK`      | `1 <<  5`   | `emfe_get_call_stack`                  |
| `EMFE_CAP_WATCHPOINTS`     | `1 <<  6`   | `emfe_add_watchpoint` et al.           |
| `EMFE_CAP_FRAMEBUFFER`     | `1 <<  7`   | `emfe_get_framebuffer_info`            |
| `EMFE_CAP_INPUT_KEYBOARD`  | `1 <<  8`   | `emfe_push_key`                        |
| `EMFE_CAP_INPUT_MOUSE`     | `1 <<  9`   | `emfe_push_mouse_*`                    |

### `emfe_create(EmfeInstance* out) -> EmfeResult`
Thread: **UI**.
Allocates a new emulator instance. Post-conditions: `BuildRegisterDefs`
and `BuildSettingDefs` are done; the instance is in `STOPPED`.

### `emfe_destroy(EmfeInstance) -> EmfeResult`
Thread: **UI**.
Plugin must stop any worker thread (typically via `stop_requested` flag +
`join`) before freeing. Safe to call even if the instance is currently
`RUNNING`.

## 2. Callbacks (plugin owns them per-instance)

### `emfe_set_console_char_callback(inst, cb, user) -> EmfeResult`
Thread: **UI**.
Plugin calls `cb(user, ch)` from the **worker** thread whenever the guest
UART writes a byte.

### `emfe_set_state_change_callback(inst, cb, user) -> EmfeResult`
Thread: **UI**.
Plugin calls `cb(user, &info)` from the **worker** thread on every state
transition that occurs autonomously (breakpoint, halt, exception, stop).
May also fire on stepping from the UI thread.

### `emfe_set_diagnostic_callback(inst, cb, user) -> EmfeResult`
Thread: **UI**.
Free-form diagnostic messages (log-level text) from the plugin. Thread
context unspecified.

## 3. Registers

### `emfe_get_register_defs(inst, const EmfeRegisterDef** out) -> int32_t count`
Thread: **UI**.
Returns the number of registers and a pointer to the plugin-owned array.
The array lives for the DLL's lifetime.

### `emfe_get_registers(inst, EmfeRegValue* values, int32_t count) -> EmfeResult`
Thread: **UI** (emulation stopped).
Caller fills `reg_id` for each entry; plugin fills `value.u64` (/f64 /f80).
Unknown reg_ids return `ERR_INVALID` and stop processing.

### `emfe_set_registers(inst, const EmfeRegValue* values, int32_t count) -> EmfeResult`
Thread: **UI** (emulation stopped).
Returns `ERR_STATE` if called while `RUNNING`. Read-only registers (flag
`READONLY`) are silently ignored.

## 4. Memory (side-effect-free)

### `emfe_peek_byte(inst, uint64_t addr) -> uint8_t`
### `emfe_peek_word(inst, uint64_t addr) -> uint16_t`
### `emfe_peek_long(inst, uint64_t addr) -> uint32_t`
### `emfe_poke_byte(inst, uint64_t addr, uint8_t) -> EmfeResult`
### `emfe_poke_word(inst, uint64_t addr, uint16_t) -> EmfeResult`
### `emfe_poke_long(inst, uint64_t addr, uint32_t) -> EmfeResult`
Thread: **UI**.
No side effects (no MMIO, no watchpoint trigger). Endianness follows the
target CPU — MC68030 / Z8000 / MC6809 are big-endian, EM8 is
little-endian-ish (byte-level). Word / long accesses do not have to be
aligned; plugins either enforce alignment or return value at unaligned
address at their discretion.

### `emfe_peek_range(inst, uint64_t addr, uint8_t* out_data, uint32_t length) -> EmfeResult`
Thread: **UI**.
Bulk read. `out_data` must have `length` bytes of capacity.

### `emfe_get_memory_size(inst) -> uint64_t`
Thread: **UI**.
Returns the addressable range (`max_address + 1`). Used for memory view
scrollbar clamping.

## 5. Disassembly

### `emfe_disassemble_one(inst, uint64_t addr, EmfeDisasmLine* out) -> EmfeResult`
Thread: **UI**.
Fills `address`, `raw_bytes`, `mnemonic`, `operands`, `length`. Strings are
plugin-owned, valid until the next disassembly call on the same instance.

### `emfe_disassemble_range(inst, uint64_t start, uint64_t end, EmfeDisasmLine* out, int32_t max_lines) -> int32_t count`
Thread: **UI**.
Batch version. May return fewer than `max_lines` if the variable-length
decoder reaches `end`.

### `emfe_get_program_range(inst, uint64_t* out_start, uint64_t* out_end) -> EmfeResult`
Thread: **UI**.
Reports the range of the most recently loaded program. Use `(0, 0)` when
nothing is loaded.

## 6. Execution control

### `emfe_step(inst) -> EmfeResult`
Thread: **UI**.
Executes exactly one instruction synchronously. Fires state callback with
`STOP_REASON_STEP`. Returns `ERR_STATE` if called while `RUNNING`.

### `emfe_step_over(inst) -> EmfeResult`
Thread: **UI**.
Async: step, but treat subroutine calls (JSR/BSR/CALL) as atomic (runs
until returning). May return `ERR_UNSUPPORTED` if the plugin can't
recognise subroutine calls.

### `emfe_step_out(inst) -> EmfeResult`
Thread: **UI**.
Async: run until current subroutine returns. Requires the plugin to track
a shadow call stack.

### `emfe_run(inst) -> EmfeResult`
Thread: **UI**.
Starts a worker thread. Returns immediately with `OK`. The caller must
watch for state-change callbacks to learn about stops.

### `emfe_stop(inst) -> EmfeResult`
Thread: **any**.
Sets the internal stop flag, joins the worker. Safe to call when already
stopped.

### `emfe_reset(inst) -> EmfeResult`
Thread: **UI**.
Resets CPU + peripherals. Does not clear memory. Fires state callback
with `STOP_REASON_NONE`.

### `emfe_get_state(inst) -> EmfeState`
Thread: **any**.

### `emfe_get_instruction_count(inst) -> int64_t`
### `emfe_get_cycle_count(inst) -> int64_t`
Thread: **any**.
Monotonic; reset only by `emfe_reset`. Used for MHz / MIPS display.

## 7. Breakpoints

### `emfe_add_breakpoint(inst, uint64_t addr) -> EmfeResult`
### `emfe_remove_breakpoint(inst, uint64_t addr) -> EmfeResult`
### `emfe_enable_breakpoint(inst, uint64_t addr, bool enabled) -> EmfeResult`
### `emfe_set_breakpoint_condition(inst, uint64_t addr, const char* cond) -> EmfeResult`
### `emfe_clear_breakpoints(inst) -> EmfeResult`
### `emfe_get_breakpoints(inst, EmfeBreakpointInfo* out, int32_t max) -> int32_t count`
Thread: **UI** (usually; some frontends set them while running via a
stop-add-resume cycle).
Addresses are execution breakpoints (not data). `condition` strings are
plugin-parsed (optional). `remove` on a missing address returns
`ERR_NOTFOUND`.

## 8. Watchpoints

### `emfe_add_watchpoint(inst, uint64_t addr, EmfeWatchpointSize, EmfeWatchpointType) -> EmfeResult`
### `emfe_remove_watchpoint(inst, uint64_t addr) -> EmfeResult`
### `emfe_enable_watchpoint(inst, uint64_t addr, bool enabled) -> EmfeResult`
### `emfe_set_watchpoint_condition(inst, uint64_t addr, const char* cond) -> EmfeResult`
### `emfe_clear_watchpoints(inst) -> EmfeResult`
### `emfe_get_watchpoints(inst, EmfeWatchpointInfo* out, int32_t max) -> int32_t count`
Thread: **UI**.
Same pattern as breakpoints but tied to memory access. Size is BYTE / WORD
/ LONG; the plugin tests the accessed byte range against the watchpoint
range.

## 9. Call stack

### `emfe_get_call_stack(inst, EmfeCallStackEntry* out, int32_t max) -> int32_t count`
Thread: **UI**.
Returns the current shadow call stack (entry 0 = innermost frame). Return
0 if the plugin doesn't track one.

## 10. Framebuffer (Phase 3)

### `emfe_get_framebuffer_info(inst, EmfeFramebufferInfo* out) -> EmfeResult`
Thread: **UI**.
Fill `width`, `height`, `bpp`, `stride`, `base_address`, `pixels`. The
`pixels` pointer is plugin-owned, valid until the plugin is destroyed or
the framebuffer is reconfigured. Return `ERR_UNSUPPORTED` if there's no
framebuffer.

### `emfe_get_palette_entry(inst, uint32_t index) -> uint32_t AARRGGBB`
### `emfe_get_palette(inst, uint32_t* out_colors, int32_t max_count) -> int32_t count`
Thread: **UI**.
Only meaningful for `EMFE_FB_FORMAT_INDEXED8`.

## 11. Input events (Phase 3)

### `emfe_push_key(inst, uint32_t scancode, bool pressed) -> EmfeResult`
### `emfe_push_mouse_move(inst, int32_t dx, int32_t dy) -> EmfeResult`
### `emfe_push_mouse_absolute(inst, int32_t x, int32_t y) -> EmfeResult`
### `emfe_push_mouse_button(inst, int32_t button, bool pressed) -> EmfeResult`
Thread: **UI**.
Used to feed host input events into a plugin with a keyboard / mouse
device. Unsupported plugins return `ERR_UNSUPPORTED`.

## 12. File loading

### `emfe_load_elf(inst, const char* path) -> EmfeResult`
Thread: **UI** (not while `RUNNING`).
Loads ELF executable segments; sets PC from entry point. Small-CPU plugins
typically return `ERR_UNSUPPORTED`.

### `emfe_load_binary(inst, const char* path, uint64_t load_address) -> EmfeResult`
Thread: **UI**.
Raw bytes at the given address. After loading, either (a) set PC from the
target's reset vector if the load covers it, or (b) set PC to
`load_address`.

### `emfe_load_srec(inst, const char* path) -> EmfeResult`
Thread: **UI**.
Parses Motorola S-Record. Support at least S1 (16-bit data) + S9
(start record). Larger plugins may also accept S2/S8 (24-bit) and
S3/S7 (32-bit).

### `emfe_get_last_error(inst) -> const char*`
Thread: **UI**.
Plugin-owned error string from the most recent failing call. Empty string
after successful operations.

## 13. Settings

### `emfe_get_setting_defs(inst, const EmfeSettingDef** out) -> int32_t count`
Thread: **UI**.
Plugin-owned array of definitions. Built once per instance.

### `emfe_get_setting(inst, const char* key) -> const char*`
### `emfe_set_setting(inst, const char* key, const char* value) -> EmfeResult`
### `emfe_apply_settings(inst) -> EmfeResult`
### `emfe_get_applied_setting(inst, const char* key) -> const char*`
Thread: **UI**.

Three setting states per key:

| State     | Accessor                      | Written by                                                |
| --------- | ----------------------------- | --------------------------------------------------------- |
| staged    | `emfe_get_setting`            | `emfe_set_setting` (every keystroke in the dialog)        |
| committed | *(internal)*                  | `emfe_apply_settings` copies staged → committed           |
| applied   | `emfe_get_applied_setting`    | hot-swap subset on `apply`; full flush on `emfe_reset`    |

`emfe_set_setting` only writes to the staged map — no side effects.
`emfe_apply_settings` commits all staged values, but only settings
**without** `EMFE_SETTING_FLAG_REQUIRES_RESET` take effect on live
hardware immediately (e.g. `Theme`, `Console*`). Settings **with**
that flag (e.g. `BoardType`, `MemorySize`, `CpuVariant`) are queued
for the next `emfe_reset`.

Frontends should show a "pending apply" indicator whenever
`emfe_get_setting(key) != emfe_get_applied_setting(key)` for a
REQUIRES_RESET setting.

#### `EmfeSettingDef::flags`

| Flag                              | Value      | Meaning                                                                 |
| --------------------------------- | ---------- | ----------------------------------------------------------------------- |
| `EMFE_SETTING_FLAG_REQUIRES_RESET`| `1u << 0`  | Change is deferred until `emfe_reset` (device not hot-swap-safe).       |

### `emfe_save_settings(inst) -> EmfeResult`
### `emfe_load_settings(inst) -> EmfeResult`
### `emfe_set_data_dir(const char* path) -> EmfeResult`
Thread: **UI**. `emfe_set_data_dir` is a **process-wide** DLL-level call
(not per-instance). The frontend calls it during startup to redirect the
plugin's persistence directory (e.g.
`%LOCALAPPDATA%\emfe_CsWPF\<plugin-specific subdir>`).

### List-item accessors (for `EMFE_SETTING_LIST`)
### `emfe_get_list_item_defs(inst, const char* list_key, const EmfeListItemDef** out) -> int32_t count`
### `emfe_get_list_item_count(inst, const char* list_key) -> int32_t`
### `emfe_get_list_item_field(inst, list_key, int32_t index, field_key) -> const char*`
### `emfe_set_list_item_field(inst, list_key, int32_t index, field_key, value) -> EmfeResult`
### `emfe_add_list_item(inst, const char* list_key) -> int32_t new_index`
### `emfe_remove_list_item(inst, const char* list_key, int32_t index) -> EmfeResult`
Thread: **UI**.
Used for dynamic row tables (e.g. SCSI disk list). Skip by returning 0 /
`ERR_UNSUPPORTED` if the plugin has no list settings.

### `emfe_is_list_pending(inst, const char* list_key) -> int32_t`
Thread: **UI**.
**Optional** export — frontends should resolve it via soft lookup
(`GetProcAddress` / `TryLoadFunc`) and skip the marker if the plugin
doesn't ship it. Returns `1` when the staged list (what `emfe_get_list_*`
reports) differs from the applied list (what the running hardware was
configured with), `0` when in sync. Used to render a pending marker on
LIST settings — the LIST counterpart to comparing
`emfe_get_setting` vs `emfe_get_applied_setting` for plain scalars.

Equality semantics are plugin-defined. The mc68030 plugin compares
element-wise: row count, then path + scsi-id per row. Returns `0` for
unknown `list_key`.

## 14. Console I/O

### `emfe_send_char(inst, char ch) -> EmfeResult`
### `emfe_send_string(inst, const char* s) -> EmfeResult`
Thread: **any** (user typing in console window usually happens while
emulation is running). The plugin must synchronise the RX FIFO between
this thread and the emulation thread's read.

## 15. String utilities

### `emfe_release_string(const char* s) -> void`
Reserved for future use where plugins hand out dynamically allocated
strings that the frontend must release. All shipped plugins implement this
as a no-op.

---

## Appendix: minimum function set for a useful plugin

To appear in the Switch Plugin dialog and do anything:

```
emfe_negotiate
emfe_get_board_info
emfe_create
emfe_destroy
emfe_get_register_defs      (can return an empty array)
emfe_get_registers
emfe_set_registers          (can reject everything)
emfe_peek_byte              (for memory view)
emfe_poke_byte
emfe_get_memory_size
emfe_disassemble_one        (can return "???")
emfe_step
emfe_get_state
emfe_reset
emfe_set_console_char_callback
emfe_set_state_change_callback
emfe_set_diagnostic_callback
emfe_get_setting_defs       (can return empty)
emfe_get_setting
emfe_set_setting
emfe_apply_settings
emfe_get_applied_setting
emfe_release_string         (usually no-op)
emfe_get_last_error         (can return "")
```

Everything else can return `ERR_UNSUPPORTED` or 0 until the plugin grows.
