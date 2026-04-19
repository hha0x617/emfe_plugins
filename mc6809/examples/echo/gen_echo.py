#!/usr/bin/env python3
"""
Generate echo.s19 — polling-based UART echo on MC6850 ACIA at $FF00.

After init, the program prints a banner and enters a tight loop:
  - Poll SR RDRF (bit 0). When an RX byte arrives, read it from RDR.
  - Poll SR TDRE (bit 1). When TX is ready, write it back to TDR.
  - On CR ($0D), also emit LF ($0A) and "> " prompt string.

Entry point $0100, reset vector at $FFFE set to $0100.
"""

import os

ACIA_CR_SR = 0xFF00
ACIA_DATA  = 0xFF01
CODE_START = 0x0100
RESET_VEC  = 0xFFFE


def emit(mem, addr, *data):
    for i, b in enumerate(data):
        mem[addr + i] = b & 0xFF
    return addr + len(data)


def emit_be(mem, addr, word):
    mem[addr]     = (word >> 8) & 0xFF
    mem[addr + 1] = word & 0xFF


def rel8(from_pc_after_branch, target):
    off = target - from_pc_after_branch
    assert -128 <= off <= 127, f"branch out of range: {off}"
    return off & 0xFF


def to_srec(mem, segments, entry):
    """Emit a Motorola S19 covering each (start, length) segment."""
    lines = ["S0100000656D66655F6D633638303960"]  # header
    for start, length in segments:
        i = 0
        while i < length:
            chunk = min(24, length - i)
            addr = start + i
            row = [chunk + 3, (addr >> 8) & 0xFF, addr & 0xFF] + list(mem[addr:addr + chunk])
            cksum = (~(sum(row) & 0xFF)) & 0xFF
            lines.append("S1" + "".join(f"{b:02X}" for b in row) + f"{cksum:02X}")
            i += chunk
    row = [3, (entry >> 8) & 0xFF, entry & 0xFF]
    cksum = (~(sum(row) & 0xFF)) & 0xFF
    lines.append("S9" + "".join(f"{b:02X}" for b in row) + f"{cksum:02X}")
    return "\n".join(lines) + "\n"


