# Phase 6 — Settings UI + debugger parity with mc6809

Status: **Design draft** — review-ready, no implementation yet.
Precedent: [`phase4_design.md`](phase4_design.md) / [`phase5_design.md`](phase5_design.md)
— same review-doc shape.

This document fills in the level of detail
[`implementation_plan.md`](implementation_plan.md) §8 leaves open.
Covers the gap between z80's current state (Phase 5 closed) and the
"debugger feature parity with mc6809" bar — what to implement, in
what order, with each sub-phase isolating one concern.

## 0. Where Phase 5 left us (recap)

Phase 5 closed 2026-05-24 with all four MVP smoke tests passing
locally (DIR / PIP / STAT / ED on a real booted CP/M 2.2; 112/112
plugin tests). The plugin has:

- Full Z80 ISA (Phases 1–3).
- Full CP/M boot stack + bootable disk image build pipeline
  (Phases 4–5).
- 9 Settings keys (General + Console + Disk groups).
- Full Z80 disassembler (Phase 2.5 + 3.1 + 3.2 + 3.3).
- 27 register definitions, including IFF1/IFF2/IM and the shadow
  set.

What's still stubbed (returning `EMFE_ERR_UNSUPPORTED` from
[`plugin_z80.cpp`](../src/plugin_z80.cpp)):

| ABI | State | Used for |
|---|---|---|
| `emfe_step_over`  | stub | "next" — execute over `CALL` |
| `emfe_step_out`   | stub | "finish" — run until current frame returns |
| `emfe_run`        | stub | continuous execution; needed for "let it free-run" + breakpoint workflow |
| `emfe_add_breakpoint` | stub | PC breakpoint |
| `emfe_remove_breakpoint` | stub | — |
| `emfe_enable_breakpoint` | stub | toggle without re-add |
| `emfe_set_breakpoint_condition` | stub | conditional BP (Phase 6 stretch) |
| `emfe_get_breakpoints` | returns 0 | enumerate BPs for the host's BP pane |
| `emfe_clear_breakpoints` | returns OK (no-op) | — |
| `emfe_add_watchpoint` | stub | RAM read/write watch |
| `emfe_remove_watchpoint` | stub | — |
| `emfe_stop` | returns OK without doing anything | needed once `emfe_run` actually runs |

Plus a couple of Settings keys mc6809 has that we don't:

| Key | mc6809 | z80 |
|---|---|---|
| `BoardType` | "Generic" combo | — |
| `Theme` | Dark/Light/System combo | — |

## 1. Acceptance criterion

Phase 6 done when:

