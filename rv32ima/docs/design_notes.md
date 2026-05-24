# emfe_plugin_rv32ima — Design Notes

Date: 2026-04-18 (revised 2026-05-22)
Status: **Design phase** — not yet implemented

## 1. Goal

Build an emfe plugin for **RV32IMA** as a **"modern RISC playground"** —
the modern-RISC counterpart to the family's classic CISC plugins
(`mc6809`, `z8000`, `mc68030`), positioned for ISA learning, embedded /
RTOS experimentation, and ISA-compliance checking rather than for
mainstream Linux distribution hosting.

Key consequence of this positioning: the plugin does **not** chase
mainstream RV Linux distros, because mainstream RV distros all target
RVA20 (RV64GC), which is a substantially larger plugin (5000–8000 LoC)
and slow without JIT (boot 10–30 min). RV32IMA's value is **smallness
and speed**, and the stress-target story is told via a **portfolio**
rather than a single Linux distro.

Reference implementation studied for behavioural cross-reference:
[cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima). Its
companion Buildroot configuration (`qemu_riscv32_nommu_virt_defconfig`)
provides one of the portfolio's stress targets (Linux nommu booting in
seconds), but is no longer the singular goal.

## 2. Background — What mini-rv32ima is

| Item | Detail |
|---|---|
| Author | Charles Lohr (2022–) |
| License | BSD / MIT / CC0 (triple-licensed) |
| Code size | Single header, ~400 lines + ~250-line demo wrapper |
| ISA | rv32ima + Zifencei + Zicsr + partial supervisor |
| Design | Header-only, stb-style (`#define MINIRV32_IMPLEMENTATION`) |
| Customization hooks | `MINIRV32_HANDLE_MEM_LOAD_CONTROL`, `MINIRV32_HANDLE_MEM_STORE_CONTROL`, `MINIRV32_POSTEXEC`, `MINIRV32_OTHERCSR_READ/WRITE` |
| Step API | `MiniRV32IMAStep(state, image, vProcAddr, elapsedUs, count)` |
| RAM | Flat, based at `0x80000000` |
| MMIO | `0x10000000–0x12000000`: 8250-compatible UART, CLINT, SYSCON |
| Binary size | ~18 KB (CLI demo build) |
| Performance | ~50 % of QEMU (~450 CoreMark) |

The header-only, stb-style design means the emulator is **reusable as a
library** despite being a complete system. This makes embedding it into an
emfe plugin straightforward.

## 3. Implementation approaches considered

### Option A — Vendor `mini-rv32ima.h` and `#include` it

| Aspect | Rating |
|---|---|
| Code size | 400–600 LoC wrapper + 400 LoC header |
| Time to Linux boot | 1–2 days |
| Consistency with em8 / z8000 plugins | low (those are self-implemented) |
| Debugger integration flexibility | low–medium (constrained by callbacks) |
| License | 3rd-party code retained; attribution required |
| Path to future JIT | mostly rewrite |

### Option B — Full self-implementation (like em8 / z8000) **(recommended)**

Implement the RV32IMA core from the RISC-V unprivileged + privileged
specifications, in the same single-translation-unit-per-instruction-group
style used by `em8`, `mc6809`, `z8000`, and `mc68030`. mini-rv32ima's
source is consulted as a behavioural cross-reference only; no code is
borrowed.

| Aspect | Rating |
|---|---|
| Code size | 1500–2000 LoC |
| Time to Linux boot | 5–8 days |
| Consistency with existing plugins | ◎ |
| Debugger flexibility | high |
| License | original throughout — no third-party attribution needed |
| Future JIT | extends naturally |
| Educational clarity | ◎ (single coherent reading of the source) |

### Option C — Fork / derive from mini-rv32ima

Use mini-rv32ima's implementation as starter code and refactor it into the
emfe plugin style: split files, install a handler table for instruction
dispatch, add debug hooks. Since the source is tri-licensed (MIT / BSD /
CC0), forking and modifying is unrestricted (attribution aside).

| Aspect | Rating |
|---|---|
| Code size | 900–1200 LoC after refactor |
| Time to Linux boot | 2–3 days |
| Consistency | ○ (em8 / z8000-style structure; only starter code is borrowed) |
| Debugger flexibility | ○ |
| License | original for our changes + credit to upstream author |
| Future JIT | feasible |

