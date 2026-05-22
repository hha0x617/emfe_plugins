# hello — emfe Z80 Phase 1 acceptance demo

A 30-byte Z80 ROM that prints `Hello, World!\r\n` via the console UART
(port `0xFE`, see [`../../docs/mmio.md`](../../docs/mmio.md)), then spins
forever at `done`.

This is the Phase 1.7 acceptance demonstration: the plugin's first
end-to-end run where a real (hand-written) program drives every opcode
family delivered through Phase 1.6 and the new INC HL / OUT (n),A
opcodes added in Phase 1.7. The companion test
[`../../tests/test_z80.cpp`](../../tests/test_z80.cpp)'s
`TestHelloWorld` boots the ROM under the plugin, runs until the
self-loop is reached, and asserts the captured console output equals
`"Hello, World!\r\n"`.

## What it exercises

| Opcode used | Source line | Phase |
|---|---|---|
| `LD HL, nn` | `ld hl, message` | 1.4 |
| `LD A, (HL)` | `ld a, (hl)` | 1.4 |
| `OR A`      | `or a` (Z flag from A) | 1.5 |
| `JR Z, e`   | `jr z, done` | 1.6 |
| `OUT (n), A`| `out (0xFE), a` | 1.7 |
| `INC HL`    | `inc hl` | 1.7 |
| `JR e`      | `jr loop` / `jr done` | 1.6 |

## Source

[`hello.asm`](hello.asm) is the canonical source. Phase 1.7 does not
introduce a z80asm toolchain dependency, so the corresponding 30 bytes
are reproduced inline in `tests/test_z80.cpp::hello_world_bin[]` with
per-line comments that match the asm.

If you re-assemble `hello.asm` with a real Z80 assembler (sdcc-z80,
z80asm, pasmo, etc.), the output bytes must match `hello_world_bin[]`
exactly; otherwise either the source or the embedded test data drifted
and needs realigning.

## Running it under the plugin

The test harness loads the bytes at `0x0000`, calls `emfe_reset`,
registers a console char callback that appends each transmitted byte
to a captured string, then single-steps the plugin until `PC` enters
the self-loop. Concretely:

```cpp
emfe_send_string(...) — not used
emfe_set_console_char_callback(inst, CaptureChar, &captured);
for (int i = 0; i < 5000; ++i) {
    emfe_step(inst);
    if (PC == done_addr && capturedEndsWithLF) break;
}
assert(captured == "Hello, World!\r\n");
```

Real CPM-class workloads will use much larger step budgets; this demo
finishes in well under 200 instructions.
