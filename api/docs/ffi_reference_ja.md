# emfe FFI 関数リファレンス

[`plugin_developer_guide_ja.md`](plugin_developer_guide_ja.md) の補助資料。
各エントリは FFI 関数 1 本について シグネチャ、意味、スレッド文脈を示す。
シグネチャは C 形式で表記。Rust プラグインは `#[no_mangle] pub extern
"C"` で同名を公開する。

本文中の記号:

- **UI** = 実行中でない状態でフロントエンド UI スレッドから呼ばれる
- **任意** = 任意スレッドから呼ばれ得る (プラグイン内部で同期必須)
- **worker** = プラグインの emulation スレッドからコールバック発火

フロントエンド側の呼出順序は
[`plugin_developer_guide_ja.md`](plugin_developer_guide_ja.md) を参照。

---

## 列挙型 (おさらい)

```c
EmfeResult { OK=0, ERR_INVALID=-1, ERR_STATE=-2, ERR_NOTFOUND=-3,
             ERR_IO=-4, ERR_MEMORY=-5, ERR_UNSUPPORTED=-6 }

EmfeState { STOPPED=0, RUNNING=1, HALTED=2, STEPPING=3 }

EmfeStopReason { NONE=0, USER=1, BREAKPOINT=2, WATCHPOINT=3, STEP=4,
                 HALT=5, EXCEPTION=6 }

EmfeRegFlag ビット: NONE, READONLY, PC, SP, FLAGS, FPU, MMU, HIDDEN
EmfeSettingType: INT, STRING, BOOL, COMBO, FILE, LIST
EmfeWatchpointType: READ, WRITE, READWRITE
EmfeBreakpointType: EXEC, READ, WRITE, RW
```

---

## 1. Discovery / Lifecycle

### `emfe_negotiate(const EmfeNegotiateInfo* info) -> EmfeResult`
スレッド: **UI**。
`LoadLibrary` 後最初に呼ばれる。呼び出し側の `api_version_major` が
`EMFE_API_VERSION_MAJOR` と一致すれば `OK`、違えば `ERR_UNSUPPORTED`。

### `emfe_get_board_info(EmfeBoardInfo* out) -> EmfeResult`
スレッド: **UI**。
プラグイン所有文字列を埋める。DLL 生存期間中有効。`emfe_create` より前に
呼び出され、"Switch Plugin" ダイアログに表示される。

#### `EmfeBoardInfo::capabilities`

プラグインが実装している任意機能を宣言する `EMFE_CAP_*` フラグの OR。
フロントエンドはこの値を見てメニュー項目、ツールバー、パネルの
enable/disable を決める。フラグを **立てたら** 対応 API を実装する責任が
あり、**立てないなら** 対応 API は `EMFE_ERR_UNSUPPORTED` 返却のスタブで
構わない。

| フラグ                       | 値          | 対象 API                               |
| ---------------------------- | ----------- | -------------------------------------- |
| `EMFE_CAP_LOAD_ELF`          | `1 <<  0`   | `emfe_load_elf`                        |
| `EMFE_CAP_LOAD_SREC`         | `1 <<  1`   | `emfe_load_srec`                       |
| `EMFE_CAP_LOAD_BINARY`       | `1 <<  2`   | `emfe_load_binary`                     |
| `EMFE_CAP_STEP_OVER`         | `1 <<  3`   | `emfe_step_over`                       |
| `EMFE_CAP_STEP_OUT`          | `1 <<  4`   | `emfe_step_out`                        |
| `EMFE_CAP_CALL_STACK`        | `1 <<  5`   | `emfe_get_call_stack`                  |
| `EMFE_CAP_WATCHPOINTS`       | `1 <<  6`   | `emfe_add_watchpoint` 等               |
| `EMFE_CAP_FRAMEBUFFER`       | `1 <<  7`   | `emfe_get_framebuffer_info`            |
| `EMFE_CAP_INPUT_KEYBOARD`    | `1 <<  8`   | `emfe_push_key`                        |
| `EMFE_CAP_INPUT_MOUSE`       | `1 <<  9`   | `emfe_push_mouse_*`                    |

