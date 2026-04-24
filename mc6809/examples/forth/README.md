# Hha Forth for MC6809

A compact Forth interpreter for the MC6809, running on the `em6809` +
`emfe_plugin_mc6809` environment (MC6850 ACIA at `$FF00/$FF01`, 64 KB RAM).

- **Single assembly source**: `forth.asm` (~4,700 lines)
- **ROM image**: ~8 KB
- **CFAs**: 183 (primitives + colon definitions combined)
- **FORTH-83 Required Word Set coverage**: ~98%
- **Tests**: 7 smoke tests, all passing

An ITC (indirect-threaded code) Forth with colon definitions;
`IF`/`ELSE`/`THEN`, `BEGIN`/`UNTIL`/`AGAIN`/`WHILE`/`REPEAT` and
`DO`/`LOOP`/`+LOOP` control structures; `VARIABLE` / `CONSTANT` /
`CREATE` / `DOES>`; `FORGET` / `MARKER`; runtime-base switching
(`HEX` / `DECIMAL`); mixed-precision and double-word arithmetic
(`UM*`, `M*`, `UM/MOD`, `SM/REM`, `FM/MOD`, `*/`, `*/MOD`, `M+`,
`D+`, `D-`, `D.`, …); pictured numeric output (`<# # #S #> HOLD
SIGN`); string ops (`COMPARE`, `/STRING`, `-TRAILING`, `CMOVE`,
`MOVE`, `FILL`, `ERASE`, `BLANK`); `."` / `S"` / `ABORT"` string
literals; block (`(`) and line (`\`) comments — a minimal-but-usable
kernel in the figForth / jonesforth tradition.

```
Hha Forth for MC6809 ready.
3 4 + .           → 7  ok
: DOUBLE DUP + ;
5 DOUBLE DOUBLE . → 20  ok
```

## Documentation

| Doc | Contents |
|---|---|
| **[docs/USER_GUIDE.md](docs/USER_GUIDE.md)** | Build & run, REPL behaviour, all built-in words, examples |
| **[docs/LANGUAGE_AND_IMPL.md](docs/LANGUAGE_AND_IMPL.md)** | Language spec (ITC threading, dictionary layout, inner interpreter) and implementation notes, including code-size metrics |

## 日本語

- [README_ja.md](README_ja.md)
- [docs/USER_GUIDE_ja.md](docs/USER_GUIDE_ja.md)
- [docs/LANGUAGE_AND_IMPL_ja.md](docs/LANGUAGE_AND_IMPL_ja.md)

## License

MIT OR Apache-2.0 — see the SPDX header in `forth.asm`.
