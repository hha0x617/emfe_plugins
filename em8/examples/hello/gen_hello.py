#!/usr/bin/env python3
"""
Generate hello.bin — EM8 "Hello, EM8!" program.

Outputs "Hello, EM8!\n" to the UART at $F000, then halts.
Binary format: 64KB raw image, code at $0200, reset vector at $FFFC.
"""

import os
import struct

UART_DATA = 0xF000
CODE_START = 0x0200
RESET_VEC = 0xFFFC


def emit(mem, addr, *data):
    """Write bytes into memory at addr. Returns address after last byte."""
    for i, b in enumerate(data):
        mem[addr + i] = b & 0xFF
    return addr + len(data)


def emit_word_le(mem, addr, word):
    """Write a 16-bit little-endian word."""
    mem[addr] = word & 0xFF
    mem[addr + 1] = (word >> 8) & 0xFF


def main():
    mem = bytearray(65536)
    pc = CODE_START

    # The string will be placed after the code.
    # We need to know its address for the LDA absolute,X instruction.
    # Assemble code first, then place the string.

    # Program:
    #   LDX #$00        ; A6 00
    # loop:
    #   LDA str,X       ; A4 lo hi    (absolute,X)
    #   BEQ done        ; 50 xx
    #   STA $F000       ; AD 00 F0
    #   INX             ; 4A
    #   BRA loop        ; 58 xx
    # done:
    #   HLT             ; 9A

    loop_addr = CODE_START + 2  # after LDX #$00

    # LDX #$00
    pc = emit(mem, pc, 0xA6, 0x00)

    # Save loop address
    assert pc == loop_addr

    # LDA str,X — we'll patch the address after placing the string
    lda_addr = pc
    pc = emit(mem, pc, 0xA4, 0x00, 0x00)  # placeholder for string address

    # BEQ done — offset to be patched
    beq_addr = pc
    pc = emit(mem, pc, 0x50, 0x00)  # placeholder

    # STA $F000
    pc = emit(mem, pc, 0xAD, UART_DATA & 0xFF, (UART_DATA >> 8) & 0xFF)

    # INX
    pc = emit(mem, pc, 0x4A)

    # BRA loop — relative offset from PC after this instruction
    bra_target_pc = pc + 2  # PC after fetching BRA + offset
    bra_offset = loop_addr - bra_target_pc
    pc = emit(mem, pc, 0x58, bra_offset & 0xFF)

    # done: HLT
    done_addr = pc
    pc = emit(mem, pc, 0x9A)

    # Place the string
    str_addr = pc
    hello = b"Hello, EM8!\n\x00"
    for i, ch in enumerate(hello):
        mem[str_addr + i] = ch
    pc += len(hello)

    # Patch LDA str,X address
    emit_word_le(mem, lda_addr + 1, str_addr)

    # Patch BEQ done offset
    beq_target_pc = beq_addr + 2  # PC after fetching BEQ + offset
    beq_offset = done_addr - beq_target_pc
    mem[beq_addr + 1] = beq_offset & 0xFF

    # Set reset vector
    emit_word_le(mem, RESET_VEC, CODE_START)

    # Write binary
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hello.bin")
    with open(out_path, "wb") as f:
        f.write(mem)

    print(f"Generated {out_path}")
    print(f"  Code:   ${CODE_START:04X}-${pc - 1:04X} ({pc - CODE_START} bytes)")
    print(f"  String: ${str_addr:04X} \"Hello, EM8!\\n\"")
    print(f"  Reset vector: ${RESET_VEC:04X} -> ${CODE_START:04X}")


if __name__ == "__main__":
    main()