## 4. RISC-V profile landscape

Application processor profiles (what mainstream Linux distributions target):

| Profile | Base ISA | Mandatory extensions |
|---|---|---|
| RVA20U64 | RV64I | M, A, F, D, C, Zicsr, Zicntr (= RV64GC) |
| RVA22U64 | RV64I | RVA20 + B, Zihpm, fence.tso |
| RVA23U64 | RV64I | RVA22 + V (vector), Zicond, Zcb, Zfa ... |

RV32IMA (mini-rv32ima's target) fits **none** of the RVA profiles, nor is it
covered by the embedded (RVM*) profile family. It exists in a custom niche
served by Buildroot's no-MMU Linux build.

## 5. Distribution support for RISC-V (as of 2026-04)

### Linux

| Distribution | Arch | Profile |
|---|---|---|
| Debian trixie (13) | riscv64 | RVA20 (RV64GC) |
| Ubuntu 26.04 LTS | riscv64 | RVA23 (first large-scale LTS adopter) |
| Fedora | riscv64 | RVA20 / partial RVA22 |
| AlmaLinux Kitten 10 (2026-03) | riscv64 | RVA20 |
| openSUSE Tumbleweed / Leap | riscv64 | RVA20 |
| Arch Linux RISC-V (unofficial) | riscv64 | RVA20 |
| Gentoo | riscv64, **riscv32** | configurable |
| Void / Alpine / NixOS / Slackware | riscv64 | RVA20 |
| OpenWrt | riscv64 | embedded |
| **Buildroot** | **any** | **any** (incl. rv32ima nommu) |

### BSD

| Distribution | Arch | Notes |
|---|---|---|
| FreeBSD 13.x+ | riscv64 (RV64GC) | SiFive Unmatched, QEMU |
| NetBSD 11.0 (2026-04) | riscv64 (RV64GC) | First stable, JH7110 / QEMU |
| OpenBSD 7.8 | riscv64 (RV64GC) | Introduced in 7.1 |

None of the BSDs support RV32. The mainstream Linux distros are likewise
riscv64 only; RV32 exists in Gentoo and Buildroot in practice.

### Implication

- Mainstream distros do not run on RV32IMA, period — even Alpine targets
  RVA20 (RV64GC), so even with Sv32 MMU added, mainstream userspace
  binaries (which use F/D/C instructions) would not execute.
- This was the original motivation for treating "boot a mainstream
  distro" as the goal, but it does not survive contact with the RV
  ecosystem reality.
- **Pivot adopted on 2026-05-22**: the plugin's value proposition is
  *not* "boot a Linux distro" — it is **"modern RISC playground"** —
  fast, small, readable, and validated against a curated portfolio of
  stress targets (see §7). Linux nommu (via mini-rv32ima Buildroot)
  remains in the portfolio as one item, not the only item.
- To run Debian / Ubuntu / NetBSD / OpenBSD / FreeBSD, a **separate
  plugin** (RV64GC + Sv39 + PLIC + JIT) would be required — explicitly
  deferred, possibly indefinitely. The family already has an OS-booting
  plugin in `mc68030` (NetBSD/m68k, Linux 6.12); a second "Linux distro"
  plugin via RISC-V is not the current priority.

## 6. Performance projections

Derived from em68030's measured throughput (Ryzen-class desktop, pure
interpreter: 44 MIPS ≈ MC68030 @ 270 MHz):

| Emulator | ISA complexity | Interpreter (projected) | Linux boot feel |
|---|---|---|---|
| MC68030 (measured) | medium | 44 MIPS | NetBSD in tens of seconds |
| **RV32IMA (mini-rv32ima)** | **low** | **100–200 MIPS** | **Buildroot nommu in seconds** |
| RV64IMAC (MMU, no F/D) | medium | 60–120 MIPS | Alpine-class in ~1 min |
| **RV64GC** | **high** | **30–60 MIPS** | **Debian / Ubuntu in 10–30 min** |
| RV64GC + V (RVA23) | max | 20–40 MIPS | impractical without JIT |

### Why RV64GC is heavy

