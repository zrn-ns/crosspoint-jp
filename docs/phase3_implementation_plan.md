# Phase 3：実装計画（決定のみ・コード未変更）

本書はコードを変更しない。実装順・ファイル・コミット単位・ロールバック・計測・チェックリストのみ決定する。
作業ブランチ: `feature/industrial-ui`。ユーザーの未コミット変更は削除・上書き・reset しない。

---

## 1. Phase 3 で最初に実装する範囲（限定）

1. **INDUSTRIALテーマの登録**（enum / switch / settings / i18n の配線）
2. **デザイントークン**（`IndustrialMetrics::values` と `ThemeMetrics` 末尾フィールド追記）
3. **共通ヘッダー**（`drawHeader` override：ステータスレール＋画面コード）
4. **共通フッター**（`drawButtonHints` override：工業罫線ボタンガイド）
5. **リストの選択表示**（`drawList` override：反転＋左マーカー＋dimmed）
6. **ダイアログ外観**（`drawPopup`/`fillPopupProgress` override：種別マーカー＋BUSY進捗）

> **起動画面・ホーム画面の大規模変更は最初のビルド成功後に分離**（本Phaseの1〜6が緑になってから別コミット）。

---

## 2. 新規追加ファイル（既存を壊さない）

| ファイル | 内容 |
|---|---|
| `src/components/themes/industrial/IndustrialTheme.h` | `namespace IndustrialMetrics { constexpr ThemeMetrics values = {…}; }` ＋ `class IndustrialTheme : public BaseTheme`（override宣言） |
| `src/components/themes/industrial/IndustrialTheme.cpp` | 上記1〜6の `draw*` override 実装（既存プリミティブのみ、Bitmap/新mallocなし） |
| （必要時）`src/components/icons/industrial/*.h` | 手続き描画で足りない記号のみ小型1bit。原則追加しない |
| `docs/*`（本Phase群） | 監査・仕様・WF・計画（コミット済み想定） |

## 3. 最小限変更する既存ファイル（各1〜数行、機能削除なし）

| ファイル | 変更 | 根拠アンカー |
|---|---|---|
| `src/CrossPointSettings.h` | `enum UI_THEME { …, INDUSTRIAL = 3 };` に1値追加 | L173 現状 `{ CLASSIC=0, LYRA=1, LYRA_3_COVERS=2 }` |
| `src/components/UITheme.cpp` | `setTheme()` switch に `case INDUSTRIAL:`（`make_unique<IndustrialTheme>()` ＋ `currentMetrics=&IndustrialMetrics::values`） | L28–46 の switch |
| `src/SettingsList.h` | テーマEnum選択肢に `StrId::STR_THEME_INDUSTRIAL` 追加 | L124 `SettingInfo::Enum(STR_UI_THEME, …, {…})` |
| `lib/I18n/translations/english.yaml` | `STR_THEME_INDUSTRIAL: "Industrial"` ＋起動/コード用の新規STR | 既存 `STR_THEME_*` 群に追記 |
| `lib/I18n/translations/japanese.yaml` | `STR_THEME_INDUSTRIAL: "インダストリアル"` ＋対応訳 | 同上 |
| `src/components/themes/BaseTheme.h` | `struct ThemeMetrics` **末尾**に新フィールド追記（`hairlineWidth` 等）。既存 `BaseMetrics/LyraMetrics` は designated initializer なので順序非依存・デフォルト0で無影響 | `keyboardCenteredText;` の直後（`keyboardKeyCornerRadius` は `ThemeMetrics` の既存フィールドではなく、角丸値は各テーマ実装内の `constexpr` 値として管理されている。INDUSTRIALの角丸方針は実装時に `IndustrialTheme` 内で管理する） |

> 生成物（`I18nKeys.h/I18nStrings.*`）は編集しない。`gen_i18n.py` が pre-build（`platformio.ini` L58）で再生成。YAML のみコミット。

---

## 4. 最初のビルド単位（この順で各回 `pio run`）

1. **B1 骨格＋配線**: `ThemeMetrics` 末尾追記 ＋ `IndustrialMetrics::values` ＋ `IndustrialTheme`（override空＝BaseThemeに委譲）＋ enum/switch/settings/i18n。
   → ここで **選択肢に INDUSTRIAL が出て、選ぶと Classic相当で描画される**状態を緑にする。
2. **B2 ヘッダー**: `drawHeader` override。
3. **B3 フッター**: `drawButtonHints` override。
4. **B4 選択表示**: `drawList` override（反転＋左マーカー＋dimmed）。
5. **B5 ダイアログ**: `drawPopup`/`fillPopupProgress` override。
6. （分離）**B6 起動画面** / **B7 ホーム**: B1–B5 安定後に着手。

