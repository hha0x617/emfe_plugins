# Hha Forth — ユーザーガイド

`forth.asm` は em6809 / emfe_plugin_mc6809 環境で動く最小 ITC Forth です。
ACIA (MC6850) 1 本の上で対話的に動作し、`:` による新語定義、制御構造、
変数・定数・文字列リテラルまで使えます。

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
| `$0100..$1FFF` | カーネルコード + 組込辞書 |
| `$2000..$9FFF` | ユーザ辞書 (`HERE` が伸びる先) |
| `$A000..$A07F` | TIB (端末入力バッファ、128 バイト) |
| `$B000..$BFFE` | データスタック (U、下方向伸長、TOS が低番地) |
| `$C000..$FEFE` | リターンスタック (S) |
| `$FF00/$FF01` | ACIA SR/CR, RDR/TDR |
| `$FFFE/$FFFF` | リセットベクタ → `cold` |

- **大文字小文字は区別します**。辞書検索は case-sensitive。
- セルは 16-bit (ビッグエンディアン)。
- トークンの最大長は 31 文字 (flags+length バイトの低 5 ビット)。

---

## 3. REPL の動作

1. `ACCEPT` が TIB に 1 行 (最大 128 バイト) 読み込む。
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
| `DROP` | `( a -- )` | TOS を捨てる |
| `SWAP` | `( a b -- b a )` | TOS と次を入れ替え |
| `OVER` | `( a b -- a b a )` | 2 番目をコピー |
| `ROT` | `( a b c -- b c a )` | 3 番目を TOS へ回す |
| `>R` | `( n -- ) (R: -- n)` | データ→リターン |
| `R>` | `( -- n ) (R: n -- )` | リターン→データ |
| `R@` | `( -- n ) (R: n -- n)` | リターン TOS を参照 |

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
| `NEGATE` | 2's complement |
| `ABS` | `( n -- \|n\| )` 絶対値 |
| `MIN` `MAX` | `( a b -- m )` 符号付き最小値 / 最大値 |
| `AND` `OR` `XOR` | ビット演算 |
| `INVERT` | 1's complement (ビット NOT) |
| `NOT` | `( flag -- !flag )` FORTH-83 の論理反転 (`0=` のエイリアス) |
| `0=` | `( n -- flag )` ゼロなら `-1`、他は `0` |
| `0<` | `( n -- flag )` 負なら `-1` |
| `=` `<>` | 等値 / 非等値、`-1` / `0` を返す |
| `<` `>` | 符号付き大小比較、`-1` / `0` を返す |

ゼロ除算は例外にせずに `rem = a`, `quot = 0` を返します
(カーネルの「誤入力は静かに失敗する」方針に合わせた挙動です)。

### 4.3 メモリ
| 語 | 効果 | 説明 |
|----|------|------|
| `@` | `( addr -- w )` | 16-bit フェッチ |
| `!` | `( w addr -- )` | 16-bit ストア |
| `C@` | `( addr -- b )` | 8-bit フェッチ |
| `C!` | `( b addr -- )` | 8-bit ストア |

### 4.4 入出力
| 語 | 効果 |
|----|------|
| `EMIT` | `( c -- )` 1 文字出力 |
| `KEY` | `( -- c )` 1 文字入力 (ブロッキング) |
| `CR` | `CRLF` 出力 |
| `SPACE` | スペース 1 文字出力 |
| `TYPE` | `( addr u -- )` 文字列出力 |
| `COUNT` | `( c-addr -- addr u )` カウント済み文字列を addr/len に |
| `.` | `( n -- )` 10 進で印字 (末尾にスペース) |

### 4.5 辞書・変数
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

### 4.6 外側インタプリタのビルディングブロック
| 語 | 効果 |
|----|------|
| `ACCEPT` | `( c-addr +n1 -- +n2 )` 1 行読む、エコー付き |
| `PARSE-NAME` | `( -- c-addr u )` 次トークン (空白区切り) |
| `SFIND` | `( c-addr u -- xt flag )` 辞書検索。flag: 0=不在、1=通常、2=IMMEDIATE |
| `NUMBER?` | `( c-addr u -- value flag )` 10 進数パーサ。成功で flag=-1 |
| `INTERPRET` | トークンを順に実行／コンパイル |
| `EXECUTE` | `( xt -- )` xt を実行 |

### 4.7 コンパイル用内部プリミティブ
| 語 | 説明 |
|----|------|
| `(LIT)` | 後続セルをデータスタックへ (コンパイル時に `,` で使用) |
| `(BRANCH)` | 後続セル分だけ IP を進める/戻す |
| `(0BRANCH)` | TOS が 0 なら (BRANCH) と同じ、非ゼロならオフセットをスキップ |
| `(LITSTR)` | 後続のカウント済み文字列を印字して IP を進める |
| `EXIT` | コロン定義から戻る (`;` がこれをコンパイルする) |

### 4.8 定義語と制御構造
| 語 | 効果 |
|----|------|
| `:` | `( "name" -- )` 新コロン定義開始、STATE=1 |
| `;` IMMEDIATE | EXIT をコンパイル、STATE=0 |
| `VARIABLE` | `( "name" -- )` DOVAR 基づく 16-bit 変数を作る |
| `CONSTANT` | `( x "name" -- )` DOCON 基づく定数を作る |
| `IF` `ELSE` `THEN` IMMEDIATE | `(0BRANCH)` / `(BRANCH)` + HERE パッチ |
| `BEGIN` `UNTIL` `AGAIN` IMMEDIATE | 後方ジャンプループ |
| `."` IMMEDIATE | 閉じ `"` までを文字列リテラルとしてコンパイル |
| `(` IMMEDIATE | 閉じ `)` までコメントとして読み飛ばす |

### 4.9 REPL
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

( これは注釈 — REPL は読み飛ばします )  
```

---

## 6. 制限事項 / 注意

- **大文字小文字区別**。`dup` と `DUP` は別物扱い (前者は未定義)。
- **数値入力は 10 進のみ**。`BASE` 切替は未実装。
- **DO/LOOP は未実装**。カウント付きループは `BEGIN ... UNTIL` を使う。
- **`."` の文字列**は最大 255 文字 (長さは 1 バイト)。
- **エラー時**は `<word>?` を表示して REPL が継続するが、
  スタックは巻き戻されません（スタックが崩れていたら `0 0 0 ...` などで
  適宜リセット、もしくはハードリセット）。
- **警告**: `TIB` を越えるトークン/辞書は 未検査。常識的な範囲で使用。
- `FORGET` / `MARKER` / `VOCABULARY` 等のワードセット管理は未実装。

---

## 7. em6809 側の前提

このカーネルは以下の MC6809 命令が動くことを期待しています。
`em6809` クレートで未実装の命令 (例: `ABX`, `TST <mem>`, `INC <mem>`) は
回避コードに置換済みなので、ユーザ側で気にする必要はありません。
将来カーネルを拡張する際は、この短いリストに注意すれば他の命令で
容易に置換可能です。

---

## 8. テスト

mc6809 プラグイン crate の `tests/smoke.rs` に以下があります:

- `forth_kernel_banner` — 起動バナー確認
- `forth_repl_dot` — `42 .` の echo と実行結果
- `forth_colon_define_and_call` — `: DOUBLE DUP + ;`
- `forth_if_then_and_begin_until` — `ABS` と `ONCE` (最小 BEGIN/UNTIL)
- `forth_variable_constant_string` — VARIABLE / CONSTANT / `."` / `(`

crate のルート (`Cargo.toml` がある場所) で:

```sh
cargo test --release forth_
```
