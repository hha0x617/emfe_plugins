# Hha Forth for MC6809

A compact Forth interpreter for the MC6809, running on the `em6809` +
`emfe_plugin_mc6809` environment (MC6850 ACIA at `$FF00/$FF01`, 64 KB RAM).

- **Single assembly source**: `forth.asm` (~1,500 lines)
- **ROM image**: ~2.5 KB
- **CFAs**: 63 (primitives + colon definitions combined)
- **Tests**: 6 smoke tests, all passing

An ITC (indirect-threaded code) Forth with colon definitions, `IF`/`THEN`/
`BEGIN`/`UNTIL` control structures, `VARIABLE`/`CONSTANT`, `."` string
literals and `(` comments — a minimal-but-usable kernel in the figForth /
jonesforth tradition.

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
