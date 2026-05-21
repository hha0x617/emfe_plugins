# emfe_plugin_z80 — Design Notes (日本語)

Date: 2026-05-21
Status: **Design phase** — 未実装

> 英語版: [`design_notes.md`](design_notes.md)。本ドキュメントは翻訳であり、
> 内容に齟齬があった場合は英語版を正とする。

## 1. 目的

Zilog Z80 8-bit プラグインを emfe プラグインファミリーに追加する。既存の
`mc6809` (Motorola 8-bit) に対する Zilog 側カウンターパート、および
既存の `z8000` (Zilog 16-bit) に対する Zilog 8-bit 補完として位置付ける。

対象ユーザー:

1. **retro エミュレーション利用者** — クラシック CP/M 2.2 ディスクや ROM、
   homebrew Z80 ソフトウェアを emfe ホスト上で動かす
2. **教育・興味本位の開発者** — `mc6809` / `z8000` と並べて読み比べできる、
   単一ファイルで丁寧にコメントされた Z80 実装

## 2. 背景 — 既存ラインナップにおける位置付け

| Plugin | ベンダ | bit | 時代 | 状態 |
|---|---|---|---|---|
| `em8` | toy (~6502 サブセット) | 8 | 教育用 | 出荷済 |
| `mc6809` | Motorola | 8 | 1979 | 出荷済 |
| **`z80`** | **Zilog** | **8** | **1976** | **本設計** |
| `z8000` | Zilog | 16 | 1979 | 出荷済 |
| `mc68030` | Motorola | 32 | 1987 | 出荷済 |
| `rv32ima` (queued) | RISC-V | 32 | 2010s | docs only |

Z80 は **最も明らかな空白** を埋める: Zilog 8-bit、その時代で文化的に
最も影響力のあった 8-bit マイクロプロセッサ。利用者にとっては 8-bit
階層での Motorola vs Zilog (`mc6809` vs `z80`)、および同一ベンダー
8/16-bit 比較 (`z80` vs `z8000`) という直接対比が可能になる。

## 3. 実装方針

3 案検討、**B 案 (完全自前実装)** を推奨。本ファミリーの他プラグイン
(`em8`, `mc6809`, `z8000`, `mc68030`) と同じ路線。

### A 案 — 既存エミュレータを vendor (`z80ex`, `libz80`)

| 観点 | 評価 |
|---|---|
| コード量 | ~3000 LoC vendored + 薄い emfe ラッパー |
| 初回 boot まで | 1–2 日 |
| `mc6809` / `z8000` (自前実装) との一貫性 | 低 |
| デバッガ / ブレークポイント hook の柔軟性 | 中 |
| ライセンス | upstream + attribution |
| 将来 JIT | 概ね書き直し |

### B 案 — 完全自前実装 (`mc6809` / `z8000` 路線) **(推奨)**

Zilog 公式リファレンスマニュアルから Z80 core を実装する。`mc6809` /
`z8000` と同じ「命令グループ単位の translation unit」スタイル。サード
パーティのエミュレータソースは読まず・借用しない。挙動の cross-reference
としてのみ参照可。

| 観点 | 評価 |
|---|---|
| コード量 | 2500–3500 LoC (Z80 は 6809 より opcode 多い) |
| 初回 boot まで | 7–12 日 |
| 既存プラグインとの一貫性 | ◎ |
| デバッガ柔軟性 | 高 |
| ライセンス | 全面 original — third-party attribution 不要 |
| 将来 JIT | 自然に拡張 |
| 教育的明快さ | ◎ (1 本筋の通った読み物) |

### C 案 — 既存 Z80 core から fork / derive

permissive license の Z80 reference (例: **z80ex**, BSD-3-Clause、
あるいは **libz80**, BSD-style) を出発点として、emfe プラグインスタイル
に refactor。

