# Motorola MC6809 — emfe プラグインリファレンス

## 1. 概要

emfe_plugin_mc6809 は [em6809](../../../em6809) (Rust 製 CPU コア) を
標準 emfe プラグイン C ABI でラップした薄いシム。両フロントエンド
(WinUI3/C++ と WPF/C#) からロード・制御できる。

## 2. レジスタ

| 名前 | 幅 | 説明 |
|---|---|---|
| A | 8 bit | アキュムレータ A |
| B | 8 bit | アキュムレータ B |
| D | 16 bit | `A:B` 連結ビュー (非表示) |
| X | 16 bit | インデックス X |
| Y | 16 bit | インデックス Y |
| U | 16 bit | ユーザスタックポインタ |
| S | 16 bit | システムスタックポインタ (SP フラグ付き) |
| PC | 16 bit | プログラムカウンタ |
| DP | 8 bit | ダイレクトページ |
| CC | 8 bit | コンディションコード (FLAGS フラグ付き) |

### CC ビット

```
  7   6   5   4   3   2   1   0
+---+---+---+---+---+---+---+---+
| E | F | H | I | N | Z | V | C |
+---+---+---+---+---+---+---+---+
```

- `E` — 直前の割込時にレジスタ全体を push したか
- `F` — FIRQ マスク
- `H` — ハーフキャリー (bit3 → bit4)
- `I` — IRQ マスク
- `N` — 負
- `Z` — ゼロ
- `V` — 符号付オーバーフロー
- `C` — キャリー / ボロー

## 3. メモリマップ

- **$0000–$FFFF**: 64 KB フラット RAM (プラグイン内配列)
- **$FFFE/$FFFF**: リセットベクタ (上位バイト先、6809 native)

### メモリマップド UART — MC6850 ACIA (デフォルトベース `$FF00`)

プラグインは **Motorola MC6850 ACIA** (Asynchronous Communications Interface
Adapter) をエミュレートする。MC6850 は 6809 時代の事実上の標準シリアル UART
で、SWTPC S/09、Tandy Color Computer、Dragon 32/64、そして大半の FLEX /
OS-9 システムで採用されている。連続する 2 アドレスを消費する：

| RS | アドレス | 読み | 書き |
|---|---|---|---|
| 0 | base+0 | **Status Register (SR)** | **Control Register (CR)** |
| 1 | base+1 | **Receive Data Register (RDR)** | **Transmit Data Register (TDR)** |

#### Status Register (base+0 から読む)

```
 7   6   5    4   3   2    1     0
+---+---+----+---+---+---+------+------+
|IRQ|PE |OVRN|FE |CTS|DCD| TDRE | RDRF |
+---+---+----+---+---+---+------+------+
```

- bit 0 **RDRF** — 受信データレジスタ Full (RX バイト到着あり)
- bit 1 **TDRE** — 送信データレジスタ Empty (次の TX バイトを書ける)
- bit 2 **DCD** — Data Carrier Detect (常に 0)
- bit 3 **CTS** — Clear To Send (常に 0)
- bit 4 **FE** — フレーミングエラー (常に 0)
- bit 5 **OVRN** — 受信オーバーラン (RX FIFO 溢れ時セット、RDR 読みでクリア)
- bit 6 **PE** — パリティエラー (常に 0)
- bit 7 **IRQ** — `(RDRF && RIE) || (TDRE && TX-IRQ-有効)`

#### Control Register (base+0 に書く)

```
 7    6    5    4   3   2   1    0
+----+----+----+-----+-----+-------+
|RIE |TC1 | TC0| WS2..WS0 | CDS1 CDS0|
+----+----+----+-----+-----+-------+
```

- bits 0-1 **CDS** — クロック分周:
  - `00` = ÷1、`01` = ÷16、`10` = ÷64、**`11` = マスタリセット**
- bits 2-4 **WS** — ワード長選択 (8N1, 7E1 等)。バイト透過レベルでは強制しない
- bits 5-6 **TC** — Transmit Control (RTS + TX IRQ):
  - `00` = RTS low, TX IRQ **無効**
  - `01` = RTS low, TX IRQ **有効**
  - `10` = RTS high, TX IRQ 無効
  - `11` = RTS low, BREAK, TX IRQ 無効
- bit 7 **RIE** — 受信割込有効

#### 典型的な初期化シーケンス

```
  LDA #$03        ; マスタリセット (CDS=11)
  STA ACIA_CR
  LDA #$15        ; CDS=01 (÷16), WS=101 (8N1), TC=00 (RTS low, TX IRQ 無効), RIE=0
  STA ACIA_CR
```

2 回目の書き込み後 TDRE がセットされ、ゲストは送信開始可能。ゲストは TDR
書き込み前に TDRE をポーリング (または TX IRQ 有効化)、RDR 読み込み前に
RDRF をポーリング (または RIE 有効化) する。

#### IRQ 線

ACIA の IRQ 出力は `Bus::irq_lines()` 経由で MC6809 の **IRQ 入力** へ配線
(FIRQ / NMI ではない)。ゲストコードは CC の I フラグをクリアしないと割込が
受け付けられない。

#### 設定

ベースアドレスは `ConsoleBase` 設定で変更可能 (例 `0xFF00`, `0xE000`,
`0xFF68` は CoCo ACIA Pak)。変更は `emfe_apply_settings` で反映。

## 4. 実装済 プラグイン API (Phase 1)

- Discovery: `emfe_negotiate`, `emfe_get_board_info`
- Lifecycle: `emfe_create`, `emfe_destroy`
- コールバック: console char / state-change / diagnostic
- レジスタ: 定義列挙、バッチ get/set
- メモリ: `peek_{byte,word,long}`, `poke_{byte,word,long}`, `peek_range`, `get_memory_size`
- 逆アセンブル: `disassemble_one`, `disassemble_range`, `get_program_range`
  (em6809 の `disasm::disasm_one` を利用)
- 実行: `step`, `run`, `stop`, `reset`, `get_state`, `get_instruction_count`, `get_cycle_count`
- ブレークポイント: add/remove/enable/condition/clear/get
- ウォッチポイント: add/remove/enable/condition/clear/get (read / write / RW)
- ファイルロード: `load_binary`, `load_srec` (ELF 非対応)
- 設定: `BoardType`, `ConsoleBase`, `Theme`, コンソールサイズ
- コンソール I/O: `send_char`, `send_string`, `console_tx_space`
  (ペースト用に RX FIFO の残り容量を返すクエリ。FIFO は最大 1024 バイト保持)
- `step_over` / `step_out`: Phase 1 未対応

## 5. 未対応 (Phase 1)

- コールスタック (`emfe_get_call_stack` は 0 を返す)
- フレームバッファ
- 入力デバイスイベント
- ELF ロード (6809 ツールチェインは S-Record が主)

## 6. プログラムロード

### S-Record

```c
emfe_load_srec(inst, "hello.s19");
```

- `S1` / `S9` レコード (16bit アドレス) を解析
- データをメモリへ配置
- `S9` のエントリポイント、無ければ最小データアドレスから PC 設定
- S (システムスタック) を `$FF00` に初期化

### Raw binary

```c
emfe_load_binary(inst, "program.bin", 0x0100);
```

指定アドレスへファイル展開。`$FFFE/$FFFF` のリセットベクタが非ゼロなら
そこから PC、ゼロなら `load_address` から。

## 7. 設計メモ

- `PluginBus` が em6809 の `Bus` trait を実装。メモリは
  `Box<[u8; 0x10000]>` (スタック溢れ回避)。
- MSVC C ランタイムは静的リンク (`.cargo/config.toml` に
  `RUSTFLAGS="-C target-feature=+crt-static"`)。
- FFI 境界での `panic` は現在 `catch_unwind` で囲っていない。Phase 2 で
  境界セーフティを追加予定。
- `run` のワーカースレッドは生ポインタを `usize` キャストで渡すことで
  ホットパスに `Arc<Mutex<..>>` を持ち込まない。

## 8. em6809 本体のサンプル

em6809 本体のリポジトリ ([hha0x617/em6809](https://github.com/hha0x617/em6809)) は
`samples/` 配下に独自のサンプルプログラム (hello / echo / vt100) を同梱して
いる。**em6809 PR #25 以降、本体のコンソールデバイスは本プラグインと同じ
Motorola MC6850 ACIA 配置に統一されたため、これらのサンプルは本プラグインで
そのまま動作する**。

| | em6809 本体 (MC6850 ACIA) | プラグインの MC6850 ACIA |
|---|---|---|
| base+0 read  | Status Register | Status Register |
| base+0 write | Control Register | Control Register |
| base+1 read  | RDR | RDR |
| base+1 write | TDR | TDR |

それ以前の em6809 では別途「Simple」コンソール (`ConsoleDev`、レジスタ反転:
data@+0, status@+1) も併存していたが、PR #25 で削除済。古い Simple 配置で
書かれた `.s19` を持っている場合は MC6850 配置 (master reset → CR/SR ポーリング)
で書き直す必要がある。本プラグインの `examples/*.py` および本体の
`samples/{hello,echo}/*.asm` が雛形になる。

## 9. 参考資料

- em6809 本体: [hha0x617/em6809](https://github.com/hha0x617/em6809)
- MC6809 プログラミングリファレンス: Motorola MC6809E データシート
- MC6850 ACIA データシート: Motorola MC6850 Asynchronous Communications
  Interface Adapter
- OS-9 / NitrOS-9: em6809 の `docs/en/os9_guide.md` 参照
