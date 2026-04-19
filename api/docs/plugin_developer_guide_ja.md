# emfe プラグイン開発者ガイド

本書では emfe プラグイン DLL が従うべき契約 (contract) を説明します。
関数ごとの詳細は [`ffi_reference_ja.md`](ffi_reference_ja.md) を参照。
コピーして使える骨組みは [`quickstart_cpp.md`](quickstart_cpp.md) または
[`quickstart_rust.md`](quickstart_rust.md) を参照してください。

---

## 1. プラグインモデル 一目で

```
┌─────────────────┐                                  ┌───────────────────┐
│ emfe フロントエンド│ LoadLibrary / P/Invoke          │ プラグイン DLL     │
│ (WinUI3 / WPF)  │ ──────────────────────────────▶  │ emfe_plugin_X.dll │
│                 │                                   │                   │
│ 発見:                                              emfe_negotiate       │
│ - plugins\ 走査                                    emfe_get_board_info  │
│                                                                         │
│ インスタンス管理:                                  emfe_create          │
│                                                    emfe_set_*_callback  │
│                                                    emfe_get_register_defs│
│                                                    emfe_get_setting_defs│
│ UI 操作:                                           emfe_step / run / ...│
│ - レジスタ/逆アセ/メモリ/コンソール               emfe_peek/poke_* ... │
│                                                                         │
│ 終了:                                              emfe_destroy         │
└─────────────────┘                                  └───────────────────┘
```

- プラグインは `emfe_*` C ABI をエクスポートする通常の Windows DLL
- DLL はフロントエンド exe 隣接の `plugins\` サブディレクトリから
  `emfe_plugin_*.dll` パターンで発見される
- フロントエンドは `emfe_create` で **インスタンス** を生成。各インスタンスが
  完全なエミュレータ状態 (CPU、メモリ、ペリフェラル) を保持する

## 2. ライフサイクル

### 2.1 プロセス起動

1. フロントエンドが `plugins\` から `emfe_plugin_*.dll` を走査
2. 各候補を `LoadLibrary` で読み込み、自身の `EMFE_API_VERSION_MAJOR/MINOR`
   を引数に `emfe_negotiate` を呼ぶ
3. `EMFE_OK` が返れば候補一覧に追加 (`emfe_get_board_info` の名前も一緒に
   表示)

### 2.2 インスタンスのライフサイクル

```
emfe_create(&instance)
  │
  │   (任意)
  ▼
emfe_set_console_char_callback(instance, cb, user)
emfe_set_state_change_callback(instance, cb, user)
emfe_set_diagnostic_callback(instance, cb, user)
  │
  ▼
emfe_get_register_defs(instance, &defs)    // レジスタ UI 構築
emfe_get_setting_defs(instance, &defs)     // 設定ダイアログ構築
  │
  ▼
... (通常運用: step/run/load/peek/...)
  │
  ▼
