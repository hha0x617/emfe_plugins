# emfe_plugin_z80 — Design Notes

Date: 2026-05-21
Status: **Design phase** — not yet implemented

## 1. Goal

Add a Zilog Z80 8-bit plugin to the emfe plugin family, positioned as the
Zilog counterpart to the existing `mc6809` (Motorola 8-bit) plugin and the
Zilog 8-bit complement to the existing `z8000` (Zilog 16-bit) plugin.

The plugin should serve two audiences:

1. **Retro emulation users** — load classic CP/M 2.2 disks, ROMs, or
   homebrew Z80 software and run them in the emfe host.
2. **Educators / curious developers** — single-file, well-commented Z80
   implementation alongside `mc6809` and `z8000` for ISA comparison.

## 2. Background — fit with the existing lineup

| Plugin | Vendor | Bits | Era | Status |
|---|---|---|---|---|
| `em8` | toy (~6502 subset) | 8 | educational | shipped |
| `mc6809` | Motorola | 8 | 1979 | shipped |
| **`z80`** | **Zilog** | **8** | **1976** | **this design** |
| `z8000` | Zilog | 16 | 1979 | shipped |
| `mc68030` | Motorola | 32 | 1987 | shipped |
| `rv32ima` (queued) | RISC-V | 32 | 2010s | docs only |

Z80 fills the most obvious gap: Zilog 8-bit, the most culturally
significant 8-bit microprocessor of its era. It also gives users a direct
Motorola-vs-Zilog comparison at the 8-bit tier (`mc6809` vs `z80`), and
a same-vendor 8/16-bit comparison (`z80` vs `z8000`).

## 3. Implementation approach

Three options were considered. The recommendation is **B (full
self-implementation)** matching the path taken by every other plugin in
this family (`em8`, `mc6809`, `z8000`, `mc68030`).

### Option A — Vendor an existing emulator (`z80ex`, `libz80`)

| Aspect | Rating |
|---|---|
| Code size | ~3000 LoC vendored + thin emfe wrapper |
| Time to first boot | 1–2 days |
| Consistency with `mc6809` / `z8000` (self-implemented) | low |
| Debugger / breakpoint hook flexibility | medium |
| License | upstream + attribution |
| Future JIT | mostly rewrite |

### Option B — Full self-implementation (like `mc6809` / `z8000`) **(recommended)**

Implement the Z80 core from the Zilog reference manual, in the same
single-translation-unit-per-instruction-group style used by `mc6809` and
`z8000`. No third-party emulator source is read or borrowed; existing
emulators may be consulted for behavioural cross-reference only.

| Aspect | Rating |
|---|---|
| Code size | 2500–3500 LoC (Z80 has more opcodes than 6809) |
| Time to first boot | 7–12 days |
| Consistency with existing plugins | ◎ |
| Debugger flexibility | high |
| License | original throughout — no third-party attribution needed |
| Future JIT | extends naturally |
| Educational clarity | ◎ (single coherent reading of the source) |

### Option C — Fork / derive from a known-good Z80 core

Pick a permissively-licensed reference (e.g. **z80ex**, BSD-3-Clause, or
**libz80**, BSD-style) as starter code, then refactor it into the emfe
plugin style.

| Aspect | Rating |
|---|---|
| Code size | 1800–2500 LoC after refactor |
| Time to first boot | 3–5 days |
| Consistency | ○ (matches existing structure but borrowed logic underneath) |
| Debugger flexibility | ○ |
| License | original on our changes + upstream attribution |
| Future JIT | feasible |

## 4. ISA scope

The Zilog Z80 specification has well-defined boundaries, but real-world
software hits the corners. The plugin will implement progressively:

### Phase 1 — Base ISA (Zilog official)

- All documented 8-bit / 16-bit data movement and arithmetic.
- All documented bit operations (`CB` prefix).
- All documented index-register operations (`DD` / `FD` prefix, `IX` / `IY`).
- All documented extended operations (`ED` prefix, including block I/O).
- Conditional jumps and calls with all flag combinations.
- Interrupt modes 0, 1, 2.
- Refresh register `R` increment (cosmetically correct, not cycle-exact).

Enough to boot CP/M 2.2, run Tiny Pascal / Hi-Tech C, and pass
`zexdoc` (documented-opcode flag test).

### Phase 2 — Undocumented opcodes (`zexall` parity)

- 8-bit operations on `IXH`, `IXL`, `IYH`, `IYL`.
- Undocumented flag bits (`Y`, `X` propagation from `BIT`, `SCF`, `CCF`).
- Undocumented `ED`-prefix opcodes (NOPs of various lengths).

