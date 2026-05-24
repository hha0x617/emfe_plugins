# emfe_plugin_rv32ima 設計検討メモ

作成日: 2026-04-18 (改訂 2026-05-22)
ステータス: **設計検討段階** — 実装未着手

## 1. 目的

**RV32IMA** の emfe プラグインを **「modern RISC playground」** として
構築する。古典 CISC 系プラグイン (`mc6809`, `z8000`, `mc68030`) に対する
modern RISC 代表として位置付け、**ISA 学習・組込み / RTOS 実験・ISA
コンプライアンス検証** を主目的とする。mainstream Linux ディストリ
ビューションのホスティングは目標としない。

この positioning の重要な帰結: 本プラグインは **mainstream RV Linux
ディストリビューションを追わない**。RV 系 mainstream distro はすべて
RVA20 (RV64GC) を要求し、それは大幅に規模の大きいプラグイン (5000–8000
LoC) かつ JIT なしでは boot 10–30 分と実用に遠い。RV32IMA の価値は
**小ささと速さ** にあり、stress target の物語は **単一の Linux distro
ではなく portfolio** で語る。

挙動 cross-reference として参照するリファレンス実装:
[cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)。付属の
Buildroot 設定 (`qemu_riscv32_nommu_virt_defconfig`) で生成された
Linux nommu イメージは Phase 7 portfolio の **1 つの stress target**
として残るが、もはや単一の目標ではない。

## 2. 背景 — mini-rv32ima の素性

| 項目 | 内容 |
|---|---|
| 作者 | Charles Lohr (2022–) |
| ライセンス | BSD / MIT / CC0 (triple license) |
| コードサイズ | ヘッダ 1 本約 400 行 + デモラッパ 250 行 |
| ISA | rv32ima + Zifencei + Zicsr + 部分的 supervisor |
| 設計 | ヘッダオンリー (`#define MINIRV32_IMPLEMENTATION` の stb スタイル) |
| カスタム化フック | `MINIRV32_HANDLE_MEM_LOAD_CONTROL`, `MINIRV32_HANDLE_MEM_STORE_CONTROL`, `MINIRV32_POSTEXEC`, `MINIRV32_OTHERCSR_READ/WRITE` |
| 実行 API | `MiniRV32IMAStep(state, image, vProcAddr, elapsedUs, count)` |
| RAM | フラット、`0x80000000` 基準 |
| MMIO | `0x10000000–0x12000000`: UART (8250 互換), CLINT, SYSCON |
| 本体バイナリ | 約 18KB (CLI demo ビルド時) |
| 性能 | QEMU 比約 50% (〜450 CoreMark) |

設計上、**ライブラリとして再利用できる構造** を取っている点が重要。単独エミュ
レータでありながら emfe プラグインへ組み込む余地がある。

## 3. 実装アプローチの選択肢

### 案 A: mini-rv32ima.h をベンダ取り込み (他ファイルから #include)

| 項目 | 評価 |
|---|---|
| 実装規模 | ラッパー 400〜600 行 + ヘッダ 400 行 |
| 着工〜Linux ブート | 1〜2 日 |
| 他プラグインとの設計一貫性 | △ (em8/z8000 は自前実装) |
| デバッガ統合自由度 | 低〜中 (コールバックで制約あり) |
| ライセンス | 3rd party コードのまま、著作権表示必要 |
| 将来 JIT | ほぼ書き直し |

### 案 B: 完全自前実装 (em8/z8000 と同じスタイル) **(推奨)**

RISC-V unprivileged + privileged 仕様から RV32IMA core を実装する。
`em8`、`mc6809`、`z8000`、`mc68030` と同じ「命令グループ単位の translation
unit」スタイル。mini-rv32ima のソースは挙動 cross-reference としてのみ
参照、コード借用なし。

| 項目 | 評価 |
|---|---|
| 実装規模 | 1500〜2000 行 |
| 着工〜Linux ブート | 5〜8 日 |
| 既存プラグインとの一貫性 | ◎ |
| デバッガ自由度 | 高 |
| ライセンス | 全面 original — third-party attribution 不要 |
| 将来 JIT | 素直に拡張可能 |
| 教育的明快さ | ◎ (1 本筋の通った読み物) |

### 案 C: mini-rv32ima をコード流用・emfe 用に改変

mini-rv32ima の実装を初期コードとして使い、emfe プラグイン用にファイル分割・
命令ハンドラ table 化・デバッグフック追加を行う。3 ライセンスのうち MIT/BSD/CC0
いずれかに基づきフォーク可能。

