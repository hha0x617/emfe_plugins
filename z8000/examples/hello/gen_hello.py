#!/usr/bin/env python3
"""
Generate hello.bin — Z8000 "Hello, Z8000!" program.

Outputs "Hello, Z8000!\r\n" to the UART at port $FE00, then halts.

Binary format: 64KB raw image, code at $0100, Program Status Area at $0000:
  $0002-$0003 : initial FCW = $2000 (System mode, interrupts disabled)
  $0004-$0005 : initial PC  = $0100
Z8000 memory is big-endian — words are stored high byte first.
"""

import os

UART_DATA = 0xFE00          # UART data port (OUTB target)
CODE_START = 0x0100
INITIAL_FCW = 0x2000        # System mode
INITIAL_SP = 0xF000


def emit(mem, addr, *data):
    """Write bytes into memory at addr. Returns address after last byte."""
    for i, b in enumerate(data):
        mem[addr + i] = b & 0xFF
    return addr + len(data)


def emit_word_be(mem, addr, word):
    """Write a 16-bit big-endian word (Z8000 native)."""
    mem[addr]     = (word >> 8) & 0xFF
    mem[addr + 1] = word & 0xFF


def main():
    mem = bytearray(65536)
    pc = CODE_START

    # ------------------------------------------------------------------
    # Code — emit instructions, patch string address at the end.
    # ------------------------------------------------------------------
    #
    #   LD   R15,#$F000    ; set SP          210F F000
    #   LD   R2,#1         ; increment       2102 0001
    #   LD   R3,#msg       ; pointer         2103 ????  (patched)
    # loop:
    #   LDB  RL0,@R3       ; load byte       2038
    #   ORB  RL0,RL0       ; test null       8488
    #   JR   Z,done        ;                 E6??       (patched)
    #   OUTB #$FE00,RL0    ; send            3A86 FE00
    #   ADD  R3,R2         ; advance         8123
    #   JR   T,loop        ;                 E8??       (patched)
    # done:
    #   HALT               ;                 7A00
    # msg: "Hello, Z8000!\r\n\0"

    # LD R15,#$F000
    pc = emit(mem, pc, 0x21, 0x0F);    emit_word_be(mem, pc, INITIAL_SP); pc += 2
    # LD R2,#1
    pc = emit(mem, pc, 0x21, 0x02);    emit_word_be(mem, pc, 0x0001);    pc += 2
    # LD R3,#msg (patch address later)
    ld_msg_addr = pc
    pc = emit(mem, pc, 0x21, 0x03, 0x00, 0x00)

    loop_addr = pc
    # LDB RL0,@R3  (s=3, d=8)
    pc = emit(mem, pc, 0x20, 0x38)
    # ORB RL0,RL0  (s=8, d=8)
    pc = emit(mem, pc, 0x84, 0x88)
    # JR Z,done — patch disp later (cc=6=Z)
    jr_z_addr = pc
    pc = emit(mem, pc, 0xE6, 0x00)
    # OUTB #$FE00,RL0  (s=8, sub=6)  → 3A 86 FE 00
    pc = emit(mem, pc, 0x3A, 0x86);    emit_word_be(mem, pc, UART_DATA);  pc += 2
    # ADD R3,R2  (s=2, d=3)
    pc = emit(mem, pc, 0x81, 0x23)
    # JR T,loop  (cc=8, backward)
    jr_back_addr = pc
    disp_back = (loop_addr - (jr_back_addr + 2)) // 2
    pc = emit(mem, pc, 0xE8, disp_back & 0xFF)

    done_addr = pc
    # HALT
    pc = emit(mem, pc, 0x7A, 0x00)

    # Patch JR Z,done
    disp_fwd = (done_addr - (jr_z_addr + 2)) // 2
    mem[jr_z_addr + 1] = disp_fwd & 0xFF

    # ------------------------------------------------------------------
    # String (null-terminated)
    # ------------------------------------------------------------------
    msg_addr = pc
    hello = b"Hello, Z8000!\r\n\x00"
    for ch in hello:
        mem[pc] = ch
        pc += 1

    # Patch LD R3,#msg
    emit_word_be(mem, ld_msg_addr + 2, msg_addr)

    # ------------------------------------------------------------------
    # Program Status Area (exception vectors)
    # ------------------------------------------------------------------
    emit_word_be(mem, 0x0002, INITIAL_FCW)
    emit_word_be(mem, 0x0004, CODE_START)

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hello.bin")
    with open(out_path, "wb") as f:
        f.write(mem)

    print(f"Generated {out_path}")
    print(f"  Code:    ${CODE_START:04X}-${done_addr + 1:04X}"
          f" ({done_addr + 2 - CODE_START} bytes)")
    print(f"  String:  ${msg_addr:04X} \"Hello, Z8000!\\r\\n\"")
    print(f"  PSA FCW: $0002 -> ${INITIAL_FCW:04X} (System mode)")
    print(f"  PSA PC:  $0004 -> ${CODE_START:04X}")


if __name__ == "__main__":
    main()