Enough to pass `zexall`. Real-world games and demoscene software depend
on these; CP/M 2.2 itself does not.

### Phase 3 — Cycle accuracy

- T-state counting matching the Zilog reference cycle counts for every
  documented opcode.
- Interrupt acknowledge timing for IM 1 / IM 2.

Phase 3 is decoupled from Phases 1–2; functional correctness comes first.

### Out of scope (initial)

- **R800** (MSX turbo-Z80 extension).
- **Z180 / eZ80** (later Zilog derivatives).
- **Refresh-cycle-driven DRAM behaviours** — modelled at the cycle level
  only, not gated to wall-clock memory timings.

## 5. Memory and I/O model

| Surface | Details |
|---|---|
| Address space | 64 KiB flat code+data (no MMU, by design) |
| I/O space | 256-byte separate I/O space, accessed via `IN`/`OUT` |
| MMIO surface | Mapped into the I/O space at well-known ports |
| Interrupt | Single `INT` line + `NMI` line, software-driven for now |

The MMIO contract with the emfe host plugin API will be drafted in
`mmio.md` during Phase 1 and will mirror the `mc6809` / `z8000` ports:

| Port (TBD) | Direction | Purpose |
|---|---|---|
| `0xFE` (placeholder) | r/w | Console UART data |
| `0xFF` (placeholder) | r | Console UART status |
| (further allocations during Phase 2: disk controller, RTC, etc.) |

## 6. Stress-test target landscape

Following the precedent set by `mc6809` (Tiny Lisp / Tiny Forth) and
`rv32ima` (mini-rv32ima Linux), each plugin ships with an in-repo
stress-test corpus. For Z80 the corpus is split into two licence zones
(see Section 7).

### In-repository (this `z80/` tree, MIT / Apache-2.0)

| Item | Origin | Licence | Purpose |
|---|---|---|---|
| `examples/zexall/` and `examples/zexdoc/` | Frank Cringle (1994) | Public domain | Z80 ISA accuracy test, the de-facto reference |
| `examples/eforth/` | Bill Muench eForth-Z80 | Public domain | Z80 Forth implementation, OS-free, good real-world workout |
| `examples/monitor/` | original | MIT (this repo) | Custom ROM monitor for the emfe Z80 plugin: LD/DUMP/GO/DISASM. Educational, mirrors what `mc6809/examples/` ships |
| `examples/tinybasic/` | original (long-term) | MIT (this repo) | Z80 port of Tiny BASIC, again following the `mc6809` examples-tier pattern |

### Companion repository — [`emfe_plugins_cpm_supplements`](#7-licence-strategy)

| Item | Origin | Licence |
|---|---|---|
| CP/M 2.2 source | "The Unofficial CP/M Web Site" | Bryan Sparks 2022 letter |
| CP/M 2.2 binaries | same | Bryan Sparks 2022 letter |
| Boot disk images | built from above + custom CBIOS | Bryan Sparks 2022 letter (whole-image redistribution) |
| Build recipe | **lives here**, in `examples/cpm22/` | MIT (original work) |
| Custom CBIOS | **lives here**, in `examples/cpm22/cbios/` | MIT (original work) |

The build recipe and CBIOS source — though they consume CP/M source —
are themselves original work, so they live in this MIT-licensed
repository. The recipe reads inputs from a sibling `emfe_plugins_cpm_supplements`
clone (or via an environment variable) and writes the resulting
disk images back into that clone.

## 7. Licence strategy

This is the most consequential design decision and deserves an explicit
record.

The 2022 Bryan Sparks letter grants permission to *copy, modify,
distribute, and use* CP/M source and binaries, including commercially. It
does **not** grant a sublicensing right. CP/M-derived files therefore
cannot be relicensed under MIT or Apache-2.0; they must travel with the
Sparks notice.

Two options were considered for bundling CP/M material with the Z80
plugin:

1. **In-tree mixed licensing** — keep CP/M files inside `z80/examples/`
   with per-file SPDX headers and a sub-LICENSE file. Rejected: the
   top-level emfe_plugins LICENSE files would not actually cover those
   files, and a casual reader would be misled.

2. **Companion repository** (chosen, "B plan") — original work stays in
   this MIT/Apache repo, CP/M-derived material lives in a dedicated
   sibling repository (`emfe_plugins_cpm_supplements`) carrying only the
   Sparks-letter licence.

Result:

- This repository's licence story stays exactly as it is today: dual
  MIT / Apache-2.0 across every file under `z80/`.
- The companion repository's licence story is a single file: the
  Sparks letter.
- The two repositories never share a licence surface, so no per-file
  SPDX policing is required to keep them straight.

Precedent for the split: the Linux kernel's `linux-firmware` companion
repository handles proprietary firmware blobs the same way.