### `emfe_create(EmfeInstance* out) -> EmfeResult`
スレッド: **UI**。
新しいエミュレータインスタンスを確保。事後条件: `BuildRegisterDefs` /
`BuildSettingDefs` 済み、`STOPPED` 状態。

### `emfe_destroy(EmfeInstance) -> EmfeResult`
スレッド: **UI**。
ワーカースレッドを停止 (典型的には `stop_requested` フラグ + `join`) して
から解放。`RUNNING` 中に呼ばれても安全に扱う。

## 2. コールバック (インスタンスごとにプラグインが保持)

### `emfe_set_console_char_callback(inst, cb, user) -> EmfeResult`
スレッド: **UI**。
ゲスト UART が 1 バイト書くたび、**worker** スレッドから `cb(user, ch)`
を呼ぶ。

### `emfe_set_state_change_callback(inst, cb, user) -> EmfeResult`
スレッド: **UI**。
自律的な状態遷移 (BP ヒット、HALT、例外、stop 受付) で **worker** スレッド
から `cb(user, &info)` を発火。stepping で UI スレッドから発火することも
ある。

### `emfe_set_diagnostic_callback(inst, cb, user) -> EmfeResult`
スレッド: **UI**。
プラグイン内部のログ的テキストメッセージを配信するコールバック。スレッド
文脈は不定。

## 3. レジスタ

### `emfe_get_register_defs(inst, const EmfeRegisterDef** out) -> int32_t count`
スレッド: **UI**。
レジスタ数を返し、プラグイン所有配列へのポインタを out に書く。DLL 生存
期間中有効。

### `emfe_get_registers(inst, EmfeRegValue* values, int32_t count) -> EmfeResult`
スレッド: **UI** (emulation 停止中)。
呼び出し側が `reg_id` を設定、プラグインが `value.u64` (/f64/f80) を
埋める。未知の reg_id は `ERR_INVALID` で以降処理停止。

### `emfe_set_registers(inst, const EmfeRegValue* values, int32_t count) -> EmfeResult`
スレッド: **UI** (emulation 停止中)。
`RUNNING` 中なら `ERR_STATE`。`READONLY` フラグ付きは黙って無視。

## 4. メモリ (副作用なし)

### `emfe_peek_byte/word/long(inst, uint64_t addr) -> uint8_t/uint16_t/uint32_t`
### `emfe_poke_byte/word/long(inst, uint64_t addr, ...) -> EmfeResult`
スレッド: **UI**。
MMIO ハンドラは発火しない、ウォッチポイントも立てない。エンディアン性は
対象 CPU に従う — MC68030 / Z8000 / MC6809 は big-endian、EM8 は
little-endian 寄り (バイト単位)。ワード/ロングの非アライン扱いは
プラグイン裁量。

### `emfe_peek_range(inst, uint64_t addr, uint8_t* out, uint32_t length) -> EmfeResult`
スレッド: **UI**。
`out` は `length` バイトの容量を持つこと。

### `emfe_get_memory_size(inst) -> uint64_t`
スレッド: **UI**。
アドレス可能範囲 (`最大アドレス + 1`) を返す。メモリビューのスクロール
上限として使用。

## 5. 逆アセンブル

### `emfe_disassemble_one(inst, uint64_t addr, EmfeDisasmLine* out) -> EmfeResult`
スレッド: **UI**。
`address`, `raw_bytes`, `mnemonic`, `operands`, `length` を埋める。文字列
はプラグイン所有で、同インスタンスの次回逆アセンブル呼出しまで有効。

### `emfe_disassemble_range(inst, start, end, out, int32_t max) -> int32_t count`
スレッド: **UI**。
バッチ版。可変長命令が `end` より手前で終わった場合は `max` 未満で
OK。

