# Hha Forth — 言語仕様と実装解説

本ドキュメントは `examples/forth/forth.asm` の**言語仕様** (Forth 系処理系
としての特徴) と**実装上の設計** (スレッディング方式、辞書構造、内側
インタプリタ等) をまとめます。ユーザ向けの使い方は
[../README_ja.md](../README_ja.md) を参照してください。

---

## 1. 実装規模

| 指標 | 値 |
|---|---|
| ソース行数 | **1,535 行** (単一ファイル `forth.asm`) |
| 生バイナリ | **2,605 bytes** (≒ 2.5 KB) |
| SREC ファイル | 7,262 bytes (ASCII 形式) |
| CFA (code-field address) | **79 個** — primitive + colon definition の総計 |
| Smoke test | **6 件** (全件 passing) |

**Hha Lisp (18.5 KB, 60 primitive) と比較して約 1/7 の規模**。古典的な
figForth / jonesforth クラスの「最小で実用」を狙った処理系です。

---

## 2. Forth 系処理系としての特徴

### 2.1 採用している方式

- **Indirect-threaded code (ITC)**: 各辞書エントリの CFA は
  「ネイティブコードへのポインタ (primitive)」または「DOCOL へのポインタ
  (colon definition)」。NEXT で 2 段の間接ジャンプ。
- **Separate data / return stacks**: U = data stack、S = return stack。
  どちらも下方向伸長、TOS は低番地側。
- **16-bit cell**: すべて符号付き 16-bit。数値入力は 10 進のみ。

### 2.2 REPL (Outer Interpreter)

- `ACCEPT` が TIB (128 バイト) に 1 行読込
- `INTERPRET` が `PARSE-NAME` → `SFIND` / `NUMBER?` の順に dispatch
- 行末到達で `" ok"` + CRLF を出力してループ
- 不明語は `<word>?` を表示、REPL は継続 (スタックは巻き戻らない)

### 2.3 コンパイルモード

- `:` で `STATE = 1`、`;` IMMEDIATE で `STATE = 0`
- コンパイル中:
  - 非 IMMEDIATE word → CFA を HERE に書き込み
  - 数値 → `(LIT)` + value の 2 セルを書き込み
  - IMMEDIATE word → 即時実行

### 2.4 ANS Forth 互換性

**部分互換**:
- ✅ `:` `;` `IF` `THEN` `ELSE` `BEGIN` `UNTIL` `AGAIN` `."` `(`
- ✅ `VARIABLE` `CONSTANT` `ALLOT` `,` `C,` `HERE` `LATEST`
- ✅ `>R` `R>` `R@` の制御フロー
- ❌ `DO` `LOOP` `+LOOP` (未実装)
- ❌ `BASE` 切替 (10 進固定)
- ❌ `WORD` `FIND` は `PARSE-NAME` + `SFIND` に置換
- ❌ `FORGET` / `MARKER` / `VOCABULARY` 系 (単一名前空間)

---

## 3. メモリマップ

```
$0100..$1FFF  kernel code + built-in dictionary  (~8 KB)
$2000..$9FFF  user dictionary (HERE grows upward, ~32 KB)
$A000..$A07F  TIB (terminal input buffer, 128 bytes)
$A080..$AFFF  未使用
$B000..$BFFE  data stack (U starts at $BFFE, grows down)
$BFFF..$BFFF  ガード
$C000..$FEFE  return stack (S starts at $FEFE)
$FF00/$FF01   ACIA SR/CR, RDR/TDR
$FFFE/$FFFF   reset vector → cold
```

- **Cell size**: 16-bit、ビッグエンディアン
- **トークン最大長**: 31 文字 (`F_LENMASK = $1F`)
- **辞書**: 最新 entry を先頭とするリンクリスト、各 entry が次の
  `LATEST` ポインタを持つ

---

## 4. 辞書エントリ構造

各辞書エントリのレイアウト (一般的な figForth 系と同じ):

```
+---------+-----------+----------+-----------+-----------+------------+
| LINK 2B | FLAGS 1B  | NAME N B | (padding) | CFA 2B    | body ...   |
+---------+-----------+----------+-----------+-----------+------------+
   ^                                          ^
   LATEST が指す                              xt (execution token)
```

- **LINK**: 前のエントリの LINK フィールド番地 (NIL 終端)
- **FLAGS**: `F_IMMED` (bit 7) / `F_HIDDEN` (bit 6) / **name length**
  (bits 0..4、最大 31)
- **NAME**: 生文字列 (null 終端なし、長さは FLAGS から取得)
- **CFA**: primitive ならネイティブコードアドレス、colon 定義なら
  DOCOL アドレス