## 8. Performance projection

Following the same back-of-envelope used for `rv32ima` (44 MIPS measured
on `mc68030` interpreter on a Ryzen-class desktop):

| Plugin | ISA complexity | Interpreter (projected) | Boot feel |
|---|---|---|---|
| `mc68030` (measured) | medium | 44 MIPS / ~270 MHz 030 | NetBSD in tens of seconds |
| `mc6809` (shipped) | low | not measured | Tiny Lisp REPL realtime |
| **`z80` (projected)** | **low (but prefix-rich)** | **80–150 MIPS / ~16 MHz × N effective Z80** | **CP/M cold boot < 1 s** |

The "prefix-rich" complexity means a non-trivial fraction of opcodes
require 2-byte decode (`CB` / `DD` / `ED` / `FD`), which costs some
dispatch overhead vs. the cleaner `mc6809` decode tree. Still well
within real-time on modern hardware; the target stress is CP/M-class
software which runs effortlessly even at ~16 MHz emulated.

A JIT path (analogous to `mc68030`'s) is feasible but not on the Phase-1
to Phase-3 roadmap.

## 9. Phase plan

| Phase | Deliverable | Stress target |
|---|---|---|
| **1** | Plugin scaffolding, base ISA, console MMIO, ELF/HEX loader, run a simple Hello-World ROM | manual test program |
| **2** | Base ISA passing `zexdoc`; ROM monitor running | `zexdoc`, ROM monitor interactive use |
| **3** | Undocumented opcodes passing `zexall`; eForth running | `zexall`, eForth REPL |
| **4** | CBIOS for emfe Z80 MMIO; CP/M 2.2 boot via companion-repo materials | CP/M 2.2 cold boot to `A>` prompt |
| **5** | CP/M-level smoke: PIP / STAT / DIR; optional Turbo Pascal 3.0 | language toolchain inside CP/M |
| **6** | Settings UI, debugger integration (breakpoints, watchpoints, disasm pane) — feature-parity with `mc6809` | n/a |
| **7** *(optional)* | Cycle-accurate T-state mode | retro game ROMs with cycle-sensitive code |

Phases 1–3 are self-contained in this repository.
Phase 4 begins the cross-repo coupling with `emfe_plugins_cpm_supplements`.

## 10. Open questions

1. **Which existing Z80 emulators are best to consult as behavioural
   cross-references?** No code is borrowed (Option B / full
   self-implementation), but `z80ex` (BSD-3-Clause, passes `zexall`) and
   `MAME`'s Z80 core are well-studied behavioural references useful for
   sanity-checking edge cases (flag propagation, undocumented opcodes,
   interrupt-acknowledge timing). Decide which to consult before
   Phase 2.

2. **Big-endian / little-endian register pair access** — Z80 is
   little-endian for memory but register pairs (`BC`, `DE`, `HL`) have a
   specific layout; the emfe register-defs ABI used by other plugins
   needs to expose both byte and pair-as-word views cleanly. Look at
   how `mc6809` exposes `D` (= `A:B`) for a template.

3. **Disk-controller MMIO shape** — `mc6809` does not have a disk
   target; `mc68030` has WD33C93. For Z80 the disk model is much
   simpler (CP/M expects sector-level r/w via BIOS calls). The cleanest
   path is a minimal "sector device" MMIO that the CBIOS drives, rather
   than emulating a specific historic controller (WD1793, NEC 765, …).
   Decide at Phase 4 start when the CBIOS is being written.

4. **Whether to ship a settings UI for "RAM size" / "ROM at 0x0000"** —
   Z80 systems vary wildly (TPA size, ROM-monitor mapping). A small
   matrix of presets (CP/M 64 KiB, ROM-based 32 KiB, …) is probably
   sufficient; full per-byte ROM/RAM mapping is overkill for an emfe
   stress-test target.

## 11. References

- Zilog Z80 CPU User Manual (UM008011-0816) — primary ISA reference.
- Frank Cringle, `zexall` and `zexdoc` source — public domain accuracy
  suite, the de-facto Z80 emulator test corpus.
- [The Unofficial CP/M Web Site](http://www.cpm.z80.de/) — canonical
  upstream archive for CP/M 2.2 source and binaries.
- Bryan Sparks (DRDOS, Inc.) 2022 licence letter — verbatim in the
  companion repository's `LICENSE-CPM22.txt`.
- This repository's `mc6809/docs/` — pattern reference for in-repo
  examples (Tiny Lisp / Tiny Forth) and design-notes style.
- This repository's `rv32ima/docs/design_notes.md` — pattern reference
  for the design-notes structure used here.
