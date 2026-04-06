# 行間隔の縦書き/横書き別設定

- **Issue**: zrn-ns/crosspoint-reader#17
- **日付**: 2026-04-06
- **ステータス**: 承認済み

## 背景

現在、行間隔（lineSpacing）は単一の設定値（`uint8_t`、80〜250、パーセンテージ）で、縦書き・横書き問わず共通。縦書きと横書きでは最適な行間隔が異なるため、それぞれ独立して設定できるようにする。

## 設計

### 1. データモデル（CrossPointSettings）

`lineSpacing` を廃止し、以下の2変数に置き換える:

| 変数名 | 型 | 範囲 | デフォルト | 用途 |
|--------|------|------|-----------|------|
| `lineSpacingHorizontal` | `uint8_t` | 80〜250 | 100 | 横書き時の行間隔 |
| `lineSpacingVertical` | `uint8_t` | 80〜250 | 100 | 縦書き時の行間隔 |

既存の定数 `LINE_SPACING_MIN`, `LINE_SPACING_MAX`, `LINE_SPACING_DEFAULT` は共有。

`getReaderLineCompression()` のシグネチャを変更:

```cpp
// 変更前
float getReaderLineCompression() const;

// 変更後
float getReaderLineCompression(bool vertical) const;
```

内部で `vertical ? lineSpacingVertical : lineSpacingHorizontal` を選択して float に変換する。

### 2. 設定の保存/読み込み（JSON）

- JSONキー: `"lineSpacingHorizontal"`, `"lineSpacingVertical"`
- `SettingsList` に2エントリを登録（既存の `lineSpacing` エントリを置き換え）
- 後方互換: `JsonSettingsIO::loadSettings()` で旧 `"lineSpacing"` キーが存在し新キーが存在しない場合、旧値を両方にコピー
- バイナリ形式からの移行（`CrossPointSettings::loadFromFile` のレガシーパス）: 旧 `lineSpacing` の値を両方にコピー

### 3. 設定UI

#### Settings画面（SettingsActivity）

`SettingsList` に2項目を登録:
- 「行間隔（横書き）」(`STR_LINE_SPACING_HORIZONTAL`) → `lineSpacingHorizontal`
- 「行間隔（縦書き）」(`STR_LINE_SPACING_VERTICAL`) → `lineSpacingVertical`

既存の `LineSpacingSelectionActivity` をそのまま再利用。呼び出し側で対象の設定値を切り替える。

#### リーダーメニュー（EpubReaderMenuActivity）

- 現在の `verticalMode` に応じて該当する設定値のみ表示
- ラベルは既存の `STR_LINE_SPACING`（「行間隔」）をそのまま使用
- 値の読み書きは `verticalMode` で分岐:
  - 縦書き中 → `SETTINGS.lineSpacingVertical`
  - 横書き中 → `SETTINGS.lineSpacingHorizontal`

`verticalMode` の判定は `EpubReaderActivity` が既に保持している値を使用:
- `WM_AUTO` + CSS `vertical-rl` → 縦書き
- `WM_AUTO` + CSS指定なし → 横書き
- `WM_HORIZONTAL` 強制 → 横書き
- `WM_VERTICAL` 強制 → 縦書き

### 4. レイアウト計算への反映

`EpubReaderActivity` で `lineCompression` を取得する箇所を変更:

```cpp
// 変更前
const float lineCompression = SETTINGS.getReaderLineCompression();

// 変更後
const float lineCompression = SETTINGS.getReaderLineCompression(verticalMode);
```

`Section::loadSectionFile()` / `createSectionFile()` のインターフェースは変更不要。呼び出し元が正しい `lineCompression` を選択済みのため。

キャッシュ無効化: 既存の仕組みがそのまま機能する。`lineCompression` 値が変わればセクションキャッシュが自動再生成される。

### 5. i18n（翻訳）

新規文字列キー:
- `STR_LINE_SPACING_HORIZONTAL`: 「行間隔（横書き）」
- `STR_LINE_SPACING_VERTICAL`: 「行間隔（縦書き）」

対応言語（translations/*.yaml）に追加。既存の `STR_LINE_SPACING` はリーダーメニュー用にそのまま残す。

### 6. 影響ファイル一覧

| ファイル | 変更内容 |
|---------|---------|
| `src/CrossPointSettings.h` | `lineSpacing` → `lineSpacingHorizontal` + `lineSpacingVertical` に分離 |
| `src/CrossPointSettings.cpp` | `getReaderLineCompression(bool)` に変更、レガシー読み込みで両方にコピー |
| `src/SettingsList.h` | 1エントリ → 2エントリに分離 |
| `src/JsonSettingsIO.cpp` | 旧 `"lineSpacing"` キーからの移行ロジック追加 |
| `src/activities/reader/EpubReaderActivity.cpp` | `getReaderLineCompression(verticalMode)` に変更 |
| `src/activities/reader/EpubReaderMenuActivity.h` | `verticalMode` を受け取れるようにする |
| `src/activities/reader/EpubReaderMenuActivity.cpp` | 表示・設定する行間隔を `verticalMode` で分岐 |
| `src/activities/settings/SettingsActivity.cpp` | 2項目の表示対応（SettingsListで自動処理） |
| `lib/I18n/translations/*.yaml` | `STR_LINE_SPACING_HORIZONTAL`, `STR_LINE_SPACING_VERTICAL` 追加 |

## Done 判定基準

- [ ] `lineSpacingHorizontal` と `lineSpacingVertical` がそれぞれ独立して保存・読み込みされる
- [ ] 旧設定（`lineSpacing`キー）から新設定への移行が正しく動作する
- [ ] Settings画面に2項目が表示される
- [ ] リーダーメニューで現在の書字方向に応じた行間隔のみ表示される
- [ ] 縦書き/横書きでそれぞれ異なる行間隔が適用される
- [ ] `pio run` がエラー・警告なしで成功する
- [ ] `git diff` で意図しない変更がないことを確認

※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
