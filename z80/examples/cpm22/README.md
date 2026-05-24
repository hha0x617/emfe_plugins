# CP/M 2.2 on emfe_plugin_z80

This directory holds the **original work** for booting CP/M 2.2 on the
plugin — primarily the custom **CBIOS** (Z80 assembly that maps the
17 CP/M BIOS jump-table entries to the plugin's UART + sector MMIO)
plus the build recipe that combines the CBIOS with upstream CCP /
BDOS into a bootable disk image.

The CP/M-derived material itself (CCP source, BDOS source, reference
binaries, finished disk images) lives in the companion repository
[`emfe_plugin_z80_cpm22`](https://github.com/hha0x617/emfe_plugin_z80_cpm22)
under the Bryan Sparks 2022 license letter. The split keeps each
repository under a single license:

| Repo | License | Contents |
|---|---|---|
| `emfe_plugins/z80/examples/cpm22/` (here) | MIT / Apache-2.0 | CBIOS, build script, design notes |
| `emfe_plugin_z80_cpm22/cpm22/` | CP/M (2022 Sparks letter) | CCP source / binary, BDOS source / binary |
| `emfe_plugin_z80_cpm22/disks/` | CP/M | Finished bootable images (output of the build) |

See [`../../docs/phase4_design.md`](../../docs/phase4_design.md) for
the full design (MMIO contract, memory layout, build pipeline,
acceptance criterion).

## Phase 4 plan (sub-phases)

| # | Status | Theme |
|---|---|---|
| 4.1 | ✅ merged (#70) | Sector-device disk MMIO (CPU side) |
| 4.2 | ✅ this PR | CBIOS skeleton (`cbios/cbios.asm`) |
| 4.3 | planned | Build pipeline — assemble CBIOS + combine with CCP / BDOS → bootable disk image |
| 4.4 | planned | Settings keys to mount a disk image (host-side wiring) |
| 4.5 | planned | Cold boot to `A>` (acceptance test) |

## Contents

### `cbios/cbios.asm`

Original-work Z80 assembly mapping the CP/M 2.2 BIOS jump table:

| # | Entry | Mapped to |
|---|---|---|
| 0 | BOOT | Sign-on + install reset vectors + select drive A + enter CCP |
| 1 | WBOOT | Reload CCP+BDOS from system tracks via disk MMIO |
| 2 | CONST | `IN A,(0xFF)` → RX_AVAIL bit |
| 3 | CONIN | poll RX_AVAIL → `IN A,(0xFE)` |
| 4 | CONOUT | `OUT (0xFE),A` |
| 5 | LIST | stub (silent drop) |
| 6 | PUNCH | stub (silent drop) |
| 7 | READER | stub (returns `^Z` = EOF) |
| 8 | HOME | zero CBIOS track variable |
| 9 | SELDSK | drive A only — returns DPH pointer |
| 10–12 | SETTRK / SETSEC / SETDMA | store value in CBIOS variable |
| 13 | READ | LBA = TRK × 26 + SEC, fire disk-MMIO READ |
| 14 | WRITE | same as READ but WRITE |
| 15 | LISTST | always ready |
| 16 | SECTRAN | identity (no skew in Phase 4) |

Single drive (A), SSSD-256K geometry (77 tracks × 26 sectors × 128 B),
DPH/DPB/DIRBUF/ALV/CSV laid out for that geometry.

**Origin address: `0xFA00`** — see the design doc's memory map.

### `cbios/` (assembled output, Phase 4.3)

Phase 4.3 will land here:
- `Makefile` (or `build.ps1`) calling `pasmo` on `cbios.asm`.
- Glue to combine the assembled CBIOS with CCP + BDOS bytes pulled
  from the companion repo, padded to a flat SSSD-256K image.
- Output written into the companion repo's `disks/` (CP/M licensed).

## Assembler

**Target: `pasmo`** (per the design doc's lean in §7). Single-binary,
Windows-friendly, takes `.asm` source and outputs flat `.bin`. The
`cbios.asm` source uses only standard mnemonics + `ORG` / `EQU` / `DB`
/ `DW` / `DS` pseudo-ops so it should also assemble cleanly under
`sjasmplus`, `z80asm`, or `tniASM` if a different toolchain is
preferred.
