# emfe Plugin Developer Guide

This document describes the contracts that an emfe plugin DLL must honour.
For per-function details see [`ffi_reference.md`](ffi_reference.md). For a
working skeleton to copy, see [`quickstart_cpp.md`](quickstart_cpp.md) or
[`quickstart_rust.md`](quickstart_rust.md).

---

## 1. Plugin model at a glance

```
┌─────────────────┐                                  ┌───────────────────┐
│ emfe frontend   │  LoadLibrary / P/Invoke          │ Plugin DLL        │
│ (WinUI3 / WPF)  │ ──────────────────────────────▶  │ emfe_plugin_X.dll │
│                 │                                   │                   │
│ Discovery:                                         emfe_negotiate       │
│ - scan plugins\                                    emfe_get_board_info  │
│                                                                         │
│ Per-instance lifecycle:                            emfe_create          │
│                                                    emfe_set_*_callback  │
│                                                    emfe_get_register_defs│
│                                                    emfe_get_setting_defs│
│ UI interaction:                                    emfe_step / run / ...│
│ - register view, disasm, memory, console, ...      emfe_peek/poke_* ... │
│                                                                         │
│ Shutdown:                                          emfe_destroy         │
└─────────────────┘                                  └───────────────────┘
```

- The plugin is a regular Windows DLL exporting the `emfe_*` C ABI.
- DLLs are discovered by scanning the `plugins\` subdirectory next to the
  frontend exe for `emfe_plugin_*.dll`.
- The frontend creates one or more **instances** via `emfe_create`. Each
  instance owns a complete emulator state (CPU, memory, peripherals).

## 2. The plugin lifecycle

### 2.1 Process start

1. Frontend scans `plugins\` for `emfe_plugin_*.dll`.
2. For each candidate, frontend loads it with `LoadLibrary`, then calls
   `emfe_negotiate` with its own `EMFE_API_VERSION_MAJOR/MINOR`.
3. If `emfe_negotiate` returns `EMFE_OK`, the plugin is added to the
   "available plugins" list (along with the name/cpu from
   `emfe_get_board_info`).

### 2.2 Per-instance lifecycle

```
emfe_create(&instance)
  │
  │   (optional)
  ▼
emfe_set_console_char_callback(instance, cb, user)
emfe_set_state_change_callback(instance, cb, user)
emfe_set_diagnostic_callback(instance, cb, user)
  │
  ▼
emfe_get_register_defs(instance, &defs)     // Build register UI
emfe_get_setting_defs(instance, &defs)      // Build settings UI
  │
  ▼
... (normal operation: step/run/load/peek/...)
  │
  ▼
