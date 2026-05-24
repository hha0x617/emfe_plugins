# eForth-Z80 — interactive Forth REPL over UART

[Bill Muench's eForth](https://www.forth.org/eforth.html) is a small,
portable Forth in the spirit of `figForth`. The Z80 port (sometimes
labelled `eForth-Z80` or `eForth-83-Z80`) fits comfortably in 8 KB of
ROM + 8 KB of RAM and runs as a bare-metal Z80 program — no operating
system, no CP/M, just a cold start at `PC := 0x0000` driving the
plugin's UART.

It is the canonical "is the plugin good enough to host an interactive
language?" demo for Z80 family emulators.

## License

eForth was placed in the **public domain** by Bill Muench in the early
1990s. Multiple ports exist; the well-known Z80 versions are mirrored
at:

- https://www.forth.org/eforth.html
- https://www.forth.org/svfig/Win32Forth/eForth.html (assembly source archive)
- https://github.com/jhlagado/z80-eforth (modernised assembly + makefile)

## How the plugin runs it

`tests/test_z80.cpp::TestEForthIfPresent` implements the harness:

1. Loads `eforth.bin` at memory address `0x0000` (cold-vector
   convention for bare-metal Z80 ROMs).
2. Sets `PC := 0x0000` and `SP := 0xFF00` (away from any default
   eForth RAM region).
3. Registers a console-char callback so every byte the guest writes
   to UART port `0xFE` is captured into a `std::string`.
4. Steps the CPU for up to a few million instructions to let cold
   start finish (the banner usually contains the word `eForth` or
   `OK` once it lands at the interpreter prompt).
5. Sends a tiny REPL transaction with `emfe_send_string` — e.g.
   `"1 2 + . CR\n"` — then steps another batch of instructions and
   asserts that the captured output contains the expected `3`.

The harness uses the **UART** path (ports `0xFE` / `0xFF`, documented
in `docs/mmio.md`), not a CP/M BDOS stub like the `zexdoc` harness.
eForth is bare-metal, so there is no operating system to intercept.

## To run it

1. Obtain an `eforth.bin` image (about 8 KB) from any of the mirrors
   above. Two practical paths:
   - Build from the assembly source with `sjasmplus` /
     `pasmo` / `z80asm` (the harness only needs a flat binary,
     not Intel HEX).
   - Find a pre-assembled `.bin` in one of the GitHub repos above.
2. Drop the file into this directory as
   `z80/examples/eforth/eforth.bin`.
3. Re-run `test_z80.exe`. `TestEForthIfPresent` detects the file
   and runs the REPL transaction. When `eforth.bin` is absent
   the test prints `(SKIP — eforth.bin not found ...)` and counts
   as a pass.

## Adjusting for non-default builds

If your eForth image:

- expects to be loaded at an address other than `0x0000`, or
- uses a different UART convention (for example a non-blocking poll
  loop that drains the RX FIFO before ever printing a banner),

edit the load address / `SP` initial value / cold-boot step budget at
the top of `TestEForthIfPresent` in `tests/test_z80.cpp`. The harness
is written to be a stable template — most users only have to change
the load address.
