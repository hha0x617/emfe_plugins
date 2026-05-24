# Phase 4 — CP/M 2.2 boot design

Status: **Design draft** — review-ready, no implementation yet.
Companion: [`emfe_plugins_cpm_supplements`](https://github.com/hha0x617/emfe_plugins_cpm_supplements)
(private as of 2026-05-24).

This document fills in the level of detail that
[`implementation_plan.md`](implementation_plan.md) §6 leaves open. It
covers the disk-controller MMIO contract, the CBIOS skeleton, the
cross-repo build pipeline, and the disk-image format. Each section
ends with the concrete sub-phase (4.1–4.5) it unblocks.

## 1. Sector-device MMIO (sub-phase 4.1)

The plugin exposes a minimal "sector device" in the Z80 I/O space.
Not a historic floppy controller (no WD1793 / NEC 765 emulation) —
the CBIOS does its own logical-to-physical mapping. This keeps the
emulator surface tiny while letting the CBIOS evolve independently.

### 1.1 Port allocation (extends `docs/mmio.md`)

| Port  | Direction | Purpose |
|-------|-----------|---------|
| `0xF0` | r/w | LBA byte 0 (bits 0–7)   |
| `0xF1` | r/w | LBA byte 1 (bits 8–15)  |
| `0xF2` | r/w | LBA byte 2 (bits 16–23) — reserved, always reads 0 in Phase 4 |
| `0xF3` | r/w | Sector buffer pointer low (in guest RAM) |
| `0xF4` | r/w | Sector buffer pointer high |
| `0xF5` | w   | Command register — write to trigger I/O |
| `0xF6` | r   | Status register |
| `0xF7` | r   | Reserved (reads 0 in Phase 4) |

24-bit LBA (16 MiB max) is overkill for CP/M 2.2 (which addresses
at most ~8 MiB per drive), but the third byte costs nothing and
keeps a hard ceiling out of the way for any later experiments.

### 1.2 Sector size

**Fixed at 128 bytes** — the CP/M 2.2 native record size. The CBIOS
deblocks larger logical sectors itself if the on-disk format uses
them; from the plugin's point of view there is exactly one size.

### 1.3 Command register (write `0xF5`)

| Value | Meaning |
|-------|---------|
| `0x01` | Read sector at LBA into guest RAM at the buffer pointer |
| `0x02` | Write sector from guest RAM at the buffer pointer to LBA |
| `0x00` | No-op (defensive — useful for clearing residual command state) |
| other  | Reserved — sets status `ERROR` bit |

Write completes synchronously from the guest's point of view (the
host performs the read / write before returning from `OUT (0xF5),A`).
This is a deliberate simplification: real hardware would be async +
status-polled, but synchronous is simpler for the guest CBIOS and
indistinguishable behaviourally when the disk image is in memory.

### 1.4 Status register (read `0xF6`)

| Bit | Name      | Meaning when set |
|-----|-----------|------------------|
| 0   | `READY`   | Device ready (always 1 in Phase 4) |
| 1   | `ERROR`   | Last command failed (bad LBA, write to RO image, etc.) |
| 2   | `WRITE_PROTECT` | Loaded image is read-only |
| 3   | `IMAGE_LOADED`  | A disk image is currently mounted |
| 4–7 | reserved | Read as 0 |

Sticky-error semantics: `ERROR` stays set until the guest writes
`0x00` to the command register.

### 1.5 Host-side mounting

A single disk image at a time in Phase 4 (drive A only). The image
path comes from the Settings dialog (sub-phase 4.4 wires this up via
the existing settings infrastructure that landed in Phase 2.6). Image
flag bits:

- **Image absent** → `IMAGE_LOADED=0`, every command sets `ERROR`.
- **Image RW** → `WRITE_PROTECT=0`; writes mutate the in-memory buffer
  AND flush back to disk asynchronously on shutdown (Phase 4) or
  per-command (Phase 5 if needed).
- **Image RO** → `WRITE_PROTECT=1`; writes set `ERROR` and are dropped.

## 2. Disk image format (sub-phase 4.4 input)

The plugin reads a **flat sector-level dump** — no DSK / IMD container
overhead. A 256 KiB SSSD floppy is 256 KiB on disk; an 8 MiB hard-
disk image is 8 MiB on disk. The CBIOS knows the on-disk geometry
(tracks-per-disk, sectors-per-track, reserved-tracks-for-system),
the emulator does not.

