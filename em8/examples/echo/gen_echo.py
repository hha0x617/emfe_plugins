#!/usr/bin/env python3
"""
Generate echo.bin — EM8 interrupt-driven UART echo program.

Sets up IRQ handler to receive characters from UART RX and echo them back.
Prints a prompt "> " then enters an idle loop waiting for interrupts.
Binary format: 64KB raw image, code at $0200, reset vector at $FFFC.
"""

import os

UART_DATA = 0xF000
UART_STATUS = 0xF001
UART_CONTROL = 0xF002
IRQEN = 0xF030
IRQFLAG = 0xF031
IRQACK = 0xF032

CODE_START = 0x0200
RESET_VEC = 0xFFFC
IRQ_VEC = 0xFFFE


def emit(mem, addr, *data):
    """Write bytes into memory at addr. Returns address after last byte."""
    for i, b in enumerate(data):
        mem[addr + i] = b & 0xFF
    return addr + len(data)


def emit_word_le(mem, addr, word):
    """Write a 16-bit little-endian word."""
    mem[addr] = word & 0xFF
    mem[addr + 1] = (word >> 8) & 0xFF


def rel8(from_pc, to_addr):
    """Compute signed 8-bit relative offset. from_pc = PC after branch instruction."""
    offset = to_addr - from_pc
    return offset & 0xFF


def main():
    mem = bytearray(65536)
    pc = CODE_START

    # -------------------------------------------------------
    # Main program
    # -------------------------------------------------------

    # SEI — disable IRQs during setup
    pc = emit(mem, pc, 0x93)                    # SEI

    # Enable UART RX interrupt in UART control register
    #   LDA #$01 / STA $F002
    pc = emit(mem, pc, 0xA0, 0x01)              # LDA #$01
    pc = emit(mem, pc, 0xAD,                    # STA UART_CONTROL
              UART_CONTROL & 0xFF, (UART_CONTROL >> 8) & 0xFF)

    # Enable UART RX bit in interrupt controller
    #   LDA #$01 / STA $F030
    pc = emit(mem, pc, 0xA0, 0x01)              # LDA #$01
    pc = emit(mem, pc, 0xAD,                    # STA IRQEN
              IRQEN & 0xFF, (IRQEN >> 8) & 0xFF)

    # Print prompt string "> "
    prompt = b"EM8 Echo. Type characters:\r\n> \x00"
    prompt_addr = pc + 100   # reserve space; will place string later
    # Use inline loop to print prompt
    #   LDX #$00
    pc = emit(mem, pc, 0xA6, 0x00)              # LDX #$00
    prompt_loop = pc
    #   LDA prompt,X
    pc = emit(mem, pc, 0xA4, 0x00, 0x00)        # LDA abs,X (placeholder)
    lda_prompt_patch = pc - 2  # address of lo byte in the instruction
    #   BEQ prompt_done
    beq_prompt = pc
    pc = emit(mem, pc, 0x50, 0x00)              # BEQ prompt_done (placeholder)
    #   STA $F000
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA
    #   INX
    pc = emit(mem, pc, 0x4A)                    # INX
    #   BRA prompt_loop
    bra_prompt = pc
    pc = emit(mem, pc, 0x58, 0x00)              # BRA prompt_loop (placeholder)
    mem[bra_prompt + 1] = rel8(bra_prompt + 2, prompt_loop)
    prompt_done = pc
    mem[beq_prompt + 1] = rel8(beq_prompt + 2, prompt_done)

    # CLI — enable IRQs
    pc = emit(mem, pc, 0x92)                    # CLI

    # idle: BRA idle — infinite loop waiting for interrupts
    idle_addr = pc
    pc = emit(mem, pc, 0x58, 0xFE)              # BRA idle (offset = -2)

    # -------------------------------------------------------
    # Place prompt string
    # -------------------------------------------------------
    prompt_addr = pc
    for ch in prompt:
        mem[pc] = ch
        pc += 1

    # Patch the LDA prompt,X address
    emit_word_le(mem, lda_prompt_patch, prompt_addr)

    # -------------------------------------------------------
    # ISR: UART echo interrupt service routine
    # -------------------------------------------------------
    isr_addr = pc

    # Read received character
    #   LDA $F000
    pc = emit(mem, pc, 0xA2,                    # LDA abs UART_DATA
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)

    # Check for carriage return ($0D) — echo CR+LF and print new prompt
    pc = emit(mem, pc, 0x24, 0x0D)              # CMP #$0D
    beq_cr = pc
    pc = emit(mem, pc, 0x50, 0x00)              # BEQ handle_cr (placeholder)

    # Normal character: echo it back
    #   STA $F000
    pc = emit(mem, pc, 0xAD,                    # STA abs UART_DATA
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)

    # Acknowledge interrupt
    #   LDA #$01 / STA $F032
    ack_and_rti = pc
    pc = emit(mem, pc, 0xA0, 0x01)              # LDA #$01
    pc = emit(mem, pc, 0xAD,                    # STA abs IRQACK
              IRQACK & 0xFF, (IRQACK >> 8) & 0xFF)

    # RTI
    pc = emit(mem, pc, 0x63)                    # RTI

    # handle_cr: echo CR + LF + "> "
    handle_cr = pc
    mem[beq_cr + 1] = rel8(beq_cr + 2, handle_cr)

    pc = emit(mem, pc, 0xA0, 0x0D)              # LDA #$0D (CR)
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA
    pc = emit(mem, pc, 0xA0, 0x0A)              # LDA #$0A (LF)
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA
    pc = emit(mem, pc, 0xA0, 0x3E)              # LDA #'>'
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA
    pc = emit(mem, pc, 0xA0, 0x20)              # LDA #' '
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA

    # Acknowledge and RTI — jump to shared ack code
    bra_ack = pc
    pc = emit(mem, pc, 0x58, 0x00)              # BRA ack_and_rti (placeholder)
    mem[bra_ack + 1] = rel8(bra_ack + 2, ack_and_rti)

    # -------------------------------------------------------
    # Vectors
    # -------------------------------------------------------
    emit_word_le(mem, RESET_VEC, CODE_START)
    emit_word_le(mem, IRQ_VEC, isr_addr)

    # Write binary
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "echo.bin")
    with open(out_path, "wb") as f:
        f.write(mem)

    print(f"Generated {out_path}")
    print(f"  Code:         ${CODE_START:04X}-${pc - 1:04X} ({pc - CODE_START} bytes)")
    print(f"  ISR:          ${isr_addr:04X}")
    print(f"  Prompt:       ${prompt_addr:04X}")
    print(f"  Reset vector: ${RESET_VEC:04X} -> ${CODE_START:04X}")
    print(f"  IRQ vector:   ${IRQ_VEC:04X} -> ${isr_addr:04X}")


if __name__ == "__main__":
    main()
