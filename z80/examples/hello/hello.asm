; SPDX-License-Identifier: MIT OR Apache-2.0
; Copyright (c) 2026 hha0x617
;
; hello.asm — Phase 1.7 acceptance demo for the emfe Z80 plugin.
;
; Prints "Hello, World!\r\n" via the console UART (port 0xFE), then
; spins forever at `done`. Exercises every opcode family delivered in
; Phase 1 — LD r,r' (LD A,(HL)), LD HL,nn (immediate-word load),
; OR A (flag set from accumulator), JR Z / JR (conditional + relative
; control flow), OUT (n),A (immediate-port I/O), INC HL (16-bit pair
; increment). The assembled bytes are reproduced in
; tests/test_z80.cpp's TestHelloWorld and the test asserts the captured
; console output.
;
; Assembled by hand (Phase 1.7 has no z80asm toolchain dependency).
; If you re-assemble with a real Z80 assembler the bytes should match
; tests/test_z80.cpp's hello_world_bin[] exactly.

        org     0x0000          ; reset vector

start:  ld      hl, message     ; 21 0E 00   — HL := message addr
loop:   ld      a, (hl)         ; 7E         — fetch next byte
        or      a               ; B7         — Z flag := (A == 0)
        jr      z, done         ; 28 05      — skip to spin on terminator
        out     (0xFE), a       ; D3 FE      — transmit byte to UART
        inc     hl              ; 23         — advance pointer
        jr      loop            ; 18 F7      — back to loop (PC -9)

done:   jr      done            ; 18 FE      — self-loop forever

message: defb    'H','e','l','l','o',',', ' ','W','o','r','l','d','!'
        defb    0x0D, 0x0A      ; CR LF
        defb    0x00            ; terminator
;
; Total: 14 code bytes + 16 string bytes = 30 bytes.