各単位でビルド失敗時はその場で原因調査・修正してから次へ。

---

## 5. コミット単位（提示→承認後にコミット、ビルド緑が条件）

1. `docs: phase0/phase2 audit, ui spec, wireframes, plan`（本Phase群）
2. `feat: register INDUSTRIAL theme skeleton and design tokens`（B1）
3. `feat: industrial common header`（B2）
4. `feat: industrial common footer / button hints`（B3）
5. `feat: industrial list selection style`（B4）
6. `feat: industrial dialog appearance`（B5）
7. `feat: industrial boot screen`（B6・分離）
8. `feat: industrial home screen`（B7・分離）
9. `docs: QA results and known issues`（Phase 5後）

コミット前に差分を提示する。`.gitignore` 対象（`*.generated.h`, `.pio/`, `platformio.local.ini`）はステージしない。メッセージは `feat:/fix:/docs:` 準拠。

---

## 6. 各段階のロールバック方法

- **コード段階**: 各コミットは小単位。問題時は該当コミットのみ `git revert`（履歴保持）。ブランチ全体は `feature/industrial-ui` に隔離され `master` は無傷。
- **テーマ選択段階**: INDUSTRIAL で不具合が出ても、設定で Classic/Lyra に戻せば既存描画に復帰（既存テーマは削除しないため）。デフォルトは既存 `uiTheme = LYRA` のまま変更しない。
- **実機段階**: 最終保証は **USB 再書き込み**（退避済み現行 `firmware.bin`/`bootloader.bin`/`partitions.bin` を esptool/Web Flasher で戻す）。OTA自動ロールバックは未確認のため頼らない（`phase0_safety_audit_v2.md` §6）。

---

## 7. RAM / Flash 計測方法

- **Flash / firmware.bin サイズ**: `pio run` のリンクサマリ（RAM/Flash %）と `.pio/build/<env>/firmware.bin` のバイト数を、B1直前(baseline)と各Bで記録。差分表を `docs` に残す。
- **RAM 実測（実機）**: 各画面遷移前後で `ESP.getFreeHeap()` をログ。計測ログは既存ランタイムフラグ **`SETTINGS.debugDisplay`** でガードし、リリースで無効化（`.skills/SKILL.md` 記載の手法、コミット可）。新規グローバルの一時デバッグ変数は使用後に除去。
- **測定点**: 起動直後 / ホーム表示後 / ライブラリ表示後 / 読書画面後 / Activity往復後。
- 「軽量化/高速化」は数値差分の提示なしに断定しない。テーマ追加は主にFlash（コード）増で、RAMはトークンがconstexpr（Flash常駐）のため増分は小さい見込み（要実測）。

---

## 8. 実機導入前チェックリスト

- [ ] 実機で動作中の版のソースと本ZIPの一致確認（`phase0_v2` §1）
- [ ] `open-x4-sdk` 取得後 `pio run -e default` が 0 error/warning
- [ ] baseline の firmware.bin サイズ・RAM/Flash% 記録済み
- [ ] 復旧用に現行動作版 `firmware.bin`/`bootloader.bin`/`partitions.bin` 退避済み
- [ ] 変更差分に OTA/パーティション/ブートローダー/SD更新処理が**含まれない**ことを `git diff` で確認
- [ ] 初回テストは UI のみ（テーマ選択で INDUSTRIAL 表示確認）、他機能は既存動作を維持
- [ ] Portrait/Landscape 双方で崩れないことを WF と照合（コード上 480/800 直書きなし）

---

## 9. 変更禁止ファイル（本Phase）

- `partitions.csv`
- bootloader 関連一切
- `src/network/OtaUpdater.cpp/.h`、`src/activities/settings/OtaUpdateActivity.*` のOTA処理本体
- `lib/Epub/`（読書エンジン・組版・`Section.cpp` キャッシュ形式）
- `EpubReaderActivity` の組版ロジック
- フォントシステム: `lib/EpdFont/SdCardFont*`, `lib/ExternalFont/*`, `FontManager`
- HAL: `lib/hal/*`
- `MappedInputManager` の物理ボタンマップ表（`kSideLayoutsX3/X4` 等）
- 生成物: `I18nKeys.h`, `I18nStrings.*`, `*.generated.h`

---

## 10. 停止条件

Phase 3 のコードには進まず、本4成果物（`phase0_safety_audit_v2.md`, `phase2_ui_spec.md`, `industrial_wireframes_v2.md`, `phase3_implementation_plan.md`）を提示して停止する。
Phase 3 着手はユーザーの明示的な承認を待つ。
