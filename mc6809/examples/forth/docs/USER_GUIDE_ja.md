# Hha Forth — ユーザーガイド

`forth.asm` は em6809 / emfe_plugin_mc6809 環境で動くコンパクトな ITC
Forth です。ACIA (MC6850) 1 本の上で対話的に動作し、
`:` によるコロン定義、`IF`/`ELSE`/`THEN`、
`BEGIN`/`UNTIL`/`AGAIN`/`WHILE`/`REPEAT`、
`DO`/`LOOP`/`+LOOP`、`VARIABLE` / `CONSTANT`、
実行時基数切替 (`HEX` / `DECIMAL`)、
混合精度・倍精度演算 (`UM*`, `M*`, `UM/MOD`, `*/`, `*/MOD`,
`D+`, `D-`, `D.` 等)、文字列リテラル `."` / `S"`、
ブロックコメント `(` と行コメント `\` まで使えます。

処理系そのものの実装 (ITC スレッディング方式、辞書エントリ構造、
内側インタプリタ、実装規模) の詳細は
**[LANGUAGE_AND_IMPL_ja.md](LANGUAGE_AND_IMPL_ja.md)** を参照してください。

English: [USER_GUIDE.md](USER_GUIDE.md)

---

## 1. ビルドと起動

### ビルド

[lwasm](http://www.lwtools.ca/) (lwtools 同梱) が PATH にあることを前提:

```sh
lwasm -9 -f srec -o forth.s19 forth.asm
```

- `-9` は MC6809 モード、`-f srec` は Motorola S-record 出力。
- 出力ファイル `forth.s19` をプラグインで読み込みます。
- PATH に無い場合はフルパスで指定 (例: `C:\path\to\lwasm.exe` /
  `/usr/local/bin/lwasm`)。

### ホストからのロード (Rust 例)

```rust
emfe_create(&mut handle);
emfe_set_console_char_callback(handle, Some(tx_cb), std::ptr::null_mut());
let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
emfe_load_srec(handle, path.as_ptr());
emfe_run(handle);

// キー入力を送る
emfe_send_char(handle, b'4' as c_char);
// ...
```

起動すると次のバナーが出て、ACCEPT が入力待ちに入ります：

```
Hha Forth for MC6809 ready.
```

---

## 2. メモリマップ

| 範囲 | 用途 |
|------|------|
| `$0100..$27FF` | カーネルコード + 組込辞書 |
| `$2800..$9FFF` | ユーザ辞書 (`HERE` が伸びる先) |
| `$A000..$A1FF` | TIB (端末入力バッファ、512 バイト) |
| `$B000..$BFFE` | データスタック (U、下方向伸長、TOS が低番地) |
| `$C000..$FEFE` | リターンスタック (S) |
| `$FF00/$FF01` | ACIA SR/CR, RDR/TDR |
| `$FFFE/$FFFF` | リセットベクタ → `cold` |

- **大文字小文字は区別します**。辞書検索は case-sensitive。
- セルは 16-bit (ビッグエンディアン)。
- トークンの最大長は 31 文字 (flags+length バイトの低 5 ビット)。

---

## 3. REPL の動作

1. `ACCEPT` が TIB に 1 行 (最大 512 バイト) 読み込む。
   - 文字は即エコーされる。
   - `BS` (0x08) / `DEL` (0x7F) で 1 文字削除、`BS SPACE BS` 表示。
   - `CR` または `LF` で入力確定、`CRLF` を出力。
2. `#TIB` に長さを、`>IN` に 0 を書く。
3. `INTERPRET` が `PARSE-NAME` → `SFIND` / `NUMBER?` を順に呼び、
   トークンごとに実行 or コンパイル。
4. 行末到達で " ok" + `CRLF` を表示して 1. に戻る。
5. 不明語は `<word>?` を表示して継続（エラーで REPL は落ちない）。

コンパイルモード (`STATE @` が非ゼロ、`:` で入って `;` で抜ける) 中は、
IMMEDIATE ワード以外は `xt` が辞書に積まれ、数値は `(LIT) value` として
埋め込まれます。

---

## 4. 使える語 (組込辞書)

