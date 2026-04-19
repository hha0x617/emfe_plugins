# Zilog Z8000 Family — emfe Plugin Reference

## 1. Overview

The Z8000 (1979) is Zilog's 16-bit processor family. This plugin supports all
four variants selectable at runtime via the `CpuVariant` setting:

| Variant | Segmented | Virtual Memory | Addr Space | PC Width |
|---------|-----------|----------------|-----------|----------|
| Z8001   | yes       | no             | 8 MB (7+16 bit) | 2 words |
| Z8002   | no        | no             | 64 KB (16 bit)  | 1 word  |
| Z8003   | yes       | yes (abort)    | 8 MB  | 2 words |
| Z8004   | no        | yes (abort)    | 64 KB | 1 word  |

**Phase 1 status**: Only Z8002 mode is fully implemented. The other variants
fall back to Z8002 behaviour; segmented addressing and VM-abort support are
scheduled for Phase 2 and Phase 3 respectively.

## 2. Registers

### General-Purpose (16 words × 16 bits)

```
R0  R1  R2  R3  R4  R5  R6  R7
R8  R9  R10 R11 R12 R13 R14 R15   <- R15 = SP (non-segmented)
                                     R14:R15 = SP pair (segmented)
```

Register views:
- **Byte**: `RH0..RH7` (high byte of R0..R7), `RL0..RL7` (low byte of R0..R7).
  Byte-register fields encode as 0..15 (RH0..RH7=0..7, RL0..RL7=8..15).
- **Long (32-bit)**: `RR0, RR2, RR4, ..., RR14` — pairs of consecutive
  even/odd word registers. `RR0 = (R0<<16) | R1`.
- **Quad (64-bit)**: `RQ0, RQ4, RQ8, RQ12` — four consecutive registers.

### Special / Control

| Name    | Width | Description |
|---------|-------|-------------|
| PC      | 16    | Program counter (32-bit in segmented mode) |
| FCW     | 16    | Flag and Control Word (status register) |
| PSAP    | 16    | Program Status Area Pointer (exception vector base) |
| REFRESH | 16    | DRAM refresh counter |
| R14'/R15' | 16  | Shadow stack pointer for the inactive mode |

### FCW bits

```
 15 14 13 12 11 10 9 8 | 7  6  5  4   3  2   1 0
  0 SEG S/N EPA VIE NVIE - -| C  Z  S  P/V  DA H   - -
```

- `SEG`  — segmented mode (Z8001/Z8003 only)
- `S/N`  — 1 = System mode, 0 = Normal mode
- `EPA`  — Extended Processor Available (unused by the base CPU)
- `VIE`  — Vectored Interrupt Enable
- `NVIE` — Non-Vectored Interrupt Enable
- `C`    — Carry (unsigned overflow / shift-out)
- `Z`    — Zero
- `S`    — Sign
- `P/V`  — Parity (logical) / signed Overflow (arithmetic)
- `DA`   — Decimal Adjust (0 = last op was add, 1 = subtract)
- `H`    — Half-carry (bit 3→4 carry on byte ops, bit 11→12 on word ops)

## 3. Memory Map (Phase 1, Z8002 mode)

```
$0000-$0001  Reserved for reset vector PC (not FCW — see below)
$0002-$0003  Initial FCW (loaded on reset)
$0004-$0005  Initial PC  (loaded on reset)
$0006-...    Program Status Area (exception vectors)
...
$FE00-$FE0F  I/O port space: UART (4 registers at offsets 0..3)
$FE10-$FE1F  I/O port space: Timer (7 registers at offsets 0..6)
```

Z8000 stores 16-bit words in **big-endian** order (high byte at lower address).
Word accesses must be even-aligned.

## 4. I/O Port Layout (Phase 1)

Unlike em8, the Z8000 has a separate I/O port space reached via the `IN` and
`OUT` instructions. Peripherals live there, not in memory.

### UART (port base $FE00)

| Port    | Reg name | Description |
|---------|----------|-------------|
| $FE00   | UART_DR  | Data register — read pops RX FIFO, write sends TX |
| $FE01   | UART_SR  | bit0=RX ready, bit1=TX empty (always 1), bit2=RX overrun |
| $FE02   | UART_CR  | bit0=RX IRQ enable, bit1=TX IRQ enable |

