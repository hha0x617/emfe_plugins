# Hha Forth — 言語仕様と実装解説

本ドキュメントは `examples/forth/forth.asm` の**言語仕様** (Forth 系処理系
としての特徴) と**実装上の設計** (スレッディング方式、辞書構造、内側
インタプリタ等) をまとめます。ユーザ向けの使い方は
[../README_ja.md](../README_ja.md) を参照してください。

---

## 1. 実装規模

| 指標 | 値 |
|---|---|
| ソース行数 | **4,561 行** (単一ファイル `forth.asm`) |
| 生バイナリ | **7,955 bytes** (≒ 7.8 KB) |
| SREC ファイル | 22,034 bytes (ASCII 形式) |
| CFA (code-field address) | **175 個** — primitive + colon definition の総計 |
| FORTH-83 Required Word Set カバー率 | **約 95%** |
| Smoke test | **7 件** (全件 passing) |

Hha Lisp (18.5 KB) の半分以下のサイズながら、FORTH-83 Required
Word Set の大半をカバー。コロン定義、`IF`/`ELSE`/`THEN`、
`BEGIN`/`UNTIL`/`AGAIN`/`WHILE`/`REPEAT`、`DO`/`LOOP`/`+LOOP`、
`CREATE`/`DOES>`、実行時基数切替、混合精度/倍精度演算、
絵的数値出力、文字列操作、`FORGET`/`MARKER`、`ABORT"` まで対応。
古典的な figForth / jonesforth クラスの「最小で実用」を狙った
処理系です。

---

## 2. Forth 系処理系としての特徴

### 2.1 採用している方式

- **Indirect-threaded code (ITC)**: 各辞書エントリの CFA は
  「ネイティブコードへのポインタ (primitive)」または「DOCOL へのポインタ
  (colon definition)」。NEXT で 2 段の間接ジャンプ。
- **Separate data / return stacks**: U = data stack、S = return stack。
  どちらも下方向伸長、TOS は低番地側。
- **16-bit cell**: すべて符号付き 16-bit。
- **倍精度セル**: 32-bit。メモリ上は 低位 @ `addr` / 高位 @ `addr+2`、
  スタック上は `( low high )` で高位が TOS。
- **実行時基数**: `BASE` (デフォルト 10) を `NUMBER?` と全出力系ワードが
  参照。

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

### 2.4 FORTH-83 / ANS Forth 互換性

**カバー済み (FORTH-83 Required Word Set の約 95%)**:
- ✅ 制御構造: `:` `;` `IF` `THEN` `ELSE` `BEGIN` `UNTIL` `AGAIN`
      `WHILE` `REPEAT` `DO` `LOOP` `+LOOP` `I` `J` `LEAVE` `UNLOOP`
      `."` `S"` `ABORT"` `(` `\`
- ✅ 定義系: `VARIABLE` `CONSTANT` `CREATE` `DOES>` `ALLOT` `,` `C,`
      `HERE` `LATEST` `IMMEDIATE` `LITERAL` `RECURSE` `POSTPONE`
      `FORGET` `MARKER` `'` `[']` `CHAR` `[CHAR]`
- ✅ スタック: `DUP` `?DUP` `DROP` `SWAP` `OVER` `NIP` `TUCK` `ROT`
      `-ROT` `PICK` `ROLL` `DEPTH`
      `2DUP` `2DROP` `2SWAP` `2OVER` `>R` `R>` `R@`
- ✅ メモリ: `@` `!` `+!` `C@` `C!` `2@` `2!` `CELL+` `CELLS`
      `ALIGN` `ALIGNED` `CMOVE` `CMOVE>` `MOVE` `FILL` `ERASE` `BLANK`
- ✅ 文字列: `COUNT` `COMPARE` `/STRING` `-TRAILING`
- ✅ 算術: `+` `-` `*` `/` `MOD` `/MOD` `1+` `1-` `2+` `2-`
      `2*` `2/` `LSHIFT` `RSHIFT` `NEGATE` `ABS` `MIN` `MAX`
- ✅ 論理: `AND` `OR` `XOR` `INVERT` `NOT`
      `0=` `0<` `0>` `=` `<>` `<` `>` `U<` `U>`
- ✅ 定数・変数: `TRUE` `FALSE` `BL` `BASE` `HEX` `DECIMAL`
- ✅ 数値出力: `.` `U.` `.R` `U.R` `D.` `D.R` `SPACES`
      `<#` `#` `#S` `#>` `HOLD` `SIGN`