### 4.1 スタック操作
| 語 | 効果 | 説明 |
|----|------|------|
| `DUP` | `( a -- a a )` | TOS を複製 |
| `?DUP` | `( a -- a a \| 0 )` | TOS が非ゼロの時だけ DUP |
| `DROP` | `( a -- )` | TOS を捨てる |
| `SWAP` | `( a b -- b a )` | TOS と次を入れ替え |
| `OVER` | `( a b -- a b a )` | 2 番目をコピー |
| `NIP` | `( a b -- b )` | NOS を捨てる |
| `TUCK` | `( a b -- b a b )` | TOS を NOS の下にコピー |
| `ROT` | `( a b c -- b c a )` | 3 番目を TOS へ回す |
| `-ROT` | `( a b c -- c a b )` | `ROT` の逆方向 |
| `PICK` | `( xn … x0 n -- xn … x0 xn )` | `0 PICK` ≡ `DUP`、`1 PICK` ≡ `OVER`… |
| `ROLL` | `( xn … x0 n -- xn-1 … x0 xn )` | n 番目をスタック先頭へ |
| `DEPTH` | `( -- n )` | 現在のデータスタック深さ |
| `2DUP` | `( a b -- a b a b )` | 上位 2 セルを複製 |
| `2DROP` | `( a b -- )` | 上位 2 セルを捨てる |
| `2SWAP` | `( a b c d -- c d a b )` | 2 セル組を入れ替え |
| `2OVER` | `( a b c d -- a b c d a b )` | NOS の 2 セル組をコピー |
| `>R` | `( n -- ) (R: -- n)` | データ→リターン |
| `R>` | `( -- n ) (R: n -- )` | リターン→データ |
| `R@` | `( -- n ) (R: n -- n)` | リターン TOS を参照 |
| `SP@` `SP!` | `( -- addr )` / `( addr -- )` | データスタックポインタの取得 / 設定 |
| `RP@` `RP!` | `( -- addr )` / `( addr -- )` | リターンスタックポインタの取得 / 設定 |

### 4.2 算術・論理 (16-bit signed)
| 語 | 効果 |
|----|------|
| `+` `-` | 加減算 |
| `*` | `( a b -- a*b )` 16bit 乗算、下位 16bit のみ (符号の有無に依らず同値) |
| `/` | `( a b -- a/b )` 符号付き除算、0 方向への切り捨て |
| `MOD` | `( a b -- a mod b )` 符号付き剰余、符号は被除数に従う |
| `/MOD` | `( a b -- rem quot )` 除算まとめ; `/` `MOD` は各々片方を捨てる薄いラッパ |
| `1+` `1-` | `( n -- n±1 )` 1 加算 / 減算 |
| `2+` `2-` | `( n -- n±2 )` 2 加算 / 減算 |
| `2*` | `( n -- n*2 )` 算術左シフト 1 bit |
| `2/` | `( n -- n/2 )` FORTH-83 算術 (符号拡張) 右シフト 1 bit |
| `LSHIFT` `RSHIFT` | `( x u -- x' )` 論理左 / 右シフト `u` ビット |
| `NEGATE` | 2's complement |
| `ABS` | `( n -- \|n\| )` 絶対値 |
| `MIN` `MAX` | `( a b -- m )` 符号付き最小値 / 最大値 |
| `AND` `OR` `XOR` | ビット演算 |
| `INVERT` | 1's complement (ビット NOT) |
| `NOT` | `( flag -- !flag )` FORTH-83 の論理反転 (`0=` のエイリアス) |
| `0=` | `( n -- flag )` ゼロなら `-1`、他は `0` |
| `0<` | `( n -- flag )` 負なら `-1` |
| `0>` | `( n -- flag )` 正なら `-1` |
| `=` `<>` | 等値 / 非等値、`-1` / `0` を返す |
| `<` `>` | 符号付き大小比較、`-1` / `0` を返す |
| `U<` `U>` | 符号なし大小比較、`-1` / `0` を返す |

