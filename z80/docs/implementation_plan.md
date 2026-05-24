# emfe_plugin_z80 — Implementation Plan

Date: 2026-05-22
Status: **Pre-implementation** — companion to [`design_notes.md`](design_notes.md)

This document breaks the Phase 1–7 roadmap from
[`design_notes.md`](design_notes.md) into commit-sized, file-by-file work
items. It is the day-to-day implementation guide; the design notes remain
the source of truth for the *why*.

## 1. Pre-implementation decisions

Before starting Phase 1, the following items from `design_notes.md` §10
need a working answer. Each line is the recommended default; revisit
when the work actually reaches the relevant phase.

| Item | Recommended default |
|---|---|
| Cross-reference implementation for behaviour | **z80ex** (BSD-3-Clause, passes `zexall`). No code borrowed — used only for sanity-checking edge cases (flag propagation, undocumented opcodes, interrupt-ack timing) |
| Register-defs ABI shape (byte view + pair view) | Mirror `mc6809`'s `D = A:B` exposure. Each Z80 pair (`AF`, `BC`, `DE`, `HL`) appears both as a 16-bit pair and as its two 8-bit halves in the register table |
| Shadow registers (`AF'`, `BC'`, `DE'`, `HL'`) | Expose as separate registers in the same table so the debugger can observe them |
| Disk-controller MMIO shape | Do **not** emulate a historic controller (WD1793, NEC 765, …). Instead define a minimal "sector device" MMIO that the CBIOS drives directly. Final shape decided at Phase 4 start when the CBIOS is being written |
| Settings UI scope | Small preset matrix (CP/M 64 KiB / ROM-based 32 KiB / fully custom). No per-byte ROM/RAM map editor |

## 2. Repository scaffold

The plugin follows the existing C++ self-implemented pattern used by
`em8` and `z8000`. Final layout:

```
emfe_plugins/z80/
├── CMakeLists.txt              top-level (~30 LoC)
├── src/
│   ├── CMakeLists.txt          sources (~50 LoC)
│   ├── pch.h, pch.cpp          precompiled header (~30 LoC)
│   ├── Z80Cpu.h, Z80Cpu.cpp    CPU core (~250 + ~2000 LoC)
│   ├── Z80Disasm.cpp           disassembler (~400 LoC)
│   ├── Z80Memory.h, .cpp       64 KiB RAM + I/O space (~50 + ~80 LoC)
│   ├── Z80Uart.h, .cpp         console UART (~50 + ~80 LoC)
│   ├── Z80Timer.h, .cpp        CTC-style timer (~40 + ~70 LoC)
│   └── plugin_z80.cpp          emfe C ABI entry points (~1500 LoC)
├── tests/
│   ├── CMakeLists.txt
│   └── test_z80.cpp            Google Test (grows with each phase)
├── examples/                   added in later phases
│   ├── monitor/                custom ROM monitor (Phase 1 deliverable)
│   ├── zexdoc/, zexall/        Frank Cringle suite (Phase 2 / Phase 3)
│   ├── eforth/                 Bill Muench eForth-Z80 (Phase 3)
│   └── cpm22/                  CP/M 2.2 build recipe (Phase 4)
└── docs/
    ├── design_notes.md         done
    ├── design_notes_ja.md      done
    ├── implementation_plan.md  this file
    └── mmio.md                 written during Phase 1
```

LoC numbers are estimates against `em8` (~2960 total) and `z8000`
(~3200 total). Z80's prefix-rich ISA pushes the CPU core larger.

## 3. Phase 1 — Plugin scaffold + base ISA (1–2 weeks)

Phase 1 lands as **seven small PRs**, each independently green on CI.