1. The host's debugger pane (in [`emfe_CsWPF`](https://github.com/hha0x617/emfe_CsWPF) /
   [`emfe_WinUI3Cpp`](https://github.com/hha0x617/emfe_WinUI3Cpp))
   can do everything against z80 that it can against mc6809: set
   PC breakpoints + watchpoints, single-step / step-over / step-
   out, run-to-cursor, observe halt on BP/WP hit.
2. The Settings dialog for z80 has the same shared keys as
   mc6809 (`BoardType`, `Theme`) plus the z80-specific keys
   already in place.
3. New CI tests cover the breakpoint / watchpoint / run lifecycle
   in isolation (no host required — same SKIP-aware pattern as
   the existing 112 tests).

## 2. Sub-phase decomposition

Five sub-phases, each commit-sized and isolating one concern:

| # | Theme | Surface area | Acceptance |
|---|---|---|---|
| 6.1 | `emfe_run` + `emfe_stop` lifecycle | Worker thread in `plugin_z80.cpp` calling `Z80Cpu::ExecuteOne` in a tight loop; state transitions stopped → running → stopped via atomic flags; existing `state` field becomes a real state machine | `TestRunStopRoundTrip` exercises start/stop/start cleanly without crash; `emfe_run` then `emfe_stop` returns the CPU to a known stopped state at any PC |
| 6.2 | Breakpoint primitives | `std::unordered_set<uint16_t>` of enabled BPs in `Z80Cpu`; check on every `ExecuteOne` before fetching the opcode; halts the worker loop when hit (worker observes the flag from `ExecuteOne`'s return / a side-channel) | `TestBreakpointHaltsOnHit`, `TestRemoveBreakpoint`, `TestEnableDisableBreakpoint`, `TestGetBreakpointsEnumeration` — all unit tests in `test_z80.cpp` |
| 6.3 | Watchpoint primitives | Per-watchpoint structs in `Z80Memory` checked on `WriteByte` / `ReadByte` for `EMFE_WATCH_READ` / `EMFE_WATCH_WRITE` / `EMFE_WATCH_READWRITE`; halts the worker on hit | `TestWatchpointHaltsOnRead`, `TestWatchpointHaltsOnWrite`, `TestWatchpointReadwrite`, `TestRemoveWatchpoint`, `TestWatchpointTypes` |
| 6.4 | `emfe_step_over` / `emfe_step_out` | `step_over`: detect `CALL`/`CALL cc`/`RST n` at PC; record `expected_return_pc = PC + opcode_len`; run until PC matches OR a BP/WP fires; for non-CALL ops degenerates to `emfe_step`. `step_out`: capture `current_sp`; run until `SP > current_sp + 1` (a RET popped) OR a BP/WP fires | `TestStepOverCall`, `TestStepOverConditionalCall`, `TestStepOverRst`, `TestStepOverNonCall`, `TestStepOutSimple`, `TestStepOutNested` |
| 6.5 | Settings parity + cosmetic catch-up | Add `BoardType` (Combo, default "Generic") + `Theme` (Combo, default "System") matching mc6809's keys; update host integrations | Existing `TestSettingsDefsPresent` extended; spot-check the host's Settings dialog visually shows BoardType/Theme entries |

## 3. Specific design decisions

### 3.1 Run / stop state machine (sub-phase 6.1)

z80's current `inst->state` is an `std::atomic<EmfeState>` but
nothing reads it. To make `emfe_run` real:

```cpp
struct EmfeInstanceData {
    // ...
    std::atomic<EmfeState> state{EMFE_STATE_STOPPED};
    std::atomic<bool>      stopRequested{false};
    std::thread            runThread;
};
```

`emfe_run`:
1. Verify state is STOPPED.
2. Transition to RUNNING.
3. Spawn worker thread.

Worker:
```
while (!stopRequested) {
    if (Z80Cpu::ExecuteOne() == 0) {  // unsupported opcode
        break;
    }
    if (BreakpointHit()) {
        break;
    }
    if (WatchpointHit()) {
        break;
    }
}
state = STOPPED;
stopRequested = false;
```

`emfe_stop` sets `stopRequested = true` and `runThread.join()`.

`emfe_destroy` must do the same before tearing down — already
handled in the plugin's destructor if we set `stopRequested`
before delete.

### 3.2 Breakpoints (sub-phase 6.2)

`Z80Cpu` gets:

```cpp
class Z80Cpu {
    std::unordered_set<uint16_t> enabledBreakpoints;
    std::unordered_set<uint16_t> disabledBreakpoints;
    // ...
};
```

`emfe_add_breakpoint(addr)` adds to `enabledBreakpoints`.
`emfe_enable_breakpoint(addr, false)` moves from enabled →
disabled set. `emfe_remove_breakpoint(addr)` erases from both.
`emfe_get_breakpoints` walks both sets and reports each entry
with its enabled flag.

Hit detection lives inside `ExecuteOne`:
```cpp
int Z80Cpu::ExecuteOne() {
    if (enabledBreakpoints.contains(PC)) {
        return -1;  // sentinel: breakpoint hit, PC unchanged
    }
    // ... normal dispatch
}
```

`-1` sentinel propagates to the worker loop, which sees the
non-positive return and breaks out. Existing `> 0` callers
(unit tests doing direct `ExecuteOne`) still see cycles ≥ 1,
so they aren't affected.

`emfe_step` is its own caller — when PC is on a BP it should
still execute (otherwise stepping past a BP would be impossible).
Add an "in-step" flag: `emfe_step` sets it, `ExecuteOne` skips
the BP check while it's set, clears it on return.

### 3.3 Watchpoints (sub-phase 6.3)

Watchpoints fire on memory access. Add to `Z80Memory`:

```cpp
struct Watchpoint {
    uint16_t addr;
    EmfeWatchpointSize  size;  // 1/2/4 bytes
    EmfeWatchpointType  type;  // READ / WRITE / READWRITE
};
class Z80Memory {
    std::vector<Watchpoint> watchpoints;
    std::atomic<bool>       watchpointHit{false};
    // ...
};
```

`ReadByte` / `WriteByte` / `ReadWord` / `WriteWord` walk
`watchpoints` and set `watchpointHit` on a match. Worker loop
observes the flag (same channel as BP).

Cost: one linear scan per memory access. With ≤ 4 watchpoints
typical, this is ~4 comparisons per access — negligible
versus the ~50-instruction CCP loop, ~5-instruction inner CPU
loop. If profiling later shows it dominates, switch to
`unordered_map<uint16_t, Watchpoint>`.

### 3.4 Step-over / step-out (sub-phase 6.4)

**`step_over`**: only behaves differently from `step` for
calling-style instructions:

| Opcode | Name | Bytes | Return PC |
|---|---|---|---|
| `CD nn nn` | CALL nn | 3 | PC + 3 |
| `C4/CC/D4/DC/E4/EC/F4/FC nn nn` | CALL cc, nn | 3 | PC + 3 |
| `C7/CF/D7/DF/E7/EF/F7/FF` | RST n | 1 | PC + 1 |

For other opcodes, `step_over` == `step` (single-instruction).

Algorithm:
```cpp
EmfeResult emfe_step_over(inst) {
    uint8_t op = peek(PC);
    if (op != CALL_OPCODES && op != RST_OPCODES) return emfe_step(inst);
    uint16_t expected_return = PC + opcode_len(op);
    // Run a bounded loop until PC == expected_return OR BP/WP fires
    for (int64_t i = 0; i < STEP_LIMIT; ++i) {
        ExecuteOne();
        if (PC == expected_return) return EMFE_OK;
        if (breakpoint_or_watchpoint_hit) return EMFE_OK;
    }
    return EMFE_ERR_TIMEOUT;  // protective cap
}
```

`STEP_LIMIT` ~ 10 M instructions — generous for any normal
CALL, but bounded so a misbehaving callee doesn't hang the host
forever.

**`step_out`**: run until SP returns past its current value
(i.e., one RET fires). Tricky because nested CALLs push deeper —
need to wait until SP > current_sp_at_entry.

```cpp
EmfeResult emfe_step_out(inst) {
    uint16_t startSp = SP;
    for (int64_t i = 0; i < STEP_LIMIT; ++i) {
        ExecuteOne();
        if (SP > startSp) return EMFE_OK;
        if (bp_or_wp) return EMFE_OK;
    }
    return EMFE_ERR_TIMEOUT;
}
```

### 3.5 Settings parity (sub-phase 6.5)

Add to the `addSetting` block in `emfe_create`:

```cpp
addSetting("BoardType", "Target Board", "General",
           EMFE_SETTING_COMBO, "Generic", "Generic", RR);
addSetting("Theme", "Theme", "General",
           EMFE_SETTING_COMBO, "System", "Dark|Light|System", 0);
```

`BoardType` is "Generic"-only for z80 today; future plugins/
boards (Xerox 1800 era, MSX, ZX Spectrum) would extend the
constraint string. `Theme` is host-rendered and doesn't
affect the plugin — just propagated through.

## 4. Concurrency notes

Phase 6.1's worker thread is the first concurrency surface in
the plugin. Disciplines:

- All worker mutations to `Z80Cpu` registers / RAM happen on
  the worker thread.
- Host queries (`emfe_get_registers`, `emfe_peek_byte` etc) can
  fire from any thread. **These already lock nothing** in the
  current codebase, but the existing per-instance unit tests
  don't exercise concurrency. Phase 6.1 deliverable includes
  audit + adding minimal locking only where strictly needed.
- The `CaptureCtx` + UART RX FIFO already use a mutex
  (`Z80Uart::m_mutex`) for cross-thread safety, since
  `emfe_send_string` and the CPU worker both touch the RX FIFO.
  That precedent extends naturally to BP/WP state.

## 5. CI implications

Existing CI runs `test_z80.exe` on the Windows runner. With
Phase 6:

- BP / WP / run / stop tests join the existing 112; expect ~10
  new tests (one per sub-phase x ~2 cases each).
- The CP/M smoke tests (5.4) still SKIP cleanly without the
  disk image; this stays the same.
- No new Windows-runner cost beyond a few hundred ms of test
  execution.

## 6. Open questions for review

1. **`-1` sentinel from `ExecuteOne` for BP hit** vs a side-
   channel `Z80Cpu::brokeOnBreakpoint` flag. Lean: **sentinel**
   — keeps the return-value contract single-source-of-truth.
   Cons: tightens the API contract; future "skipped instruction
   because of something else" returns would have to use 0 or
   negative-but-not-`-1` markers.
2. **Watchpoint scope** — at PC fetch (instruction-level) vs
   on every byte of memory access (data-level). z80 access
   patterns are mostly single-byte / 16-bit-word; mc6809 same.
   Lean: **per-byte at `ReadByte/WriteByte`** — simplest, finest
   granularity, the user can range-watch by adding multiple WPs.
3. **`emfe_set_breakpoint_condition`** — full Z80 expression
   language is overkill for Phase 6. Lean: **stub-with-success**
   that records the string but never evaluates it; conditional
   BPs land in a Phase 6.x follow-up if demand exists.
4. **`emfe_get_watchpoints`** — not in the ABI today? The mc6809
   plugin doesn't expose one either. Check whether the host
   debugger pane needs it. Lean: **defer** until asked.
5. **`step_over` semantics for non-CALL opcodes** — Lean:
   **degenerate to `step`** so the host can call `step_over`
   unconditionally without checking PC's opcode.
6. **`STEP_LIMIT`** — 10 M is roughly 0.1 s of guest CPU at
   plugin throughput. Generous for "is this CALL going to
   return?" but capped so a runaway doesn't hang the host
   debugger UI. Lean: **10 M with `EMFE_ERR_TIMEOUT`** so the
   host can show "step-over timed out — bug in guest?" .
7. **Thread safety audit** — what register/memory state can
   the host poll mid-run safely? Atomic reads of registers are
   probably fine (Z80 has 16-bit pair stores; tearing on a
   read midway through a write could give junk). Lean: **no
   locking on register reads** (accept torn views during run),
   advise host to only poll when state == STOPPED. Document
   this in the API.
8. **`BoardType` future values** — what boards eventually go
   here? Lean: leave at `Generic` only for Phase 6; revisit
   when a specific RC2014 / Xerox 1800 / MSX target plan
   materialises.

## 7. Phase 6 done when

- Sub-phases 6.1–6.5 all merged.
- `TestRunStopRoundTrip`, `TestBreakpointHaltsOnHit`,
  `TestWatchpointHaltsOnWrite`, `TestStepOverCall`,
  `TestStepOutSimple` all pass in CI.
- z80's `emfe_get_setting_defs` returns `BoardType` and `Theme`
  in addition to the existing 9 keys.
- One spot-check session in the host (`emfe_CsWPF` or
  `emfe_WinUI3Cpp`) confirms the debugger pane behaves the same
  way against z80 as it does against mc6809.

## 8. After Phase 6

- **Phase 7** (optional cycle-accurate T-state mode) becomes
  the natural next step — the staging in Phase 1.x already
  uses a per-opcode cycle table; Phase 7 enforces per-T-state
  ordering on memory accesses for cycle-accurate testing.
- **Phase 5.5** (TP3 / Hi-Tech C) can land alongside Phase 6
  if license posture is worked out — these are independent.