- **body**:
  - Primitive の場合: ネイティブコード本体
  - Colon definition の場合: CFA のリスト (最後は `EXIT`)
  - VARIABLE: DOVAR に続き 2 バイトのセル領域
  - CONSTANT: DOCON に続き 2 バイトの定数値

### 4.1 xt (execution token) について

ANS Forth の xt に相当する値として **CFA** を使用。`EXECUTE` は TOS を
CFA として扱い、`jmp [,x]` 相当を実行。

---

## 5. 内側インタプリタ

### 5.1 NEXT / DOCOL / EXIT

```asm
NEXT:       ldy ,x++         ; W = *IP; IP += 2
            jmp [,y]         ; indirect jump through CFA → code

DOCOL:      pshs x           ; push old IP onto return stack
            leax 2,y         ; IP = CFA + 2 (body address)
            jmp NEXT

EXIT:       puls x           ; pop IP from return stack
            jmp NEXT
```

- **IP (X)**: 実行中のスレッド内位置
- **W (Y)**: 現在解釈中の CFA
- colon definition の本体は CFA の連続列 (`fdb cfa_word_1, cfa_word_2, ...`)
- `DOCOL` で IP を push してネスト、`EXIT` で pop

### 5.2 DOVAR / DOCON

- **DOVAR**: PFA (CFA + 2) をスタックに push → VARIABLE
- **DOCON**: `*PFA` をスタックに push → CONSTANT

### 5.3 NEXT の実行コスト

1 語あたり:
- `ldy ,x++` : 8 cycles (postbyte indirect with autoinc)
- `jmp [,y]` : 7 cycles (indirect JMP)
- primitive の場合、本体は ~10-30 cycle
- 合計 ~30 cycle/word

---

## 6. プリミティブカテゴリ

### 6.1 スタック操作
`DUP` `DROP` `SWAP` `OVER` `ROT` `>R` `R>` `R@`

### 6.2 算術・論理
`+` `-` `*` `/` `MOD` `/MOD`
`1+` `1-` `2+` `2-` `2*` `2/`
`NEGATE` `ABS` `MIN` `MAX`
`AND` `OR` `XOR` `INVERT` `NOT`
`0=` `0<` `=` `<>` `<` `>`

### 6.3 メモリ
`@` `!` `C@` `C!`

### 6.4 入出力
`EMIT` `KEY` `CR` `SPACE` `TYPE` `COUNT` `.`

### 6.5 辞書・変数
`HERE` `,` `C,` `ALLOT` `STATE` `LATEST` `>IN` `#TIB`

### 6.6 Outer interpreter building blocks
`ACCEPT` `PARSE-NAME` `SFIND` `NUMBER?` `INTERPRET` `EXECUTE`

### 6.7 コンパイル用内部プリミティブ
`(LIT)` `(BRANCH)` `(0BRANCH)` `(LITSTR)` `EXIT`

### 6.8 定義語・制御構造
`:` `;` (IMMEDIATE) `VARIABLE` `CONSTANT`
`IF` `ELSE` `THEN` (IMMEDIATE) `BEGIN` `UNTIL` `AGAIN` (IMMEDIATE)
`."` `(` (IMMEDIATE)

### 6.9 REPL
`QUIT`

---

## 7. 実装解説

### 7.1 SFIND — 辞書検索

```
SFIND ( c-addr u -- xt flag )
  flag: 0 = not found
        1 = regular word
        2 = IMMEDIATE word
```

LATEST から LINK を辿り、各エントリの NAME と引数文字列を比較。
`F_HIDDEN` が立っているエントリはスキップ (smudge による自己参照防止)。

### 7.2 NUMBER? — 数値パーサ

- 10 進数のみ、符号付き 16-bit
- 先頭 `-` で負数化
- 数字以外があれば `flag = 0` (失敗)
- 成功時 `flag = -1`、TOS に値

### 7.3 `:` の実装

```forth
:   ( "name" -- )
    create-header-with-CFA-pointing-to-DOCOL
    set F_HIDDEN  ( 再帰時の自己参照を防ぐ )
    STATE = 1
;   ( IMMEDIATE )
    compile EXIT-CFA
    clear F_HIDDEN
    STATE = 0
```

コンパイル中は数値・非 IMMEDIATE word は `,` で dictionary に書かれ、
IMMEDIATE word は即実行されて制御構造のパッチが入る。

### 7.4 制御構造 (IF / BEGIN 系)

- `IF`: compile `(0BRANCH)` + placeholder offset、placeholder 位置を
  data stack に push