1. Soft-float emulation of F/D double every FP instruction.
2. MMU Sv39 page-table walks + TLB machinery.
3. M/S/U three-mode with delegation + trap hooks.
4. 16/32-bit compressed instruction decoding branches.
5. A mainstream distro takes ~10⁹ guest instructions to reach userspace,
   ~10–100× more than a nommu Buildroot image.

mini-rv32ima sidesteps most of these while still booting Linux — a sweet
spot in the design space.

## 7. Direction (agreed)

### Positioning — "modern RISC playground"

The plugin is the family's **modern-RISC representative** rather than a
"boot Linux on RV" vehicle. It earns its place in the lineup through:

- **Smallness and speed** — 1500–2000 LoC interpreter, 100–200 MIPS
  projected; everything boots in seconds.
- **Educational clarity** — RV32IMA is the textbook ISA used in the
  current edition of Patterson & Hennessy and most contemporary CS
  curricula. Source is meant to be read alongside the spec.
- **Portfolio of stress targets** (see Phase 7) — Linux nommu, FreeRTOS,
  riscv-tests, CoreMark, bare-metal C examples — covering several
  different teaching and demo angles, not a single "did Linux boot"
  question.
- **mc68030 carries the OS-booting story** — the family already has
  NetBSD/m68k and Linux 6.12 booting via that plugin. The RV plugin
  does not need to duplicate that effort.

### This cycle

**Implement emfe_plugin_rv32ima using Option B (full self-implementation).**

mini-rv32ima's source serves only as a behavioural cross-reference; no
code is borrowed. The plugin is written from the RISC-V unprivileged +
privileged specifications, in the same style as every other plugin in
this family.