| # | Branch | Commit theme | LoC | Acceptance |
|---|---|---|---|---|
| 1.1 | `feat/z80-phase-1-1-scaffold` | CMakeLists × 2, pch, empty headers, minimal `plugin_z80.cpp` that satisfies the emfe ABI enough to be loaded by the host | ~200 | `emfe_plugin_z80.dll` loads in the host with no functionality |
| 1.2 | `feat/z80-phase-1-2-memory-uart` | `Z80Memory` (flat 64 KiB + I/O space), `Z80Uart` (console), bring the ABI up to `emfe_negotiate` / `emfe_create` / `emfe_destroy` / `emfe_step` | ~300 | Host can create / step / destroy an instance with no opcodes implemented (step returns a `not-implemented` trap) |
| 1.3 | `feat/z80-phase-1-3-registers` | `Z80Cpu.h` declares all registers; `emfe_get_register_defs` / `emfe_get_registers` / `emfe_set_registers` work; `step()` executes only `NOP` (`$00`) | ~250 | Host shows register pane; stepping a NOP advances PC by 1 |
| 1.4 | `feat/z80-phase-1-4-data-movement` | `LD r,r` / `LD r,n` / `LD A,(nn)` / `LD (nn),A` / `LD rr,nn` / `LD HL,(nn)` and similar load/store opcodes | ~400 | Hand-written test ROM exercises every documented load form |
| 1.5 | `feat/z80-phase-1-5-arithmetic` | `ADD` / `ADC` / `SUB` / `SBC` / `AND` / `OR` / `XOR` / `CP` against `A,r`, `A,n`, `A,(HL)` | ~300 | Hand-written test ROM exercises arithmetic + flag updates |
| 1.6 | `feat/z80-phase-1-6-control-flow` | `JP` / `JR` / `CALL` / `RET` (with all condition codes), `RST`, `PUSH` / `POP` | ~250 | Test ROM uses subroutines and conditional branches |
| 1.7 | `feat/z80-phase-1-7-hello-world` | `examples/hello/hello.asm` — hand-written hello-world ROM that prints `"Hello, World!\r\n"` via the UART; `test_z80.cpp` boots it and asserts the UART output | ~100 (ROM) | First end-to-end demo running in CI |

**Phase 1 done when**: the hello-world ROM prints the expected string
when the plugin is loaded and stepped under a Google Test harness.

## 4. Phase 2 — Documented ISA complete + `zexdoc` (1–2 weeks)

| # | Theme |
|---|---|
| 2.1 | Bit operations (`CB` prefix): `BIT` / `SET` / `RES` / `RLC` / `RRC` / `RL` / `RR` / `SLA` / `SRA` / `SLL` / `SRL` |
| 2.2 | Index operations (`DD` / `FD` prefix): `IX` / `IY` register set, `(IX+d)` / `(IY+d)` addressing modes |
| 2.3 | Extended operations (`ED` prefix): block I/O (`LDIR` / `LDDR` / `CPIR` / `CPDR`), `IM 0`/`1`/`2`, `NEG`, `RETN` / `RETI` |
| 2.4 | Interrupt machinery: `INT` acceptance, `IM 1` (`RST 38h`), `IM 2` (vector table), `NMI` (`RST 66h`), `EI` / `DI`, `IFF1` / `IFF2` semantics |
| 2.5 | `Z80Disasm` implementation |
| 2.6 | **Minimal Settings — placeholder set** so the host's Settings dialog renders non-empty and z80 has family parity with `em8` / `z8000` / `mc68030` / `mc6809`. Scope: a few read-only "Plugin info" entries (CPU name, ABI version, RAM size) plus 1–2 truly settable items (e.g. "Reset vector", "Clear RAM on reset"). **Full** settings UI (RAM-size matrix, ROM image picker, disk image list, interrupt-mode default, …) remains Phase 6 — this sub-phase exists only to retire the empty-dialog UX gap noticed during the Phase 1.7 demo |
| 2.7 | `zexdoc` integration: CP/M-stub harness in `tests/test_z80.cpp` (BDOS funcs 2/9/0, warm-boot exit at `PC=0x0000`) verified by a tiny inline self-test; `TestZexdocIfPresent` loads `examples/zexdoc/zexdoc.com` if the user has dropped a copy in place, otherwise skips. The `.COM` binary is **not** redistributed by this repo — sourcing instructions live in `examples/zexdoc/README.md` |

**Phase 2 done when**: the harness self-test passes in CI AND the host's
Settings dialog for the z80 plugin renders with at least the placeholder
entries from 2.6. A clean local `zexdoc.com` run (no `ERROR` lines,
final `Tests complete` marker captured) is the gate for moving on to
Phase 3, but is not blocked on by CI since the binary is user-supplied.

## 5. Phase 3 — Undocumented opcodes + `zexall` (3–7 days)

