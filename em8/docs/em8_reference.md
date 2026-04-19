# EM8 ISA Reference

## 1. Overview

The EM8 is a custom 8-bit CPU designed for embedded systems education and emulation. It features a simple yet capable instruction set inspired by classic 8-bit architectures, with a 64KB address space, three general-purpose registers, hardware stack, and memory-mapped I/O peripherals.

Key characteristics:

- 8-bit data bus, 16-bit address bus
- 64KB linear address space ($0000-$FFFF)
- Three general-purpose 8-bit registers (A, X, Y)
- Hardware stack at $0100-$01FF (256 bytes, grows downward)
- Memory-mapped I/O at $F000-$F03F
- Vectored interrupts (IRQ, NMI, Reset)

## 2. Registers

| Register | Width | Description |
|----------|-------|-------------|
| A | 8-bit | Accumulator. Primary register for arithmetic, logic, and I/O. |
| X | 8-bit | Index register X. Used for indexed addressing and loop counters. |
| Y | 8-bit | Index register Y. Used for indexed addressing. |
| SP | 8-bit | Stack pointer. Points within the stack page $0100-$01FF. Initialized to $FF on reset. |
| PC | 16-bit | Program counter. Points to the next instruction to execute. |
| FL | 8-bit | Flags register. Contains processor status flags. |

## 3. Flags Register (FL)

The flags register is 8 bits wide. Bits are assigned as follows:

```
  Bit 7   Bit 6   Bit 5   Bit 4   Bit 3   Bit 2   Bit 1   Bit 0
+-------+-------+-------+-------+-------+-------+-------+-------+
|   N   |   V   |   -   |   B   |   -   |   I   |   Z   |   C   |
+-------+-------+-------+-------+-------+-------+-------+-------+
```

