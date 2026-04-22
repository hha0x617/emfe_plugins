# emfe_plugin_rv32ima 設計検討メモ

作成日: 2026-04-18
ステータス: **設計検討段階** — 実装未着手

## 1. 目的

emfe プラグインアーキテクチャ上で、MMU を持たない RISC-V サブセット (rv32ima)
で動作する Linux をホストする emfe プラグインを作成する。

リファレンスは [cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)。
付属の Buildroot 設定 (`qemu_riscv32_nommu_virt_defconfig`) で生成された Linux
カーネル + DTB + rootfs を直接ブート可能にする。

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

### 案 B: 完全自前実装 (em8/z8000 と同じスタイル)

| 項目 | 評価 |
|---|---|
| 実装規模 | 1500〜2000 行 |
| 着工〜Linux ブート | 5〜8 日 |
| 一貫性 | ◎ |
| デバッガ自由度 | 高 |
| ライセンス | オリジナル |
| 将来 JIT | 素直に拡張可能 |

### 案 C: mini-rv32ima をコード流用・emfe 用に改変 **(推奨)**

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

- mini-rv32ima プラグインで動く OS: **Buildroot カスタムビルド (mini-rv32ima
  専用設定) の Linux のみ**
- Debian / Ubuntu / NetBSD などを動かしたい場合は **別プラグイン (RV64GC +
  MMU + PLIC)** として設計する必要あり

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

### 今回着手

**案 C (コード流用 + 改変) で emfe_plugin_rv32ima を実装する**

構成:
- `D:\projects\emfe_plugins\rv32ima\`
- DLL 名: `emfe_plugin_rv32ima.dll`
- 他プラグインと同じ命名規則・配置ルール
- デフォルト RAM: 64MB (mini-rv32ima デフォルト)

### 将来拡張 (優先度順)

1. **rv64ima 対応** (MMU 付き) — 32bit → 64bit 拡張、F/D/C 無しで Alpine / Void
   などに到達。Phase 1 のコード基盤を流用しやすい
2. **RV64GC 対応** — F/D 追加、圧縮命令、C/D 各拡張。JIT 前提で取り組む
3. **JIT 化** — em68030 で得た知見 (deferred snapshot, block cache, bailout
   blacklist, 動的インラインディスパッチ) を転用

## 8. Phase 1 (rv32ima) の作業分割案

| Phase | 内容 | 目安行数 |
|---|---|---|
| 1a | RV32I 基本命令 (算術/ロジック/ロード/ストア/分岐/JAL[R]) | 400 |
| 1b | M 拡張 (MUL/DIV) + Zifencei + Zicsr | 200 |
| 1c | A 拡張 (AMO) | 150 |
| 1d | 特権モード・CSR・trap/interrupt | 500 |
| 1e | CLINT + UART (8250) + SYSCON + ブート stub | 400 |
| 1f | 逆アセンブラ + プラグイン ABI + テスト | 400 |

1a〜1d が完了すれば Linux ブート (最低限の machine-mode trap) にたどり着く。
1e/1f で emfe プラグイン化 + ユーザ体験完成。

## 9. 未決事項

- [ ] mini-rv32ima の初期コード流用元をどこにコピーするか (`rv32ima/third_party/` か `rv32ima/src/` 内直接か)
- [ ] ユーザが用意する Linux イメージ (kernel + DTB) の想定デフォルトパス
- [ ] デバイスツリーを emfe プラグイン内に埋め込むか、外部ファイルとするか
- [ ] ブロックデバイス (rootfs 用 virtio-blk 相当) を Phase 1 に含めるか Phase 2 に回すか
- [ ] ELF 直接ロード対応の可否 (mini-rv32ima は raw binary 前提)

## 10. 参考資料

- [cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)
- [cnlohr/buildroot_for_mini_rv32ima](https://github.com/cnlohr/buildroot_for_mini_rv32ima)
- [RISC-V Unprivileged ISA](https://github.com/riscv/riscv-isa-manual)
- [RVA23 Profile Specification](https://docs.riscv.org/reference/profiles/rva23/_attachments/rva23-profile.pdf)
- [RISC-V Profiles repo](https://github.com/riscv/riscv-profiles)
- [Debian RISC-V Wiki](https://wiki.debian.org/RISC-V)
- [NetBSD 11.0 release](https://www.netbsd.org/releases/formal-11/NetBSD-11.0.html)
- [OpenBSD/riscv64](https://www.openbsd.org/riscv64.html)
- [AlmaLinux Kitten 10 riscv64](https://almalinux.org/blog/2026-03-17-almalinux-goes-riscv/)