Acceptable extensions: `.img`, `.dsk` (flat-format only), `.cpm`.
Loader detection is by file extension — the emulator does not
auto-detect formats with embedded headers.

### 2.1 Reference image layouts (Phase 4 target)

| Profile | Size | Geometry | Use |
|---|---|---|---|
| **SSSD-256K** | 256 KiB | 77 tracks × 26 sectors × 128 B | "Classic" CP/M 2.2 floppy. First boot image. |
| **HD-8M** | 8 MiB | 256 tracks × 256 sectors × 128 B | Optional. For later Phase 5 work with large utility sets. |

Both layouts are flat byte arrays. The CBIOS computes LBA from
(track, sector) at the CP/M end.

## 3. CBIOS skeleton (sub-phase 4.2)

CP/M 2.2 expects 17 BIOS entry points at a 3-byte JMP-table aligned
just below the CCP. The standard table layout is:

| # | Vector | Purpose | Phase 4 strategy |
|---|--------|---------|------------------|
| 0 | BOOT    | Cold boot — system entry | Init UART + disk MMIO; jump to CCP |
| 1 | WBOOT   | Warm boot — reload CCP from disk | Read CCP tracks via disk MMIO |
| 2 | CONST   | Console status | Read UART status port `0xFF` |
| 3 | CONIN   | Console read | Loop until `RX_AVAIL`, then `IN A,(0xFE)` |
| 4 | CONOUT  | Console write | `OUT (0xFE),A` |
| 5 | LIST    | Printer write | Stub (no-op) |
| 6 | PUNCH   | Paper-tape punch | Stub |
| 7 | READER  | Paper-tape reader | Stub (returns `^Z`) |
| 8 | HOME    | Move to track 0 | Update CBIOS-internal track variable |
| 9 | SELDSK  | Select disk | Validate drive A; return DPH pointer |
| 10 | SETTRK | Set track number | Update CBIOS variable |
| 11 | SETSEC | Set sector number | Update CBIOS variable |
| 12 | SETDMA | Set DMA address | Update CBIOS variable |
| 13 | READ   | Read one logical record | Map (track,sector) → LBA, write to MMIO, trigger read |
| 14 | WRITE  | Write one logical record | Same, but trigger write |
| 15 | LISTST | Printer status | Return ready always |
| 16 | SECTRAN | Logical → physical sector | Identity in Phase 4 (no skew) |

The skeleton lives at `examples/cpm22/cbios/cbios.asm` (MIT, original
work). It is **not** a port of any specific historic CBIOS — it is
written from scratch against the BIOS spec and the plugin's MMIO
contract.

### 3.1 Memory map for the boot image

CP/M 2.2 with a 64 KiB memory image traditionally lands the
CCP+BDOS+BIOS at the top of memory:

| Address | Component | Notes |
|---|---|---|
| `0x0000–0x00FF` | Zero page | Reset vector, BIOS jump table |
| `0x0100–0xE3FF` | TPA | Transient Program Area |
| `0xE400–0xEBFF` | CCP    | Console Command Processor (~2 KiB) |
| `0xEC00–0xF9FF` | BDOS   | Basic Disk OS (~3.5 KiB) |
| `0xFA00–0xFFFF` | CBIOS  | Custom BIOS (~1.5 KiB) |

These addresses are conventional but not absolute — the CBIOS knows
where it lives and updates the JMP-table accordingly. Sub-phase 4.2
fixes the exact load address in `cbios.asm`.

### 3.2 Cold-boot sequence

1. Plugin loads the boot image into RAM at `0x0000` (sub-phase 4.5
   wires this up via Settings — same path that `lastLoadedFile`
   already follows for the Phase 1.7 hello-world).
2. Plugin sets `PC := 0x0000` and starts execution.
3. Boot sector (≤128 B at `0x0000`) jumps to CBIOS BOOT entry.
4. CBIOS BOOT loads CCP+BDOS from track 0 / sector 1 onwards into
   their canonical addresses via repeated MMIO reads.
5. CBIOS sets up the BDOS entry vector at `0x0005` and CCP entry at
   `0x0000` (the standard CP/M reset vector convention).
6. CBIOS jumps into the CCP, which prints `A>`.

The reset vector at `0x0000` is special: CCP cold start.
`JP 0x0000` from anywhere is a warm boot in CP/M 2.2 convention.

## 4. Cross-repo build pipeline (sub-phase 4.3)

The flow:

