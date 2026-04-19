# MC6809 Sample Programs

Each sample is a Python generator (`gen_*.py`) that emits a Motorola S-Record
file (`*.s19`) targeting the plugin's **MC6850 ACIA** at `$FF00/$FF01` with
the reset vector at `$FFFE`.

Load via the emfe frontend's **File → Load S-Record**, or from Rust via
`emfe_load_srec(inst, path)`.

## hello/

Prints `"Hello, MC6809!\r\n"` to the UART then halts with `BRA *`
(branch-to-self — idiomatic MC6809 halt that avoids the uninitialized SWI
vector).

Illustrates:
- MC6850 master reset (`CR = $03`)
- 8N1 configuration (`CR = $15`)
- TDRE polling before `STA TDR`
- Autoincremented string iteration (`LDA ,X+`)

## echo/

UART echo via polling. Prints a banner, echoes every typed character, and
expands CR into CR + LF + `"> "` prompt.

Illustrates:
- RDRF polling
- Conditional branch on received character
- `BSR` / `RTS` subroutine call (`print_str`)

## stack/

Recursive descent printer — a stress test for the call-stack debugger view
and for stack manipulation (`PSHS` / `PULS` / `BSR` / `RTS` / `LEAS`).

Produces the output:

```
E6
E5
E4
E3
E2
E1
X1
X2
X3
X4
X5
X6
```

Going in (`E6..E1`) the call stack grows to 6 nested `BSR` frames
(plus the outer one from `main`), so the **Call Stack** panel peaks at 7
entries at the innermost point. Breaking inside `recurse` and inspecting
the call-stack view is a good way to sanity-check `emfe_get_call_stack`.

Illustrates:
- Recursive subroutine via self-referential `BSR`
- Saving live registers on the S stack (`PSHS X` / `PULS X`)
- 5-bit signed indexed addressing (`LDX ,S`, `LEAX -1,X`)
- Register-to-register transfer for ASCII conversion (`TFR X,D`, `TFR B,A`)

## Regenerating

```bash
cd examples/hello && python gen_hello.py
cd examples/echo  && python gen_echo.py
cd examples/stack && python gen_stack.py
```

No external Python dependencies beyond CPython 3.8+.

## Verification

`tests/smoke.rs` contains `hello_srec_end_to_end`, which loads `hello.s19`
and checks the captured UART output. Run with:

```bash
set EMFE_MC6809_EXAMPLES_DIR=%CD%\examples
cargo test --release
```

## Compatibility note

These samples target the plugin's **MC6850 ACIA** emulation. They are
**not** binary-compatible with the sample programs shipped in the upstream
[em6809](../../../em6809) project — see `docs/mc6809_reference.md` §
"Upstream em6809 samples" for details.
