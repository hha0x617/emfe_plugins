# Hha Lisp for MC6809

## Origin

Hha Lisp — together with its sibling [Hha Forth](../forth/) — was
originally written as a deliberate stress test for the **`mc6809`
plugin's ISA implementation**.  We needed a program large enough to
exercise a broad range of instructions and addressing modes,
looking for bugs that the unit-test grid was likely to miss.  It
worked: writing these two languages surfaced a number of real bugs
in the underlying CPU emulator and contributed materially to
maturing the ISA.

The sample is kept here in the `mc6809` plugin's `examples/`
directory so future ISA-touching changes can keep using it as the
end-to-end regression target it has become.

## Overview

A compact Lisp interpreter for the MC6809, running on the `em6809` +
`emfe_plugin_mc6809` environment (MC6850 ACIA at `$FF00/$FF01`, 64 KB RAM).

- **Single assembly source**: `lisp.asm` (~6,600 lines)
- **ROM image**: ~19 KB (code + initialised data)
- **Primitives**: 62, **stdlib entries**: 51
- **Tests**: 38 smoke tests, all passing

Classic Lisp surface syntax (`defun` / `T` / `NIL` / `'x`), `defmacro`
+ quasiquote (manual hygiene via `with-gensyms`), mark-sweep GC,
tail-call optimisation, strings, characters, vectors, and automatic
15-bit fixnum ↔ 32-bit box promotion — enough to run serious tiny
programs.

**Lineage at a glance**: Common Lisp surface syntax (`defun` / `setq`
/ `t` / `nil`) + **Lisp-1 evaluation** (Scheme / Arc / Clojure
tradition) + Scheme-style utility names (`string->symbol` /
`vector-set!`).  Closest single named relative is uLisp, but Lisp-1.
Cross-tradition aliases (`null?` / `atom?` / `eq?` / `zero?` for the
predicates, `set!` for `setq`) are **additive**, never replacements,
so SICP / CL / Emacs Lisp users can read the same code without
friction.  See [docs/LANGUAGE_AND_IMPL.md §0](docs/LANGUAGE_AND_IMPL.md)
for the full lineage map and design principles.

```
> (defun fact (n) (if (< n 2) 1 (* n (fact (- n 1)))))
FACT
> (fact 10)
3628800
> (format "2+3=~D, hello ~A!" 5 'world)
2+3=5, hello WORLD!
NIL
```

## Documentation

| Doc | Contents |
|---|---|
| **[docs/USER_GUIDE.md](docs/USER_GUIDE.md)** | Build & run, REPL, all primitives + stdlib, examples |
| **[docs/LANGUAGE_AND_IMPL.md](docs/LANGUAGE_AND_IMPL.md)** | Language spec (value tags, special forms, memory model) and implementation notes (GC, TCO, stdlib bootstrap), including code-size metrics |

## 日本語

- [README_ja.md](README_ja.md)
- [docs/USER_GUIDE_ja.md](docs/USER_GUIDE_ja.md)
- [docs/LANGUAGE_AND_IMPL_ja.md](docs/LANGUAGE_AND_IMPL_ja.md)

## License

MIT OR Apache-2.0 (see the SPDX header in `lisp.asm`).