ゼロ除算は例外にせず `/`・`MOD`・`/MOD`・`UM/MOD` および混合精度演算で
`rem = a` (符号無し版は `0`)、`quot = 0` を返します
(カーネルの「誤入力は静かに失敗する」方針に合わせた挙動です)。

### 4.3 定数

| 語 | 効果 |
|----|------|
| `TRUE` | `( -- -1 )` 真フラグの正準値 |
| `FALSE` | `( -- 0 )` 偽フラグの正準値 |
| `BL` | `( -- 32 )` ASCII 空白文字 |

### 4.4 混合精度・倍精度演算

倍精度数はスタック上で 2 セル (`d` は `( d-low d-high )`、TOS が上位)。
`2@` / `2!` でのメモリ配置は低位アドレスに下位ワード、
`addr+2` に上位ワード。

| 語 | スタック効果 | 備考 |
|----|------|------|
| `2@` | `( addr -- d )` | 倍精度フェッチ |
| `2!` | `( d addr -- )` | 倍精度ストア |
| `D+` | `( d1 d2 -- d )` | 倍精度加算 |
| `D-` | `( d1 d2 -- d )` | 倍精度減算 |
| `DNEGATE` | `( d -- -d )` | 倍精度 2 の補数 |
| `DABS` | `( d -- \|d\| )` | 倍精度絶対値 |
| `D.` | `( d -- )` | 現在の `BASE` で符号付き倍精度印字 |
| `D.R` | `( d w -- )` | 符号付き倍精度を幅 `w` で右詰め印字 |
| `UM*` | `( u1 u2 -- ud )` | 符号なし 16×16→32 乗算 |
| `M*` | `( n1 n2 -- d )` | 符号付き 16×16→32 乗算 |
| `M+` | `( d n -- d' )` | 倍精度に単精度を加算 |
| `UM/MOD` | `( ud u -- urem uquot )` | 符号なし 32÷16 除算 |
| `SM/REM` | `( d n -- rem quot )` | 対称 (truncate) 符号付き除算 |
| `FM/MOD` | `( d n -- rem quot )` | floor 符号付き除算 |
| `M/` | `( d n -- quot )` | 倍÷単 の対称符号付き商 |
| `*/` | `( n1 n2 n3 -- n )` | `n1*n2/n3`、中間値 32bit、符号付き |
| `*/MOD` | `( n1 n2 n3 -- rem quot )` | `*/` と同じで剰余も残す |

### 4.5 メモリ
| 語 | 効果 | 説明 |
|----|------|------|
| `@` | `( addr -- w )` | 16-bit フェッチ |
| `!` | `( w addr -- )` | 16-bit ストア |
| `+!` | `( n addr -- )` | `addr` のセルに `n` を加算 |
| `C@` | `( addr -- b )` | 8-bit フェッチ |
| `C!` | `( b addr -- )` | 8-bit ストア |
| `CELL+` | `( addr -- addr+2 )` | 1 セル分進める |
| `CELLS` | `( n -- n*2 )` | セル数をバイト数に変換 |
| `ALIGN` | `( -- )` | HERE をセル境界にアラインする |
| `ALIGNED` | `( addr -- addr' )` | `addr` を次のセル境界へ切り上げ |
| `CMOVE` | `( src dst u -- )` | `u` バイトを低位→高位にコピー |
| `CMOVE>` | `( src dst u -- )` | `u` バイトを高位→低位にコピー (`dst>src` でも安全) |
| `MOVE` | `( src dst u -- )` | 重なりに応じて方向を選んでコピー |
| `FILL` | `( addr u byte -- )` | `addr` から `u` バイトを `byte` で埋める |
| `ERASE` | `( addr u -- )` | `0 FILL` |
| `BLANK` | `( addr u -- )` | `BL FILL` |
| `COUNT` | `( c-addr -- addr u )` | カウント済み文字列を addr/len に展開 |
| `COMPARE` | `( a1 u1 a2 u2 -- n )` | 辞書順比較、-1 / 0 / 1 を返す |
| `/STRING` | `( addr u n -- addr' u' )` | 文字列の先頭から `n` バイト切り捨て |
| `-TRAILING` | `( addr u -- addr u' )` | 末尾の空白を削除 |

