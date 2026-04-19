# emfe_plugin_z8000

Zilog Z8000 family (Z8001 / Z8002 / Z8003 / Z8004) emulator plugin for emfe.

## Status

Phase 1 complete — Z8002 (non-segmented, no VM) mode fully functional. 20+
unit tests passing. Plugin DLL loadable by either the C++ WinUI3 or C# WPF
emfe frontend.

The other three family variants are selectable via `CpuVariant` in Settings
but currently fall back to Z8002 behaviour. Segmented addressing lands in
Phase 2 and VM/abort support in Phase 3.

## Build

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
./build/bin/Release/test_z8000.exe
```

Output: `build/bin/Release/emfe_plugin_z8000.dll`

## Documentation

- English ISA & plugin reference: [docs/z8000_reference.md](docs/z8000_reference.md)
- Japanese: [docs/z8000_reference_ja.md](docs/z8000_reference_ja.md)

## License

SPDX-License-Identifier: MIT