- ✅ 混合/倍精度: `M+` `UM*` `M*` `UM/MOD` `SM/REM` `FM/MOD`
      `*/` `*/MOD` `D+` `D-` `DNEGATE` `DABS`
- ✅ エラー処理: `ABORT` `ABORT"`
- ✅ デバッグ: `.S` `WORDS` `DUMP`

**意図的に除外 (価値が低い・セキュリティ懸念あり)**:
- ❌ `SP@` / `SP!` / `RP@` / `RP!` (スタックポインタ操作)
- ❌ `EXPECT` / `QUERY` (`ACCEPT` と冗長)
- ❌ `VOCABULARY` / `DEFINITIONS` / `ONLY` (単一名前空間)
- ❌ `WORD` + `FIND` — `WORD` は提供、`FIND` は `PARSE-NAME` + `SFIND`
      で代替
- ❌ 大容量記憶ワード (`BLOCK` / `BUFFER` / `UPDATE` / `SAVE-BUFFERS`)
      — このハードウェア構成では非対応

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
`DUP` `?DUP` `DROP` `SWAP` `OVER` `NIP` `TUCK` `ROT`
`-ROT` `PICK` `ROLL` `DEPTH` `2DUP` `2DROP` `2SWAP` `2OVER`
`>R` `R>` `R@`

### 6.2 算術・論理 (16-bit)
`+` `-` `*` `/` `MOD` `/MOD`
`1+` `1-` `2+` `2-` `2*` `2/` `LSHIFT` `RSHIFT`
`NEGATE` `ABS` `MIN` `MAX`
`AND` `OR` `XOR` `INVERT` `NOT`
`0=` `0<` `0>` `=` `<>` `<` `>` `U<` `U>`

### 6.3 混合精度・倍精度
`2@` `2!` `D+` `D-` `DNEGATE` `DABS` `D.` `D.R`
`M+` `UM*` `M*` `UM/MOD` `SM/REM` `FM/MOD` `*/` `*/MOD`

### 6.4 定数
`TRUE` `FALSE` `BL`

### 6.5 メモリ
`@` `!` `+!` `C@` `C!` `CELL+` `CELLS`
`ALIGN` `ALIGNED` `CMOVE` `CMOVE>` `MOVE` `FILL` `ERASE` `BLANK`

### 6.6 文字列
`COMPARE` `/STRING` `-TRAILING`

### 6.7 入出力・フォーマット
`EMIT` `KEY` `CR` `SPACE` `SPACES` `TYPE` `COUNT`
`.` `U.` `.R` `U.R` `DUMP`
`<#` `#` `#S` `#>` `HOLD` `SIGN`

### 6.8 基数制御
`BASE` `HEX` `DECIMAL`

### 6.9 辞書・変数
`HERE` `,` `C,` `ALLOT` `STATE` `LATEST` `>IN` `#TIB`

### 6.10 Outer interpreter building blocks
`ACCEPT` `PARSE-NAME` `WORD` `SFIND` `NUMBER?`
`INTERPRET` `EXECUTE` `'` `CHAR` `[CHAR]`

### 6.11 コンパイル用内部プリミティブ
`(LIT)` `(BRANCH)` `(0BRANCH)` `(LITSTR)` `(SLITERAL)`
`(DO)` `(LOOP)` `(+LOOP)` `(;DOES)` `(ABORT")` `EXIT`

### 6.12 定義語・制御構造
`:` `;` (IMMEDIATE) `VARIABLE` `CONSTANT` `CREATE` `DOES>`
`IMMEDIATE` `LITERAL` `RECURSE` `POSTPONE` `[']`
`IF` `ELSE` `THEN` (IMMEDIATE)
`BEGIN` `UNTIL` `AGAIN` `WHILE` `REPEAT` (IMMEDIATE)
`DO` `LOOP` `+LOOP` (IMMEDIATE) `I` `J` `LEAVE` `UNLOOP`
`FORGET` `MARKER`
`."` `S"` `ABORT"` `(` `\` (IMMEDIATE)

### 6.13 エラー処理
`ABORT` `ABORT"`

### 6.14 デバッグ
`.S` `WORDS`

### 6.15 REPL
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

- 符号付き 16-bit、基数は呼び出し時の `BASE` を参照
- 数字 `0`–`9` と英字 `A`–`Z` (大小両方) を受け付け、各桁値が
  `BASE` 未満であることを検査
- 先頭 `-` で負数化
- 範囲外の文字が 1 つでもあれば `flag = 0` (失敗)
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
- `WHILE`: compile `(0BRANCH)` + placeholder、その位置を覚えておく
- `REPEAT`: compile `(BRANCH)` + BEGIN への負オフセット、そのあとで
  `WHILE` の placeholder を patch して条件偽時は `REPEAT` の先へ飛ばす