### 4.6 入出力と数値フォーマット
| 語 | 効果 |
|----|------|
| `EMIT` | `( c -- )` 1 文字出力 |
| `KEY` | `( -- c )` 1 文字入力 (ブロッキング) |
| `CR` | `CRLF` 出力 |
| `SPACE` | スペース 1 文字出力 |
| `SPACES` | `( n -- )` スペースを `n` 個出力 (0 以下なら何もしない) |
| `TYPE` | `( addr u -- )` 文字列出力 |
| `COUNT` | `( c-addr -- addr u )` カウント済み文字列を addr/len に |
| `.` | `( n -- )` 現在の `BASE` で符号付き印字 (末尾スペース付き) |
| `U.` | `( u -- )` 符号なし版 |
| `.R` | `( n w -- )` 符号付き `n` を幅 `w` で右詰め印字 (末尾スペースなし) |
| `U.R` | `( u w -- )` 符号なし版の右詰め |
| `DUMP` | `( addr u -- )` `addr` から `u` バイトを 16 バイト/行で 16 進ダンプ |

**Pictured Numeric Output** (HOLD バッファに 1 文字ずつ書き戻して
組み立てる手動数値整形。組立中は逆順に並んでおり `#>` で取り出す):

| 語 | スタック効果 | 説明 |
|----|------|------|
| `<#` | `( -- )` | 整形セッション開始 |
| `#` | `( ud -- ud' )` | 現在の `BASE` で 1 桁変換 |
| `#S` | `( ud -- 0 0 )` | `ud` がゼロになるまで桁変換 |
| `HOLD` | `( c -- )` | リテラル文字を 1 つ挿入 |
| `SIGN` | `( n -- )` | `n` が負なら `-` を挿入 |
| `#>` | `( ud -- addr u )` | セッション終了、`( addr u )` を返す |

### 4.7 基数制御

| 語 | 効果 |
|----|------|
| `BASE` | `( -- addr )` 現在の入出力基数 (セル、デフォルト 10) |
| `HEX` | `BASE` を 16 に設定 |
| `DECIMAL` | `BASE` を 10 に設定 |

`NUMBER?` は `BASE > 10` のとき `A`–`Z` (大小どちらでも) を受け付けます。
`.`, `U.`, `.R`, `U.R`, `D.`, `DUMP` も `BASE` に従います。

### 4.8 辞書・変数
| 語 | 効果 | 備考 |
|----|------|------|
| `HERE` | `( -- addr )` 辞書の次の空き番地 |
| `,` | `( w -- )` HERE に 16-bit を書いて HERE+=2 |
| `C,` | `( b -- )` HERE に 8-bit を書いて HERE+=1 |
| `ALLOT` | `( n -- )` HERE を n バイト進める |
| `STATE` | `( -- addr )` コンパイルモード変数の番地 |
| `LATEST` | `( -- addr )` 最新辞書エントリの番地 |
| `>IN` | `( -- addr )` TIB 内の次読み取り位置 |
| `#TIB` | `( -- addr )` TIB 内有効バイト数 |

### 4.9 外側インタプリタのビルディングブロック
| 語 | 効果 |
|----|------|
| `ACCEPT` | `( c-addr +n1 -- +n2 )` 1 行読む、エコー付き |
| `EXPECT` | `( c-addr +n -- )` `c-addr` に読み込み、長さは `SPAN` に格納 |
| `SPAN` | `( -- addr )` 直前の `EXPECT` の長さを保持するセル |
| `QUERY` | `( -- )` `0 TIB ACCEPT` — TIB を再充填し `>IN` をクリア |
| `PARSE-NAME` | `( -- c-addr u )` 次トークン (空白区切り) |
| `WORD` | `( char "<chars>name<char>" -- c-addr )` 旧式パース語、HERE をスクラッチに |
| `SFIND` | `( c-addr u -- xt flag )` 辞書検索。flag: 0=不在、1=通常、2=IMMEDIATE |
| `FIND` | `( c-addr -- xt 1 \| xt -1 \| c-addr 0 )` ANS 風カウント済み文字列版 |
| `NUMBER?` | `( c-addr u -- value flag )` `BASE` 対応数値パーサ、成功で flag=-1 |
| `INTERPRET` | トークンを順に実行／コンパイル |
| `EXECUTE` | `( xt -- )` xt を実行 |
| `'` | `( "name" -- xt )` ワードの xt を検索 (失敗時 0) |
| `[']` IMMEDIATE | `( "name" -- )` コンパイル時 `'` — xt を定義に埋め込む |
| `CHAR` | `( "name" -- c )` 次トークンの先頭文字を push |
| `[CHAR]` IMMEDIATE | `( "name" -- )` コンパイル時 `CHAR` |