| # | Theme |
|---|---|
| 3.1 | 8-bit operations on `IXH`, `IXL`, `IYH`, `IYL`: INC / DEC / `LD r,n` for the halves, `LD r, IXY{H,L}` and `LD IXY{H,L}, r` (with `(HL)` left untouched), and ALU `A, IXY{H,L}`. Also fix a latent `DD 76` / `FD 76` bug — those must execute as plain `HALT`, not `LD (HL), (IX+d)` |
| 3.2 | Undocumented flag propagation (`Y`, `X` bit propagation from `BIT b,(HL)` / `BIT b,(IX+d)` / `SCF` / `CCF` / 16-bit `ADD HL,rr`) **plus** the eight single-byte accumulator / flag ops that were missing entirely from Phase 1: `RLCA` / `RRCA` / `RLA` / `RRA` (0x07/0x0F/0x17/0x1F, preserve S/Z/PV), `DAA` (0x27), `CPL` (0x2F), `SCF` (0x37), `CCF` (0x3F). The bundle is justified by the fact that the new ops' flag behaviour *is* the Y/X-propagation story for this sub-phase |
| 3.3 | `ED`-prefix undocumented NOPs of various lengths. CPU already treated unassigned ED slots as 2-byte 8 T NOPs in Phase 2.3; this sub-phase verifies that behaviour with explicit tests and updates the disassembler to render them as `NOP*` instead of the misleading `DB $ED, $xx` |
| 3.4 | Add `examples/eforth/` with README + `.gitignore` documenting Bill Muench's public-domain eForth-Z80, and a `TestEForthIfPresent` harness that loads `examples/eforth/eforth.bin` if the user has dropped a copy in place. The harness boots cold, sends `"1 2 + . CR\r\n"` via the UART, and asserts the captured response contains `3`. Same SKIP-when-binary-absent pattern as Phase 2.7's zexdoc. The `.bin` itself is **not** redistributed in this repo |

**Phase 3 done when**: `zexall` passes every test; eForth REPL is
interactive over the plugin's UART.

## 6. Phase 4 — CP/M 2.2 boot via the companion repo (1–2 weeks)

