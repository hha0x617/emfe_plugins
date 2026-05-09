# emfe_plugins

[![Build and Release](https://github.com/hha0x617/emfe_plugins/actions/workflows/build.yml/badge.svg)](https://github.com/hha0x617/emfe_plugins/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hha0x617/emfe_plugins?include_prereleases&sort=semver)](https://github.com/hha0x617/emfe_plugins/releases)
[![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue)](LICENSE-APACHE)

[日本語 (README_ja.md)](README_ja.md)

Guest-CPU plugins for the `emfe` emulator framework. Each subdirectory is a
self-contained plugin that exposes the `emfe` C ABI.

*Developed through vibe coding with
[Claude Code](https://docs.anthropic.com/en/docs/claude-code).*

| Plugin | Target | Status |
|--------|--------|--------|
| `mc6809` | Motorola 6809 (wraps the `em6809-core` Rust crate) | **Shipped** — DLL built and packaged in releases |
| `mc68030` | Motorola 68030 | **Shipped** — DLL built and packaged in releases |
| `z8000` | Zilog Z8000 family (Z8001/Z8002/Z8003/Z8004) | **Partial — Z8002 only** — DLL built and packaged in releases.  Z8002 (non-segmented, no VM) is fully functional.  Z8001 / Z8003 / Z8004 are selectable in Settings but currently fall back to Z8002 behaviour; segmented addressing lands in Phase 2 and VM/abort support in Phase 3. |
| `em8` | Small educational CPU | **Shipped** — ABI-validation target; DLL built and packaged in releases |
| `rv32ima` | RISC-V RV32IMA | **Design notes only** — `docs/` exists, no source yet |
| `api` | Shared C ABI headers (`emfe_plugin.h`) | **Header-only** — not a plugin DLL; consumed by all real plugins |

Plugins listed as *Shipped* are built by CI on every push and bundled
in the GitHub Release zip / installer for tagged commits.  The *z8000*
DLL is also packaged on every release, but with the Z8002-only
caveat above — selecting Z8001 / Z8003 / Z8004 in Settings does not
yet exercise their distinguishing behaviour.  *Design notes only*
and *Header-only* rows are documented for completeness so the
directory layout doesn't surprise anyone — the GitHub Release will
not contain a DLL for them.

Sample guest programs (e.g. Hha Forth / Hha Lisp for MC6809) live under each
plugin's `examples/` directory.

## Cloning

This repository vendors **one** upstream source tree as a git submodule:

- `external/em68030_WinUI3Cpp` — [hha0x617/Em68030_WinUI3Cpp](https://github.com/hha0x617/Em68030_WinUI3Cpp)
  (required by the `mc68030` C++ plugin — the build pulls headers and
  Core/IO sources directly from this tree)

Clone recursively:

```bash
git clone --recurse-submodules https://github.com/hha0x617/emfe_plugins.git
```

Or, if you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

The `mc6809` Rust plugin depends on
[em6809-core](https://github.com/hha0x617/em6809-core) via Cargo's
`git`-with-pinned-rev dependency, so `cargo build` fetches it
automatically — no submodule step is needed for that one.

## Building

| Plugin | Toolchain | Command |
|--------|-----------|---------|
| `mc6809` | Rust stable | `cd mc6809 && cargo build --release` |
| `em8` | MSVC + CMake | `cd em8 && cmake -S . -B build && cmake --build build --config Release` |
| `mc68030` | MSVC + CMake | `cd mc68030 && cmake -S . -B build && cmake --build build --config Release` |
| `z8000` | MSVC + CMake | `cd z8000 && cmake -S . -B build && cmake --build build --config Release` |

GitHub Actions runs the same steps on every push; tagged commits (`v*`)
publish the built DLLs as a GitHub Release. See
[`.github/workflows/build.yml`](.github/workflows/build.yml).

## Contributing and Policies

- Contribution workflow: [`CONTRIBUTING.md`](CONTRIBUTING.md)
- Code of Conduct: [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) (Contributor Covenant 2.1)
- Security: [`SECURITY.md`](SECURITY.md)

## License

Licensed under either of

 * Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or
   <http://www.apache.org/licenses/LICENSE-2.0>)
 * MIT license ([LICENSE-MIT](LICENSE-MIT) or
   <http://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally
submitted for inclusion in the work by you, as defined in the Apache-2.0
license, shall be dual licensed as above, without any additional terms or
conditions.