Layout:
- `D:\projects\emfe_plugins\rv32ima\`
- DLL: `emfe_plugin_rv32ima.dll`
- Same naming / deployment rules as other plugins
- Default RAM: 64 MB (matches mini-rv32ima's default for ease of
  cross-comparison while validating)

### Future extensions (deferred — possibly indefinitely)

The pre-pivot plan listed `rv64ima → RV64GC → JIT` as a priority queue
toward mainstream Linux distros. With the playground positioning, that
ladder is no longer on the active roadmap:

1. **rv64ima + Sv39** (no F/D/C) — would unlock custom Alpine-musl
   rebuilds, but the procurement friction remains and the extra
   1000–1500 LoC buys little marquee value. **Deferred.**
2. **RV64GC** — full mainstream RV Linux. 5000–8000 LoC + soft-float +
   Sv39 + compressed decode + M/S/U delegation. Boot 10–30 min without
   JIT. **Treated as a separate future plugin, not an extension of this
   one.** When (if) tackled, it gets its own design notes.
3. **JIT** — applies to either this plugin or a future RV64GC plugin.
   Lessons from `mc68030`'s JIT (deferred snapshot, block cache,
   bailout blacklist, inline dispatch) carry over. Not on the Phase
   1–7 roadmap for `rv32ima`.

## 8. Phase plan

The interpreter implementation (Phases 1–6) is unchanged by the pivot to
playground positioning — the core ISA and supervisor machinery is what
it is. **Phase 7** is new and exists to make the playground positioning
concrete via a curated stress-target portfolio.

### Phases 1–6 (interpreter implementation, unchanged)

| Phase | Scope | Est. LoC |
|---|---|---|
| 1 | RV32I base (arith / logic / load / store / branch / JAL[R]) | 400 |
| 2 | M extension (MUL/DIV) + Zifencei + Zicsr | 200 |
| 3 | A extension (AMO) | 150 |
| 4 | Privileged modes + CSRs + trap/interrupt | 500 |
| 5 | CLINT + UART (8250) + SYSCON + boot stub | 400 |
| 6 | Disassembler + emfe plugin ABI + `riscv-tests` passing | 400 |

`riscv-tests` (BSD-licensed official RISC-V ISA compliance suite) is the
Phase 6 acceptance gate. Once it passes, the plugin is provably
spec-correct for RV32IMA + Zifencei + Zicsr + machine-mode supervisor.

### Phase 7 — stress-target portfolio (new)

| Category | Target | License | Source / status |
|---|---|---|---|
| Linux nommu | mini-rv32ima Buildroot Linux image | GPL-2.0 (Linux) + Buildroot composition licenses | Pre-built in companion repo (see §10), one-shot reproduction recipe alongside |
| Bare-metal RTOS | **FreeRTOS** RISC-V port | MIT | Demo bundle in companion repo |
| Bare-metal RTOS | **Zephyr** (RISC-V support) | Apache-2.0 | Optional — added if FreeRTOS demo lands cleanly |
| Compliance | **riscv-tests** | BSD | Re-used from Phase 6 acceptance gate; also shipped to end users as a "you can run the official compliance suite" demo |
| Benchmark | **CoreMark** / **Dhrystone** | BSD with attribution | For "modern RISC playground" performance demonstration |
| Bare-metal C | **picolibc**-based examples | BSD | Original example programs showcasing the ISA at minimal scaffolding |

Phase 7 does not add interpreter LoC; it adds curated demo content and
recipes. The actual binaries / images that ship are stored in a
**companion repository** (see §10) to keep this plugin's MIT/Apache
tree clean of upstream-licensed payloads — same pattern as the Z80
plugin's `emfe_plugins_cpm_supplements` companion.

## 9. Open questions

- [ ] Default search path / setting for the user-supplied Linux image (kernel
      + DTB)?
- [ ] Embed the device tree blob in the plugin, or leave it as an external
      file the user must provide?
- [ ] Block device (virtio-blk-equivalent) for rootfs: Phase 5 or deferred to
      Phase 7 (as part of the Linux nommu demo)?
- [ ] ELF loader support (in addition to raw-binary load), to match the
      convention used by `mc68030`?
- [ ] Which existing implementations to consult as behavioural cross-
      references during Phase 1? `mini-rv32ima` is the most obvious; QEMU
      and Spike are options for trickier corners (CSR semantics, trap
      delivery).
- [ ] Companion-repository naming for the Phase 7 portfolio. Working name:
      `emfe_plugin_rv32ima_demos`. Should each stress-target category be a
      sub-directory under one repo, or split across multiple companions?
      First answer: single repo, sub-directories — keeps the Phase 7
      portfolio discoverable in one place; precedent is
      `emfe_plugins_cpm_supplements` doing the analogous thing for one license
      zone.

## 10. Companion repository

Phase 7 stress targets are delivered through a companion repository (to
keep this plugin's MIT/Apache source tree clean of upstream-licensed
payloads). Working name: **`emfe_plugin_rv32ima_demos`** (not yet
created). Anticipated layout:

```
emfe_plugin_rv32ima_demos/
├── README.md                — purpose and license posture
├── linux-nommu/             — mini-rv32ima Buildroot image + recipe
│                              (Linux GPL-2.0 + per-package licenses)
├── freertos/                — FreeRTOS RISC-V port demo (MIT)
├── riscv-tests/             — official ISA compliance suite (BSD)
├── coremark/                — CoreMark benchmark (BSD, attribution)
└── baremetal-c/             — picolibc-based examples (BSD + original)
```

The companion-repo pattern is the same one used by the Z80 plugin's
`emfe_plugins_cpm_supplements` (CP/M-only payloads under a separate license).
Here the motivation is mixed-license payloads from a variety of
upstream projects rather than a single restrictive license, but the
solution structure is identical.

The companion repo will be created when Phase 6 is in sight, not
sooner — there is no point assembling stress targets for an
interpreter that does not yet exist.

## 11. References

- [cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)
- [cnlohr/buildroot_for_mini_rv32ima](https://github.com/cnlohr/buildroot_for_mini_rv32ima)
- [RISC-V Unprivileged ISA](https://github.com/riscv/riscv-isa-manual)
- [RVA23 Profile Specification](https://docs.riscv.org/reference/profiles/rva23/_attachments/rva23-profile.pdf)
- [RISC-V Profiles repo](https://github.com/riscv/riscv-profiles)
- [Debian RISC-V Wiki](https://wiki.debian.org/RISC-V)
- [NetBSD 11.0 release](https://www.netbsd.org/releases/formal-11/NetBSD-11.0.html)
- [OpenBSD/riscv64](https://www.openbsd.org/riscv64.html)
- [AlmaLinux Kitten 10 riscv64](https://almalinux.org/blog/2026-03-17-almalinux-goes-riscv/)
