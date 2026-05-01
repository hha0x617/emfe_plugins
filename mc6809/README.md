# emfe_plugin_mc6809

Motorola MC6809 emfe plugin DLL — wraps the [em6809](../../em6809) CPU core
and exposes it through the standard emfe plugin C ABI.

## Status

Phase 1 — CPU core + 64 KB flat memory + memory-mapped UART (default at
`$FF00`). 9 Rust integration tests passing. Loadable by either emfe frontend.

## Build

```bash
cd emfe_plugins/mc6809
cargo build --release
```

Output: `target/release/emfe_plugin_mc6809.dll`

## Test

```bash
cargo test --release
```

## Dependencies

- [em6809](../../em6809) with `default-features = false` (CPU core only — no
  eframe, egui, serde, i18n).
- Statically links the MSVC C runtime (`.cargo/config.toml` sets
  `target-feature=+crt-static`).

## Runtime dependencies of the built DLL

Only Windows OS libraries (`KERNEL32.dll`, `api-ms-win-core-synch-l1-2-0.dll`,
`ntdll.dll`). No VCRedist, no .NET runtime.

## Samples

See [examples/README.md](examples/README.md). The plugin ships two ready-to-
load S-records (`hello.s19`, `echo.s19`) that exercise the MC6850 ACIA.

The [upstream em6809 project's samples](https://github.com/hha0x617/em6809/tree/main/samples)
also work directly: as of em6809 PR #25 the upstream console device shares
the same MC6850 ACIA register layout — see `docs/mc6809_reference.md`
§ "Upstream em6809 samples" for the historical Simple-layout note.

## Documentation

- English: [docs/mc6809_reference.md](docs/mc6809_reference.md)
- Japanese: [docs/mc6809_reference_ja.md](docs/mc6809_reference_ja.md)

## License

SPDX-License-Identifier: MIT OR Apache-2.0 (inherits from em6809).