| 項目 | 評価 |
|---|---|
| 実装規模 | 900〜1200 行 (リファクタ後) |
| 着工〜Linux ブート | 2〜3 日 |
| 一貫性 | ○ (em8/z8000 風の構造、初期コードのみ借用) |
| デバッガ自由度 | ○ |
| ライセンス | 自前コードの扱いに準じる + 元コード作者のクレジット |
| 将来 JIT | 可能 |

## 4. RISC-V プロファイルの整理

アプリケーションプロセッサ向けプロファイル (一般的な Linux ディストリが要求):

| プロファイル | ベース ISA | 必須拡張 |
|---|---|---|
| RVA20U64 | RV64I | M, A, F, D, C, Zicsr, Zicntr (= RV64GC) |
| RVA22U64 | RV64I | RVA20 + B, Zihpm, fence.tso |
| RVA23U64 | RV64I | RVA22 + V(vector), Zicond, Zcb, Zfa ... |

mini-rv32ima の RV32IMA は **どの RVA プロファイルにも該当しない**。組込み
プロファイル (RVM*) でもカバーされず、Linux nommu カスタム構成 (Buildroot)
として独立したエコシステム。

## 5. OS ディストリビューション対応状況 (2026-04 時点)

### Linux — RISC-V 対応

| ディストロ | アーキ | プロファイル |
|---|---|---|
| Debian trixie (13) | riscv64 | RVA20 (RV64GC) |
| Ubuntu 26.04 LTS | riscv64 | RVA23 (最初の大量採用 LTS) |
| Fedora | riscv64 | RVA20 / 部分 RVA22 |
| AlmaLinux Kitten 10 (2026-03) | riscv64 | RVA20 |
| openSUSE Tumbleweed / Leap | riscv64 | RVA20 |
| Arch Linux RISC-V (非公式) | riscv64 | RVA20 |
| Gentoo | riscv64, **riscv32** | 設定可能 |
| Void / Alpine / NixOS / Slackware | riscv64 | RVA20 |
| OpenWrt | riscv64 | 組込み向け |
| **Buildroot** | **任意** | **任意** (rv32ima nommu 含む) |

### BSD — RISC-V 対応

| ディストロ | アーキ | 備考 |
|---|---|---|
| FreeBSD 13.x 以降 | riscv64 (RV64GC) | SiFive Unmatched 等 |
| NetBSD 11.0 (2026-04) | riscv64 (RV64GC) | 初 stable、JH7110 / QEMU |
| OpenBSD 7.8 | riscv64 (RV64GC) | 7.1 で導入 |

いずれも **RV32 未対応**。メジャー Linux も同様で 32bit は Gentoo / Buildroot
など限定的な用途のみ。

### 帰結

- mainstream distro は **RV32IMA 上では絶対に動かない**。Alpine ですら
  RVA20 (RV64GC) を要求するため、Sv32 MMU を追加しても userspace バイナリ
  (F/D/C 命令を含む) が実行できない
- これは元来 「mainstream distro 起動」 を目標としていた前提を覆す現実
- **2026-05-22 に方針転換**: 本プラグインの value proposition は
  「Linux distro を起動する」ではなく、**「modern RISC playground」** —
  小型・高速・読みやすく、**curated portfolio の stress target** (§7) で
  検証する形に統一。Linux nommu (mini-rv32ima Buildroot) は portfolio
  の 1 項目として残るが、唯一の目標ではない
- Debian / Ubuntu / NetBSD / OpenBSD / FreeBSD を動かしたい場合は
  **別プラグイン (RV64GC + Sv39 + PLIC + JIT)** が必要で、これは明示的に
  deferred (場合により無期延期)。emfe_plugins family は既に `mc68030` で
  NetBSD/m68k + Linux 6.12 を起動できる OS-booting プラグインを持っており、
  RISC-V 経由の 2 つ目の "Linux distro" プラグインは現状の優先事項では
  ない

## 6. 性能見積もり

em68030 プラグインのベンチマーク (Ryzen 級デスクトップ, 純インタプリタ 44
MIPS = MC68030 @ 270 MHz 相当) を起点とした予測:

| エミュレータ | ISA 複雑度 | 純インタプリタ予測 | Linux ブート体感 |
|---|---|---|---|
| MC68030 (実測) | 中 | 44 MIPS | NetBSD 数十秒 |
| **RV32IMA (mini-rv32ima)** | **低** | **100〜200 MIPS** | **Buildroot nommu 数秒〜十数秒** |
| RV64IMAC (MMU, F/D なし) | 中 | 60〜120 MIPS | Alpine 等 ~1 分 |
| **RV64GC** | **高** | **30〜60 MIPS** | **Debian/Ubuntu ブート 10〜30 分** |
| RV64GC + V (RVA23) | 最高 | 20〜40 MIPS | 実用困難 (JIT 必須) |

