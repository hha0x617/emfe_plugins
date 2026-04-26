# MVME147 設定の挙動

設定ダイアログの **MVME147** タブで変更を加えたとき、エミュレータに
どう適用されるかを説明します。プラグイン版フロントエンド
(`emfe_WinUI3Cpp` / `emfe_CsWPF`) でも、スタンドアロン版
(`em68030_WinUI3Cpp` / `em68030_CsWPF`) でも挙動は同じです — 設定層
(MC68030 コアの `EmulatorConfig` と `mc68030` プラグインの apply 処理)
が共通だからです。

English: [mvme147_settings.md](mvme147_settings.md)

---

## 1. SCSI バス構成は Target OS ごとに独立

MVME147 ボードは NetBSD と Linux のどちらでもブートできますが、両者は
SCSI バス構成 (ディスクイメージ、`sd0` の SCSI ID、CD-ROM の ISO など)
が異なるのが普通です。

OS を切り替えるたびに SCSI 一覧を手で書き直さずに済むよう、以下のフィー
ルドは **Target OS ごとに独立して保存**されます:

- **SCSI Disks** (Path / SCSI ID を行ごとに持つ動的リスト)
- **SCSI CD-ROM Path**
- **SCSI CD-ROM ID**

設定ダイアログ上で **Target OS** コンボを切り替えると、SCSI Disks
リスト、CD-ROM Path、CD-ROM ID の表示が、選択した OS 用に記憶されている
値に切り替わります。もう一方の OS で編集していた値はなくなりません —
内部マップの該当スロットに保存されたままで、戻すと再表示されます。

それ以外の MVME147 フィールド (ROM Path、NetBSD/Linux Kernel Path、
Linux Command Line など) は OS ごと独立保存にはなっていません — 名前で
OS を区別するもの (`NetBsdKernelImagePath`) か、両 OS で共有されるもの
かのどちらかです。

### 旧形式 config からの移行

以前のビルドが保存した設定では、SCSI Disks / CD-ROM Path / CD-ROM ID は
両 OS で共有されていました。現行ビルドでこの古い設定を読み込むと、各値
は **NetBSD と Linux の両スロットに自動コピー**されます。これにより
既存のデバイスはどちらの OS を選んでも見えたままです。実際にユーザーが
編集した時点で初めて両スロットが分岐します。

旧形式の単一 JSON キー (`Mvme147ScsiDisks`、`Mvme147ScsiCdromPath`、
`Mvme147ScsiCdromId`) は、新形式マップ
(`Mvme147ScsiDisksByTargetOS` 等) と並べて書き出され続けるので、後方
互換 (新しい設定ファイルを古いビルドで開く) も大丈夫です — 古いビルドは
現在の OS に対応する値を旧キー経由で見ます。

---

## 2. **OK** を押したとき何が起きるか

「OK」ボタンの挙動は、何を変更したかと、エミュレーションを既に開始済み
か、によって 3 通りに分岐します。これは意図的な設計で、CPU やデバイスが
動作中に構造変更を即適用すると、in-flight のステートが沈黙のうちに破壊
されてしまうためです。次の 3 つのクラスに分けて扱われます:

### a. ホットスワップ可能な設定 — 常に即時適用

CPU は中断されません。例:
- JIT 有効化 / 閾値
- テーマ (Dark / Light / System)
- コンソールスクロールバックバッファ / 列・行サイズ

OK を押した瞬間に反映され、Reset は要りません。

### b. リムーバブルメディアデバイス (SCSI CD-ROM) — 常に即時適用

SCSI CD-ROM は SCSI 規格上の **リムーバブルメディア**デバイスです:
INQUIRY 応答で `RMB=1` (Removable Media Bit、byte 1 bit 7 = `0x80`)
を返し、メディア交換後の最初の SCSI コマンドに対して
`CHECK CONDITION` + `UNIT ATTENTION` (sense key `0x06`, ASC `0x28` —
*MEDIUM MAY HAVE CHANGED*) を返すことで、適切に書かれたゲスト OS が
ディスクの再検出を行います。

現状、以下の 2 つの CD-ROM フィールドはエミュレーション開始済みか
どうかに関わらず OK 押下時に即時適用されます:
- **SCSI CD-ROM Path** (ISO イメージ) — `ScsiCdrom::UnmountImage`
  + `MountImage` で実装。`m_mediaChanged` フラグを立てて次の CDB で
  UNIT ATTENTION を返します。
- **SCSI CD-ROM ID** (SCSI バス上のターゲット ID) —
  `WD33C93Device::DetachTarget(oldId)` +
  `AttachTarget(newId, scsiCdrom)` で実装。他のターゲットに影響を
  与えずに live SCSI バス上で CD-ROM の位置だけを変えます。