emfe_destroy(instance)
```

### 2.3 Version negotiation

`emfe_negotiate` is the **first** function the frontend calls. Return:

| API major match | Outcome |
|---|---|
| Caller's major == plugin's major | `EMFE_OK` |
| Different majors | `EMFE_ERR_UNSUPPORTED` (frontend will skip the plugin) |

Minor version mismatches should return `EMFE_OK` — it's the frontend's job
to only invoke functions declared in its own header. Plugins may
conservatively reject older-than-expected minors if they strictly depend on
newer types.

## 3. Core design rules

### 3.1 Opaque handles

`EmfeInstance` is an opaque pointer (`void*`). The plugin chooses the
concrete type. The frontend **never** inspects it. One common pattern:

```c
struct EmfeInstanceData { /* ... */ };
EmfeResult EMFE_CALL emfe_create(EmfeInstance* out) {
    auto inst = new EmfeInstanceData();
    *out = reinterpret_cast<EmfeInstance>(inst);
    return EMFE_OK;
}
```

```rust
struct PluginInstance { /* ... */ }
#[no_mangle]
pub extern "C" fn emfe_create(out: *mut EmfeInstance) -> EmfeResult {
    let b = Box::new(PluginInstance::new());
    unsafe { *out = Box::into_raw(b) as EmfeInstance; }
    EmfeResult::Ok
}
```

### 3.2 String ownership

All `const char*` returned by the plugin (mnemonic, operands, raw_bytes,
setting names/labels/values, error messages, …) are **plugin-owned** and
valid at least until **the next call on the same instance** that might
update the same buffer.

Conventions in practice:

| Function returning `const char*` | Lifetime of the pointer |
|---|---|
| `emfe_get_register_defs` | Valid for the DLL's lifetime (plugin builds the array once) |
| `emfe_get_setting_defs` | Valid for the DLL's lifetime |
| `emfe_get_setting` | Valid until the next `emfe_get_setting` on the same instance |
| `emfe_disassemble_one/range` | Valid until the next disassembly call on the same instance |
| `emfe_get_last_error` | Valid until the next plugin call that might update it |
| `EmfeBoardInfo` strings | Valid for the DLL's lifetime |

The frontend never writes to these pointers and never frees them. If a
plugin ever needs to hand out dynamically allocated strings, it must keep
them alive in a buffer owned by the `EmfeInstanceData` (all existing
plugins do exactly this: they maintain `std::vector<std::string>` or
`Vec<CString>` storage fields).

`emfe_release_string` exists for future expansion but is currently a no-op
in all shipped plugins.

### 3.3 Threading

**Rules the plugin must observe**:

- `emfe_stop` may be called from **any thread** (typically the UI thread
  while the emulation thread is spinning inside `emfe_run`). It must set
  a stop flag and return promptly; it should not block on the emulation
  thread finishing the current instruction.
- All callbacks (`console_char_callback`, `state_change_callback`,
  `diagnostic_callback`) may fire from **the emulation thread** (not the
  UI thread). The frontend marshals them to its UI dispatcher as needed.
- All other functions are called from the UI thread while the emulation
  is **not running** (after `emfe_stop` has returned). Exceptions:
  - `emfe_send_char` / `emfe_send_string` may arrive while running (user
    typing in the console). They must be thread-safe w.r.t. the
    emulation thread's reads from the same FIFO.
- The plugin should treat the state machine (§ 3.4) as authoritative.

**What the frontend guarantees**:

- `emfe_destroy` is called only after any active `emfe_run` worker has
  been stopped and joined. Plugins still typically `stop_requested = true`
  and `join` internally in `emfe_destroy` as a safety net.

### 3.4 Execution state machine

```
             emfe_reset / emfe_create
                      │
                      ▼
    ┌─────────────STOPPED◀──────────────┐
    │               │                    │
    │               │ emfe_step          │ state callback
    │               ▼                    │ (STOP_REASON_STEP)
    │           STEPPING ────────────────┘
    │               │
    │ emfe_stop     │ emfe_run
    │               ▼
    └────────── RUNNING ───── emulation thread ───┐
                    │                              │
                    │ (BP / WP / HALT / user stop)│
                    ▼                              │
                HALTED  ─── state callback ────────┘
                              (STOP_REASON_*)