### RV64GC が重い理由

1. 浮動小数点 (F/D) の soft-float エミュレーション
2. MMU Sv39 ページテーブルウォーク + TLB
3. M/S/U 3 モード + delegation + trap フック
4. 16/32 bit 混在の圧縮命令デコード
5. 一般ディストロのカーネルは起動までに ~10 億命令 (nommu の 10〜100 倍)

mini-rv32ima はこれらを概ね**回避**しながら Linux を起動できる稀有な立ち位置。

## 7. 方針 (合意事項)

### Positioning — "modern RISC playground"

本プラグインは family の **modern RISC 代表** であり、「RV 上で Linux を
動かす」装置ではない。ラインナップでの居場所を以下の特性で正当化する:

- **小型・高速** — 1500–2000 LoC、100–200 MIPS 予測、すべて秒スケール
  で起動
- **教育的明快さ** — RV32IMA は Patterson & Hennessy 現行版および
  現代 CS 教育の正典 ISA。ソースは仕様と並べて読まれることを想定
- **stress target portfolio** (Phase 7 参照) — Linux nommu、FreeRTOS、
  riscv-tests、CoreMark、bare-metal C 例 — 複数の教育・デモ angle を
  カバー、「Linux 起動したか」単一質問ではない
- **OS-booting story は mc68030 が担う** — family は既に NetBSD/m68k
  + Linux 6.12 を mc68030 で動かしている。RV プラグインで重複する
  必要はない

### 今回着手

**案 B (完全自前実装) で emfe_plugin_rv32ima を実装する**

mini-rv32ima のソースは挙動 cross-reference としてのみ参照、コード
借用なし。RISC-V unprivileged + privileged 仕様から、他プラグインと
同じスタイルで書き下ろす。