- `THEN`: pop placeholder 位置、現在の HERE との差分を patch
- `ELSE`: compile `(BRANCH)` + new placeholder、IF の placeholder を patch、
  新 placeholder を push
- `BEGIN`: HERE を push
- `UNTIL`: compile `(0BRANCH)` + (BEGIN - here) の負オフセット
- `AGAIN`: compile `(BRANCH)` + 同様の負オフセット

### 7.5 `."` 文字列リテラル

- コンパイル時: `(LITSTR)` + 長さ 1 byte + 文字列バイト を dictionary に
- 実行時: `(LITSTR)` が長さ byte を読み、文字列を TYPE、IP を文字列末尾に
  進める

### 7.6 コメント `(`

- IMMEDIATE word、`)` まで `PARSE` で読み飛ばす
- コンパイルモード中も対話モード中も機能

### 7.7 ACCEPT の特徴

- 文字即エコー
- `BS` (0x08) / `DEL` (0x7F): 1 文字削除、`BS SPACE BS` でコンソール表示
  も削除
- `CR` / `LF`: 行確定、CRLF を出力、`#TIB` 更新

---

## 8. スタックサイズと限界

| 領域 | サイズ | 上限 |
|---|---|---|
| Data stack | $BFFE → $B000 = 4 KB | 2048 セル |
| Return stack | $FEFE → $C000 = 16 KB | 8192 セル (BSR 戻り番地と共用) |
| TIB | 128 bytes | 1 行 |
| User dict | $2000 → $9FFF = 32 KB | 十分な余裕 |

両スタックとも底に向かって伸長、衝突検出は無し。オーバーフローすると
他領域を破壊し得るので、常識的な深さで使用。

---

## 9. em6809 への依存

このカーネルは MC6809 の以下の命令セットに依存:

- 基本命令 (ABX を除く): ADD/SUB/ADC/SBC/AND/OR/EOR 等
- 16-bit: LDD/STD/ADDD/SUBD/CMPD、LDX/LDY/LDU、STX/STY/STU
- Indexed: `,X`, `,X++`, `,--X`, `d,X`
- Indirect: `[,Y]` (DOCOL で使用)
- 分岐: BSR/LBSR, BRA/LBRA, Bcc/LBcc
- スタック: PSHS/PULS with register set
- `LEAX` `LEAY` `LEAS` `LEAU`

**実装初期に発見した em6809 クレートのバグ** (いずれも upstream で
修正済):

- LEAS と LEAU の効果が入れ替わっていた
- `ABX` が未実装 → Forth が ABX を使わないように調整
- `TST <mem>` / `INC <mem>` の一部が未実装 → 同上

---

## 10. 実装統計

### 10.1 ファイル構成

```
forth.asm  1,535 lines
  ├ equ / 定数            1-45
  ├ cold / banner / puts  45-90
  ├ NEXT / DOCOL / EXIT   90-120
  ├ DOVAR / DOCON         100-120
  ├ stack primitives     120-260
  ├ arithmetic           260-400
  ├ memory access        400-470
  ├ I/O (EMIT/KEY/etc.)  470-580
  ├ dict primitives      580-720
  ├ ACCEPT / PARSE-NAME  720-870
  ├ SFIND                870-1000
  ├ NUMBER?              1000-1100
  ├ INTERPRET            1100-1220
  ├ compile primitives   1220-1340
  ├ control structures   1340-1450
  └ built-in dictionary  1450-1535 (LATEST chain)
```

### 10.2 スタートアップ消費

- Cold boot 直後の HERE: `$2000` (ユーザ辞書は空)
- ビルトイン辞書サイズ: 約 1.5 KB (binary 2.6 KB のうちコードが 1.1 KB)

---

## 11. 今後の改善候補

| 項目 | 予想コスト | 備考 |
|---|---|---|
| `DO` / `LOOP` / `+LOOP` | 4-6 h | カウント付きループ |
| `BASE` 切替 (16 進 / 2 進) | 2-3 h | NUMBER? と `.` を拡張 |
| 大文字小文字非依存検索 | 1-2 h | SFIND 内の比較を改造 |
| `WORDS` (辞書ダンプ) | 1 h | LATEST から walk |
| `FORGET` / `MARKER` | 3-4 h | LATEST と HERE をセーブ |
| Floating point (Q8.8 or IEEE) | 大 | 用途次第 |
| Metacompiler / Target compiler | 大 | セルフホスト向け |

---

## 12. ライセンス

MIT OR Apache-2.0 (デュアルライセンス)。詳細は `forth.asm` の SPDX
ヘッダを参照してください。