| Bit | Name | Description |
|-----|------|-------------|
| 7 | N (Negative) | Set if the result has bit 7 set (negative in two's complement). |
| 6 | V (Overflow) | Set if a signed arithmetic overflow occurred. |
| 5 | - | Unused, always 1. |
| 4 | B (Break) | Set if the interrupt was caused by a BRK instruction. Exists only on the stack copy of FL. |
| 3 | - | Unused, always 1. |
| 2 | I (Interrupt Disable) | When set, IRQ interrupts are masked. NMI is not affected. |
| 1 | Z (Zero) | Set if the result is zero. |
| 0 | C (Carry) | Set if an unsigned arithmetic carry or borrow occurred. Also used by shift/rotate instructions. |

## 4. Memory Map

| Address Range | Size | Description |
|---------------|------|-------------|
| $0000-$00FF | 256 bytes | Zero page. Fast access for zero-page addressing modes. |
| $0100-$01FF | 256 bytes | Hardware stack. SP indexes within this page. |
| $0200-$EFFF | 60,928 bytes | General-purpose RAM. Program code and data. |
| $F000-$F03F | 64 bytes | Memory-mapped I/O peripherals. |
| $F040-$FFF9 | 4,026 bytes | Reserved / additional RAM. |
| $FFFA-$FFFB | 2 bytes | NMI vector (little-endian). |
| $FFFC-$FFFD | 2 bytes | Reset vector (little-endian). |
| $FFFE-$FFFF | 2 bytes | IRQ/BRK vector (little-endian). |

## 5. Addressing Modes

| Mode | Syntax | Description | Example |
|------|--------|-------------|---------|
| Implied | (none) | Operand is implicit in the instruction. | `INX` |
| Immediate | `#$nn` | 8-bit constant operand follows the opcode. | `LDA #$42` |
| Zero Page | `$nn` | 8-bit address in zero page ($0000-$00FF). | `LDA $10` |
| Absolute | `$nnnn` | Full 16-bit address. | `LDA $1234` |
| Zero Page,X | `$nn,X` | Zero-page address + X register (wraps within zero page). | `LDA $10,X` |
| Absolute,X | `$nnnn,X` | Absolute address + X register. | `LDA $1234,X` |
| Absolute,Y | `$nnnn,Y` | Absolute address + Y register. | `LDA $1234,Y` |
| Indirect | `($nnnn)` | 16-bit address read from the given address (for JMP only). | `JMP ($1234)` |
| Relative | `$nn` (signed) | Signed 8-bit offset from PC+2 (for branch instructions). | `BEQ label` |

## 6. Instruction Set

### 6.1 Load / Store

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $A9 | LDA | Immediate | 2 | 2 | N, Z | Load A with immediate value |
| $A5 | LDA | Zero Page | 2 | 3 | N, Z | Load A from zero page |
| $AD | LDA | Absolute | 3 | 4 | N, Z | Load A from absolute address |
| $B5 | LDA | Zero Page,X | 2 | 4 | N, Z | Load A from zero page + X |
| $BD | LDA | Absolute,X | 3 | 4 | N, Z | Load A from absolute + X |
| $B9 | LDA | Absolute,Y | 3 | 4 | N, Z | Load A from absolute + Y |
| $A2 | LDX | Immediate | 2 | 2 | N, Z | Load X with immediate value |
| $A6 | LDX | Zero Page | 2 | 3 | N, Z | Load X from zero page |
| $AE | LDX | Absolute | 3 | 4 | N, Z | Load X from absolute address |
| $A0 | LDY | Immediate | 2 | 2 | N, Z | Load Y with immediate value |
| $A4 | LDY | Zero Page | 2 | 3 | N, Z | Load Y from zero page |
| $AC | LDY | Absolute | 3 | 4 | N, Z | Load Y from absolute address |
| $85 | STA | Zero Page | 2 | 3 | - | Store A to zero page |
| $8D | STA | Absolute | 3 | 4 | - | Store A to absolute address |
| $95 | STA | Zero Page,X | 2 | 4 | - | Store A to zero page + X |
| $9D | STA | Absolute,X | 3 | 5 | - | Store A to absolute + X |
| $99 | STA | Absolute,Y | 3 | 5 | - | Store A to absolute + Y |
| $86 | STX | Zero Page | 2 | 3 | - | Store X to zero page |
| $8E | STX | Absolute | 3 | 4 | - | Store X to absolute address |
| $84 | STY | Zero Page | 2 | 3 | - | Store Y to zero page |
| $8C | STY | Absolute | 3 | 4 | - | Store Y to absolute address |

### 6.2 Arithmetic

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $69 | ADC | Immediate | 2 | 2 | N, V, Z, C | Add with carry to A |
| $65 | ADC | Zero Page | 2 | 3 | N, V, Z, C | Add with carry from zero page |
| $6D | ADC | Absolute | 3 | 4 | N, V, Z, C | Add with carry from absolute |
| $E9 | SBC | Immediate | 2 | 2 | N, V, Z, C | Subtract with borrow from A |
| $E5 | SBC | Zero Page | 2 | 3 | N, V, Z, C | Subtract with borrow from zero page |
| $ED | SBC | Absolute | 3 | 4 | N, V, Z, C | Subtract with borrow from absolute |
| $E8 | INX | Implied | 1 | 2 | N, Z | Increment X |
| $C8 | INY | Implied | 1 | 2 | N, Z | Increment Y |
| $E6 | INC | Zero Page | 2 | 5 | N, Z | Increment memory (zero page) |
| $EE | INC | Absolute | 3 | 6 | N, Z | Increment memory (absolute) |
| $CA | DEX | Implied | 1 | 2 | N, Z | Decrement X |
| $88 | DEY | Implied | 1 | 2 | N, Z | Decrement Y |
| $C6 | DEC | Zero Page | 2 | 5 | N, Z | Decrement memory (zero page) |
| $CE | DEC | Absolute | 3 | 6 | N, Z | Decrement memory (absolute) |

### 6.3 Logic

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $29 | AND | Immediate | 2 | 2 | N, Z | Bitwise AND with A |
| $25 | AND | Zero Page | 2 | 3 | N, Z | Bitwise AND from zero page |
| $2D | AND | Absolute | 3 | 4 | N, Z | Bitwise AND from absolute |
| $09 | ORA | Immediate | 2 | 2 | N, Z | Bitwise OR with A |
| $05 | ORA | Zero Page | 2 | 3 | N, Z | Bitwise OR from zero page |
| $0D | ORA | Absolute | 3 | 4 | N, Z | Bitwise OR from absolute |
| $49 | EOR | Immediate | 2 | 2 | N, Z | Bitwise exclusive OR with A |
| $45 | EOR | Zero Page | 2 | 3 | N, Z | Bitwise exclusive OR from zero page |
| $4D | EOR | Absolute | 3 | 4 | N, Z | Bitwise exclusive OR from absolute |

### 6.4 Shift / Rotate

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $0A | ASL | Accumulator | 1 | 2 | N, Z, C | Arithmetic shift left A. Bit 7 goes to C, 0 enters bit 0. |
| $06 | ASL | Zero Page | 2 | 5 | N, Z, C | Arithmetic shift left memory (zero page) |
| $4A | LSR | Accumulator | 1 | 2 | N, Z, C | Logical shift right A. Bit 0 goes to C, 0 enters bit 7. |
| $46 | LSR | Zero Page | 2 | 5 | N, Z, C | Logical shift right memory (zero page) |
| $2A | ROL | Accumulator | 1 | 2 | N, Z, C | Rotate left A through carry. |
| $26 | ROL | Zero Page | 2 | 5 | N, Z, C | Rotate left memory (zero page) through carry. |
| $6A | ROR | Accumulator | 1 | 2 | N, Z, C | Rotate right A through carry. |
| $66 | ROR | Zero Page | 2 | 5 | N, Z, C | Rotate right memory (zero page) through carry. |

### 6.5 Compare / Test

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $C9 | CMP | Immediate | 2 | 2 | N, Z, C | Compare A with immediate (A - operand) |
| $C5 | CMP | Zero Page | 2 | 3 | N, Z, C | Compare A with zero page |
| $CD | CMP | Absolute | 3 | 4 | N, Z, C | Compare A with absolute |
| $E0 | CPX | Immediate | 2 | 2 | N, Z, C | Compare X with immediate |
| $E4 | CPX | Zero Page | 2 | 3 | N, Z, C | Compare X with zero page |
| $EC | CPX | Absolute | 3 | 4 | N, Z, C | Compare X with absolute |
| $C0 | CPY | Immediate | 2 | 2 | N, Z, C | Compare Y with immediate |
| $C4 | CPY | Zero Page | 2 | 3 | N, Z, C | Compare Y with zero page |
| $CC | CPY | Absolute | 3 | 4 | N, Z, C | Compare Y with absolute |
| $24 | BIT | Zero Page | 2 | 3 | N, V, Z | Test bits. Z = A AND mem == 0, N = mem bit 7, V = mem bit 6. |
| $2C | BIT | Absolute | 3 | 4 | N, V, Z | Test bits (absolute). |

### 6.6 Branch

All branch instructions are 2 bytes: opcode + signed 8-bit relative offset. The offset is relative to PC after fetching the branch instruction (PC+2). Branches take 2 cycles if not taken, 3 cycles if taken, +1 if page boundary crossed.

| Opcode | Mnemonic | Condition | Description |
|--------|----------|-----------|-------------|
| $F0 | BEQ | Z = 1 | Branch if equal (zero) |
| $D0 | BNE | Z = 0 | Branch if not equal (not zero) |
| $B0 | BCS | C = 1 | Branch if carry set |
| $90 | BCC | C = 0 | Branch if carry clear |
| $30 | BMI | N = 1 | Branch if minus (negative) |
| $10 | BPL | N = 0 | Branch if plus (positive) |
| $70 | BVS | V = 1 | Branch if overflow set |
| $50 | BVC | V = 0 | Branch if overflow clear |
| $80 | BRA | (always) | Branch always (unconditional) |

### 6.7 Jump / Subroutine

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $4C | JMP | Absolute | 3 | 3 | - | Jump to absolute address |
| $6C | JMP | Indirect | 3 | 5 | - | Jump to address stored at given address |
| $20 | JSR | Absolute | 3 | 6 | - | Jump to subroutine. Pushes return address - 1 onto stack. |
| $60 | RTS | Implied | 1 | 6 | - | Return from subroutine. Pulls PC from stack and adds 1. |

### 6.8 Stack

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $48 | PHA | Implied | 1 | 3 | - | Push A onto stack |
| $68 | PLA | Implied | 1 | 4 | N, Z | Pull A from stack |
| $08 | PHP | Implied | 1 | 3 | - | Push FL onto stack (B flag set in pushed copy) |
| $28 | PLP | Implied | 1 | 4 | All | Pull FL from stack |

### 6.9 Transfer

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $AA | TAX | Implied | 1 | 2 | N, Z | Transfer A to X |
| $A8 | TAY | Implied | 1 | 2 | N, Z | Transfer A to Y |
| $8A | TXA | Implied | 1 | 2 | N, Z | Transfer X to A |
| $98 | TYA | Implied | 1 | 2 | N, Z | Transfer Y to A |
| $BA | TSX | Implied | 1 | 2 | N, Z | Transfer SP to X |
| $9A | TXS | Implied | 1 | 2 | - | Transfer X to SP |

### 6.10 Flag Control

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $18 | CLC | Implied | 1 | 2 | C | Clear carry flag |
| $38 | SEC | Implied | 1 | 2 | C | Set carry flag |
| $58 | CLI | Implied | 1 | 2 | I | Clear interrupt disable flag (enable IRQs) |
| $78 | SEI | Implied | 1 | 2 | I | Set interrupt disable flag (disable IRQs) |
| $B8 | CLV | Implied | 1 | 2 | V | Clear overflow flag |

### 6.11 System / Miscellaneous

| Opcode | Mnemonic | Mode | Bytes | Cycles | Flags | Description |
|--------|----------|------|-------|--------|-------|-------------|
| $EA | NOP | Implied | 1 | 2 | - | No operation |
| $00 | BRK | Implied | 1 | 7 | I, B | Software interrupt. Pushes PC+1, FL (with B set) onto stack. Loads PC from IRQ vector ($FFFE). |
| $40 | RTI | Implied | 1 | 6 | All | Return from interrupt. Pulls FL and PC from stack. |
| $02 | HLT | Implied | 1 | 1 | - | Halt the CPU. Stops execution. |

## 7. Interrupt Mechanism

### 7.1 IRQ (Interrupt Request)

Hardware interrupt triggered by peripheral devices. Maskable via the I flag.

1. Current instruction completes.
2. If I flag is clear (interrupts enabled):
   a. Push PC high byte onto stack.
   b. Push PC low byte onto stack.
   c. Push FL onto stack (B flag clear in pushed copy).
   d. Set I flag (disable further IRQs).
   e. Load PC from IRQ vector at $FFFE-$FFFF (little-endian).

### 7.2 NMI (Non-Maskable Interrupt)

Non-maskable interrupt triggered on the NMI input edge. Cannot be disabled by the I flag.

1. Current instruction completes.
2. Push PC high byte onto stack.
3. Push PC low byte onto stack.
4. Push FL onto stack (B flag clear in pushed copy).
5. Set I flag.
6. Load PC from NMI vector at $FFFA-$FFFB (little-endian).

### 7.3 BRK (Software Interrupt)

Software-triggered interrupt via the BRK instruction ($00).

1. PC is incremented by 1 (BRK has a padding byte).
2. Push PC high byte onto stack.
3. Push PC low byte onto stack.
4. Push FL onto stack (B flag SET in pushed copy).
5. Set I flag.
6. Load PC from IRQ vector at $FFFE-$FFFF (little-endian).

The ISR can distinguish BRK from hardware IRQ by examining the B flag in the stacked FL.

### 7.4 RTI (Return from Interrupt)

The RTI instruction reverses the interrupt entry sequence:

1. Pull FL from stack (restoring all flags including I).
2. Pull PC low byte from stack.
3. Pull PC high byte from stack.
4. Resume execution at the restored PC.

### 7.5 Interrupt Priority

| Priority | Source | Vector | Maskable |
|----------|--------|--------|----------|
| Highest | Reset | $FFFC | No |
| High | NMI | $FFFA | No |
| Low | IRQ/BRK | $FFFE | Yes (I flag) |

## 8. Peripheral Registers

All peripherals are memory-mapped in the I/O region $F000-$F03F.

### 8.1 UART ($F000-$F00F)

Simple UART for serial character I/O.

| Address | Name | R/W | Description |
|---------|------|-----|-------------|
| $F000 | UART_DATA | R/W | **Read**: Receive data register. Returns the next character from the RX FIFO. **Write**: Transmit data register. Sends a character to the TX output. |
| $F001 | UART_STATUS | R | Status register (see below). |
| $F002 | UART_CONTROL | R/W | Control register (see below). |

**UART_STATUS ($F001) bits:**

| Bit | Name | Description |
|-----|------|-------------|
| 0 | RX_READY | 1 = data available in RX FIFO |
| 1 | TX_READY | 1 = transmitter ready to accept data |
| 7-2 | - | Reserved, read as 0 |

**UART_CONTROL ($F002) bits:**

| Bit | Name | Description |
|-----|------|-------------|
| 0 | RX_IRQ_EN | 1 = enable IRQ when RX_READY becomes set |
| 1 | TX_IRQ_EN | 1 = enable IRQ when TX_READY becomes set |
| 7-2 | - | Reserved, write 0 |

### 8.2 Timer ($F010-$F01F)

Programmable interval timer. Counts down from the reload value and generates an IRQ on underflow.

| Address | Name | R/W | Description |
|---------|------|-----|-------------|
| $F010 | TMR_COUNT_LO | R | Current counter value, low byte. |
| $F011 | TMR_COUNT_HI | R | Current counter value, high byte. |
| $F012 | TMR_RELOAD_LO | R/W | Reload value, low byte. |
| $F013 | TMR_RELOAD_HI | R/W | Reload value, high byte. |
| $F014 | TMR_CONTROL | R/W | Timer control register (see below). |
| $F015 | TMR_STATUS | R | Timer status register (see below). |

**TMR_CONTROL ($F014) bits:**

| Bit | Name | Description |
|-----|------|-------------|
| 0 | ENABLE | 1 = timer is running |
| 1 | IRQ_EN | 1 = generate IRQ on underflow |
| 2 | AUTO_RELOAD | 1 = reload counter on underflow (periodic mode). 0 = one-shot. |
| 7-3 | - | Reserved, write 0 |

**TMR_STATUS ($F015) bits:**

| Bit | Name | Description |
|-----|------|-------------|
| 0 | UNDERFLOW | 1 = counter underflowed. Write 1 to clear. |
| 7-1 | - | Reserved, read as 0 |

The timer decrements at 1 count per CPU cycle. When the counter reaches 0 and ENABLE is set:

1. UNDERFLOW flag is set in TMR_STATUS.
2. If IRQ_EN is set, an IRQ is asserted.
3. If AUTO_RELOAD is set, the counter is reloaded from TMR_RELOAD. Otherwise, the timer stops.

### 8.3 GPIO ($F020-$F02F)

8-bit general-purpose I/O port.

| Address | Name | R/W | Description |
|---------|------|-----|-------------|
| $F020 | GPIO_DATA | R/W | **Read**: current pin levels. **Write**: output latch. |
| $F021 | GPIO_DIR | R/W | Direction register. 1 = output, 0 = input. Default: $00 (all input). |
| $F022 | GPIO_IRQ_EN | R/W | IRQ enable per pin. 1 = enable IRQ on pin change (input pins only). |
| $F023 | GPIO_IRQ_FLAG | R/W | IRQ flags per pin. 1 = pin triggered. Write 1 to clear. |

### 8.4 Interrupt Controller ($F030-$F03F)

Global interrupt status and control.

| Address | Name | R/W | Description |
|---------|------|-----|-------------|
| $F030 | IRQEN | R/W | Interrupt enable register (see below). |
| $F031 | IRQFLAG | R | Interrupt pending flags (see below). |
| $F032 | IRQACK | W | Interrupt acknowledge. Write 1-bits to clear corresponding IRQFLAG bits. |

**IRQEN / IRQFLAG bit assignments:**

| Bit | Source | Description |
|-----|--------|-------------|
| 0 | UART_RX | UART receive data available |
| 1 | UART_TX | UART transmitter ready |
| 2 | TIMER | Timer underflow |
| 3 | GPIO | GPIO pin change |
| 7-4 | - | Reserved |

An IRQ is asserted to the CPU when `(IRQFLAG & IRQEN) != 0` and the CPU's I flag is clear.

## 9. Reset Behavior

When the CPU is reset (power-on or explicit reset):

1. The I flag is set (interrupts disabled).
2. SP is initialized to $FF.
3. Registers A, X, Y are set to $00.
4. The unused FL bits (5 and 3) are set to 1.
5. PC is loaded from the reset vector at $FFFC-$FFFD (little-endian).
6. Execution begins at the loaded PC address.
7. All peripheral registers are reset to their default values.
