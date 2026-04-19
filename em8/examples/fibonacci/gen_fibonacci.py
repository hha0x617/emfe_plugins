#!/usr/bin/env python3
"""
Generate fibonacci.bin — EM8 Fibonacci sequence program.

Computes and outputs the first 13 Fibonacci numbers (1,1,2,3,5,8,13,21,34,55,89,144,233)
as decimal strings to the UART at $F000.

Uses zero-page variables and a JSR/RTS subroutine for decimal printing.
Binary format: 64KB raw image, code at $0200, reset vector at $FFFC.
"""

import os

UART_DATA = 0xF000
CODE_START = 0x0200
RESET_VEC = 0xFFFC

# Zero-page variables
ZP_CURR = 0x10      # current Fibonacci number (8-bit)
ZP_NEXT = 0x11      # next Fibonacci number (8-bit)
ZP_COUNT = 0x12     # remaining count
ZP_TEMP = 0x13      # temp for addition
ZP_PRINT_VAL = 0x14 # value to print (used by print_decimal)
ZP_HUNDREDS = 0x15   # hundreds digit
ZP_TENS = 0x16       # tens digit
ZP_ONES = 0x17       # ones digit
ZP_STARTED = 0x18    # leading-zero suppression flag


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
    # Main program: compute and print 13 Fibonacci numbers
    # -------------------------------------------------------
    # Initialize: curr=0, next=1, count=13
    #   LDA #$00 / STA ZP_CURR
    #   LDA #$01 / STA ZP_NEXT
    #   LDA #$0D / STA ZP_COUNT
    pc = emit(mem, pc, 0xA0, 0x00)              # LDA #$00
    pc = emit(mem, pc, 0xAC, ZP_CURR)           # STA ZP_CURR
    pc = emit(mem, pc, 0xA0, 0x01)              # LDA #$01
    pc = emit(mem, pc, 0xAC, ZP_NEXT)           # STA ZP_NEXT
    pc = emit(mem, pc, 0xA0, 13)                # LDA #13
    pc = emit(mem, pc, 0xAC, ZP_COUNT)          # STA ZP_COUNT

    # fib_loop:
    fib_loop = pc

    # Print next (the Fibonacci number to output)
    #   LDA ZP_NEXT / STA ZP_PRINT_VAL / JSR print_decimal
    pc = emit(mem, pc, 0xA1, ZP_NEXT)           # LDA ZP_NEXT
    pc = emit(mem, pc, 0xAC, ZP_PRINT_VAL)      # STA ZP_PRINT_VAL
    # JSR print_decimal — address to be patched
    jsr_print_addr = pc
    pc = emit(mem, pc, 0x61, 0x00, 0x00)        # JSR print_decimal (placeholder)

    # Print newline
    pc = emit(mem, pc, 0xA0, 0x0A)              # LDA #$0A (newline)
    pc = emit(mem, pc, 0xAD, UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA $F000

    # Compute next: temp = curr + next, curr = next, next = temp
    pc = emit(mem, pc, 0xA1, ZP_CURR)           # LDA ZP_CURR
    pc = emit(mem, pc, 0x90)                    # CLC
    pc = emit(mem, pc, 0x11, ZP_NEXT)           # ADD ZP_NEXT
    pc = emit(mem, pc, 0xAC, ZP_TEMP)           # STA ZP_TEMP
    pc = emit(mem, pc, 0xA1, ZP_NEXT)           # LDA ZP_NEXT
    pc = emit(mem, pc, 0xAC, ZP_CURR)           # STA ZP_CURR
    pc = emit(mem, pc, 0xA1, ZP_TEMP)           # LDA ZP_TEMP
    pc = emit(mem, pc, 0xAC, ZP_NEXT)           # STA ZP_NEXT

    # Decrement count and loop: LDA zpg / DEC A / STA zpg
    pc = emit(mem, pc, 0xA1, ZP_COUNT)          # LDA ZP_COUNT
    pc = emit(mem, pc, 0x49)                    # DEC A
    pc = emit(mem, pc, 0xAC, ZP_COUNT)          # STA ZP_COUNT
    bne_addr = pc
    pc = emit(mem, pc, 0x51, 0x00)              # BNE fib_loop (placeholder)
    mem[bne_addr + 1] = rel8(bne_addr + 2, fib_loop)

    # HLT
    pc = emit(mem, pc, 0x9A)                    # HLT

    # -------------------------------------------------------
    # Subroutine: print_decimal
    # Prints the 8-bit value in ZP_PRINT_VAL as a decimal string to UART.
    # Handles values 0-255. Suppresses leading zeros.
    # -------------------------------------------------------
    print_decimal = pc

    # Patch the JSR target
    emit_word_le(mem, jsr_print_addr + 1, print_decimal)

    # Extract hundreds digit: subtract 100 repeatedly
    pc = emit(mem, pc, 0xA0, 0x00)              # LDA #$00
    pc = emit(mem, pc, 0xAC, ZP_HUNDREDS)       # STA ZP_HUNDREDS
    pc = emit(mem, pc, 0xAC, ZP_STARTED)        # STA ZP_STARTED (no digits printed yet)
    pc = emit(mem, pc, 0xA1, ZP_PRINT_VAL)      # LDA ZP_PRINT_VAL

    hundreds_loop = pc
    pc = emit(mem, pc, 0x24, 100)               # CMP #100
    bcc_hundreds = pc
    pc = emit(mem, pc, 0x53, 0x00)              # BCC hundreds_done (placeholder)
    pc = emit(mem, pc, 0x91)                    # SEC
    pc = emit(mem, pc, 0x14, 100)               # SUB #100
    # INC ZP_HUNDREDS: save A, load zpg, inc, store, restore A
    pc = emit(mem, pc, 0x70)                    # PHA
    pc = emit(mem, pc, 0xA1, ZP_HUNDREDS)       # LDA ZP_HUNDREDS
    pc = emit(mem, pc, 0x48)                    # INC A
    pc = emit(mem, pc, 0xAC, ZP_HUNDREDS)       # STA ZP_HUNDREDS
    pc = emit(mem, pc, 0x71)                    # PLA
    bra_hundreds = pc
    pc = emit(mem, pc, 0x58, 0x00)              # BRA hundreds_loop (placeholder)
    mem[bra_hundreds + 1] = rel8(bra_hundreds + 2, hundreds_loop)
    hundreds_done = pc
    mem[bcc_hundreds + 1] = rel8(bcc_hundreds + 2, hundreds_done)

    # Store remainder for tens extraction
    pc = emit(mem, pc, 0xAC, ZP_PRINT_VAL)      # STA ZP_PRINT_VAL (remainder)

    # Print hundreds digit if non-zero
    pc = emit(mem, pc, 0xA1, ZP_HUNDREDS)       # LDA ZP_HUNDREDS
    beq_skip_hundreds = pc
    pc = emit(mem, pc, 0x50, 0x00)              # BEQ skip_hundreds (placeholder)
    pc = emit(mem, pc, 0x90)                    # CLC
    pc = emit(mem, pc, 0x10, 0x30)              # ADD #'0'
    pc = emit(mem, pc, 0xAD, UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA $F000
    pc = emit(mem, pc, 0xA0, 0x01)              # LDA #$01
    pc = emit(mem, pc, 0xAC, ZP_STARTED)        # STA ZP_STARTED (mark started)
    skip_hundreds = pc
    mem[beq_skip_hundreds + 1] = rel8(beq_skip_hundreds + 2, skip_hundreds)

    # Extract tens digit: subtract 10 repeatedly
    pc = emit(mem, pc, 0xA0, 0x00)              # LDA #$00
    pc = emit(mem, pc, 0xAC, ZP_TENS)           # STA ZP_TENS
    pc = emit(mem, pc, 0xA1, ZP_PRINT_VAL)      # LDA ZP_PRINT_VAL

    tens_loop = pc
    pc = emit(mem, pc, 0x24, 10)                # CMP #10
    bcc_tens = pc
    pc = emit(mem, pc, 0x53, 0x00)              # BCC tens_done (placeholder)
    pc = emit(mem, pc, 0x91)                    # SEC
    pc = emit(mem, pc, 0x14, 10)                # SUB #10
    # INC ZP_TENS: save A, load zpg, inc, store, restore A
    pc = emit(mem, pc, 0x70)                    # PHA
    pc = emit(mem, pc, 0xA1, ZP_TENS)           # LDA ZP_TENS
    pc = emit(mem, pc, 0x48)                    # INC A
    pc = emit(mem, pc, 0xAC, ZP_TENS)           # STA ZP_TENS
    pc = emit(mem, pc, 0x71)                    # PLA
    bra_tens = pc
    pc = emit(mem, pc, 0x58, 0x00)              # BRA tens_loop (placeholder)
    mem[bra_tens + 1] = rel8(bra_tens + 2, tens_loop)
    tens_done = pc
    mem[bcc_tens + 1] = rel8(bcc_tens + 2, tens_done)

    # Store ones remainder
    pc = emit(mem, pc, 0xAC, ZP_ONES)           # STA ZP_ONES

    # Print tens digit if started or non-zero
    pc = emit(mem, pc, 0xA1, ZP_TENS)           # LDA ZP_TENS
    bne_print_tens = pc
    pc = emit(mem, pc, 0x51, 0x00)              # BNE print_tens (placeholder)
    # Check if started (hundreds was printed)
    pc = emit(mem, pc, 0xA1, ZP_STARTED)        # LDA ZP_STARTED
    beq_skip_tens = pc
    pc = emit(mem, pc, 0x50, 0x00)              # BEQ skip_tens (placeholder)
    # Print '0' for tens (hundreds was non-zero but tens is zero)
    pc = emit(mem, pc, 0xA1, ZP_TENS)           # LDA ZP_TENS (reload 0)
    print_tens = pc
    mem[bne_print_tens + 1] = rel8(bne_print_tens + 2, print_tens)
    pc = emit(mem, pc, 0x90)                    # CLC
    pc = emit(mem, pc, 0x10, 0x30)              # ADD #'0'
    pc = emit(mem, pc, 0xAD, UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA $F000
    skip_tens = pc
    mem[beq_skip_tens + 1] = rel8(beq_skip_tens + 2, skip_tens)

    # Print ones digit (always printed)
    pc = emit(mem, pc, 0xA1, ZP_ONES)           # LDA ZP_ONES
    pc = emit(mem, pc, 0x90)                    # CLC
    pc = emit(mem, pc, 0x10, 0x30)              # ADD #'0'
    pc = emit(mem, pc, 0xAD, UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)  # STA $F000

    # RTS
    pc = emit(mem, pc, 0x62)                    # RTS

    # -------------------------------------------------------
    # Vectors
    # -------------------------------------------------------
    emit_word_le(mem, RESET_VEC, CODE_START)

    # Write binary
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fibonacci.bin")
    with open(out_path, "wb") as f:
        f.write(mem)

    print(f"Generated {out_path}")
    print(f"  Code:            ${CODE_START:04X}-${pc - 1:04X} ({pc - CODE_START} bytes)")
    print(f"  print_decimal:   ${print_decimal:04X}")
    print(f"  Zero-page vars:  ${ZP_CURR:02X}-${ZP_STARTED:02X}")
    print(f"  Reset vector:    ${RESET_VEC:04X} -> ${CODE_START:04X}")
    print(f"  Output: 1,1,2,3,5,8,13,21,34,55,89,144,233 (one per line)")


if __name__ == "__main__":
    main()