構成:
- `D:\projects\emfe_plugins\rv32ima\`
- DLL 名: `emfe_plugin_rv32ima.dll`
- 他プラグインと同じ命名規則・配置ルール
- デフォルト RAM: 64MB (mini-rv32ima デフォルトと一致、検証時の cross-comparison しやすさのため)

### 将来拡張 (deferred — 場合により無期延期)

転換前の計画では `rv64ima → RV64GC → JIT` を mainstream Linux distro
への優先度順序として記載していた。playground positioning 採用に伴い、
このはしごはアクティブなロードマップから外れる:

1. **rv64ima + Sv39** (F/D/C 無し) — カスタム Alpine-musl リビルドが
   可能になるが、procurement friction は残り、+1000–1500 LoC の対費用
   効果が低い。**Deferred**
2. **RV64GC** — full mainstream RV Linux。5000–8000 LoC + soft-float +
   Sv39 + 圧縮命令デコード + M/S/U delegation。JIT なし boot 10–30 分。
   **本プラグインの拡張ではなく、将来別プラグインとして扱う**。
   着手する場合は専用 design_notes を起こす
3. **JIT 化** — 本プラグインか将来の RV64GC プラグインのいずれかに
   適用。`mc68030` の JIT 知見 (deferred snapshot, block cache,
   bailout blacklist, inline dispatch) は転用可能。`rv32ima` の
   Phase 1–7 ロードマップには載せない

## 8. Phase 計画

インタープリタ実装 (Phase 1–6) は playground positioning 採用後も
**変更なし** — コア ISA と supervisor 機構は変わらない。**Phase 7** が
新規追加で、playground positioning を curated stress-target portfolio
として具現化する。

### Phase 1–6 (インタープリタ実装、変更なし)

| Phase | 内容 | 目安行数 |
|---|---|---|
| 1 | RV32I 基本命令 (算術/ロジック/ロード/ストア/分岐/JAL[R]) | 400 |
| 2 | M 拡張 (MUL/DIV) + Zifencei + Zicsr | 200 |
| 3 | A 拡張 (AMO) | 150 |
| 4 | 特権モード・CSR・trap/interrupt | 500 |
| 5 | CLINT + UART (8250) + SYSCON + ブート stub | 400 |
| 6 | 逆アセンブラ + emfe プラグイン ABI + **`riscv-tests` 通過** | 400 |

`riscv-tests` (BSD ライセンス、RISC-V 公式 ISA コンプライアンス
スイート) を Phase 6 の合格基準とする。通過時点で本プラグインは
RV32IMA + Zifencei + Zicsr + machine-mode supervisor について
spec-correct であることが証明される。

### Phase 7 — stress-target portfolio (新規)

| カテゴリ | ターゲット | ライセンス | 出典 / 状況 |
|---|---|---|---|
| Linux nommu | mini-rv32ima Buildroot Linux image | GPL-2.0 (Linux) + Buildroot 構成各種 | 別 repo (§10) にビルド済 image + 再現レシピを置く |
| Bare-metal RTOS | **FreeRTOS** RISC-V port | MIT | 別 repo に demo bundle |
| Bare-metal RTOS | **Zephyr** (RISC-V support) | Apache-2.0 | optional — FreeRTOS demo が片付いてから追加 |
| Compliance | **riscv-tests** | BSD | Phase 6 合格基準と兼用、end user 向けに「ISA 公式コンプライアンス suite が走らせられる」demo として再利用 |
| Benchmark | **CoreMark** / **Dhrystone** | BSD with attribution | "modern RISC playground" の性能デモ用 |
| Bare-metal C | **picolibc** ベースの例 | BSD | ISA を最小スキャフォールディングで動かす自作例 |

Phase 7 はインタープリタ LoC を増やさず、curated demo content と
recipe を追加する。実際の binary / image は **companion repository**
(§10) に置き、本プラグインの MIT/Apache ツリーを upstream ライセンス
ペイロードで汚さない方針 — Z80 plugin の `emfe_plugins_cpm_supplements`
companion と同じパターン。

## 9. 未決事項

- [ ] ユーザが用意する Linux イメージ (kernel + DTB) の想定デフォルトパス
- [ ] デバイスツリーを emfe プラグイン内に埋め込むか、外部ファイルとするか
- [ ] ブロックデバイス (rootfs 用 virtio-blk 相当) を Phase 5 に含めるか Phase 7 (Linux nommu demo の一部) に回すか
- [ ] ELF ローダ対応 (`mc68030` の作法と整合させるか、raw binary load のみで十分か)
- [ ] Phase 1 中、挙動 cross-reference として参照すべき既存実装はどれか — mini-rv32ima が最も近い、QEMU や Spike は CSR セマンティクス / trap 配信などの難所で参考になる
- [ ] Phase 7 portfolio 用 companion-repo の命名。仮名: `emfe_plugin_rv32ima_demos`。stress-target カテゴリごとに subdirectory にするか、複数 companion に分けるか — 第一案は単一 repo + subdir 構成 (Phase 7 portfolio が 1 箇所で discoverable)、`emfe_plugins_cpm_supplements` が単一ライセンスゾーン用にとった同様パターンが先例

## 10. Companion リポジトリ

Phase 7 の stress target は **別 companion repo** で配布する (本プラグイン
の MIT/Apache ソースツリーを upstream ライセンスペイロードで汚さない
ため)。仮名: **`emfe_plugin_rv32ima_demos`** (未作成)。想定構成:

```
emfe_plugin_rv32ima_demos/
├── README.md                — 目的とライセンス姿勢
├── linux-nommu/             — mini-rv32ima Buildroot image + recipe
│                              (Linux GPL-2.0 + パッケージ別ライセンス)
├── freertos/                — FreeRTOS RISC-V port demo (MIT)
├── riscv-tests/             — ISA 公式コンプライアンス (BSD)
├── coremark/                — CoreMark ベンチマーク (BSD, attribution)
└── baremetal-c/             — picolibc ベース例 (BSD + 自作)
```

companion-repo パターンは Z80 plugin の `emfe_plugins_cpm_supplements` で
取った方針 (CP/M-only ペイロードを別ライセンスで隔離) と同じ。本件
は単一 restrictive license ではなく複数 upstream の mixed-license
ペイロードを束ねる動機だが、構造的解は同一。

companion repo は Phase 6 が見えた段階で作成、それ以前は作らない —
インタープリタ未完成のうちに stress target だけ集めても意味がない。

## 11. 参考資料

- [cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)
- [cnlohr/buildroot_for_mini_rv32ima](https://github.com/cnlohr/buildroot_for_mini_rv32ima)
- [RISC-V Unprivileged ISA](https://github.com/riscv/riscv-isa-manual)
- [RVA23 Profile Specification](https://docs.riscv.org/reference/profiles/rva23/_attachments/rva23-profile.pdf)
- [RISC-V Profiles repo](https://github.com/riscv/riscv-profiles)
- [Debian RISC-V Wiki](https://wiki.debian.org/RISC-V)
- [NetBSD 11.0 release](https://www.netbsd.org/releases/formal-11/NetBSD-11.0.html)
- [OpenBSD/riscv64](https://www.openbsd.org/riscv64.html)
- [AlmaLinux Kitten 10 riscv64](https://almalinux.org/blog/2026-03-17-almalinux-goes-riscv/)