### 4.10 コンパイル用内部プリミティブ
| 語 | 説明 |
|----|------|
| `(LIT)` | 後続セルをデータスタックへ |
| `(BRANCH)` | 後続セル分だけ IP を進める/戻す |
| `(0BRANCH)` | TOS が 0 なら (BRANCH) と同じ、非ゼロならオフセットをスキップ |
| `(LITSTR)` | 後続のカウント済み文字列を印字して IP を進める (`."` 用) |
| `(SLITERAL)` | 後続のカウント済み文字列を `( addr u )` として push (`S"` 用) |
| `(DO)` `(LOOP)` `(+LOOP)` | `DO` / `LOOP` / `+LOOP` のランタイム |
| `EXIT` | コロン定義から戻る (`;` がこれをコンパイルする) |

### 4.11 定義語と制御構造
| 語 | 効果 |
|----|------|
| `:` | `( "name" -- )` 新コロン定義開始、STATE=1 |
| `;` IMMEDIATE | EXIT をコンパイル、STATE=0 |
| `VARIABLE` | `( "name" -- )` DOVAR 基づく 16-bit 変数を作る |
| `CONSTANT` | `( x "name" -- )` DOCON 基づく定数を作る |
| `CREATE` | `( "name" -- )` ランタイムで PFA を push するヘッダを作る |
| `DOES>` | `( -- )` 直前 `CREATE` した語のランタイム部を差し替える |
| `IMMEDIATE` | 直前に定義したワードを IMMEDIATE に |
| `LITERAL` IMMEDIATE | コンパイル時 `( x -- )` → `(LIT) x` をコンパイル |
| `RECURSE` IMMEDIATE | 定義中のコロン定義自身への呼び出しをコンパイル |
| `POSTPONE` IMMEDIATE | `( "name" -- )` 指定語の遅延呼び出しをコンパイル |
| `FORGET` | `( "name" -- )` `name` の手前まで HERE / LATEST を巻き戻す |
| `MARKER` | `( "name" -- )` 呼ぶと自分自身を `FORGET` する語を作る |
| `IF` `ELSE` `THEN` IMMEDIATE | `(0BRANCH)` / `(BRANCH)` + HERE パッチ |
| `BEGIN` `UNTIL` `AGAIN` IMMEDIATE | 後方ジャンプループ |
| `BEGIN` `WHILE` `REPEAT` IMMEDIATE | 条件抜け出し付きループ |
| `DO` `LOOP` `+LOOP` IMMEDIATE | カウント付きループ |
| `I` `J` | 内側 / 外側ループのインデックスを参照 |
| `LEAVE` | 現在のループを脱出 (index := limit、実際の退出は次の `LOOP`/`+LOOP`) |
| `UNLOOP` | 内側ループのリターンスタック 3 セルを破棄 (`DO ... LOOP` 中の `EXIT` 前に必須) |
| `."` IMMEDIATE | 閉じ `"` までを文字列リテラルとしてコンパイル (実行時に印字) |
| `S"` IMMEDIATE | 閉じ `"` までを文字列リテラルとしてコンパイル (実行時 `( addr u )`) |
| `ABORT` | データ／リターンスタックを空にして `QUIT` へジャンプ |
| `ABORT"` IMMEDIATE | 条件付き abort + インラインメッセージをコンパイル |
| `(` IMMEDIATE | 閉じ `)` までコメントとして読み飛ばす |
| `\` IMMEDIATE | 行末までコメントとして読み飛ばす |

### 4.12 ボキャブラリ

このカーネルは単一の `FORTH` ワードリストで動作するため、すべての定義は
1 つの名前空間を共有します。FORTH-83 でボキャブラリを使うソースとの
互換性のためにダミーが揃っており、`CONTEXT` と `CURRENT` は常に同じ
リストを指します。

| 語 | 効果 |
|----|------|
| `VOCABULARY` | `( "name" -- )` 新ボキャブラリ定義 (現状 `FORTH` のエイリアス) |
| `FORTH` | `CONTEXT` を `FORTH` に設定 |
| `CONTEXT` | `( -- addr )` 現在の検索ワードリストを保持するセル |
| `CURRENT` | `( -- addr )` 現在の定義先ワードリスト (== `CONTEXT`) |
| `DEFINITIONS` | `CURRENT` を `CONTEXT` に追従 (このビルドでは no-op) |
| `ONLY` | 検索リストを `FORTH` のみにリセット |

### 4.13 デバッグ
| 語 | 効果 |
|----|------|
| `.S` | `( -- )` スタック破壊しないダンプ: `<depth> a b c …` |
| `WORDS` | `( -- )` 辞書エントリを新しい順に全て印字 |

### 4.14 REPL
| 語 | 効果 |
|----|------|
| `QUIT` | REPL ループ本体 (起動時から走っている) |

---

## 5. 使用例

### 5.1 四則と `.`

```forth
3 4 + .           → 7  ok
10 3 - .          → 7  ok
-5 NEGATE .       → 5  ok
```

### 5.2 新語定義

```forth
: DOUBLE DUP + ;
3 DOUBLE .        → 6  ok
5 DOUBLE DOUBLE . → 20  ok
```

### 5.3 条件分岐

```forth
: ABS DUP 0< IF NEGATE THEN ;
-17 ABS .         → 17  ok
42 ABS .          → 42  ok