emfe_destroy(instance)
```

### 2.3 バージョンネゴシエーション

`emfe_negotiate` はフロントエンドが **最初** に呼ぶ関数。戻り値:

| API major 一致 | 戻り値 |
|---|---|
| 呼び出し側の major == プラグインの major | `EMFE_OK` |
| major 不一致 | `EMFE_ERR_UNSUPPORTED` (フロントエンドは当該プラグインをスキップ) |

minor 不一致は `EMFE_OK` を返してよい。フロントエンドが自分のヘッダに
宣言されている関数のみを呼び出すのが原則。プラグイン側は新しい型に
強く依存する場合のみ、古すぎる minor を保守的に拒否してもよい。

## 3. 基本設計ルール

### 3.1 Opaque handle

`EmfeInstance` は不透明ポインタ (`void*`)。具体型はプラグイン側で決定、
フロントエンドからは**決して内部を覗かない**。典型的なパターン:

```c
struct EmfeInstanceData { /* ... */ };
EmfeResult EMFE_CALL emfe_create(EmfeInstance* out) {
    auto inst = new EmfeInstanceData();
    *out = reinterpret_cast<EmfeInstance>(inst);
    return EMFE_OK;
}
```

```rust
struct PluginInstance { /* ... */ }
#[no_mangle]
pub extern "C" fn emfe_create(out: *mut EmfeInstance) -> EmfeResult {
    let b = Box::new(PluginInstance::new());
    unsafe { *out = Box::into_raw(b) as EmfeInstance; }
    EmfeResult::Ok
}
```

### 3.2 文字列の所有権

プラグインが返すすべての `const char*` (mnemonic, operands, raw_bytes,
設定名/ラベル/値、エラーメッセージ、...) は **プラグイン所有**で、
少なくとも「同じバッファを更新しうる次回呼出しまで」有効。

実運用での寿命:

| `const char*` を返す関数 | ポインタの有効範囲 |
|---|---|
| `emfe_get_register_defs` | DLL の生存期間中 (配列はプラグイン側で一度だけ構築) |
| `emfe_get_setting_defs` | DLL の生存期間中 |
| `emfe_get_setting` | 同インスタンスで次回 `emfe_get_setting` が呼ばれるまで |
| `emfe_disassemble_one/range` | 同インスタンスで次回逆アセンブル呼出しまで |
| `emfe_get_last_error` | 次に last_error を更新しうる呼出しまで |
| `EmfeBoardInfo` 内文字列 | DLL の生存期間中 |

フロントエンドはこれらのポインタへ書き込まず、free もしない。
プラグインが動的に文字列を発行する場合は `EmfeInstanceData` 内の
バッファ (例: `std::vector<std::string>` や `Vec<CString>`) に保持する
必要がある (既存プラグインは全てこの方式)。

`emfe_release_string` は将来拡張のためにシグネチャだけ用意されているが、
同梱プラグインは全て no-op。

### 3.3 スレッド契約

**プラグインが守るべきこと**:

- `emfe_stop` は **任意のスレッド** から呼ばれ得る (多くの場合 UI スレッド
  から、`emfe_run` 中の emulation スレッドへ)。即座に停止フラグを立てて
  return、current instruction の完了を待たない
- 全コールバック (`console_char_callback`, `state_change_callback`,
  `diagnostic_callback`) は **emulation スレッド** から発火しうる
  (UI スレッドではない)。UI 側は必要に応じて dispatcher で marshal する
- それ以外の関数は emulation が **実行中でない** 状態 (emfe_stop 後) で
  UI スレッドから呼ばれる。例外:
  - `emfe_send_char` / `emfe_send_string` は実行中にも到着し得る
    (ユーザがコンソールで打鍵)。同一 RX FIFO への emulation 側読みに
    対してスレッドセーフである必要がある
- プラグインは state machine (§ 3.4) を権威的に扱う

**フロントエンドが保証すること**:

- `emfe_destroy` は `emfe_run` ワーカーが停止・joined した後にのみ呼ばれる。
  安全策としてプラグイン側でも `stop_requested = true` + `join` を
  `emfe_destroy` 内で行うのが慣例

### 3.4 実行ステートマシン

```
             emfe_reset / emfe_create
                      │
                      ▼
    ┌─────────────STOPPED◀──────────────┐
    │               │                    │
    │               │ emfe_step          │ state callback
    │               ▼                    │ (STOP_REASON_STEP)
    │           STEPPING ────────────────┘
    │               │
    │ emfe_stop     │ emfe_run
    │               ▼
    └────────── RUNNING ───── emulation thread ───┐
                    │                              │
                    │ (BP / WP / HALT / user stop)│
                    ▼                              │
                HALTED  ─── state callback ────────┘
                              (STOP_REASON_*)
