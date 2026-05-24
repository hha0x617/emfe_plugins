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
| 4.2 | ✅ merged (#71) | CBIOS skeleton (`cbios/cbios.asm`) |
| 4.3 | ✅ this PR | Build pipeline — `build.ps1` assembles CBIOS + combines with CCP / BDOS → bootable disk image |
| 4.4 | planned | Settings keys to mount a disk image (host-side wiring) |
| 4.5 | planned | Cold boot to `A>` (acceptance test) |

## Building a disk image

```powershell
# Default: companion repo is a sibling directory next to emfe_plugins
pwsh ./build.ps1

# Explicit companion location
pwsh ./build.ps1 -CompanionPath D:\path\to\emfe_plugin_z80_cpm22

# Explicit pasmo (otherwise looked up in PATH)
pwsh ./build.ps1 -PasmoExe C:\tools\pasmo.exe

# Just assemble cbios.asm — skip the disk-image combine step
pwsh ./build.ps1 -SkipImage
```

The script:

1. Looks for `pasmo` on PATH (single-binary Z80 assembler — install
   from <https://pasmo.speccy.org/>). Other assemblers work too
   (`sjasmplus`, `z80asm`, `tniASM`) — the source uses only standard
   mnemonics + `ORG`/`EQU`/`DB`/`DW`/`DS`.
2. Assembles `cbios/cbios.asm` → `cbios/cbios.bin` (flat binary,
   `pasmo --bin`).
3. Resolves the companion repo (`emfe_plugin_z80_cpm22`) as a sibling
   directory next to `emfe_plugins`. If absent or not populated with
   CCP / BDOS binaries, prints a SKIP message and exits 0 — same
   "user-supplied binary" pattern as zexdoc (Phase 2.7) and eForth
   (Phase 3.4).
4. Builds a 256 256-byte SSSD layout (77 × 26 × 128 B) with:
   - LBA 0: 3-byte boot stub (`JP 0xFA00` into CBIOS BOOT)
   - LBA 1..N: CCP + BDOS bytes (44 sectors max)
   - LBA 45+: CBIOS bytes
   - All other bytes: `0xE5` (CP/M "empty directory entry" marker —
     looks blank to a fresh BDOS).
5. Writes the image to `<companion>/disks/cpm22-sssd.img`. The
   output inherits the CP/M licence by virtue of containing CCP and
   BDOS bytes — that's why it lands in the companion repo, not here.

## File flow recap

```
  cbios.asm  (this repo)          cpm22/binary/{ccp,bdos}.bin
       │                                   │ (companion repo)
       ▼                                   │
   pasmo                                   │
       │                                   │
       ▼                                   ▼
   cbios.bin   ──────►   build.ps1 combine
                                  │
                                  ▼
                       disks/cpm22-sssd.img
                          (companion repo)
```

One-way: the build reads from both repos, writes only into the
companion repo. The plugin itself builds and tests without the
companion present.

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