| 観点 | 評価 |
|---|---|
| コード量 | refactor 後 1800–2500 LoC |
| 初回 boot まで | 3–5 日 |
| 一貫性 | ○ (構造は揃うが内部は借用ロジック) |
| デバッガ柔軟性 | ○ |
| ライセンス | 我々の変更分は original + upstream attribution |
| 将来 JIT | 実現可能 |

## 4. ISA スコープ

Zilog Z80 仕様は境界が明確だが、実ソフトウェアは隅々まで叩く。段階的に
実装:

### Phase 1 — Base ISA (Zilog 公式)

- documented 8/16-bit データ転送・算術命令すべて
- documented bit 命令すべて (`CB` プレフィックス)
- documented index レジスタ命令すべて (`DD` / `FD` プレフィックス、
  `IX` / `IY`)
- documented 拡張命令すべて (`ED` プレフィックス、ブロック I/O 含む)
- 全フラグ組み合わせの条件分岐・コール
- 割込モード 0, 1, 2
- リフレッシュレジスタ `R` インクリメント (見かけ上の正しさ、サイクル正確
  ではない)

CP/M 2.2 boot、Tiny Pascal / Hi-Tech C 動作、`zexdoc` (documented
opcode フラグテスト) 通過に十分。

### Phase 2 — Undocumented opcode (`zexall` 通過)

- `IXH`, `IXL`, `IYH`, `IYL` への 8-bit 操作
- 未文書フラグビット (`BIT`, `SCF`, `CCF` での `Y`, `X` 伝播)
- 未文書 `ED` プレフィックス opcode (各種長 NOP)

`zexall` 通過に十分。実ゲームや demoscene が依存する一方、CP/M 2.2 自身は
依存しない。

### Phase 3 — サイクル精度

- documented 全 opcode の Zilog reference T-state 一致
- IM 1 / IM 2 の割込 acknowledge タイミング

Phase 3 は Phase 1–2 と独立。機能正しさが先。

### スコープ外 (初期)

- **R800** (MSX turbo-Z80 拡張)
- **Z180 / eZ80** (後発 Zilog 派生)
- **リフレッシュサイクル駆動 DRAM 挙動** — サイクル単位でのモデル化のみ、
  実メモリ wall-clock タイミングまでは追わない

## 5. メモリ・I/O モデル

| 面 | 詳細 |
|---|---|
| アドレス空間 | 64 KiB フラット code+data (MMU なし、設計上) |
| I/O 空間 | 256-byte 独立 I/O 空間、`IN`/`OUT` 経由 |
| MMIO surface | I/O 空間内の既知ポートにマップ |
| 割込 | `INT` 1 本 + `NMI` 1 本、当面はソフトウェア駆動 |

emfe ホストプラグイン API との MMIO 契約は Phase 1 中に `mmio.md` で
策定。`mc6809` / `z8000` のポート割り当てと整合させる:

| Port (仮) | 方向 | 用途 |
|---|---|---|
| `0xFE` (placeholder) | r/w | コンソール UART data |
| `0xFF` (placeholder) | r | コンソール UART status |
| (Phase 2 以降で追加割り当て: ディスクコントローラ、RTC 等) |

## 6. Stress-test ターゲット

`mc6809` (Tiny Lisp / Tiny Forth) と `rv32ima` (mini-rv32ima Linux) の
先例に倣い、各プラグインは in-repo stress テストコーパスを同梱する。
Z80 ではライセンスゾーンを 2 つに分離する (§7 参照)。

### 同 repo 内 (`z80/` 配下、MIT / Apache-2.0)

| 項目 | 出典 | ライセンス | 用途 |
|---|---|---|---|
| `examples/zexall/`, `examples/zexdoc/` | Frank Cringle (1994) | Public domain | Z80 ISA 精度テスト、デファクト基準 |
| `examples/eforth/` | Bill Muench eForth-Z80 | Public domain | OS 不要の Z80 Forth、現実的負荷 |
| `examples/monitor/` | 自作 | MIT (本 repo) | emfe Z80 plugin 用 ROM monitor: LD/DUMP/GO/DISASM、`mc6809/examples/` と同パターン |
| `examples/tinybasic/` | 自作 (長期) | MIT (本 repo) | Tiny BASIC の Z80 移植、同上 |

