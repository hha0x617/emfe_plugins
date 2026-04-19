#!/usr/bin/env python3
"""
Generate hello.s19 — "Hello, MC6809!" via an MC6850 ACIA at $FF00.

Output target:
  - Motorola S-Record (S1 records + S9 start record)
  - Entry point $0100
  - Reset vector at $FFFE/$FFFF set to $0100

Program outline:
  reset:
      LDA  #$03          ; ACIA master reset
      STA  ACIA_CR       ; $FF00
      LDA  #$15          ; 8N1, divide-16, no IRQ
      STA  ACIA_CR
      LDX  #msg
  loop:
      LDA  ,X+
      BEQ  done
  wait:
      LDB  ACIA_SR       ; poll TDRE (bit 1)
      BITB #$02
      BEQ  wait
      STA  ACIA_TDR      ; transmit
      BRA  loop
  done:
      BRA  done          ; idiomatic MC6809 halt — branch to self
  msg:
      .asciz "Hello, MC6809!\r\n"
"""

import os

ACIA_CR  = 0xFF00  # write = CR
ACIA_SR  = 0xFF00  # read  = SR
ACIA_TDR = 0xFF01  # write = TDR
CODE_START = 0x0100
RESET_VEC = 0xFFFE


def emit(mem, addr, *bytes_):
    for i, b in enumerate(bytes_):
        mem[addr + i] = b & 0xFF
    return addr + len(bytes_)


def emit_be(mem, addr, word):
    mem[addr]     = (word >> 8) & 0xFF
    mem[addr + 1] = word & 0xFF


def make_srec(mem, start, length, entry):
    """Emit a Motorola S19 file covering mem[start:start+length]."""
    lines = []
    lines.append("S00600006861690F")  # header ("hai")
    i = 0
    while i < length:
        chunk = min(24, length - i)
        addr = start + i
        count = chunk + 3  # addr(2) + data(chunk) + checksum(1)
        row = [count, (addr >> 8) & 0xFF, addr & 0xFF] + list(mem[addr:addr + chunk])
        cksum = (~(sum(row) & 0xFF)) & 0xFF
        hexs = "".join(f"{b:02X}" for b in row) + f"{cksum:02X}"
        lines.append(f"S1{hexs}")
        i += chunk
    # S9 start record
    row = [3, (entry >> 8) & 0xFF, entry & 0xFF]
    cksum = (~(sum(row) & 0xFF)) & 0xFF
    hexs = "".join(f"{b:02X}" for b in row) + f"{cksum:02X}"
    lines.append(f"S9{hexs}")
    return "\n".join(lines) + "\n"


def main():
    mem = bytearray(0x10000)
    pc = CODE_START

    # reset:
    pc = emit(mem, pc, 0x86, 0x03)                     # LDA #$03
    pc = emit(mem, pc, 0xB7, 0xFF, 0x00)               # STA $FF00 (CR, master reset)
    pc = emit(mem, pc, 0x86, 0x15)                     # LDA #$15
    pc = emit(mem, pc, 0xB7, 0xFF, 0x00)               # STA $FF00 (CR, 8N1/÷16)
    pc = emit(mem, pc, 0x8E, 0x00, 0x00)               # LDX #msg  (patch addr later)
    ldx_addr = pc - 2

    loop_addr = pc
    pc = emit(mem, pc, 0xA6, 0x80)                     # LDA ,X+
    # BEQ done  — 8-bit relative, patched later
    pc = emit(mem, pc, 0x27, 0x00)
    beq_done = pc - 1
    wait_addr = pc
    pc = emit(mem, pc, 0xF6, 0xFF, 0x00)               # LDB $FF00 (SR)
    pc = emit(mem, pc, 0xC5, 0x02)                     # BITB #$02
    pc = emit(mem, pc, 0x27, (wait_addr - (pc + 2)) & 0xFF)  # BEQ wait
    pc = emit(mem, pc, 0xB7, 0xFF, 0x01)               # STA $FF01 (TDR)
    # BRA loop
    pc = emit(mem, pc, 0x20, (loop_addr - (pc + 2)) & 0xFF)

    done_addr = pc
    # Patch BEQ done offset
    mem[beq_done] = (done_addr - (beq_done + 1)) & 0xFF
    # BRA to self ($FE = -2) — idiomatic MC6809 halt. Avoids SWI, which would
    # otherwise trap through the uninitialized SWI vector at $FFFA/$FFFB.
    pc = emit(mem, pc, 0x20, 0xFE)

    # msg:
    msg_addr = pc
    text = b"Hello, MC6809!\r\n\x00"
    for b in text:
        mem[pc] = b
        pc += 1

    # Patch LDX #msg
    emit_be(mem, ldx_addr, msg_addr)

    # Reset vector
    emit_be(mem, RESET_VEC, CODE_START)

    # Emit S19 covering code + msg + reset vector
    # We write two S1 segments: [CODE_START..pc) and [RESET_VEC..RESET_VEC+2)
    part1 = make_srec(mem, CODE_START, pc - CODE_START, CODE_START)
    # Strip the S0/S9 from the helper and build a combined file manually.
    lines = []
    lines.append("S0100000656D66655F6D633638303960")  # header "emfe_mc6809"
    # Part 1: code + string
    i = 0
    while i < pc - CODE_START:
        chunk = min(24, pc - CODE_START - i)
        addr = CODE_START + i
        row = [chunk + 3, (addr >> 8) & 0xFF, addr & 0xFF] + list(mem[addr:addr + chunk])
        cksum = (~(sum(row) & 0xFF)) & 0xFF
        lines.append("S1" + "".join(f"{b:02X}" for b in row) + f"{cksum:02X}")
        i += chunk
    # Part 2: reset vector
    row = [2 + 3, (RESET_VEC >> 8) & 0xFF, RESET_VEC & 0xFF,
           mem[RESET_VEC], mem[RESET_VEC + 1]]
    cksum = (~(sum(row) & 0xFF)) & 0xFF
    lines.append("S1" + "".join(f"{b:02X}" for b in row) + f"{cksum:02X}")
    # S9 start
    row = [3, (CODE_START >> 8) & 0xFF, CODE_START & 0xFF]
    cksum = (~(sum(row) & 0xFF)) & 0xFF
    lines.append("S9" + "".join(f"{b:02X}" for b in row) + f"{cksum:02X}")
    srec = "\n".join(lines) + "\n"
    # Discard unused helper value.
    _ = part1

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hello.s19")
    with open(out_path, "w") as f:
        f.write(srec)

    print(f"Generated {out_path}")
    print(f"  Code:   ${CODE_START:04X}-${pc - 1:04X} ({pc - CODE_START} bytes)")
    print(f"  String: ${msg_addr:04X} \"Hello, MC6809!\\r\\n\"")
    print(f"  Reset vector @ ${RESET_VEC:04X} -> ${CODE_START:04X}")


if __name__ == "__main__":
    main()