```

`emfe_get_state` が現状を返す。フロントエンドからの指示以外で起きる
遷移 (BP ヒット、HALT 命令など) は適切な `EmfeStopReason` とともに
state-change コールバックを**必ず**発火すること。

## 4. レジスタ

### 4.1 定義リスト

`emfe_get_register_defs` はプラグイン所有の `EmfeRegisterDef` 配列へ
ポインタを返す。フロントエンドは配列を走査してレジスタパネルを構築
する。主要フィールド:

| フィールド | 用途 |
|---|---|
| `reg_id` | プラグインが付番する安定 ID (`get/set_registers` でも同一を期待) |
| `name` | 短い表示名 (例: `"D0"`, `"PC"`, `"RH0"`) |
| `group` | タブ/パネルグループ (例: `"Data"`, `"Address"`, `"System"`, `"FPU"`, `"Counters"`) |
| `type` | `EMFE_REG_INT` / `_FLOAT` / `_FLOAT80` |
| `bit_width` | 8, 16, 32, 64, 80 |
| `flags` | ビットマスク: `_PC`, `_SP`, `_FLAGS`, `_FPU`, `_MMU`, `_READONLY`, `_HIDDEN` |

フロントエンドはフラグを特別扱いする:
- `_PC` — 逆アセンブル画面・ステータスバー上の PC 表示
- `_SP` — スタックポインタ装飾
- `_FLAGS` — ビット内訳表示 (対応プラグインのみ)
- `_HIDDEN` — デフォルトパネルから除外 (`get/set_registers` は可能)。
  サイクルカウンタ等で利用

### 4.2 get / set

`emfe_get_registers` / `emfe_set_registers` は `reg_id` 設定済みの配列を
受け取り、プラグインが `value.u64` (または `f64` / `f80`) を埋める。

set は RUNNING 中は拒否: `EMFE_STATE_RUNNING` なら `EMFE_ERR_STATE` を
返す。既存プラグイン全てが準拠。

## 5. メモリ

`emfe_peek_*` / `emfe_poke_*` は **副作用なし** で動作する — デバッガ
(メモリビュー、逆アセンブル) が使うため、冪等、MMIO ハンドラを発火させず、
サイクルカウンタに影響せず、ウォッチポイントフラグも立てない。

**ゲスト側はバスを別経路で見る** — そちらは MMIO などが有効。混同しない
こと。

`emfe_get_memory_size` は「最大有効アドレス + 1」を返す (例: 64 KB フラット
なら 65536)。フロントエンドはメモリビューのスクロール境界として使用。

## 6. 逆アセンブル

`emfe_disassemble_one` は `EmfeDisasmLine` に `address`, `raw_bytes`,
`mnemonic`, `operands`, `length` を埋める。文字列はプラグイン所有で、
同インスタンスの次回逆アセンブル呼出しまで有効。

`emfe_disassemble_range` は複数エントリ書き込み。可変長命令が
`end_address` より手前で終わった場合は `max_lines` 未満で停止して OK。

`emfe_get_program_range` は直近ロードしたプログラムのアドレス範囲を返す
(逆アセンブル画面の自動スクロール用)。不明なら `{0, 0}` を返す。

## 7. 実行制御

`emfe_step` は **1 命令** を同期実行し、`EMFE_STOP_REASON_STEP` で
state コールバックを発火する。

`emfe_run` はワーカースレッドを起動し、以下で停止:
- `emfe_stop` 呼出 (`EMFE_STOP_REASON_USER`)
- ブレークポイントヒット (`_BREAKPOINT`)
- ウォッチポイントヒット (`_WATCHPOINT`)
- HALT 相当命令 (`_HALT`)
- 回復不能な例外 (`_EXCEPTION`)

`emfe_step_over` / `emfe_step_out` は任意実装 (未実装なら
`EMFE_ERR_UNSUPPORTED`; フロントエンドは単純 step にフォールバック)。

`emfe_reset` は CPU + ペリフェラルをリセットするが、**ロード済みプログラム
はメモリに残す** (フロントエンドは再ロードしない)。保留中割込のクリア、
リセットベクタ再読込、state コールバック発火も行う。

`emfe_get_instruction_count` / `emfe_get_cycle_count` はステータスバーの
MHz / MIPS 計算で使う。`emfe_reset` 以外では単調増加。

## 8. ブレークポイント・ウォッチポイント

- アドレス → `{enabled, condition}` のテーブルをプラグイン側で管理
- emulation ループは毎命令 fetch **前** にテーブルを確認
- ヒット時は `EMFE_STOP_REASON_BREAKPOINT` で state コールバック発火・
  ワーカー停止。再開 (`emfe_run` 再呼出) 時に同 BP で即再ヒットしない
  よう、プラグイン側で「再開時は最初の 1 命令を BP チェック無しで実行」
  するのが典型パターン
- 条件文字列は `emfe_set_breakpoint_condition` で渡される。解析・評価は
  プラグイン側の責任。既存プラグインは `==`, `!=`, `<=`, `>=`, `<`,
  `>`, `&`, `&&`, `||`, レジスタ名を扱う小さな式評価器を同梱

ウォッチポイントはメモリアクセス (read / write / R-W) をキーにする点
以外は同様。

## 9. 設定 (データ駆動 UI)

`emfe_get_setting_defs` が `EmfeSettingDef` 配列を返す。フロントエンドは
`group` フィールドごとにタブを生成。

主要フィールド:

| フィールド | 意味 |
|---|---|
| `key` | 内部識別子 (`emfe_get/set_setting` で使用) |
| `label` | 表示用ラベル |
| `group` | タブ名 (`"General"`, `"Console"`, プラグイン固有など) |
| `type` | `EMFE_SETTING_INT` / `_STRING` / `_BOOL` / `_COMBO` / `_FILE` / `_LIST` |
| `default_value` | デフォルト値 (文字列形式) |
| `constraints` | 型依存:<br>• `INT`: `"min\|max"` (例 `"1\|256"`)<br>• `COMBO`: `"Dark\|Light\|System"` 等<br>• `FILE` / `STRING`: 自由 |
| `depends_on` / `depends_value` | `depends_on` の値が `depends_value` と一致する時のみ表示。ボード毎タブに使用 |
| `flags` | ビットマスク。定義済みフラグ: `EMFE_SETTING_FLAG_REQUIRES_RESET` (活線挿抜できない設定。次回の `emfe_reset` まで適用保留)。 |

**適用セマンティクス**: 各設定キーは 3 つの状態を持つ:

1. **staged** — ダイアログの入力ごとに `emfe_set_setting` で書き込まれる
   値。`emfe_get_setting` で読める。
2. **committed** — `emfe_apply_settings` (OK ボタン) で更新される値。
   `emfe_save_settings` が永続化するのはこの値。
3. **applied** — 現在エミュレートされているハードウェアで実際に使われて
   いる値。`emfe_get_applied_setting` で読める。

`emfe_apply_settings` は全キーを staged → committed にコピーするが、
`applied` に即伝搬するのは **hot-swap 可能** な設定 (REQUIRES_RESET
フラグ **なし**) のみ。メモリサイズ、ボード種別、CPU バリアントなど、
デバイスの破棄・再構築が必要な設定 (REQUIRES_RESET フラグ **あり**) は
次の `emfe_reset` (full reset / 再起動) まで適用保留となり、reset
時にプラグインが `applied = committed` のフラッシュを行う。

典型的なプラグイン側実装:

```cpp
std::unordered_map<std::string, std::string> stagedSettings;   // ダイアログの状態
std::unordered_map<std::string, std::string> settings;         // committed
std::unordered_map<std::string, std::string> appliedSettings;  // 実機上で有効な値
std::unordered_map<std::string, uint32_t>    settingFlags;     // キー毎フラグ