### 7.5 `DO` / `LOOP` / `+LOOP`

- `DO`: `(DO)` を compile、戻り先パッチ用に HERE を push
- `(DO)` (runtime): data stack から limit と start を pop し、
  **return stack** に (limit, index) の順で push (index が TOS 側)。
  そのため `I` は `ldd 0,s` で index を読める
- `LOOP`: `(LOOP)` + `DO` 時の HERE への負オフセットを compile
- `(LOOP)`: index をインクリメント、limit と一致したら両方捨てて
  フォールスルー、それ以外は分岐
- `+LOOP`: `LOOP` と同じ構造で、増分は TOS (符号付き)。
  limit をまたいだら脱出
- `I` / `J`: 内側 / 外側ループの index を読む (return stack の
  `0,s` と `4,s`)
- `LEAVE`: `index := limit` を設定するのみ。実際の脱出は次の
  `LOOP` / `+LOOP` で起きる (即時脱出ではない)。

### 7.6 `."` / `S"` 文字列リテラル

- コンパイル時: ランタイム (`."` は `(LITSTR)`、`S"` は `(SLITERAL)`)
  + 長さ 1 byte + 文字列バイトを dictionary に
- `(LITSTR)`: 長さを読んで文字列を `TYPE`、IP を末尾に進める
- `(SLITERAL)`: 長さを読んで `( addr u )` を push、IP を末尾に進める

### 7.7 コメント

- `(` は IMMEDIATE、TIB を `)` まで読み飛ばす。REPL とコロン定義中の両方で
  機能
- `\` は IMMEDIATE、`>IN` を行末まで進める

### 7.8 ACCEPT の特徴

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
forth.asm  3,331 lines  (セクション境界は目安)
  ├ equ / 定数
  ├ cold / banner / puts
  ├ NEXT / DOCOL / EXIT / DOVAR / DOCON
  ├ stack primitives (?DUP / NIP / TUCK / PICK / 2DUP / ... を含む)
  ├ arithmetic (16-bit、divmod、シフト)
  ├ mixed-precision / double  (UM* / M* / UM/MOD / */ / */MOD /
  │                             D+ / D- / DNEGATE / DABS / D.)
  ├ memory access (@ / ! / +! / CMOVE / FILL / 2@ / 2!)
  ├ I/O・フォーマット (EMIT / ... / . / U. / .R / U.R / DUMP / SPACES)
  ├ BASE / HEX / DECIMAL (と BASE 対応 NUMBER? + fmt_sd / fmt_ud)
  ├ dict primitives と state variables
  ├ ACCEPT / PARSE-NAME
  ├ SFIND / sfind_kernel (' と共用)
  ├ NUMBER?
  ├ INTERPRET
  ├ compile primitives ((LIT) / (BRANCH) / (0BRANCH) / (LITSTR) /
  │                      (SLITERAL) / (DO) / (LOOP) / (+LOOP))
  ├ control structures (: / ; / IMMEDIATE / LITERAL / RECURSE /
  │                      IF / ELSE / THEN /
  │                      BEGIN / UNTIL / AGAIN / WHILE / REPEAT /
  │                      DO / LOOP / +LOOP / I / J / LEAVE /
  │                      ." / S" / ( / \)
  ├ debug (.S / WORDS)
  └ built-in dictionary (LATEST chain、137 CFAs)
```

### 10.2 スタートアップ消費

- Cold boot 直後の HERE: `$2000` (ユーザ辞書は空)
- 5.7 KB のバイナリに、コードとビルトイン辞書が収まる

---

## 11. 今後の改善候補

| 項目 | 予想コスト | 備考 |
|---|---|---|
| 絵的数値出力 (`<# # #S #> HOLD SIGN`) | 2-3 h | `D.` / `.R` の補完 |
| 大文字小文字非依存検索 | 1-2 h | SFIND 内の比較を改造 |
| `FORGET` / `MARKER` | 3-4 h | LATEST と HERE をセーブ |
| 文字列系 `COMPARE` / `/STRING` / `-TRAILING` | 2-3 h | メモリ系の拡張 |
| ブロックストレージ | 中 | ファイルレス永続化 |
| Floating point (Q8.8 or IEEE) | 大 | 用途次第 |
| Metacompiler / Target compiler | 大 | セルフホスト向け |

---

## 12. ライセンス

MIT OR Apache-2.0 (デュアルライセンス)。詳細は `forth.asm` の SPDX
ヘッダを参照してください。
