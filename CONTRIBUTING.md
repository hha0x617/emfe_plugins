# Contributing to emfe_plugins

Thanks for your interest! This document outlines the practical steps for
getting a local build working and for sending a change back upstream.

## Getting the source

This repository vendors **one** upstream source tree as a git submodule:

- `external/em68030_WinUI3Cpp` — required by the `mc68030` C++ plugin
  (the build pulls headers and Core/IO sources directly from this tree)

Always clone recursively:

```bash
git clone --recurse-submodules https://github.com/hha0x617/emfe_plugins.git
```

Or, after a plain clone:

```bash
git submodule update --init --recursive
```

The `mc6809` Rust plugin depends on
[em6809-core](https://github.com/hha0x617/em6809-core) via Cargo's
`git`-with-pinned-rev dependency, so `cargo build` fetches it
automatically — no submodule step is needed for that one.

## Build prerequisites

| Plugin | Toolchain |
|--------|-----------|
| `mc6809` | Rust stable (install via [rustup](https://rustup.rs)) |
| `em8`, `mc68030`, `z8000` | Visual Studio 2022+ (MSVC v143) + CMake |

## Building

Each plugin builds independently:

```bash
# mc6809 (Rust)
cd mc6809 && cargo build --release

# em8 / mc68030 / z8000 (CMake)
cd em8 && cmake -S . -B build && cmake --build build --config Release
cd mc68030 && cmake -S . -B build && cmake --build build --config Release
cd z8000 && cmake -S . -B build && cmake --build build --config Release
```

Output DLLs land under `mc6809/target/release/` and `<plugin>/build/bin/Release/`
respectively.  The CI runs these exact commands on `windows-latest`; see
[`.github/workflows/build.yml`](.github/workflows/build.yml).

## Making a change

1. Fork the repository and create a feature branch off `master`.
2. Keep commits small and focused; write commit messages that explain the
   *why*, not just the *what*.
3. If the change touches the C ABI (`api/emfe_plugin.h`), update the version
   number in the header and flag the break in your commit message.
4. Open a pull request against `master`.  CI must pass before merge.

## Commit style

- Subject line ≤ 72 chars, imperative mood (e.g. "fix mc6809 SBC borrow
  inversion"), optional `type(scope):` prefix (`feat(mc68030):`, `fix:`,
  `docs:`, `ci:`, `chore:`).
- Body wrapped to 72 chars, focused on motivation and trade-offs.

## Reporting bugs / requesting features

Use the issue templates in [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/).
Security vulnerabilities go through [`SECURITY.md`](SECURITY.md) instead.

## Code of Conduct

This project follows the [Contributor Covenant 2.1](CODE_OF_CONDUCT.md).
By participating you are expected to uphold those standards.  Reports
of unacceptable behaviour go to the contact address listed in the
Code of Conduct.

## License

By submitting a contribution you agree it will be licensed under the same
dual **MIT OR Apache-2.0** terms as the rest of the repository, without any
additional terms or conditions.