### `emfe_get_program_range(inst, uint64_t* out_start, uint64_t* out_end) -> EmfeResult`
スレッド: **UI**。
直近ロードしたプログラムの範囲。未ロードなら `(0, 0)`。

## 6. 実行制御

### `emfe_step(inst) -> EmfeResult`
スレッド: **UI**。
同期 1 命令実行。`STOP_REASON_STEP` で state コールバック発火。
`RUNNING` 中は `ERR_STATE`。

### `emfe_step_over(inst) -> EmfeResult`
スレッド: **UI**。
Async: 1 命令ステップ。ただしサブルーチン呼出し (JSR/BSR/CALL) はアトミックに
(戻るまで実行) 扱う。サブルーチン認識できないプラグインは `ERR_UNSUPPORTED`。

### `emfe_step_out(inst) -> EmfeResult`
スレッド: **UI**。
Async: 現在のサブルーチンから戻るまで実行。プラグイン側の shadow call
stack 追跡が前提。

### `emfe_run(inst) -> EmfeResult`
スレッド: **UI**。
ワーカースレッドを起動し `OK` で即 return。停止理由は state コールバック
経由で通知。

### `emfe_stop(inst) -> EmfeResult`
スレッド: **任意**。
内部停止フラグを立ててワーカーを join。既停止時呼び出しも安全。

### `emfe_reset(inst) -> EmfeResult`
スレッド: **UI**。
CPU + ペリフェラルをリセット。メモリは消去しない。state コールバックを
`STOP_REASON_NONE` で発火。

### `emfe_get_state(inst) -> EmfeState`
スレッド: **任意**。

### `emfe_get_instruction_count(inst) / emfe_get_cycle_count(inst) -> int64_t`
スレッド: **任意**。
単調増加。`emfe_reset` でのみ 0 に戻る。MHz / MIPS 表示用。

## 7. ブレークポイント

### `emfe_add_breakpoint(inst, uint64_t addr) -> EmfeResult`
### `emfe_remove_breakpoint(inst, uint64_t addr) -> EmfeResult`
### `emfe_enable_breakpoint(inst, uint64_t addr, bool enabled) -> EmfeResult`
### `emfe_set_breakpoint_condition(inst, addr, const char* cond) -> EmfeResult`
### `emfe_clear_breakpoints(inst) -> EmfeResult`
### `emfe_get_breakpoints(inst, EmfeBreakpointInfo* out, int32_t max) -> int32_t count`
スレッド: **UI** (停止中が基本。実行中に stop/add/resume するフロント
エンドもある)。
実行ブレークポイント (データではない)。`condition` 文字列の解析は
プラグイン責任 (任意)。未登録アドレスの `remove` は `ERR_NOTFOUND`。

## 8. ウォッチポイント

### `emfe_add_watchpoint(inst, addr, EmfeWatchpointSize, EmfeWatchpointType) -> EmfeResult`
### `emfe_remove_watchpoint(inst, addr) -> EmfeResult`
### `emfe_enable_watchpoint(inst, addr, bool) -> EmfeResult`
### `emfe_set_watchpoint_condition(inst, addr, const char*) -> EmfeResult`
### `emfe_clear_watchpoints(inst) -> EmfeResult`
### `emfe_get_watchpoints(inst, EmfeWatchpointInfo* out, int32_t max) -> int32_t count`
スレッド: **UI**。
ブレークポイントと同じパターンだがメモリアクセスに紐づく。サイズは
BYTE / WORD / LONG でアクセスバイト範囲と照合。

## 9. コールスタック

### `emfe_get_call_stack(inst, EmfeCallStackEntry* out, int32_t max) -> int32_t count`
スレッド: **UI**。
現在のシャドウコールスタックを返す (entry 0 が最内フレーム)。
追跡していなければ 0 を返す。

## 10. フレームバッファ (Phase 3)

### `emfe_get_framebuffer_info(inst, EmfeFramebufferInfo* out) -> EmfeResult`
スレッド: **UI**。
`width`, `height`, `bpp`, `stride`, `base_address`, `pixels` を埋める。
`pixels` ポインタはプラグイン所有でインスタンス破棄 / FB 再構成まで有効。
FB 無しは `ERR_UNSUPPORTED`。