Phase 2 will replace this with a Z8530 SCC implementation.

### Timer (port base $FE10)

| Port    | Reg name     | Description |
|---------|--------------|-------------|
| $FE10   | TMR_LO       | Reload value low byte |
| $FE11   | TMR_HI       | Reload value high byte |
| $FE12/13| TMR_CTR_LO/HI| Current counter (read-only) |
| $FE14   | TMR_CR       | bit0=enable, bit1=auto-reload, bit2=IRQ enable |
| $FE15   | TMR_SR       | bit0=overflow flag |
| $FE16   | TMR_ACK      | Any write clears overflow |

Phase 2 will replace this with a Z8036 CIO or Z8430 CTC.

## 5. Condition Codes (4-bit cc field)

Used by `JR`, `JP`, `RET`, `CALR`-less variants.

| Code | Mnemonic | Meaning                  |
|------|----------|--------------------------|
| 0x0  | F        | Never                    |
| 0x1  | LT       | Signed less than         |
| 0x2  | LE       | Signed less or equal     |
| 0x3  | ULE      | Unsigned less or equal   |
| 0x4  | OV / PE  | Overflow / parity even   |
| 0x5  | MI       | Minus (sign set)         |
| 0x6  | Z / EQ   | Zero / equal             |
| 0x7  | C / ULT  | Carry / unsigned less than |
| 0x8  | T        | Always                   |
| 0x9  | GE       | Signed greater or equal  |
| 0xA  | GT       | Signed greater than      |
| 0xB  | UGT      | Unsigned greater than    |
| 0xC  | NOV / PO | No overflow / parity odd |
| 0xD  | PL       | Plus (sign clear)        |
| 0xE  | NE / NZ  | Not equal / non-zero     |
| 0xF  | NC / UGE | No carry / unsigned ≥    |

## 6. Implemented Instructions (Phase 1)

Encodings taken from the Zilog Z8000 CPU Technical Manual (Jan 83) and
cross-verified against the MAME `z8000` CPU core.

### Register-to-register (word and byte)

| Mnemonic | Encoding | Operation |
|----------|----------|-----------|
| `ADDB RBd,RBs` | `$80[s][d]` | byte add |
| `ADD Rd,Rs`    | `$81[s][d]` | word add |
| `SUBB`         | `$82[s][d]` | byte sub |
| `SUB`          | `$83[s][d]` | word sub |
| `ORB`          | `$84[s][d]` | byte or  |
| `OR`           | `$85[s][d]` | word or  |
| `ANDB`         | `$86[s][d]` | byte and |
| `AND`          | `$87[s][d]` | word and |
| `XORB`         | `$88[s][d]` | byte xor |
| `XOR`          | `$89[s][d]` | word xor |
| `CPB`          | `$8A[s][d]` | byte compare (sets flags) |
| `CP`           | `$8B[s][d]` | word compare |
| `LDB RBd,RBs`  | `$A0[s][d]` | byte register copy |
| `LD Rd,Rs`     | `$A1[s][d]` | word register copy |

### Immediate loads

| Mnemonic | Encoding | Notes |
|----------|----------|-------|
| `LDB RBd,#imm8`  | `$C[d][imm8]` | 4-bit byte reg, 8-bit immediate |
| `LD Rd,#imm16`   | `$210[d] imm16` | 4-bit word reg, 16-bit immediate |

### Memory (register indirect)

| Mnemonic | Encoding |
|----------|----------|
| `LDB RBd,@Rs` | `$20[s][d]` (s≠0) |
| `LD Rd,@Rs`   | `$21[s][d]` (s≠0 — s=0 is the LD-immediate form) |
| `LDB @Rd,RBs` | `$2E[d][s]` (d≠0) |
| `LD @Rd,Rs`   | `$2F[d][s]` (d≠0) |

### Memory (direct address)

| Mnemonic | Encoding |
|----------|----------|
| `LDB RBd,addr` | `$70[0][d] addr16` |
| `LD Rd,addr`   | `$71[0][d] addr16` |
| `LDB addr,RBs` | `$78[0][s] addr16` |
| `LD addr,Rs`   | `$79[0][s] addr16` |