```

The `emfe_get_state` function reports the current state. State transitions
that aren't directly caused by the frontend (e.g. hit breakpoint, halt
instruction) **must** fire the state-change callback with an appropriate
`EmfeStopReason`.

## 4. Registers

### 4.1 Definition list

`emfe_get_register_defs` returns a pointer to a plugin-owned array of
`EmfeRegisterDef`. The frontend iterates the array to build the register
panel (one column per register). Key fields:

| Field | Purpose |
|---|---|
| `reg_id` | Plugin-chosen stable identifier (must match the id you expect in `get/set_registers`) |
| `name` | Short display name (e.g. `"D0"`, `"PC"`, `"RH0"`) |
| `group` | Tab / panel group (e.g. `"Data"`, `"Address"`, `"System"`, `"FPU"`, `"Counters"`) |
| `type` | `EMFE_REG_INT`, `EMFE_REG_FLOAT`, or `EMFE_REG_FLOAT80` |
| `bit_width` | 8, 16, 32, 64, 80 |
| `flags` | Bitmask: `_PC`, `_SP`, `_FLAGS`, `_FPU`, `_MMU`, `_READONLY`, `_HIDDEN` |

Frontends display flags specially:
- `_PC` — PC indicator in the disassembly / status bar
- `_SP` — stack pointer decoration
- `_FLAGS` — bit breakdown view (if the plugin implements one)
- `_HIDDEN` — excluded from the default register panel (but can still be
  accessed via `get/set_registers`). Typically used for cycle counters etc.

### 4.2 Get / set

`emfe_get_registers` / `emfe_set_registers` receive an array where each
entry has `reg_id` set; the plugin fills `value.u64` (or `f64` / `f80`).

Set should reject while running: return `EMFE_ERR_STATE` if the plugin
is in `EMFE_STATE_RUNNING`. All existing plugins follow this rule.

## 5. Memory

`emfe_peek_*` / `emfe_poke_*` operate **without side effects** — they're
used by the debugger (memory view, disassembly) and must be idempotent,
not fire MMIO handlers, not advance cycle counters, not mutate watchpoint
flags, etc.

**Guests see their bus through a separate path** that does include all of
the above. Do not confuse the two.

`emfe_get_memory_size` returns the largest valid address + 1 (for a flat
64 KB system, returns 65536). The frontend uses this to clamp the memory
view scrollbar.

## 6. Disassembly

`emfe_disassemble_one` fills an `EmfeDisasmLine` with `address`,
`raw_bytes`, `mnemonic`, `operands`, `length`. The strings are
plugin-owned and valid until the next disassembly call on the same
instance.

`emfe_disassemble_range` writes multiple entries. It's OK to stop early
(return fewer than `max_lines`) if a variable-length encoding ends before
`end_address`.

`emfe_get_program_range` reports the last-loaded program's address range
(for auto-scrolling the disassembly view). If unknown, return `{0, 0}`.

## 7. Execution

`emfe_step` executes **one** instruction synchronously and fires the state
callback with `EMFE_STOP_REASON_STEP`.

`emfe_run` starts a worker thread that executes until:
- `emfe_stop` is called (`EMFE_STOP_REASON_USER`)
- A breakpoint is hit (`EMFE_STOP_REASON_BREAKPOINT`)
- A watchpoint is hit (`EMFE_STOP_REASON_WATCHPOINT`)
- The CPU halts on a HALT-equivalent instruction (`EMFE_STOP_REASON_HALT`)
- An unrecoverable exception occurs (`EMFE_STOP_REASON_EXCEPTION`)

`emfe_step_over` / `emfe_step_out` are optional (return
`EMFE_ERR_UNSUPPORTED` if not implemented; the frontend degrades gracefully
to plain step).

`emfe_reset` resets CPU + peripheral state but **keeps the loaded program
in memory** (the frontend won't reload). It should also clear any pending
interrupts, re-read the reset vector if applicable, and notify state change.

`emfe_get_instruction_count` / `emfe_get_cycle_count` are used for MHz /
MIPS calculations in the status bar. They should never decrease, only
reset on `emfe_reset`.

## 8. Breakpoints & watchpoints

- Store breakpoints by address → `{enabled, condition}` in a plugin-side
  table.
- The emulation loop checks the table **before fetching** each instruction.
- A hit notifies the state callback with `EMFE_STOP_REASON_BREAKPOINT`
  and stops the worker. On resume, the frontend calls `emfe_run` again,
  and the plugin must not immediately re-trigger the same breakpoint
  (usual pattern: execute one instruction before checking BPs on resume).
- Conditions are free-form strings passed via
  `emfe_set_breakpoint_condition`; the plugin is responsible for parsing
  and evaluating them. Existing plugins ship a small expression evaluator
  supporting `==`, `!=`, `<=`, `>=`, `<`, `>`, `&`, `&&`, `||`, and
  register names.

Watchpoints behave identically but are keyed by memory access (read /
write / R-W).

## 9. Settings (data-driven UI)

`emfe_get_setting_defs` returns an array of `EmfeSettingDef`. The frontend
builds a Settings dialog tab per `group` field.

Key fields:

| Field | Meaning |
|---|---|
| `key` | Internal identifier (used by `emfe_get/set_setting`) |
| `label` | Human-readable display label |
| `group` | Tab name (`"General"`, `"Console"`, plugin-specific, ...) |
| `type` | `EMFE_SETTING_INT`, `_STRING`, `_BOOL`, `_COMBO`, `_FILE`, `_LIST` |
| `default_value` | Default as string |
| `constraints` | Type-specific:<br>• `INT`: `"min\|max"` (e.g. `"1\|256"`)<br>• `COMBO`: pipe-separated choices (e.g. `"Dark\|Light\|System"`)<br>• `FILE` / `STRING`: free form |
| `depends_on` / `depends_value` | The setting is shown only when `depends_on` equals `depends_value`. Used for per-board settings tabs. |
| `flags` | Bitmask. Currently defined: `EMFE_SETTING_FLAG_REQUIRES_RESET` (the setting is not hot-swap-safe; applies on next `emfe_reset` only). |

**Apply semantics**: Each setting lives in three states:

1. **staged** — written by `emfe_set_setting` on every dialog keystroke.
   Readable via `emfe_get_setting`.
2. **committed** — updated by `emfe_apply_settings` (OK button). This is
   what gets persisted by `emfe_save_settings`.
3. **applied** — currently in effect on the emulated hardware. Readable
   via `emfe_get_applied_setting`.

`emfe_apply_settings` copies staged → committed for every key, but only
**hot-swap-safe** settings (those **without** `EMFE_SETTING_FLAG_REQUIRES_RESET`)
propagate to `applied` immediately. REQUIRES_RESET settings — memory size,
board type, CPU variant, things that require tearing down and rebuilding
devices — wait until the next `emfe_reset` (full reset / restart), at
which point the plugin flushes `applied = committed`.

A typical plugin layout:

```cpp
std::unordered_map<std::string, std::string> stagedSettings;   // dialog state
std::unordered_map<std::string, std::string> settings;         // committed
std::unordered_map<std::string, std::string> appliedSettings;  // live on HW
std::unordered_map<std::string, uint32_t>    settingFlags;     // per-key flags