def main():
    mem = bytearray(0x10000)
    pc = CODE_START

    # -------------------- main program --------------------
    # Init ACIA
    pc = emit(mem, pc, 0x86, 0x03)                  # LDA #$03
    pc = emit(mem, pc, 0xB7, 0xFF, 0x00)            # STA $FF00 (CR, master reset)
    pc = emit(mem, pc, 0x86, 0x15)                  # LDA #$15
    pc = emit(mem, pc, 0xB7, 0xFF, 0x00)            # STA $FF00 (CR, 8N1/÷16)

    # Print banner — use the same tx-poll subroutine.
    pc = emit(mem, pc, 0x8E, 0x00, 0x00)            # LDX #banner (patch)
    ldx_banner = pc - 2
    # BSR print_str
    pc = emit(mem, pc, 0x8D, 0x00)                  # placeholder
    bsr_banner = pc - 1

    idle_loop = pc
    # wait RX
    pc = emit(mem, pc, 0xF6, 0xFF, 0x00)            # LDB $FF00 (SR)
    pc = emit(mem, pc, 0xC5, 0x01)                  # BITB #$01 (RDRF)
    pc = emit(mem, pc, 0x27, rel8(pc + 2, idle_loop))  # BEQ idle_loop
    # Read RX, save to A
    pc = emit(mem, pc, 0xB6, 0xFF, 0x01)            # LDA $FF01 (RDR)
    # poll TX empty and echo
    wait_tx_1 = pc
    pc = emit(mem, pc, 0xF6, 0xFF, 0x00)            # LDB $FF00 (SR)
    pc = emit(mem, pc, 0xC5, 0x02)                  # BITB #$02
    pc = emit(mem, pc, 0x27, rel8(pc + 2, wait_tx_1))  # BEQ wait_tx_1
    pc = emit(mem, pc, 0xB7, 0xFF, 0x01)            # STA $FF01 (TDR)
    # If A != CR, go back to idle_loop
    pc = emit(mem, pc, 0x81, 0x0D)                  # CMPA #$0D
    # BNE idle_loop
    pc = emit(mem, pc, 0x26, rel8(pc + 2, idle_loop))
    # --- CR path: also send LF and "> " ---
    # Send LF
    wait_tx_2 = pc
    pc = emit(mem, pc, 0xF6, 0xFF, 0x00)
    pc = emit(mem, pc, 0xC5, 0x02)
    pc = emit(mem, pc, 0x27, rel8(pc + 2, wait_tx_2))
    pc = emit(mem, pc, 0x86, 0x0A)                  # LDA #$0A
    pc = emit(mem, pc, 0xB7, 0xFF, 0x01)            # STA $FF01
    # Send "> "
    pc = emit(mem, pc, 0x8E, 0x00, 0x00)            # LDX #prompt2 (patch)
    ldx_prompt2 = pc - 2
    pc = emit(mem, pc, 0x8D, 0x00)                  # BSR print_str (patch)
    bsr_prompt2 = pc - 1
    # jump back to idle_loop
    pc = emit(mem, pc, 0x20, rel8(pc + 2, idle_loop))  # BRA idle_loop

    # -------------------- print_str subroutine --------------------
    # X -> zero-terminated string. Uses A, B.
    print_str = pc
    # loop:
    ps_loop = pc
    pc = emit(mem, pc, 0xA6, 0x80)                  # LDA ,X+
    pc = emit(mem, pc, 0x27, 0x00)                  # BEQ ps_done (patch)
    beq_ps_done = pc - 1
    # wait TX
    ps_wait = pc
    pc = emit(mem, pc, 0xF6, 0xFF, 0x00)            # LDB $FF00 (SR)
    pc = emit(mem, pc, 0xC5, 0x02)                  # BITB #$02
    pc = emit(mem, pc, 0x27, rel8(pc + 2, ps_wait)) # BEQ ps_wait
    pc = emit(mem, pc, 0xB7, 0xFF, 0x01)            # STA $FF01 (TDR)
    pc = emit(mem, pc, 0x20, rel8(pc + 2, ps_loop)) # BRA ps_loop
    # ps_done:
    ps_done = pc
    mem[beq_ps_done] = rel8(beq_ps_done + 1, ps_done)
    pc = emit(mem, pc, 0x39)                        # RTS

    # Patch BSRs now that print_str is known.
    mem[bsr_banner]  = rel8(bsr_banner  + 1, print_str)
    mem[bsr_prompt2] = rel8(bsr_prompt2 + 1, print_str)

    # -------------------- strings --------------------
    banner_addr = pc
    for b in b"MC6809 Echo. Type characters; Enter for a new line.\r\n> \x00":
        mem[pc] = b
        pc += 1

    prompt2_addr = pc
    for b in b"> \x00":
        mem[pc] = b
        pc += 1

    # Patch LDX #... immediates
    emit_be(mem, ldx_banner,  banner_addr)
    emit_be(mem, ldx_prompt2, prompt2_addr)

    # Reset vector
    emit_be(mem, RESET_VEC, CODE_START)

    # Emit S19: code + strings in one span, plus the 2-byte reset vector
    srec = to_srec(
        mem,
        [(CODE_START, pc - CODE_START), (RESET_VEC, 2)],
        CODE_START,
    )

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "echo.s19")
    with open(out_path, "w") as f:
        f.write(srec)

    print(f"Generated {out_path}")
    print(f"  idle_loop: ${idle_loop:04X}")
    print(f"  print_str: ${print_str:04X}")
    print(f"  banner:    ${banner_addr:04X}")
    print(f"  prompt2:   ${prompt2_addr:04X}")
    print(f"  code end:  ${pc - 1:04X}  ({pc - CODE_START} bytes)")


if __name__ == "__main__":
    main()
