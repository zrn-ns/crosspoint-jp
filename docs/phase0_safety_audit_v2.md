# Phase 0：安全性・現状監査 v2

対象: `crosspoint-jp` (master) / 比較: `crosspoint-reader` (develop, v1.4.1)
状態: **コード未変更**。実機書き込み・OTA・SD更新・パーティション・ブートローダー・読書エンジンには一切触れていない。
v1 からの訂正5点（バージョン表記／X4動作／解像度・向き／Escape Hatch／OTAロールバック）を反映。

すべての記述は実ファイル確認に基づく。未確認は「未確認」と明記。

---

## 1. リポジトリ情報

| 項目 | 値 | 根拠 |
|---|---|---|
| フォーク元 | `zrn-ns/crosspoint-jp`（本家 `crosspoint-reader` のフォーク） | `.skills/SKILL.md` 個人フォーク節 |
| リリースタグ | **v0.1.7**（ユーザー提示） | ユーザー申告 |
| 内部ビルド用バージョン値 | `[crosspoint] version = 0.1.3` | `platformio.ini` |
| ベース本家バージョン | v1.2.0 | `README.md` / `.skills/SKILL.md` |
| 統合 | CJKフォーク＋PR#1392（SDフォント）＋PR#875（X3対応） | `.skills/SKILL.md` |

### 1-a. バージョン表記についての訂正（v1誤りの是正）

- **リリースタグ = v0.1.7**。
- **内部ビルド用バージョン値 = 0.1.3**。公式 v0.1.7 タグでも `platformio.ini` の値は 0.1.3 のまま運用されている（両者は別レイヤーの値であり矛盾しない）。
- したがって v1 監査書の「0.1.3 と v0.1.7 が一致しないため ZIP が古い可能性がある」という判断は**撤回**する。
- 正しい状態: **「ソースタグ同一性未確認」** — ZIP に `.git` が無いため、添付ソースが v0.1.7 タグのコミットと完全一致するかはコミットハッシュ照合ができず未確認。`platformio.ini` の 0.1.3 だけを根拠に古いソースと断定してはならない。
- 影響: この不確実性は UI 変更の可否に影響しない（テーマ機構・描画APIはこのソースに実在し確認済み）。ただし**実機導入前チェックリストで、実機で動作中の版のソースと本ZIPの一致を確認する**（下記 §7）。

---

## 2. 対応ハードウェア

- ESP32-C3（単コア RISC-V @160MHz）、PSRAMなし、RAM ~380KB、Flash 16MB。`.skills/SKILL.md`。
- ディスプレイ: E-Ink、単一48KBフレームバッファ（`-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1`）。
- **解像度・向き（v1訂正）**: パネルは **Portrait 480×800 / Landscape 800×480**。`GfxRenderer.h` L28–33 の Orientation enum が明記：`Portrait` = 「480x800 logical coordinates (current default)」。既定は Portrait。
  - `getScreenWidth()` は Portrait で 480（`panelHeight`）、Landscape で 800（`panelWidth`）を返す（`GfxRenderer.cpp` L1403–）。
  - `getScreenHeight()` は Portrait で 800、Landscape で 480 を返す（同 L1417–）。
  - → **`getScreenWidth/Height` を使えば向きに応じて自動的に正しい値**になる。480/800 のハードコードは不要かつ禁止。
- X3/X4 判定: `HalGPIO::deviceIsX3()`（NVS `cphw/dev_det` キャッシュ＋`cphw/dev_ovr` override）。**X4がデフォルト**（X3プローブ陰性で X4）。`lib/hal/HalGPIO.cpp` L118–165。

---

## 3. X4 動作状況（v1訂正）

- **公式 README 上は X4 未検証**（`README.md` L11「現状 Xteink X3 でのみ動作確認」）。
- **ただし本プロジェクト対象の特定実機（ユーザー所有 AliExpress 版 Xteink X4）では、CrossPoint JP v0.1.7系について以下を実機確認済み**（ユーザー申告）:
  正常起動 / EPUB表示 / 日本語表示 / 縦書き / ルビ / 日本語フォント / 青空文庫関連 / SDカード利用。
- 採用表現: **「公式にはX4未検証。ただし本プロジェクト対象の特定実機では既存ファームウェアの動作確認済み」**。
- この確認済み事実により、**X4未検証を理由に作業を停止しない**。UI変更は既存動作版を土台とする。

---

## 4. ビルド環境

- PlatformIO / Arduino-ESP32（`pioarduino platform-espressif32 55.03.37`）、C++20（`-std=gnu++2a`）、例外なし、`-Os`、`--gc-sections`。
- 環境: `default`（LOG_LEVEL=2）/ `gh_release`（LOG_LEVEL=1）。i18n = `ENGLISH, JAPANESE`。
- **当解析環境では `pio run` 実行不可**（PlatformIO未導入＋ネットワーク遮断＋`open-x4-sdk` サブモジュール未展開）。ビルド検証はユーザー側実行＋ログ貼付の往復で満たす。
- 参考目安（`.skills/SKILL.md`）: RAM ~32%、Flash **~85%**（残余が薄い→Bitmap追加回避方針と整合）。

---

## 5. 更新・復旧経路

### 5-a. 各経路の有無

