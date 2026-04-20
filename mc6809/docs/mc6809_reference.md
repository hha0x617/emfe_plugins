# Motorola MC6809 — emfe Plugin Reference

## 1. Overview

emfe_plugin_mc6809 is a thin C ABI wrapper around the [em6809](../../../em6809)
Rust CPU core, exposing it through the standard emfe plugin interface so
that either emfe frontend (WinUI3/C++ or WPF/C#) can load and drive it.

## 2. Registers

| Name | Width | Description |
|------|-------|-------------|
| A    | 8 bit | Accumulator A |
| B    | 8 bit | Accumulator B |
| D    | 16 bit | Concatenation `A:B` (hidden view) |
| X    | 16 bit | Index register X |
| Y    | 16 bit | Index register Y |
| U    | 16 bit | User stack pointer |
| S    | 16 bit | System stack pointer (flagged as SP) |
| PC   | 16 bit | Program counter |
| DP   | 8 bit | Direct page |
| CC   | 8 bit | Condition code (flagged as FLAGS) |

### CC bits

```
  7   6   5   4   3   2   1   0
+---+---+---+---+---+---+---+---+
| E | F | H | I | N | Z | V | C |
+---+---+---+---+---+---+---+---+
```

- `E` — entire register state pushed on last interrupt
- `F` — FIRQ mask
- `H` — half-carry (bit-3 to bit-4)
- `I` — IRQ mask
- `N` — negative
- `Z` — zero
- `V` — signed overflow
- `C` — carry/borrow

## 3. Memory map

- **0x0000–0xFFFF**: 64 KB flat RAM (array in the plugin).
- **0xFFFE/0xFFFF**: reset vector (high byte first).

### Memory-mapped UART — MC6850 ACIA (default base `$FF00`)

The plugin emulates a **Motorola MC6850 ACIA** (Asynchronous Communications
Interface Adapter) — the canonical 6809-era serial UART used by SWTPC S/09,
Tandy Color Computer, Dragon 32/64, and most FLEX / OS-9 systems. Two
consecutive addresses are consumed:

| RS | Address | Read  | Write |
|----|---------|-------|-------|
| 0  | base+0  | **Status Register (SR)** | **Control Register (CR)** |
| 1  | base+1  | **Receive Data Register (RDR)** | **Transmit Data Register (TDR)** |

#### Status Register (read from base+0)

```
 7   6   5    4   3   2    1     0
+---+---+----+---+---+---+------+------+
|IRQ|PE |OVRN|FE |CTS|DCD| TDRE | RDRF |
+---+---+----+---+---+---+------+------+
```

- bit 0 **RDRF** — Receive Data Register Full (an RX byte is waiting)
- bit 1 **TDRE** — Transmit Data Register Empty (ready for next TX byte)
- bit 2 **DCD** — Data Carrier Detect (tied low; reads 0)
- bit 3 **CTS** — Clear To Send (tied low; reads 0)
- bit 4 **FE** — Framing Error (always 0)
- bit 5 **OVRN** — Receive Overrun (set when RX FIFO overflows; cleared by
  reading RDR)
- bit 6 **PE** — Parity Error (always 0)
- bit 7 **IRQ** — `(RDRF && RIE) || (TDRE && TX-IRQ-enabled)`

#### Control Register (write to base+0)

```
 7    6    5    4   3   2   1    0
+----+----+----+-----+-----+-------+
|RIE |TC1 | TC0| WS2..WS0 | CDS1 CDS0|
+----+----+----+-----+-----+-------+
```

- bits 0-1 **CDS** — Counter Divide Select:
  - `00` = ÷1, `01` = ÷16, `10` = ÷64, **`11` = Master Reset**
- bits 2-4 **WS** — Word Select (8N1, 7E1 etc.). Parsed but not enforced at
  the byte-transport level.
- bits 5-6 **TC** — Transmit Control (RTS + TX IRQ):
  - `00` = RTS low, TX IRQ **disabled**
  - `01` = RTS low, TX IRQ **enabled**
  - `10` = RTS high, TX IRQ disabled
  - `11` = RTS low, BREAK, TX IRQ disabled
- bit 7 **RIE** — Receive Interrupt Enable

#### Typical initialization sequence

```
  LDA #$03        ; Master Reset (CDS=11)
  STA ACIA_CR
  LDA #$15        ; CDS=01 (÷16), WS=101 (8N1), TC=00 (RTS low, no TX IRQ), RIE=0
  STA ACIA_CR
```

After the second write TDRE is set and the guest can transmit. The guest
polls TDRE (or enables TX IRQ) before writing TDR, and polls RDRF (or
enables RIE) before reading RDR.

#### IRQ line

The ACIA's IRQ output is routed into the MC6809 via `Bus::irq_lines()` as
the IRQ input (not FIRQ / NMI). Guest code must clear the I-flag in CC for
the interrupt to be taken.

#### Configuration

The base address is configurable via the `ConsoleBase` setting (e.g.
`0xFF00`, `0xE000`, `0xFF68` for CoCo ACIA Pak). The setting is picked up
on `emfe_apply_settings`.

## 4. Implemented plugin API (Phase 1)

- Discovery: `emfe_negotiate`, `emfe_get_board_info`
- Lifecycle: `emfe_create`, `emfe_destroy`
- Callbacks: console char / state-change / diagnostic
- Registers: definition list, batch get/set
- Memory: `peek_{byte,word,long}`, `poke_{byte,word,long}`, `peek_range`,
  `get_memory_size`
- Disassembly: `disassemble_one`, `disassemble_range`, `get_program_range`
  (uses em6809's `disasm::disasm_one`)
- Execution: `step`, `run`, `stop`, `reset`, `get_state`,
  `get_instruction_count`, `get_cycle_count`
- Breakpoints: add/remove/enable/condition/clear/get
- Watchpoints: add/remove/enable/condition/clear/get (read / write / RW)
- File loading: `load_binary`, `load_srec` (ELF unsupported)
- Settings: `BoardType`, `ConsoleBase`, `Theme`, console size
- Console I/O: `send_char`, `send_string`
- `step_over` / `step_out`: not supported in Phase 1

## 5. Not supported (Phase 1)

- Call stack introspection (`emfe_get_call_stack` returns 0)
- Framebuffer
- Input device events
- ELF loading (MC6809 toolchains produce S-records primarily)

## 6. Loading a program

### S-Record

```c
emfe_load_srec(inst, "hello.s19");
```

The loader:
- Parses `S1` and `S9` records (16-bit addresses)
- Places data into memory
- Sets PC from the `S9` entry point, or from the lowest loaded address
- Initialises S (system stack) to `$FF00`

### Raw binary

```c
emfe_load_binary(inst, "program.bin", 0x0100);
```

Loads the file at the given address. PC is set from the reset vector at
`$FFFE/$FFFF` if non-zero, otherwise from `load_address`.

## 7. Design notes

- The plugin embeds a `PluginBus` implementing the em6809 `Bus` trait.
  Memory is a `Box<[u8; 0x10000]>` to avoid large stack allocations.
- The MSVC C runtime is statically linked via
  `RUSTFLAGS="-C target-feature=+crt-static"` set in `.cargo/config.toml`.
- Panics inside FFI are currently not wrapped in `catch_unwind`; a Phase 2
  hardening task will add that for safety at the C ABI boundary.
- The `run` worker thread uses a raw pointer captured as `usize` so it can
  be sent across threads without `Arc<Mutex<..>>` overhead in the hot path.

## 8. Upstream em6809 samples

The upstream [em6809](../../../em6809) project ships its own sample programs
under `samples/` (hello, echo, vt100). **They target em6809's custom
`ConsoleDev` at `$FF00`, which is NOT binary-compatible with our MC6850
ACIA emulation.**

| | em6809 `ConsoleDev` | Plugin's MC6850 ACIA |
|---|---|---|
| base+0 read  | **RX FIFO pop** | **Status Register** |
| base+0 write | **TX byte**     | **Control Register** |
| base+1 read  | Status (custom) | **RDR** |
| base+1 write | Control (custom)| **TDR** |

Running em6809's `samples/hello/hello.s19` on this plugin will:
- Interpret `STA $FF00` as "write to CR" (likely corrupting the ACIA state)
- Interpret `LDA $FF00` as "read SR" (getting flag bits, not an RX byte)

**Workarounds (choose one)**:

1. Use the plugin's own samples under `examples/` — these correctly drive
   the MC6850.
2. Rebuild em6809's samples using MC6850 register layout (master reset,
   CR / SR polling). Templates are in this plugin's `examples/*.py`.
3. (Future Phase 2) add an optional `UartModel=EmConsoleDev` setting that
   switches the plugin's UART to em6809's custom layout for sample compat.

## 9. References

- em6809 upstream: [hha0x617/em6809](https://github.com/hha0x617/em6809)
- MC6809 programming reference: Motorola MC6809E 8-bit microprocessor datasheet
- MC6850 ACIA datasheet: Motorola MC6850 Asynchronous Communications
  Interface Adapter
- OS-9 / NitrOS-9: see em6809's `docs/en/os9_guide.md`