EmfeResult emfe_apply_settings(EmfeInstance h) {
    settings = stagedSettings;  // 全てコミット
    for (auto& [k, v] : stagedSettings) {
        if (!(settingFlags[k] & EMFE_SETTING_FLAG_REQUIRES_RESET))
            appliedSettings[k] = v;  // hot-swap のみ即反映
    }
    return EMFE_OK;
}

EmfeResult emfe_reset(EmfeInstance h) {
    appliedSettings = settings;  // 保留分をフラッシュ
    // ... REQUIRES_RESET キーに依存するデバイスを再構築 ...
    cpu.Reset();
    return EMFE_OK;
}
```

フロントエンドは REQUIRES_RESET 設定について
`emfe_get_setting(key) != emfe_get_applied_setting(key)` のとき
ラベル横に「適用保留」インジケータ (`*` 等) を表示して、変更が
まだ反映されていないことをユーザに示す。

**永続化**: `emfe_save_settings` / `emfe_load_settings` は committed
(staged でない) 値を保存/復元する。load 時はプラグイン側で
`appliedSettings = settings` に同期して、起動直後に pending 表示が
出ないようにするのが良い。データディレクトリは `emfe_set_data_dir`
で `emfe_create` 前に設定。

**リスト項目** (`EMFE_SETTING_LIST`) は動的行 (例: SCSI ディスク一覧)
用。別 API 群 (`emfe_get_list_item_defs`, `emfe_get_list_item_count`,
`emfe_get/set_list_item_field`, `emfe_add/remove_list_item`) で行×列
グリッドを操作。使わないなら 0 / `EMFE_ERR_UNSUPPORTED` を返せばよい。

## 10. コンソール I/O

プラグインは通常、仮想 UART (または同等のシリアルデバイス) を用意し、
フロントエンドのコンソールウィンドウと配線する:

- **TX (guest → host)**: ゲストが UART に書くと
  `console_char_callback(user_data, ch)` を呼ぶ。フロントエンドは VT100
  ターミナルへキューイング
- **RX (host → guest)**: ユーザがコンソールで打鍵すると、フロントエンドは
  `emfe_send_char(instance, ch)` を呼ぶ。プラグインは UART の RX FIFO
  に push

スレッド: TX コールバックは emulation スレッドから。UI への marshal は
フロントエンド責任、プラグインは意識しなくてよい。

## 11. フレームバッファ・入力・コールスタック (Phase 3)

これらの API はヘッダに定義だけあり、ほとんどのプラグインは
`EMFE_ERR_UNSUPPORTED` または 0 を返す。グラフィック/ポインタデバイス/
サブルーチン追跡を実装する時に埋める。

## 12. プログラムロード

- `emfe_load_binary(path, load_address)` — 指定アドレスへ生バイトを配置。
  通常はリセットベクタまたは `load_address` から PC を設定
- `emfe_load_srec(path)` — Motorola S-record (S1/S9 は 16bit、S2/S8 は
  24bit、S3/S7 は 32bit)。受理バリアントはプラグインが判断
- `emfe_load_elf(path)` — ELF 実行ファイル。任意対応 (8/16 bit CPU は
  通常 `EMFE_ERR_UNSUPPORTED`)

ロード範囲は `emfe_get_program_range` 用に記録。ファイルのエントリポイント
から PC を更新。なければ PC は変更しない。

失敗時は有用な `emfe_get_last_error` メッセージとともに `EMFE_ERR_IO`
を返す。

## 13. 続きの資料

- **関数ごとの仕様**: [`ffi_reference_ja.md`](ffi_reference_ja.md)
- **C++ 骨組み**: [`quickstart_cpp.md`](quickstart_cpp.md)
- **Rust 骨組み**: [`quickstart_rust.md`](quickstart_rust.md)
- **実装例**: この `api/` と同じ `emfe_plugins/` 配下にある 4 プラグイン
  (`mc68030/`, `em8/`, `z8000/`, `mc6809/`) が本書で述べたパターンを
  ほぼ網羅している