| 経路 | 有無 | 根拠 |
|---|---|---|
| OTA（WiFi） | **あり** | `src/network/OtaUpdater.cpp` `esp_https_ota_begin`(L295)。元 `api.github.com/repos/zrn-ns/crosspoint-jp/releases/latest`(L13)、`firmware.bin` 取得 |
| デュアルOTAパーティション | **あり** app0/app1 各6.5MB | `partitions.csv` |
| **CrossPoint JP自身のSD更新機能** | **確認できず**（SDはブック/キャッシュ/フォント用途のみ、`esp_ota_begin`/`Update.begin` をSDパスと結ぶコードは0件） | grep結果0件 |
| **Escape Hatch経由のSD導入** | **ユーザー実機で成功済み**（update.bin導入実績） | ユーザー申告 |
| **Escape Hatchの内部実装・互換性保証** | **未確認**（crosspoint-jp本体ではなく、導入前ファームまたは別更新機構がupdate.binを書き込む経路。リポジトリ外のためコード確認不能） | リポジトリ外 |
| USB（Web Flasher / esptool） | **あり** `firmware.bin`+`bootloader.bin`+`partitions.bin` | `README.md`、`https://zrn-ns.github.io/crosspoint-jp/` |

### 5-b. Escape Hatch の扱い（v1訂正）

CrossPoint JP 内に SD ファーム更新処理が無いことと、ユーザーが Escape Hatch 経由で update.bin を導入できたことは**矛盾しない**。Escape Hatch は crosspoint-jp 本体ではなく、**導入前のファームウェアまたは別の更新機構が update.bin を書き込む経路**として扱う。上表のとおり3層に分けて記載した。

---

## 6. OTA ロールバック（v1訂正・追加調査済み）

デュアルOTAパーティションの存在だけをもって自動ロールバックが確実に有効とは**断定しない**。指定4項目＋起動確定処理を実コードで調査した結果:

| 調査対象 | 結果 | 根拠 |
|---|---|---|
| `esp_ota_mark_app_valid_cancel_rollback` | **呼び出し無し** | `grep -rn` 結果0件（src/lib全域） |
| `esp_ota_mark_app_invalid_rollback_and_reboot` | **呼び出し無し** | 同0件 |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | **リポジトリ内に該当sdkconfig/フラグ無し**（platformio.iniにも無し） | grep0件 |
| 新ファーム起動確認後の valid 確定処理 | **無し** | 上記より |
| 起動失敗時の旧パーティション復帰条件 | **未確認**（IDFデフォルト挙動に依存、ソースからは判定不能） | — |

OTA書き込みフローは `esp_https_ota_begin → perform ループ → is_complete_data_received → finish`（`OtaUpdater.cpp` L295–330）で、**finish後に valid 確定を行うコードは存在しない**。

**結論（採用表現）**:
> 旧ファームウェアが別パーティションに残る可能性はあるが、自動ロールバック動作は未確認。アプリ側に valid 確定/明示ロールバック処理は存在せず、IDF のロールバック機能が有効化されている確証もソースからは得られない。

**実務対応**: OTA本体は本作業で変更しない。復旧の最終保証はあくまで **USB（Web Flasher/esptool）による再書き込み**とし、事前に現行動作版バイナリを退避する。

---

## 7. 実機投入前に必要な確認（Phase 3直前チェックリスト）

- [ ] 実機で動作中の版のソースと本ZIPの一致確認（コミット照合の代替として主要ファイルのハッシュ/目視）
- [ ] `open-x4-sdk` 取得後 `pio run -e default` 成功（0 error/warning）、baseline の firmware.bin サイズ・RAM/Flash使用率記録
- [ ] 復旧用に**現行動作版 firmware.bin / bootloader.bin / partitions.bin を事前退避**
- [ ] OTA更新は `firmware.bin` 単体で可能か（パーティション不変前提、要実証）
- [ ] 初回UIテスト前に OTA/パーティション/ブートローダー/SD更新処理が**未変更**であることを差分で確認

---

## 8. 変更禁止領域（本作業中）

`partitions.csv` / bootloader / `src/network/OtaUpdater.*` / `OtaUpdateActivity` のOTA処理本体 / 読書エンジン（`lib/Epub/`, `EpubReaderActivity` の組版, `Section.cpp` キャッシュ形式）/ フォントシステム（`SdCardFont`, `ExternalFont`, `FontManager`）/ HAL（`lib/hal/`）/ `MappedInputManager` の物理マップ表。

---

## 9. 推奨ベース案 → A案（現行 crosspoint-jp をUI改造）

復旧可能性・OTA維持・ビルド成功率・保守性で優位（v1比較表を維持）。X4実機で既存版が動作確認済みのため、これを土台に UI 層＝`BaseTheme` 派生追加に閉じるのが最も安全。B案（本家v1.4.1へ日本語機能移植）は `lib/Epub` 大改修を伴い破損リスクが桁違い。

---

## 10. 残要確認事項（作業は停止しない）

1. ソースタグ同一性（v0.1.7タグと本ZIPの完全一致）— 未確認だがUI変更可否には非影響。実機導入前に確認。
2. ビルド検証運用 — 当環境不可のためユーザー側実行＋ログ往復。
3. OTAロールバックのIDF側設定 — 未確認。復旧はUSB再書き込みを最終保証とする。

いずれも Phase 2 の停止理由にはならない。