### Stack

| Mnemonic | Encoding | Notes |
|----------|----------|-------|
| `PUSH @Rd,Rs` | `$13[d][s]` (d≠0) | Pre-decrement @Rd, write Rs |
| `POP Rd,@Rs`  | `$17[s][d]` (s≠0) | Read @Rs into Rd, post-increment Rs |

### Subroutines & Jumps

| Mnemonic | Encoding | Notes |
|----------|----------|-------|
| `CALL addr`  | `$5F00 addr16` | absolute call |
| `CALL @Rd`   | `$5F[d][0]` (d≠0) | indirect call |
| `CALR disp12` | `$D[disp12]` | target = PC - 2*disp12 (12-bit unsigned, backward) |
| `RET cc`     | `$9E0[cc]` | conditional return |
| `JP cc,addr` | `$5E0[cc] addr16` | conditional absolute jump |
| `JP cc,@Rd`  | `$1E[d][cc]` (d≠0) | conditional indirect jump |
| `JR cc,disp8`| `$E[cc][disp8]` | target = PC + 2*disp8 (8-bit signed) |

### I/O

| Mnemonic | Encoding | Notes |
|----------|----------|-------|
| `INB RBd,#port`  | `$3A[d]4 imm16` | byte input from direct port |
| `IN Rd,#port`    | `$3B[d]4 imm16` | word input |
| `OUTB #port,RBs` | `$3A[s]6 imm16` | byte output |
| `OUT #port,Rs`   | `$3B[s]6 imm16` | word output |
| `INB RBd,@Rs`    | `$3C[s][d]` | byte input, port in Rs |
| `IN Rd,@Rs`      | `$3D[s][d]` | word input, port in Rs |
| `OUTB @Rd,RBs`   | `$3E[d][s]` | byte output, port in Rd |
| `OUT @Rd,Rs`     | `$3F[d][s]` | word output, port in Rd |

All I/O instructions are privileged (trap if S/N=0).

### Control

| Mnemonic | Encoding | Notes |
|----------|----------|-------|
| `NOP`  | `$8D07` | |
| `HALT` | `$7A00` | privileged |
| `IRET` | `$7B00` | pop FCW then PC; privileged |
| `DI`   | `$7C0[0..3]` | low bits select NVIE/VIE to disable; privileged |
| `EI`   | `$7C0[4..7]` | enable; privileged |

## 7. Hello World Example (Phase 1b)

Write 'Z' to the UART:

```
   Address    Encoding      Assembly
   $0100      C85A          LDB  RL0,#$5A     ; 'Z'
   $0102      3A86 FE00     OUTB #$FE00,RL0   ; send to UART
   $0106      7A00          HALT
```

Load at $0100, set PC=$0100, run. The plugin's console character callback
receives 'Z'.

## 8. Not Yet Implemented

Phase 1c candidates:

- `LDA Rd,addr` — load effective address (compute address without fetch)
- `INC Rd,#n` / `DEC Rd,#n` — register increment/decrement
- `LDL RRd,...` — long (32-bit) load/store variants
- `BIT / SET / RES` — bit manipulation
- `SHL / SHR / RL / RR` — shifts and rotates
- `DBJNZ` — decrement-and-branch (loop primitive)
- `EX Rd,Rs` — register exchange
- `MULT / DIV` — multiply and divide

Phase 2 will add:

- Segmented addressing (Z8001/Z8003 mode, 7+16 bit PC)
- Zilog SCC (Z8530) replacement for the UART
- Zilog CIO (Z8036) or CTC (Z8430) replacement for the Timer
- Traps: Segment Trap, Extended Instruction Trap, System Call

Phase 3 adds VM/abort support (Z8003/Z8004), optional Olivetti M20 board
emulation, and framebuffer integration.

## 9. References

- Zilog Z8000 CPU Technical Manual, January 1983 — bitsavers.org
- MAME Z8000 CPU core — `src/devices/cpu/z8000/` in the MAME repository
  ([mamedev/mame](https://github.com/mamedev/mame))
- GNU binutils Z8000 opcode tables — `gas/config/tc-z8k.c`
