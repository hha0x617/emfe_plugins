# Hha Lisp for MC6809

A compact Lisp interpreter for the MC6809, running on the `em6809` +
`emfe_plugin_mc6809` environment (MC6850 ACIA at `$FF00/$FF01`, 64 KB RAM).

- **Single assembly source**: `lisp.asm` (~6,600 lines)
- **ROM image**: ~19 KB (code + initialised data)
- **Primitives**: 60, **stdlib entries**: 47
- **Tests**: 35 smoke tests, all passing

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