### 別 repo — [`emfe_plugin_z80_cpm22`](#7-ライセンス戦略)

| 項目 | 出典 | ライセンス |
|---|---|---|
| CP/M 2.2 source | "The Unofficial CP/M Web Site" | Bryan Sparks 2022 letter |
| CP/M 2.2 binary | 同上 | Bryan Sparks 2022 letter |
| Boot ディスクイメージ | 上記 + 自作 CBIOS から build | Bryan Sparks 2022 letter (イメージ全体としての再配布) |
| Build recipe | **本 repo**, `examples/cpm22/` 配下 | MIT (original work) |
| 自作 CBIOS | **本 repo**, `examples/cpm22/cbios/` 配下 | MIT (original work) |

build recipe と CBIOS は **CP/M source を消費するが** original work で
あるため、MIT の本 repo に置く。recipe は兄弟ディレクトリの
`emfe_plugin_z80_cpm22` clone (もしくは環境変数指定) から source を読み、
build 結果のディスクイメージを再度そちらの `disks/` に書き出す。

## 7. ライセンス戦略

最も影響の大きい設計判断、明示的に記録する。

2022 Bryan Sparks letter は CP/M source / binary の *コピー・改変・配布・
利用* (商用含む) を許諾する。しかし *サブライセンス権* は付与しない。
従って CP/M 派生ファイルを MIT / Apache-2.0 に再ライセンスはできず、
Sparks notice を付随させて配布する必要がある。

Z80 plugin に CP/M 素材を同梱する 2 案検討:

1. **同 repo 内 mixed licensing** — `z80/examples/` 配下に CP/M ファイル
   を置き、per-file SPDX header + sub-LICENSE ファイルで分離。**却下**:
   top-level emfe_plugins LICENSE ファイルは実際にはそれらを覆っていない
   ため、casual な読者を誤誘導する

2. **別 repo (B 案、採用)** — original work は MIT/Apache の本 repo、
   CP/M 派生材料は専用の sister repo `emfe_plugin_z80_cpm22` に Sparks
   letter 単一ライセンスで配置

結果:

- 本 repo のライセンスストーリーは現状維持 — `z80/` 配下全ファイル dual
  MIT / Apache-2.0
- 別 repo のライセンスストーリーは単一 — Sparks letter のみ
- 両 repo は **ライセンス surface を共有しない**、per-file SPDX 監視も
  不要

先例: Linux kernel の `linux-firmware` 別 repo が proprietary firmware
blob を同じパターンで処理している。

## 8. パフォーマンス見積もり

`rv32ima` 設計で使った back-of-envelope (`mc68030` インタープリタが
Ryzen-class デスクトップで 44 MIPS 計測) を同じ路線で適用:

| Plugin | ISA 複雑度 | インタープリタ (予測) | Boot 体感 |
|---|---|---|---|
| `mc68030` (計測値) | 中 | 44 MIPS / ~270 MHz 030 相当 | NetBSD 数十秒 |
| `mc6809` (出荷済) | 低 | 未計測 | Tiny Lisp REPL リアルタイム |
| **`z80` (予測)** | **低 (ただしプレフィックス多用)** | **80–150 MIPS / ~16 MHz × N 倍の Z80 相当** | **CP/M cold boot < 1 秒** |

「プレフィックス多用」とは、opcode の有意な割合が 2-byte デコード
(`CB` / `DD` / `ED` / `FD`) を要し、`mc6809` の素直なデコードツリーに
比べて dispatch オーバーヘッドが乗ること。それでも現代ハードウェア上で
リアルタイムには余裕、ターゲットの CP/M クラスソフトウェアは emulated
~16 MHz でも快適。