EmfeResult emfe_apply_settings(EmfeInstance h) {
    settings = stagedSettings;  // commit all
    for (auto& [k, v] : stagedSettings) {
        if (!(settingFlags[k] & EMFE_SETTING_FLAG_REQUIRES_RESET))
            appliedSettings[k] = v;  // hot-swap only
    }
    return EMFE_OK;
}

EmfeResult emfe_reset(EmfeInstance h) {
    appliedSettings = settings;  // flush deferred
    // ... rebuild devices that depend on REQUIRES_RESET keys ...
    cpu.Reset();
    return EMFE_OK;
}
```

Frontends show a pending indicator ("*" next to the label) whenever
`emfe_get_setting(key) != emfe_get_applied_setting(key)` for a
REQUIRES_RESET setting, so the user knows the change has not yet taken
effect.

**Persistence**: `emfe_save_settings` / `emfe_load_settings` persist the
committed (non-staged) values. On load, the plugin should set
`appliedSettings = settings` so that nothing is marked pending at
startup. The frontend can set the data directory with `emfe_set_data_dir`
before `emfe_create` (or during lifetime).

**List items** (`EMFE_SETTING_LIST`) are for dynamic rows (e.g. SCSI disk
list). A separate set of accessors (`emfe_get_list_item_defs`,
`emfe_get_list_item_count`, `emfe_get/set_list_item_field`,
`emfe_add/remove_list_item`) handles the row-by-column grid. Skip these
by returning 0/`EMFE_ERR_UNSUPPORTED` if the plugin has no list settings.

### 9.1 Three setting classes

In practice the simple "hot-swap vs REQUIRES_RESET" split above is too
coarse. The mc68030 plugin distinguishes three classes when handling
`emfe_apply_settings`:

| Class | When applied | Examples |
|---|---|---|
| **Hot-swappable** | Always immediate | JIT toggle, theme, console scrollback |
| **Hot-pluggable device** | Always immediate, with live HW reconfig | SCSI CD-ROM path/ID (UNIT ATTENTION + SCSI bus detach/attach) |
| **Deferred device-affecting** | Immediate only if emulation has not yet started since last reset; otherwise deferred until next `emfe_reset` | SCSI Disks list, Memory Size, Network Mode, Framebuffer geometry, Board Type |

Mechanism: track an `emulationStarted` flag (set by `emfe_run`, cleared
by `emfe_reset`). In `emfe_apply_settings`, run hot-swappable updates
unconditionally, run hot-pluggable updates unconditionally (with their
own live-reconfig hooks), then run the device-affecting rebuild only
when `!emulationStarted && config_differs`. While emulation is in
progress, deferred edits stay in `committed`/`stagedConfig` so the
frontend can render a pending marker.

**Critical**: when the device tree is torn down and rebuilt (because
e.g. memory size changed, board changed, or just `emfe_reset` after a
device-affecting edit), memory is re-initialized — which silently
wipes any kernel/program ELF that was loaded with `emfe_load_elf`.
After the rebuild, **re-load** the kernel from a remembered path
(e.g. `inst->lastLoadedFile`) and re-run any boot-stub setup so the
next `emfe_run` still has executable code in RAM.

### 9.2 LIST settings — pending markers via `emfe_is_list_pending`

The pending marker for plain settings works by comparing
`emfe_get_setting(key)` against `emfe_get_applied_setting(key)` —
two scalars. LIST settings have no scalar to compare, so they need a
plugin-provided indicator:

```c
EMFE_EXPORT int32_t EMFE_CALL emfe_is_list_pending(
    EmfeInstance instance,
    const char* list_key);