```
emfe_plugins_cpm_supplements/cpm22/source/   (CP/M-licensed, upstream)
            │
            ▼  z80asm / pasmo
       (assemble CCP, BDOS)
            │
            ▼
emfe_plugins/z80/examples/cpm22/cbios/cbios.asm   (MIT, this repo)
            │
            ▼  z80asm / pasmo
       (assemble CBIOS)
            │
            ▼
emfe_plugins/z80/examples/cpm22/Makefile  (or build.ps1, MIT)
            │
            ▼  combine + write disk image
            ▼
emfe_plugins_cpm_supplements/disks/cpm22-sssd.img   (CP/M-licensed, output)
```

The Makefile (or PowerShell equivalent — TBD; PowerShell is more
host-native for Windows, but make is more portable) lives in
`examples/cpm22/` and assumes the companion repo is cloned as a
sibling. It does **not** require the companion to be cloned for the
plugin to build / test — only the disk-image generation step does.

Sub-phase 4.3 fixes:

- The assembler choice (likely **`pasmo`** — single-binary, Windows-
  friendly, takes both `.asm` source and outputs flat `.bin`).
- The exact CCP+BDOS+CBIOS+padding layout that produces a flat
  SSSD-256K image.
- The verification step (boot the resulting image and grep for the
  `A>` prompt in the captured UART output).

## 5. Settings additions (sub-phase 4.4)

The existing Phase 2.6 settings infrastructure (StagedSettings,
REQUIRES_RESET deferral) handles all of these without infrastructure
changes:

| Setting key | Type | Range | Default | REQUIRES_RESET |
|---|---|---|---|---|
| `DiskImagePath`        | string | path or empty | `""` | yes |
| `DiskImageWriteProtect` | bool   | 0/1 | 0 | yes |
| `DiskImageSizeBytes`   | int (read-only) | 0..16M | 0 | n/a |
| `BootFromDisk`         | bool   | 0/1 | 0 | yes |

`BootFromDisk` flips the cold-boot behaviour:

- `0` → bare-metal boot at PC=0 (current Phase 1.7 / Phase 3.4 mode).
- `1` → load disk's boot sector into RAM, set PC=0, but ALSO mount
  the rest of the image into the disk MMIO.

The two modes are mutually exclusive at runtime but easy to flip
between via the Settings dialog.

## 6. Acceptance criterion

**Phase 4 done when** a CP/M 2.2 cold boot from the SSSD-256K image
prints `A>` into the captured UART output and accepts a `DIR`
command (the latter belongs to Phase 5 but is the natural smoke
test). A new `TestCpmBootIfPresent` test in `tests/test_z80.cpp`
mirrors the zexdoc / eForth SKIP-when-absent pattern: looks for
`examples/cpm22/cpm22-sssd.img` (a symlink or copy of the companion
repo's output), boots it, asserts the captured tail contains `A>`.

## 7. Open questions for review

1. **Assembler choice** — pasmo vs sjasmplus vs z80asm. pasmo is
   the smallest dependency footprint; sjasmplus has better macros;
   z80asm is the most "purist" syntax. Lean: **pasmo**, unless the
   CBIOS macros end up uncomfortable.
2. **Disk image flush policy** — flush on shutdown only (simple,
   risk of lost writes on crash) vs flush per-command (durable,
   slower). Lean: **flush on shutdown** for Phase 4; revisit at
   Phase 5 if it bites.
3. **Single drive vs multi-drive** — CP/M 2.2 supports A..P. Phase
   4 is single-drive only (drive A). Multi-drive could come in Phase
   5 if a use case needs it. Lean: **single drive in Phase 4**.
4. **CCP/BDOS source vs binary** — assemble from source in the
   companion repo (Phase 4.3 builds CCP+BDOS+CBIOS), or use the
   pre-assembled reference binaries from the companion's
   `cpm22/binary/` and only assemble the CBIOS. The former is purer
   end-to-end; the latter is faster to iterate. Lean: **start with
   the reference binaries**, switch to building from source if
   anything goes wrong with the binaries.
5. **Build host PowerShell vs Makefile** — see §4. Lean: a
   PowerShell driver script that calls pasmo / cat / etc. is the
   most native on Windows and matches the rest of the toolchain.
6. **Boot stub size** — the boot stub at `0x0000` is at most 128 B
   (one sector). Is that enough to load CCP+BDOS+CBIOS, or do we
   need a two-stage loader? Lean: **single-stage** if it fits;
   two-stage if the BIOS loader doesn't fit in 128 B.