: SIGN DUP 0< IF DROP -1 ELSE 0= IF 0 ELSE 1 THEN THEN ;
-5 SIGN .         → -1 ok
0 SIGN .          → 0 ok
7 SIGN .          → 1 ok
```

### 5.4 BEGIN / UNTIL

```forth
( 0..n-1 のカウントダウン出力 )
: STARS BEGIN 42 EMIT 1 - DUP 0= UNTIL DROP ;
( 注: 初期値が 0/負のままだと無限ループ注意 )
```

### 5.5 変数・定数

```forth
VARIABLE CNT
0 CNT !
CNT @ . → 0  ok
42 CNT !
CNT @ . → 42  ok

100 CONSTANT MAX
MAX .  → 100  ok
```

### 5.6 文字列と注釈

```forth
: GREET ." Hello, Forth!" CR ;
GREET
→ Hello, Forth!
  ok

( これはブロックコメント — REPL は読み飛ばします )
\ これは行コメント — 行末まで読み飛ばします
```

### 5.7 DO / LOOP と倍精度

```forth
: SQUARES  10 0 DO I I * . LOOP ;
SQUARES     → 0 1 4 9 16 25 36 49 64 81  ok

( 32-bit 積: 40000 * 3 = 120000、倍精度で印字 )
40000 3 M* D.  → 120000  ok
```

### 5.8 HEX / DECIMAL

```forth
HEX  255 .     → FF  ok
     FF .      → FF  ok
DECIMAL
     FF .      → FF?             \ 10 進モードではワード扱い、数値にならない
     255 .     → 255  ok
```

### 5.9 BEGIN / WHILE / REPEAT

```forth
: COUNTDOWN  10 BEGIN DUP 0> WHILE DUP . 1- REPEAT DROP ;
COUNTDOWN   → 10 9 8 7 6 5 4 3 2 1  ok
```

### 5.10 スタック操作ヘルパ

```forth
( ?DUP でゼロ検査を簡潔に書く )
: NON-ZERO?  ?DUP IF ." yes" DROP ELSE ." no" THEN ;
42 NON-ZERO?   → yes  ok
0  NON-ZERO?   → no   ok

( PICK は DUP / OVER の一般化 )
11 22 33 44  0 PICK .   → 44  ok     \ DUP と同じ
11 22 33 44  2 PICK .   → 22  ok     \ TOS から 3 番目