### `emfe_get_palette_entry(inst, uint32_t index) -> uint32_t AARRGGBB`
### `emfe_get_palette(inst, uint32_t* out, int32_t max) -> int32_t count`
スレッド: **UI**。`EMFE_FB_FORMAT_INDEXED8` の時のみ意味を持つ。

## 11. 入力イベント (Phase 3)

### `emfe_push_key(inst, uint32_t scancode, bool pressed) -> EmfeResult`
### `emfe_push_mouse_move(inst, dx, dy) -> EmfeResult`
### `emfe_push_mouse_absolute(inst, x, y) -> EmfeResult`
### `emfe_push_mouse_button(inst, button, bool pressed) -> EmfeResult`
スレッド: **UI**。
ホスト入力を キーボード / マウスデバイス を持つプラグインに渡す。
未対応プラグインは `ERR_UNSUPPORTED`。

## 12. ファイルロード

### `emfe_load_elf(inst, const char* path) -> EmfeResult`
スレッド: **UI** (`RUNNING` 中は不可)。
ELF セグメントをロード、エントリポイントから PC 設定。小規模 CPU 向けは
通常 `ERR_UNSUPPORTED`。

### `emfe_load_binary(inst, const char* path, uint64_t load_address) -> EmfeResult`
スレッド: **UI**。
指定アドレスへ生バイト配置。ロード後: (a) ロード範囲がリセットベクタを
含むならそこから PC、(b) 含まなければ `load_address` から PC。

### `emfe_load_srec(inst, const char* path) -> EmfeResult`
スレッド: **UI**。
Motorola S-Record 解析。S1 (16bit データ) + S9 (start) は最低限対応。
大規模プラグインは S2/S8 (24bit) や S3/S7 (32bit) も対応可。

### `emfe_get_last_error(inst) -> const char*`
スレッド: **UI**。
直近の失敗呼出しのエラー文字列 (プラグイン所有)。成功時は空文字列。

## 13. 設定

### `emfe_get_setting_defs(inst, const EmfeSettingDef** out) -> int32_t count`
スレッド: **UI**。
プラグイン所有の定義配列。インスタンス生成時に一度だけ構築。

### `emfe_get_setting(inst, const char* key) -> const char*`
### `emfe_set_setting(inst, const char* key, const char* value) -> EmfeResult`
### `emfe_apply_settings(inst) -> EmfeResult`
### `emfe_get_applied_setting(inst, const char* key) -> const char*`
スレッド: **UI**。

設定キーごとに 3 つの状態を持つ:

| 状態       | アクセサ                        | 書き込みタイミング                                    |
| ---------- | ------------------------------- | ----------------------------------------------------- |
| staged     | `emfe_get_setting`              | `emfe_set_setting` (ダイアログでの入力ごと)           |
| committed  | *(内部)*                        | `emfe_apply_settings` が staged → committed にコピー  |
| applied    | `emfe_get_applied_setting`      | apply 時は hot-swap 可のみ、`emfe_reset` で全フラッシュ |

`emfe_set_setting` は staged マップにのみ書き込み、副作用は発生しない。
`emfe_apply_settings` は全 staged 値をコミットするが、実際に稼働中の
ハードウェアに即反映されるのは `EMFE_SETTING_FLAG_REQUIRES_RESET` **が
立っていない** 設定 (例: `Theme`, `Console*`) のみ。REQUIRES_RESET が
立っている設定 (例: `BoardType`, `MemorySize`, `CpuVariant`) は次の
`emfe_reset` まで適用保留となる。

フロントエンドは REQUIRES_RESET 設定について
`emfe_get_setting(key) != emfe_get_applied_setting(key)` のとき
「適用保留」インジケータを表示すべき。

#### `EmfeSettingDef::flags`

