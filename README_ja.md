# emfe_plugins

[![Build and Release](https://github.com/hha0x617/emfe_plugins/actions/workflows/build.yml/badge.svg)](https://github.com/hha0x617/emfe_plugins/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hha0x617/emfe_plugins?include_prereleases&sort=semver)](https://github.com/hha0x617/emfe_plugins/releases)
[![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue)](LICENSE-APACHE)

[English documentation (README.md)](README.md)

`emfe` エミュレータフレームワーク用のゲスト CPU プラグイン集です。各サブディレクトリはそれぞれ独立したプラグインで、`emfe` の C ABI を介してホストから呼び出されます。

*[Claude Code](https://docs.anthropic.com/en/docs/claude-code) との vibe coding で開発しています。*

| プラグイン | ターゲット |
|-----------|----------|
| `mc6809` | Motorola 6809 (`em6809` Rust クレートを薄くラップ) |
| `mc68030` | Motorola 68030 |
| `z8000` | Zilog Z8000 ファミリ (Z8001/Z8002/Z8003/Z8004) |
| `em8` | 小規模な教育用 CPU |
| `rv32ima` | RISC-V RV32IMA |
| `api` | 共通 C ABI ヘッダ |

各プラグインの `examples/` ディレクトリ配下にサンプルゲストプログラムを収録しています (例: MC6809 向けの Hha Forth / Hha Lisp)。

## クローン

本リポジトリは **1 つ** のアップストリームソースツリーを git submodule として取り込んでいます:

- `external/em68030_WinUI3Cpp` — [hha0x617/Em68030_WinUI3Cpp](https://github.com/hha0x617/Em68030_WinUI3Cpp)
  (`mc68030` C++ プラグインに必要 — ビルドはこのツリーから直接ヘッダと Core/IO ソースを取り込みます)

再帰クローン:

```bash
git clone --recurse-submodules https://github.com/hha0x617/emfe_plugins.git
```

または `--recurse-submodules` なしでクローン済みの場合:

```bash
git submodule update --init --recursive
```

`mc6809` Rust プラグインは [em6809-core](https://github.com/hha0x617/em6809-core)
を Cargo の `git`-with-pinned-rev 依存として参照するため、`cargo build` 時に
自動取得されます — submodule 設定は不要です。

## ビルド

| プラグイン | ツールチェーン | コマンド |
|-----------|---------------|---------|
| `mc6809` | Rust stable | `cd mc6809 && cargo build --release` |
| `em8` | MSVC + CMake | `cd em8 && cmake -S . -B build && cmake --build build --config Release` |
| `mc68030` | MSVC + CMake | `cd mc68030 && cmake -S . -B build && cmake --build build --config Release` |
| `z8000` | MSVC + CMake | `cd z8000 && cmake -S . -B build && cmake --build build --config Release` |

GitHub Actions が同じ手順で毎プッシュごとにビルドし、タグ付きコミット (`v*`) では
ビルド済 DLL を GitHub Release として公開します。詳細は
[`.github/workflows/build.yml`](.github/workflows/build.yml) を参照。

## 貢献とポリシー

- 貢献ワークフロー: [`CONTRIBUTING.md`](CONTRIBUTING.md)
- 行動規範: [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md)（Contributor Covenant 2.1 準拠）
- セキュリティ: [`SECURITY.md`](SECURITY.md)

## ライセンス

以下のいずれかのライセンスで提供されます:

 * Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) または
   <http://www.apache.org/licenses/LICENSE-2.0>)
 * MIT license ([LICENSE-MIT](LICENSE-MIT) または
   <http://opensource.org/licenses/MIT>)

利用者がいずれかを選択できます。

### コントリビューション

Apache-2.0 ライセンスで定義されるとおり、明示的に別段の意思表示をしない限り、
本作品への取り込みを目的として意図的に提出されたあらゆる貢献は、上記の両ライセンスで
デュアルライセンスとなるものとし、追加の条件は一切付されません。
