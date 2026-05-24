# zexdoc — Z80 documented-opcode exerciser

[Frank Cringle's `zexdoc`](https://github.com/anotherlin/z80emu/blob/master/testfiles/zexdoc.com) is the de-facto correctness test for Z80 emulators. It runs as a
CP/M `.COM` program: a self-contained binary that exercises every
documented Z80 opcode, hashes the resulting flag state, and prints
`OK` (or `ERROR`) for each subtest via `BDOS` calls.

The companion `zexall` (Phase 3) extends the coverage to the well-known
undocumented opcodes and the Y/X flag bits.

## License

`zexdoc` source and binary are **public domain** as released by Frank
Cringle in 1994. Multiple mirrors exist:
- https://mdfs.net/Software/Z80/Exerciser/
- https://github.com/anotherlin/z80emu/tree/master/testfiles
- https://github.com/raxoft/z80/tree/master/zex

## How the plugin runs it

The plugin's test harness (`tests/test_z80.cpp::TestZexdocIfPresent`)
implements a minimal CP/M-stub:

1. Loads the `.COM` image at memory address `0x0100` (the CP/M TPA
   convention).
2. Sets `PC := 0x0100`, `SP := 0xFFF0`.
3. Stops if `PC` reaches `0x0000` (CP/M warm boot — program exit).
4. When `PC == 0x0005` (CP/M `BDOS` call entry), services the BDOS
   request:
   - `C = 2` (`putchar`): captures the byte in `E`.
   - `C = 9` (`print $-terminated string`): captures the string at
     `(DE)` until a `$` byte.
   - `C = 0`: warm-boot equivalent — stop the harness.
   - Any other `C`: ignored (no other BDOS calls are used by zexdoc).
   - Pops the return address off the stack manually and resumes.

zexdoc takes roughly 70 million guest instructions to complete every
subtest (a few seconds on this plugin's projected throughput).
Successful runs end the captured output with `Tests complete`.

## To run it

1. Download `zexdoc.com` from any of the mirrors above (about 8 KB).
2. Drop the file into this directory as
   `z80/examples/zexdoc/zexdoc.com`.
3. Re-run `test_z80.exe`. The `TestZexdocIfPresent` test detects the
   file and runs the full suite. When `zexdoc.com` is absent the
   test prints `(SKIP — zexdoc.com not found at ...)` and counts
   as a pass.

The harness wiring is verified independently in
`TestCpmStubHarness`, which runs a tiny self-contained CP/M program
that prints `OK\r\n` via BDOS function 9. That test is part of the
default CI run; only the full zexdoc execution is gated on having
the binary locally.