> **注意 (HDD は HotSwap 非対応):** SCSI Disks (`ScsiDisk`) は固定
> メディア (`RMB=0`) を表します。HDD の live hot-swap は実機でも
> ゲスト OS 側のサポートを必要としますが、NetBSD/mvme68k や
> Linux/m68k のデフォルト構成ではそのサポートがありません。
> したがって SCSI Disks はこの即時パスではなく、後述の **deferred
> パス (§2c)** を通ります — `ScsiDisk` 自体は
> `MountImage`/`UnmountImage` メソッドを持ちますが、これを live
> 操作として公開するとゲスト OS のファイルシステム状態を無音で
> 破壊してしまうためです。SCSI Disks を変更したい場合は OK 後に
> Reset が必要です。

### c. 構造的なデバイス設定 (デバイス再構築が必要)

その他の MVME147 フィールドは、エミュレーション実行中なら deferred
扱い、未実行なら即時適用です:

- **SCSI Disks** (リスト全体 — 行ごとの Path + SCSI ID)
- **Target OS**、**ROM Path**
- **NetBSD/Linux Kernel Path**、**Linux Command Line**、**Boot Partition**
- **Network Mode**、**NAT Gateway IP/MAC**、**TAP Adapter**
- **Memory Size**、**Framebuffer 有効化 / 幅 / 高さ / BPP**

挙動は現在の状態に依存:

| 状態 | OK を押したときの挙動 |
|---|---|
| 直前の Reset 以降にまだ Run されていない | デバイスツリーを破棄→新しい値で再構築、カーネル ELF が以前ロード済みなら自動で再ロード、CPU を Reset。次の **Run** で新しい構成でブート。 |
| エミュレーション中 (Stop が必要 — Run 中は OK ボタンが無効) | — |
| Run された後、Stop で一時停止中 | **deferred**: 変更は staged config に入りますが、稼働中のデバイスツリーには適用されません。ダイアログに `*` の保留マーカーが表示されます (§3 参照)。実機への反映は **Reset** または **Full Reset** が必要です。 |

なぜ実行中は deferred? MVME147 のデバイスツリー (メモリレイアウト、
マウント済み SCSI ディスク、フレームバッファ形状、ネットワークインター
フェイス) は電源 ON 時に決まる構造なので、実行途中で破棄すると OS の
状態 (open file、ページテーブル、ソケット等) が無音で壊れるためです。

---

## 3. 保留マーカー

設定が deferred 扱いになる (上記 2c) と、ダイアログには橙色の `*` が
出ます — 「この変更は staged だが実機にはまだ反映されていない」という
意味です。マーカーにマウスオーバーすると、Reset / Full Reset (または
emfe を再起動) で反映される旨のツールチップが表示されます。

- 通常設定 (文字列 / コンボ / 数値) では、エディタの左に `*` が出ます。
- **SCSI Disks** のようなリスト設定では、「SCSI Disks」のセクション
  ヘッダの隣に `*` が出ます。

コンボ操作 (例: Target OS の切替) のたびにダイアログは内部で再構築さ
れ、保留マーカーも編集状態に応じて即更新されます。

---

## 4. 典型的なワークフロー

### 別のディスクで新しいカーネルをブート

1. Settings → MVME147 タブを開く
2. SCSI Disks を編集 (追加 / 削除 / Path や ID を変更)
3. OK
4. **Run (F5)**

結果: デバイスツリーが新値で再構築され、カーネル ELF も再ロード、
新しいディスクからブート。(emfe を起動してから Run を押していない
場合のみ即時反映 — §2c 参照)

### NetBSD 実行中に CD-ROM を交換

1. Stop (Run 中は OK が無効なので一旦停止)
2. Settings → MVME147 タブ → SCSI CD-ROM Path を変更
3. OK
4. Run

結果: 新しい ISO が即座にバス上に乗ります。NetBSD 内で CD デバイスに
次にアクセスすると media change の UNIT ATTENTION が返り、新しい
イメージを `mount /dev/cd0a /mnt` できます。

### 実行中セッション中に SCSI Disks を編集

1. Run、その後 Stop (途中停止状態)
2. Settings → MVME147 タブ → SCSI Disks を編集
3. OK

結果: 変更は **deferred** (SCSI Disks セクションヘッダの隣に橙の `*`
が表示されます)。動作中の NetBSD/Linux はそのまま既存のデバイス構成
を維持。

実際に適用するには **Reset** または **Full Reset** を押します。
新しい SCSI Disks 構成でデバイスツリーが再構築され、カーネル ELF が
再ロードされ、CPU がカーネルエントリポイントから開始します。

### NetBSD ⇄ Linux を切り替え

1. Settings → MVME147 タブ → Target OS コンボを変更
2. SCSI Disks リスト、SCSI CD-ROM Path、SCSI CD-ROM ID は、もう一方
   の OS 用に記憶している値 (初回なら空 or 既定) に即切り替わる
3. (任意) OS 別フィールドをさらに編集
4. OK

エミュレーション未開始なら → 新 OS の構成で即時 device 再構築。
開始済みなら → Reset まで deferred。
