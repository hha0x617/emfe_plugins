# emfe_plugin_rv32ima — Design Notes

Date: 2026-04-18
Status: **Design phase** — not yet implemented

## 1. Goal

Build an emfe plugin that hosts an MMU-less RISC-V subset (rv32ima) Linux
system on top of the existing emfe plugin architecture.

Reference: [cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima).
The target is to boot a Linux kernel + DTB + rootfs built with the companion
Buildroot configuration (`qemu_riscv32_nommu_virt_defconfig`) directly inside
the plugin.

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

### Option B — Full self-implementation (like em8 / z8000)

| Aspect | Rating |
|---|---|
| Code size | 1500–2000 LoC |
| Time to Linux boot | 5–8 days |
| Consistency | ◎ |
| Debugger flexibility | high |
| License | original |
| Future JIT | extends naturally |

### Option C — Fork / derive from mini-rv32ima **(recommended)**

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

- OSes bootable on our mini-rv32ima plugin: **Linux built via the
  mini-rv32ima-specific Buildroot configuration only.**
- To run Debian / Ubuntu / NetBSD, a **separate plugin** (RV64GC + MMU +
  PLIC) would be required — not in scope here.

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

### This cycle

**Implement emfe_plugin_rv32ima using Option C (fork + modify).**

Layout:
- `D:\projects\emfe_plugins\rv32ima\`
- DLL: `emfe_plugin_rv32ima.dll`
- Same naming / deployment rules as other plugins
- Default RAM: 64 MB (mini-rv32ima's default)

### Future extensions (priority order)

1. **rv64ima** (with MMU) — 32 → 64-bit extension without F/D/C. Brings Alpine
   and Void within reach while reusing Phase 1 foundations.
2. **RV64GC** — add F/D + compressed. JIT required to be usable.
3. **JIT** — reapply em68030 lessons (deferred snapshot, block cache,
   bailout blacklist, inline dispatch).

## 8. Phase 1 (rv32ima) work breakdown

| Phase | Scope | Est. LoC |
|---|---|---|
| 1a | RV32I base (arith / logic / load / store / branch / JAL[R]) | 400 |
| 1b | M extension (MUL/DIV) + Zifencei + Zicsr | 200 |
| 1c | A extension (AMO) | 150 |
| 1d | Privileged modes + CSRs + trap/interrupt | 500 |
| 1e | CLINT + UART (8250) + SYSCON + boot stub | 400 |
| 1f | Disassembler + plugin ABI + tests | 400 |

Linux boot becomes feasible once 1a–1d are functional (minimal machine-mode
trap path). 1e / 1f complete the emfe integration and user-facing story.

## 9. Open questions

- [ ] Where to place mini-rv32ima starter code: `rv32ima/third_party/` vs.
      pasted directly into `rv32ima/src/`?
- [ ] Default search path / setting for the user-supplied Linux image (kernel
      + DTB)?
- [ ] Embed the device tree blob in the plugin, or leave it as an external
      file the user must provide?
- [ ] Block device (virtio-blk-equivalent) for rootfs: Phase 1 or deferred to
      Phase 2?
- [ ] ELF loading on top of mini-rv32ima's raw-binary expectation?

## 10. References

- [cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)
- [cnlohr/buildroot_for_mini_rv32ima](https://github.com/cnlohr/buildroot_for_mini_rv32ima)
- [RISC-V Unprivileged ISA](https://github.com/riscv/riscv-isa-manual)
- [RVA23 Profile Specification](https://docs.riscv.org/reference/profiles/rva23/_attachments/rva23-profile.pdf)
- [RISC-V Profiles repo](https://github.com/riscv/riscv-profiles)
- [Debian RISC-V Wiki](https://wiki.debian.org/RISC-V)
- [NetBSD 11.0 release](https://www.netbsd.org/releases/formal-11/NetBSD-11.0.html)
- [OpenBSD/riscv64](https://www.openbsd.org/riscv64.html)
- [AlmaLinux Kitten 10 riscv64](https://almalinux.org/blog/2026-03-17-almalinux-goes-riscv/)