```

Returns 1 when the staged list differs from the applied list,
otherwise 0. The plugin is free to choose its own equality definition
(mc68030 compares element-wise: row count, then path + scsi-id per row).
Returns 0 also for unknown list_key or when the plugin doesn't track
applied list state separately.

This export is **optional** — frontends should use a soft `GetProcAddress`
(C++) / `TryLoadFunc` (C#) and skip the marker on older plugins that
don't ship it.

### 9.3 Per-target-OS settings pattern (mc68030 case study)

When a single board can boot multiple OSes that each want a different
device configuration (e.g. MVME147 + NetBSD vs Linux: different SCSI
disks, different CD-ROM ISOs), don't make the user re-edit the values
on every OS toggle. Instead, store **per-OS slots** alongside an active
denormalized snapshot:

```cpp
struct EmulatorConfig {
    // Active values — what SetupMvme147Devices reads.
    std::vector<ScsiDiskConfig>   Mvme147ScsiDisks;
    std::string                   Mvme147ScsiCdromPath;
    int                           Mvme147ScsiCdromId;
    // Per-OS storage — source of truth on disk.
    std::map<std::string, std::vector<ScsiDiskConfig>> Mvme147ScsiDisksByTargetOS;
    std::map<std::string, std::string>                 Mvme147ScsiCdromPathByTargetOS;
    std::map<std::string, int>                         Mvme147ScsiCdromIdByTargetOS;
    std::string TargetOS;

    void SyncMvme147ScsiForTargetOS(
        const std::string& oldOS, const std::string& newOS);
};
```

`SyncMvme147ScsiForTargetOS` saves the active values under the old OS's
slot, then reseats the active values from the new OS's slot (or
defaults if absent), and sets `TargetOS = newOS`. Hook it in
`emfe_set_setting`:

```cpp
else if (key == "TargetOS")
    cfg.SyncMvme147ScsiForTargetOS(cfg.TargetOS, val);
```

JSON persistence writes BOTH the legacy single-value field AND the
per-OS map, so older builds without the map can still read the legacy
field. Load auto-migrates configs that only have the legacy field by
copying it into both OS slots. The active value is reseated from the
map's entry for the current TargetOS.

**Frontend ordering constraint** — when a frontend's "save dialog
edits to staging" loop walks every setting and calls `emfe_set_setting`
for each, **TargetOS must be written last**. If TargetOS is written
first, the plugin's Sync swaps the active per-OS values to the new
OS's slot, and then the rest of the loop overwrites those just-restored
values with whatever the dialog UI was still rendering from the old
OS. Solution: split into two passes (everything-except-TargetOS, then
TargetOS).

Frontends that cache LIST edits dialog-side (e.g. `m_pendingLists`)
must also clear the cache on TargetOS change so that the rebuild
re-reads the new OS's list from the plugin instead of replaying the
stale snapshot.

## 10. Console I/O

A plugin typically exposes a virtual UART (or equivalent serial device)
and wires it to the frontend's console window:

- **TX (guest → host)**: when the guest writes a byte to the UART, call
  `console_char_callback(user_data, ch)`. The frontend enqueues the byte
  to its VT100 terminal.
- **RX (host → guest)**: when the user types in the console window, the
  frontend calls `emfe_send_char(instance, ch)`. The plugin pushes the
  byte into the UART's RX FIFO.

Threading: the TX callback fires from the emulation thread. The frontend
marshals to UI; the plugin need not.

## 11. Framebuffer, input, call stack (Phase 3)

These APIs exist in the header but most plugins return `EMFE_ERR_UNSUPPORTED`
or 0. Implement when your plugin models graphics / pointer devices /
subroutine call tracking.

## 12. Loading programs

- `emfe_load_binary(path, load_address)` — raw bytes placed at
  `load_address`. Typical follow-up: set PC from reset vector or from
  `load_address`.
- `emfe_load_srec(path)` — Motorola S-record (S1/S9 for 16-bit,
  S2/S8 for 24-bit, S3/S7 for 32-bit). Plugin decides which variants to
  accept.
- `emfe_load_elf(path)` — ELF executable. Optional; 8-bit/16-bit CPUs
  typically return `EMFE_ERR_UNSUPPORTED`.

Record the loaded range for `emfe_get_program_range`. Update PC from the
file's entry point if present; otherwise leave it unchanged.

Return `EMFE_ERR_IO` with a useful `emfe_get_last_error` message on failure.

## 13. Where to continue

- **Per-function spec**: [`ffi_reference.md`](ffi_reference.md).
- **C++ scaffold**: [`quickstart_cpp.md`](quickstart_cpp.md).
- **Rust scaffold**: [`quickstart_rust.md`](quickstart_rust.md).
- **Real examples**: the four shipped plugins — `mc68030/`, `em8/`,
  `z8000/`, `mc6809/` — sit alongside this `api/` directory inside
  `emfe_plugins/` and illustrate almost every pattern in this document.
