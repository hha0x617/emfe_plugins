#!/usr/bin/env python3
"""
Generate timer_blink.bin — EM8 timer interrupt demo.

Uses the programmable timer to generate periodic interrupts.
Each timer tick toggles between outputting '*' and '.' to the UART,
demonstrating timer peripheral and IRQ handling.

Binary format: 64KB raw image, code at $0200, reset vector at $FFFC.
"""

import os

UART_DATA = 0xF000
TMR_RELOAD_LO = 0xF012
TMR_RELOAD_HI = 0xF013
TMR_CONTROL = 0xF014
TMR_STATUS = 0xF015
IRQEN = 0xF030
IRQACK = 0xF032

CODE_START = 0x0200
RESET_VEC = 0xFFFC
IRQ_VEC = 0xFFFE

# Zero-page variables
ZP_TICK_COUNT = 0x10    # tick counter (for display)
ZP_CHAR_TOGGLE = 0x11   # 0 = '*', 1 = '.'


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

    # Timer reload value: $1000 (4096 cycles per tick)
    TIMER_RELOAD = 0x1000

    # -------------------------------------------------------
    # Main program
    # -------------------------------------------------------

    # SEI — disable IRQs during setup
    pc = emit(mem, pc, 0x93)                    # SEI

    # Initialize zero-page variables
    pc = emit(mem, pc, 0xA0, 0x00)              # LDA #$00
    pc = emit(mem, pc, 0xAC, ZP_TICK_COUNT)     # STA ZP_TICK_COUNT
    pc = emit(mem, pc, 0xAC, ZP_CHAR_TOGGLE)    # STA ZP_CHAR_TOGGLE

    # Print banner
    banner = b"Timer demo (tick=4096 cycles):\r\n\x00"
    banner_addr_placeholder = pc  # we'll patch after placing the string

    # LDX #$00
    pc = emit(mem, pc, 0xA6, 0x00)              # LDX #$00
    banner_loop = pc
    # LDA banner,X
    pc = emit(mem, pc, 0xA4, 0x00, 0x00)        # LDA abs,X (placeholder)
    lda_banner_patch = pc - 2
    # BEQ banner_done
    beq_banner = pc
    pc = emit(mem, pc, 0x50, 0x00)              # BEQ banner_done (placeholder)
    # STA $F000
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)
    # INX
    pc = emit(mem, pc, 0x4A)                    # INX
    # BRA banner_loop
    bra_banner = pc
    pc = emit(mem, pc, 0x58, 0x00)              # BRA banner_loop (placeholder)
    mem[bra_banner + 1] = rel8(bra_banner + 2, banner_loop)
    banner_done = pc
    mem[beq_banner + 1] = rel8(beq_banner + 2, banner_done)

    # Set timer reload value
    pc = emit(mem, pc, 0xA0, TIMER_RELOAD & 0xFF)       # LDA #lo
    pc = emit(mem, pc, 0xAD,
              TMR_RELOAD_LO & 0xFF, (TMR_RELOAD_LO >> 8) & 0xFF)  # STA TMR_RELOAD_LO
    pc = emit(mem, pc, 0xA0, (TIMER_RELOAD >> 8) & 0xFF)  # LDA #hi
    pc = emit(mem, pc, 0xAD,
              TMR_RELOAD_HI & 0xFF, (TMR_RELOAD_HI >> 8) & 0xFF)  # STA TMR_RELOAD_HI

    # Set timer control: ENABLE | IRQ_EN | AUTO_RELOAD = 0x07
    pc = emit(mem, pc, 0xA0, 0x07)              # LDA #$07
    pc = emit(mem, pc, 0xAD,
              TMR_CONTROL & 0xFF, (TMR_CONTROL >> 8) & 0xFF)  # STA TMR_CONTROL

    # Enable timer interrupt in interrupt controller (bit 2)
    pc = emit(mem, pc, 0xA0, 0x04)              # LDA #$04
    pc = emit(mem, pc, 0xAD,
              IRQEN & 0xFF, (IRQEN >> 8) & 0xFF)  # STA IRQEN

    # CLI — enable IRQs
    pc = emit(mem, pc, 0x92)                    # CLI

    # idle: BRA idle — infinite loop, timer ISR does all work
    idle_addr = pc
    pc = emit(mem, pc, 0x58, 0xFE)              # BRA idle (offset = -2)

    # -------------------------------------------------------
    # Place banner string
    # -------------------------------------------------------
    banner_str_addr = pc
    for ch in banner:
        mem[pc] = ch
        pc += 1
    emit_word_le(mem, lda_banner_patch, banner_str_addr)

    # -------------------------------------------------------
    # ISR: Timer interrupt service routine
    # -------------------------------------------------------
    isr_addr = pc

    # Clear timer underflow flag: write $01 to TMR_STATUS
    pc = emit(mem, pc, 0xA0, 0x01)              # LDA #$01
    pc = emit(mem, pc, 0xAD,
              TMR_STATUS & 0xFF, (TMR_STATUS >> 8) & 0xFF)  # STA TMR_STATUS

    # Toggle character: check ZP_CHAR_TOGGLE
    pc = emit(mem, pc, 0xA1, ZP_CHAR_TOGGLE)    # LDA ZP_CHAR_TOGGLE
    bne_dot = pc
    pc = emit(mem, pc, 0x51, 0x00)              # BNE print_dot (placeholder)

    # Print '*' and set toggle to 1
    pc = emit(mem, pc, 0xA0, ord('*'))          # LDA #'*'
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA
    pc = emit(mem, pc, 0xA0, 0x01)              # LDA #$01
    pc = emit(mem, pc, 0xAC, ZP_CHAR_TOGGLE)    # STA ZP_CHAR_TOGGLE
    bra_after_print = pc
    pc = emit(mem, pc, 0x58, 0x00)              # BRA after_print (placeholder)

    # print_dot: Print '.' and set toggle to 0
    print_dot = pc
    mem[bne_dot + 1] = rel8(bne_dot + 2, print_dot)
    pc = emit(mem, pc, 0xA0, ord('.'))          # LDA #'.'
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA
    pc = emit(mem, pc, 0xA0, 0x00)              # LDA #$00
    pc = emit(mem, pc, 0xAC, ZP_CHAR_TOGGLE)    # STA ZP_CHAR_TOGGLE

    # after_print:
    after_print = pc
    mem[bra_after_print + 1] = rel8(bra_after_print + 2, after_print)

    # Increment tick counter: LDA zpg / INC A / STA zpg
    pc = emit(mem, pc, 0xA1, ZP_TICK_COUNT)     # LDA ZP_TICK_COUNT
    pc = emit(mem, pc, 0x48)                    # INC A
    pc = emit(mem, pc, 0xAC, ZP_TICK_COUNT)     # STA ZP_TICK_COUNT

    # Every 16 ticks, print a newline (tick_count AND $0F == 0)
    pc = emit(mem, pc, 0xA1, ZP_TICK_COUNT)     # LDA ZP_TICK_COUNT
    pc = emit(mem, pc, 0x18, 0x0F)              # AND #$0F
    bne_no_newline = pc
    pc = emit(mem, pc, 0x51, 0x00)              # BNE no_newline (placeholder)
    pc = emit(mem, pc, 0xA0, 0x0A)              # LDA #$0A (newline)
    pc = emit(mem, pc, 0xAD,
              UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA UART_DATA
    no_newline = pc
    mem[bne_no_newline + 1] = rel8(bne_no_newline + 2, no_newline)

    # Acknowledge timer interrupt (bit 2)
    pc = emit(mem, pc, 0xA0, 0x04)              # LDA #$04
    pc = emit(mem, pc, 0xAD,
              IRQACK & 0xFF, (IRQACK >> 8) & 0xFF)  # STA IRQACK

    # RTI
    pc = emit(mem, pc, 0x63)                    # RTI

    # -------------------------------------------------------
    # Vectors
    # -------------------------------------------------------
    emit_word_le(mem, RESET_VEC, CODE_START)
    emit_word_le(mem, IRQ_VEC, isr_addr)

    # Write binary
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "timer_blink.bin")
    with open(out_path, "wb") as f:
        f.write(mem)

    print(f"Generated {out_path}")
    print(f"  Code:         ${CODE_START:04X}-${pc - 1:04X} ({pc - CODE_START} bytes)")
    print(f"  ISR:          ${isr_addr:04X}")
    print(f"  Banner:       ${banner_str_addr:04X}")
    print(f"  Timer reload: ${TIMER_RELOAD:04X} ({TIMER_RELOAD} cycles)")
    print(f"  Reset vector: ${RESET_VEC:04X} -> ${CODE_START:04X}")
    print(f"  IRQ vector:   ${IRQ_VEC:04X} -> ${isr_addr:04X}")
    print(f"  Output: alternating '*' and '.' with newline every 16 ticks")


if __name__ == "__main__":
    main()