( NIP は 2 番目を捨てる、TUCK は TOS を NOS の下へコピー )
1 2 3 NIP .S    → <2> 1 3     ok
1 2   TUCK .S   → <3> 2 1 2   ok

( 2DUP: 値自身を比較対象に使いたいとき便利 )
: CLAMP-LOW  ( n lo -- n' )   2DUP < IF SWAP THEN DROP ;
  5 0 CLAMP-LOW .    → 5  ok
 -3 0 CLAMP-LOW .    → 0  ok
```

### 5.11 メモリ系ヘルパ

```forth
VARIABLE COUNTER   0 COUNTER !
1 COUNTER +!       COUNTER @ .   → 1  ok
5 COUNTER +!       COUNTER @ .   → 6  ok

( VARIABLE は 2 バイトだけ。ALLOT で拡張してバッファにする )
VARIABLE BUF   14 ALLOT         \ BUF は 16 バイト
BUF 16 0 FILL                   \ 全部 0 で埋める
BUF 16 DUMP                     \ 16 進ダンプ (アドレスも BASE に従う)

( S" が ( addr u ) を push、CMOVE がバイト列を転送 )
: PUT-HELLO   S" HELLO" BUF SWAP CMOVE ;
PUT-HELLO
BUF 5 TYPE                      → HELLO  ok
```

### 5.12 倍精度値と `2@` / `2!`

```forth
VARIABLE BIG  4 ALLOT            \ BIG に倍精度 1 個分の領域

( BIG に 0x0001_2345 を格納 )
HEX  2345 1 BIG 2!  DECIMAL
BIG 2@ D.                         → 74565  ok     \ 0x00012345

( 倍精度加算: 65000 + 100 = 65100、16bit では桁あふれする計算もOK )
65000 0  100 0  D+  D.            → 65100  ok

( DABS は負の倍精度値を正にする )
-1 -1  DABS  D.                   → 1  ok         \ 0xFFFFFFFF → 1
```

### 5.13 混合精度演算 (`M*`, `UM*`, `UM/MOD`, `*/`)

```forth
( 乗算結果を 32bit に拡張: 40000 * 7 = 280000 )
40000 7 M* D.                     → 280000  ok

( 符号なし 16×16 の積を下位・上位に分けて得る )
1000 1000 UM*  SWAP .  .          → 16960 15  ok
 \ 1000 * 1000 = 1_000_000 = 15 * 65536 + 16960

( 倍精度÷単精度 で商とあまりを得る )
0 1  100 UM/MOD  SWAP . .         → 655 36  ok
 \ (1 * 65536) / 100 = 655 あまり 36

( */ は 32bit 中間値を使うので比率計算で桁あふれしない )
12345 37 100 */ .                 → 4567  ok       \ 12345 * 37 / 100
12345 37 100 */MOD . .            → 65 4567  ok    \ rem=65 quot=4567
```

### 5.14 DO / LOOP / +LOOP / I / J / LEAVE

```forth
( +LOOP に負のステップを渡すとカウントダウンになる )
: BLASTOFF  0 10 DO I . -1 +LOOP ." LIFTOFF" CR ;
BLASTOFF    → 10 9 8 7 6 5 4 3 2 1 LIFTOFF  ok

( ネストした DO/LOOP: 九九表、J が外側ループのインデックス )
: TABLE  5 1 DO 5 1 DO J I * 4 .R LOOP CR LOOP ;
TABLE
   →    1   2   3   4
        2   4   6   8
        3   6   9  12
        4   8  12  16   ok

( LEAVE は即時脱出ではなく「index := limit」を設定するだけで、
  実際のループ脱出は次の LOOP に到達した時点で起きる )
: FIRST-FIVE   10 0 DO I . I 4 = IF LEAVE THEN LOOP ;
FIRST-FIVE  → 0 1 2 3 4  ok
```

### 5.15 数値フォーマットと基数

```forth
( .R で 10 進の右詰め表示 )
: HIST   10 0 DO I DUP * 6 .R LOOP CR ;
HIST    →      0     1     4     9    16    25    36    49    64    81  ok

( U.R で 16 進アドレス表 )
HEX
: MAP  3 0 DO I 1000 * 5 U.R LOOP CR ;
MAP     →     0  1000  2000  ok
DECIMAL

( U. は符号なしとして解釈 )
-1 .      → -1  ok
-1 U.     → 65535  ok
```

### 5.16 デバッグヘルパ

```forth
1 2 3 .S               → <3> 1 2 3  ok
DROP DROP DROP

WORDS                  \ 組込語 + ユーザー定義語を新しい順に全列挙

HEX
C000 40 DUMP           \ リターンスタック領域の先頭 64 バイトを 16 進ダンプ
DECIMAL
```

### 5.17 コンパイル時の小技

```forth
( ' (tick) でワードの xt を取得、EXECUTE で後から実行 )
' + .                → <アドレス>  ok
3 4 ' + EXECUTE .    → 7  ok

( RECURSE で自分自身を呼ぶ。コンパイル中は F_HIDDEN 中なので
  名前では呼べない — RECURSE が必要 )
: FACT  DUP 1 > IF DUP 1- RECURSE * THEN ;
6 FACT .             → 720  ok

( S" が残す ( addr u ) は TYPE や CMOVE にそのまま渡せる )
: SHOUT  S" HELLO!" TYPE CR ;
SHOUT                → HELLO!  ok

( LITERAL は IMMEDIATE — TOS をインラインリテラルとしてコンパイルする。
  他の IMMEDIATE ワードから呼び出して、計算結果を呼び出し側の定義に
  埋め込む用途が主 )
```

---

## 6. 制限事項 / 注意

- **大文字小文字区別**。`dup` と `DUP` は別物扱い (前者は未定義)。
- **`."` / `S"` の文字列**は最大 255 文字 (長さは 1 バイト)。
- `LEAVE` は index := limit を行うだけで、実際のループ脱出は
  次の `LOOP` / `+LOOP` に到達したときに起きます (即時脱出ではない)。
- **エラー時**は `<word>?` を表示して REPL が継続するが、
  スタックは巻き戻されません（スタックが崩れていたら `0 0 0 ...` などで
  適宜リセット、もしくはハードリセット）。
- **警告**: `TIB` を越えるトークン/辞書は 未検査。常識的な範囲で使用。
- **シングルボキャブラリ**: `VOCABULARY` 等はソース互換のため用意して
  ありますが、本カーネルは単一の `FORTH` ワードリストで運用されており、
  `CONTEXT` と `CURRENT` は同じリストを指します。
- **マスストレージワード** (`BLOCK` / `BUFFER` / `UPDATE` / `SAVE-BUFFERS`)
  は未実装 — このハードウェアにはブロックデバイスがありません。

---

## 7. em6809 側の前提

プラグインを支える `em6809` クレートには、本カーネルの開発中に
発見された複数の問題 (LEAS/LEAU 入替、`ABX` 未実装、`TST <mem>` /
`INC <mem>` の一部欠如、SBC borrow-in 反転、PC-relative postbyte の
`,S` 誤判定) がありましたが、いずれも upstream で修正済みで、
カーネルは制約なしに完全な命令セットを使っています。

---

## 8. テスト

mc6809 プラグイン crate の `tests/smoke.rs` に以下があります:

- `forth_kernel_banner` — 起動バナー確認
- `forth_repl_dot` — `42 .` の echo と実行結果
- `forth_arithmetic` — 主要な算術 / スタック語
- `forth_colon_define_and_call` — `: DOUBLE DUP + ;`
- `forth_if_then_and_begin_until` — `ABS` と最小限の `BEGIN`/`UNTIL`
- `forth_variable_constant_string` — `VARIABLE` / `CONSTANT` / `."` / `(`
- `forth_new_features` — 絵的数値出力、`CREATE`/`DOES>`、
  `FORGET`/`MARKER`、`DO`/`LOOP`/`+LOOP`、混合精度演算

crate のルート (`Cargo.toml` がある場所) で:

```sh
cargo test --release forth_
```

7 件 / 全パス。
