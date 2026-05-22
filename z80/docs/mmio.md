# emfe_plugin_z80 — MMIO Contract

The Z80 has a 256-byte **separate I/O space** addressed by `IN` / `OUT`
instructions, distinct from the 64 KiB main memory accessed by `LD` /
`ST` and friends. emfe peripherals attach to this I/O space (not to
main memory), so guest software that ignores I/O ports sees a flat
64 KiB RAM with nothing surprising in it.

## Port allocation

| Port range | Direction | Purpose | Phase |
|---|---|---|---|
| `0xFE` | r/w | Console UART data | 1.2 |
| `0xFF` | r | Console UART status | 1.2 |
| `0xF0–0xF7` | r/w | Disk MMIO (reserved) | 4 |
| `0xE0–0xE3` | r/w | Timer / CTC (reserved) | 6 (if needed) |
| any other port | — | Reserved for future use; reads return `0xFF`, writes are dropped |

Allocations are reviewed against `em8` and `z8000` so the family keeps
a consistent feel for guest authors writing test programs that target
multiple plugins.

## Console UART (ports `0xFE` / `0xFF`)

The UART exposes two ports. The data port is bidirectional; the status
port is read-only.

### Port `0xFE` — data

| Direction | Behaviour |
|---|---|
| `IN A, (0xFE)` | Pops the next byte from the receive FIFO. Returns `0` if the FIFO is empty (use status port first to check) |
| `OUT (0xFE), A` | Transmits `A` to the host. The host receives it via `EmfeConsoleCharCallback` if one was installed |

### Port `0xFF` — status

| Direction | Behaviour |
|---|---|
| `IN A, (0xFF)` | Reads a status byte (see bit layout below) |
| `OUT (0xFF), A` | Currently ignored. Reserved for future use (e.g. clearing an overrun flag) |

Status byte bit layout:

| Bit | Name | Meaning when set |
|---|---|---|
| 0 | `TX_READY` | The transmitter is empty — `OUT` will succeed immediately. **Always set** in this implementation (TX is non-blocking) |
| 1 | `RX_AVAIL` | The receive FIFO holds at least one byte |
| 2 | `RX_OVERRUN` | The host pushed a byte while the FIFO was full. Sticky until the FIFO drains and a fresh byte is accepted |
| 3–7 | reserved | Read as 0 |

### Receive FIFO

The plugin maintains an 8-byte circular receive FIFO. The host pushes
bytes via `emfe_send_char` / `emfe_send_string`; the guest pops them
with `IN A, (0xFE)`. If the host pushes while the FIFO is full, the
byte is dropped and the `RX_OVERRUN` status bit is set.

## Disk MMIO (`0xF0–0xF7`, reserved for Phase 4)

Specification will be finalised in Phase 4 alongside the custom CBIOS
written for the [`emfe_plugin_z80_cpm22`](https://github.com/hha0x617/emfe_plugin_z80_cpm22)
companion repository. The intent is a minimal "sector device" — sector
buffer + LBA + read/write trigger — not an emulation of a historic
controller (WD1793, NEC 765, etc.). Keeping it minimal lets the CBIOS
be small and platform-specific instead of dragging in a full
floppy-controller emulation.

## Timer / CTC (`0xE0–0xE3`, reserved for Phase 6)

Optional. Decided at Phase 6 if the settings UI grows a "tick at N Hz"
feature that needs guest visibility.

## Why a separate I/O space rather than memory-mapped?

The Z80's I/O space is part of its ISA — `IN` / `OUT` are first-class
opcodes. Using it for peripherals matches what real Z80 systems did
(CP/M, MSX, ZX Spectrum, SAM Coupé) and keeps the 64 KiB memory map
clean for guest software.

This is a deliberate departure from the `em8` plugin (which
memory-maps its UART at `$F000–$F00F` because the 6502 has no separate
I/O space). The `z8000` plugin made the same I/O-space choice this
plugin does, since the Zilog family has always carried `IN` / `OUT`.