| フラグ                              | 値         | 意味                                                                    |
| ----------------------------------- | ---------- | ----------------------------------------------------------------------- |
| `EMFE_SETTING_FLAG_REQUIRES_RESET`  | `1u << 0`  | 変更は `emfe_reset` まで保留 (活線挿抜できないデバイス)。               |

### `emfe_save_settings(inst) / emfe_load_settings(inst) -> EmfeResult`
### `emfe_set_data_dir(const char* path) -> EmfeResult`
スレッド: **UI**。`emfe_set_data_dir` は DLL 全体に効く **プロセスレベル**
呼出し (インスタンスごとではない)。フロントエンドが起動時に
プラグインの永続化ディレクトリを (例:
`%LOCALAPPDATA%\emfe_CsWPF\<plugin subdir>`) へリダイレクトするのに使用。

### リスト項目アクセサ (`EMFE_SETTING_LIST` 用)
### `emfe_get_list_item_defs(inst, const char* list_key, const EmfeListItemDef** out) -> int32_t count`
### `emfe_get_list_item_count(inst, list_key) -> int32_t`
### `emfe_get_list_item_field(inst, list_key, int32_t index, field_key) -> const char*`
### `emfe_set_list_item_field(inst, list_key, index, field_key, value) -> EmfeResult`
### `emfe_add_list_item(inst, list_key) -> int32_t new_index`
### `emfe_remove_list_item(inst, list_key, index) -> EmfeResult`
スレッド: **UI**。
動的行テーブル (例: SCSI ディスク一覧) 用。リスト設定を持たないなら 0 /
`ERR_UNSUPPORTED` でスキップ可。

### `emfe_is_list_pending(inst, const char* list_key) -> int32_t`
スレッド: **UI**。
**オプション**エクスポート — フロントエンドはソフト解決
(`GetProcAddress` / `TryLoadFunc`) を使い、見つからなければマーカー
機能をスキップしてください。staged リスト (`emfe_get_list_*` が返す
値) と applied リスト (実機に適用済み) が異なれば `1`、同じなら `0`
を返します。LIST 設定の保留マーカー表示用 — 通常スカラー設定での
`emfe_get_setting` vs `emfe_get_applied_setting` 比較に対応する LIST
版です。

等価判定はプラグイン定義です。mc68030 プラグインは要素比較 (行数 +
行ごとの path + scsi-id) を行います。未知の `list_key` には `0` を
返します。

## 14. コンソール I/O

### `emfe_send_char(inst, char ch) -> EmfeResult`
### `emfe_send_string(inst, const char* s) -> EmfeResult`
スレッド: **任意** (ユーザがコンソールウィンドウで打鍵するのは通常
emulation 実行中)。RX FIFO への書き込みと emulation スレッドからの読み
をプラグイン側で同期すること。

## 15. 文字列ユーティリティ

### `emfe_release_string(const char* s) -> void`
将来拡張用予約 (プラグインが動的確保文字列を返してフロントエンドが解放
する形態)。現行プラグインは全て no-op。

---

## 付録: 最小限の実装セット

Switch Plugin ダイアログに出てきて何かする最小セット:

```
emfe_negotiate
emfe_get_board_info
emfe_create
emfe_destroy
emfe_get_register_defs      (空配列でも可)
emfe_get_registers
emfe_set_registers          (全拒否でも可)
emfe_peek_byte              (メモリビュー用)
emfe_poke_byte
emfe_get_memory_size
emfe_disassemble_one        ("???" でも可)
emfe_step
emfe_get_state
emfe_reset
emfe_set_console_char_callback
emfe_set_state_change_callback
emfe_set_diagnostic_callback
emfe_get_setting_defs       (空でも可)
emfe_get_setting
emfe_set_setting
emfe_apply_settings
emfe_get_applied_setting
emfe_release_string         (通常 no-op)
emfe_get_last_error         ("" でも可)
```

その他は `ERR_UNSUPPORTED` または 0 を返せば、プラグインが育つまでは OK。