JIT 路線 (`mc68030` 類似) は実現可能だが Phase 1–3 のロードマップ外。

## 9. Phase 計画

| Phase | 成果物 | Stress ターゲット |
|---|---|---|
| **1** | プラグイン scaffold、base ISA、コンソール MMIO、ELF/HEX ローダ、Hello-World ROM 動作 | 手書きテストプログラム |
| **2** | Base ISA で `zexdoc` 通過、ROM monitor 動作 | `zexdoc`、ROM monitor 対話利用 |
| **3** | undocumented opcode で `zexall` 通過、eForth 動作 | `zexall`、eForth REPL |
| **4** | emfe Z80 MMIO 向け CBIOS、別 repo 素材から CP/M 2.2 boot | CP/M 2.2 cold boot で `A>` プロンプト |
| **5** | CP/M 上で PIP / STAT / DIR smoke、オプション Turbo Pascal 3.0 | CP/M 上の言語処理系 |
| **6** | Settings UI、デバッガ統合 (breakpoint, watchpoint, disasm pane) — `mc6809` 同等まで | n/a |
| **7** *(オプション)* | サイクル正確 T-state モード | サイクル依存 retro game ROM |

Phase 1–3 は本 repo 単独で完結。Phase 4 から
`emfe_plugin_z80_cpm22` との cross-repo 連携が始まる。

## 10. オープン課題

1. **挙動 cross-reference として参照すべき既存 Z80 エミュレータはどれか**
   — コード借用はなし (B 案 / 完全自前実装) だが、エッジケース (フラグ
   伝播、未文書 opcode、割込 ack タイミング) の正しさを sanity check
   するのに既存実装の挙動が役立つ。z80ex (BSD-3-Clause、zexall 通過) や
   MAME の Z80 core が候補。Phase 2 着手前にどれを参照するか決める。

2. **エンディアン / レジスタペアアクセス** — Z80 はメモリ little-endian
   だが、レジスタペア (`BC`, `DE`, `HL`) は固有レイアウト。他プラグインで
   使っている emfe register-defs ABI で「バイト view」と「ペア=ワード
   view」両方をきれいに公開する必要がある。`mc6809` の `D` (= `A:B`)
   公開方法をテンプレートとして参照。

3. **ディスクコントローラ MMIO の形** — `mc6809` はディスクターゲット
   なし、`mc68030` は WD33C93。Z80 のディスクモデルは遥かに単純 (CP/M は
   BIOS call 経由のセクタレベル r/w を期待)。歴史的特定コントローラ
   (WD1793, NEC 765, ...) を emulate するより、CBIOS が叩く最小の
   「セクタデバイス」MMIO を提供するのが clean。Phase 4 開始時 (CBIOS
   実装時) に決定。

4. **「RAM size」「ROM at 0x0000」の Settings UI 提供範囲** — Z80
   システムは構成が多様 (TPA size、ROM monitor map)。プリセット小マトリ
   クス (CP/M 64 KiB、ROM-based 32 KiB、…) で十分。完全な byte 単位の
   ROM/RAM map は emfe stress-test ターゲットには overkill。

## 11. References

- Zilog Z80 CPU User Manual (UM008011-0816) — 1 次 ISA reference
- Frank Cringle, `zexall` / `zexdoc` ソース — public domain 精度スイート、
  Z80 エミュレータテストコーパスのデファクト
- [The Unofficial CP/M Web Site](http://www.cpm.z80.de/) — CP/M 2.2
  source / binary の canonical アーカイブ
- Bryan Sparks (DRDOS, Inc.) 2022 ライセンスレター — 別 repo の
  `LICENSE-CPM22.txt` に逐語コピー
- 本 repo `mc6809/docs/` — in-repo examples (Tiny Lisp / Tiny Forth) と
  設計ノートスタイルのパターン参照
- 本 repo `rv32ima/docs/design_notes.md` — 本ドキュメント構造のパターン
  参照
