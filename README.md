# emfe_plugins

[![Build and Release](https://github.com/hha0x617/emfe_plugins/actions/workflows/build.yml/badge.svg)](https://github.com/hha0x617/emfe_plugins/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hha0x617/emfe_plugins?include_prereleases&sort=semver)](https://github.com/hha0x617/emfe_plugins/releases)
[![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue)](LICENSE-APACHE)

Guest-CPU plugins for the `emfe` emulator framework. Each subdirectory is a
self-contained plugin that exposes the `emfe` C ABI.

| Plugin | Target |
|--------|--------|
| `mc6809` | Motorola 6809 (wraps the `em6809` Rust crate) |
| `mc68030` | Motorola 68030 |
| `z8000` | Zilog Z8000 family (Z8001/Z8002/Z8003/Z8004) |
| `em8` | Small educational CPU |
| `rv32ima` | RISC-V RV32IMA |
| `api` | Shared C ABI headers |

Sample guest programs (e.g. Hha Forth / Hha Lisp for MC6809) live under each
plugin's `examples/` directory.

## Cloning

This repository vendors two upstream source trees as git submodules:

- `external/em6809` — [hha0x617/em6809](https://github.com/hha0x617/em6809)
  (required by the `mc6809` plugin)
- `external/em68030_WinUI3Cpp` — [hha0x617/Em68030_WinUI3Cpp](https://github.com/hha0x617/Em68030_WinUI3Cpp)
  (required by the `mc68030` plugin)

Clone recursively:

```bash
git clone --recurse-submodules https://github.com/hha0x617/emfe_plugins.git
```

Or, if you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

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