| # | Theme |
|---|---|
| 4.1 | Disk-controller MMIO spec finalised in `docs/phase4_design.md` §1 (PR #69) and implemented as `Z80DiskDevice` (8 ports `0xF0..0xF7`, 24-bit LBA, 16-bit buffer pointer, READ/WRITE/NOP commands, sticky ERROR + WRITE_PROTECT + IMAGE_LOADED status, fixed 128 B CP/M-native sector size). Vendor-prefixed `emfe_z80_mount_disk_image` host hook exposes mount for tests; Settings wiring lands in Phase 4.4 |
| 4.2 | Custom CBIOS in `examples/cpm22/cbios/cbios.asm` (MIT, original work). 17-entry BIOS jump table mapping CONOUT/CONIN/CONST to UART (0xFE/0xFF), READ/WRITE to the sector-device MMIO (0xF0..0xF7), single-drive SSSD-256K geometry, DPH+DPB+DIRBUF+ALV+CSV laid out. Originates at 0xFA00. README documents the layout, license split, and Phase 4.3 build-pipeline hand-off |
| 4.3 | `examples/cpm22/build.ps1`: PowerShell driver that invokes `pasmo` on `cbios.asm`, locates the sibling `emfe_plugin_z80_cpm22/` clone, reads its `cpm22/binary/{ccp,bdos}.bin`, builds the SSSD-256K layout (boot stub at LBA 0, CCP+BDOS at LBA 1.., CBIOS at LBA 45+, rest filled with `0xE5`), and writes the result to `<companion>/disks/cpm22-sssd.img`. SKIPs cleanly when the companion isn't populated yet, mirroring the zexdoc / eForth pattern |
| 4.4 | Plugin gains the minimum disk MMIO needed by the CBIOS |
| 4.5 | Load the generated disk image into the plugin; confirm cold boot to `A>` prompt |

**Phase 4 done when**: CP/M 2.2 cold-boots to its `A>` prompt.

This is the first phase that crosses the repository boundary into
`emfe_plugin_z80_cpm22`. Files inside this repo (the CBIOS source, the
build recipe) stay MIT. Files written into the sibling clone (CP/M
binaries, assembled disk image) inherit the CP/M licence.

## 7. Phase 5 — CP/M smoke (3–5 days)

- `DIR`, `PIP`, `STAT`, `ED` and other standard CP/M tools run.
- Optional: Turbo Pascal 3.0 / Hi-Tech C compile a Hello World.

## 8. Phase 6 — Settings UI + debugger integration (~1 week)

- Settings dialog: RAM size, ROM image path, disk image paths,
  default interrupt mode.
- Debugger pane parity with `mc6809`: breakpoints, watchpoints,
  disassembly view, register / memory view.

## 9. Phase 7 (optional) — Cycle accuracy

- T-state counts match the Zilog reference manual for every documented
  opcode.
- Interrupt-acknowledge timing matches `IM 1` / `IM 2` reference cycle
  counts.

## 10. MMIO contract draft

The Z80 has a 256-byte separate I/O space accessed by `IN` / `OUT`. The
emfe Z80 plugin allocates the following ports (final values fixed
during Phase 1 in `docs/mmio.md`):

| Port | Direction | Purpose |
|---|---|---|
| `0xFE` | r/w | Console UART data (mirror `em8` / `z8000` convention) |
| `0xFF` | r | Console UART status (bit 0 = TX ready, bit 1 = RX available) |
| `0xF0–0xF7` | r/w | Disk MMIO (finalised at Phase 4) |
| `0xE0–0xE3` | r/w | Timer / CTC (finalised at Phase 6 if needed) |
| all others | — | reserved for future use |

Allocation is reviewed against `em8` and `z8000` so the family keeps a
consistent feel.

## 11. Test strategy

Google Test, mirroring `em8/tests/test_em8.cpp` and
`z8000/tests/test_z8000.cpp`. Tests accumulate per phase:

| Phase | Test class |
|---|---|
| 1–3 | Per-opcode unit tests: execute one instruction, assert post-state |
| 2 | `zexdoc` integration test: load ROM → run → verify expected "PASS" output |
| 3 | `zexall` integration test: same shape as `zexdoc` |
| 4 | CP/M boot integration test: sequence-driven, "boot to `A>`" expected |

Final test count should land in the same neighbourhood as `em8` /
`z8000` (1000+ tests).

## 12. Reference plugins to study

| Plugin | Why look at it |
|---|---|
| **`em8`** (986 + 1463 LoC) | Smallest C++ self-implementation. The cleanest reading for "how is a register-machine plugin scaffolded?" |
| **`z8000`** (786 + 1514 LoC) | Z80's Zilog sibling. Prefix-rich ISA dispatch pattern, disassembler layout, test harness shape, MMIO port allocation |
| **`mc6809`** (Rust) | Not directly applicable as C++ template, but `examples/` shows the Tiny Lisp / Tiny Forth in-repo pattern that Phase 3 (eForth) will mirror |

## 13. Risk areas

| Risk | Impact | Mitigation |
|---|---|---|
| Prefix maze (`CB` / `DD` / `ED` / `FD`) makes dispatch hard to read | Phase 1–3 source becomes opaque | Mirror `em8`'s opcode handler-table layout; split sub-tables per prefix |
| Undocumented flag rules (`Y` / `X` propagation) | Phase 3 `zexall` parity is tough | Land documented flag rules cleanly in Phase 2; treat Phase 3 as a focused "diff against `zexall` results" pass |
| `IM 2` vector-fetch timing | Interrupt edge cases | CP/M 2.2 only uses `IM 1`, so Phase 4 can ignore this. Refine in Phase 6 |
| Custom CBIOS fails to boot CP/M | Phase 4 stalls | Cross-check both against the upstream CP/M reference BIOS *and* against the chosen behaviour-reference (`z80ex` or similar) to isolate "ours vs CP/M's vs CBIOS's" failures |
| Companion repo (`emfe_plugin_z80_cpm22`) source-asset capture not done by Phase 4 | Phase 4 blocked on data | Start the companion's Phase 1 (source capture) in parallel during this plugin's Phase 2 |

## 14. Getting started

Order of work to begin the plugin today:

1. Branch `feat/z80-phase-1-1-scaffold` from `emfe_plugins` master.
2. Build the scaffold described in §2 above with the minimum content
   needed to compile and load: empty headers, empty source files,
   `plugin_z80.cpp` that returns `EMFE_OK` from `emfe_negotiate` and
   sensible defaults from the discovery functions.
3. PR the scaffold and let CI build the DLL. Merge once green.
4. Proceed with Phase 1.2 (memory + UART) on a new branch following
   the same pattern: small, reviewable, builds clean.

In parallel, advance `emfe_plugin_z80_cpm22` to Phase 1 (source-asset
capture from "The Unofficial CP/M Web Site") so that Phase 4 of this
plugin does not stall waiting for data.
