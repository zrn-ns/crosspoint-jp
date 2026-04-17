# ルビ表示ON/OFF切替設定 設計書

- **Issue**: zrn-ns/crosspoint-jp#46
- **作成日**: 2026-04-17

## Context

EPUB中の `<ruby>/<rt>` タグによる振り仮名（ルビ）表示は、日本語書籍の読書体験に
大きく関わる。一方で、既に漢字を読める読者にはルビが視覚的ノイズとなり、
行間や余白の見た目のバランスも損なう。このため、ルビ表示のON/OFFを
ユーザーが切り替えられるようにする。

縦書きと横書きでは読者の想定層が異なる（縦書き＝日本語文学、横書き＝混在）
ため、方向別に独立した設定とする。

## 要件

- DirectionSettings に `rubyEnabled` フラグを追加し、縦書き/横書きで独立設定可能にする
- デフォルトはON（現状動作を維持）
- 設定画面の方向別設定（横書き設定/縦書き設定）にトグル項目として追加
- OFF時はルビ描画を完全にスキップ（パース結果には影響しない）
- 設定変更後、ページ再レンダリングで即時反映される

## スコープ外

- レイアウト時のカラム間スペース最適化（OFF時に縦書きカラム間を詰めない）
  → 現状の `charSpacing=15` 既定値で視覚的に大きな問題はないため後回し
- Web UI 経由での設定変更（Device UIのみで対応）
- ルビサイズ・位置のカスタマイズ

## 設計

### 1. 設定モデル拡張

`DirectionSettings` 構造体に `uint8_t rubyEnabled = 1;` を追加
（`src/CrossPointSettings.h`）。`vertical` の初期化子リストにも `1` を追加。

### 2. 永続化 (JSON)

`src/JsonSettingsIO.cpp` の `saveDirection` / `loadDirection` ラムダに
`rubyEnabled` のシリアライズ/デシリアライズを追加。既存キーが無いJSONから
読む場合は構造体既定値（1=ON）にフォールバック。

### 3. 設定UI

`src/activities/settings/DirectionSettingsActivity.cpp::buildItems()` の末尾に
Toggle型項目を追加:

```cpp
items.push_back({StrId::STR_RUBY_ENABLED, Item::Type::TOGGLE,
                 &DirectionSettings::rubyEnabled, {}, {}});
```

`items.reserve(11)` → `reserve(12)`。

### 4. 描画制御

`TextBlock::rubyFontId` は static グローバル。ON/OFF 判定はレンダリング直前に
このIDを 0 にすることで既存コードの条件分岐 (`if (rubyFontId != 0 && ...)`)
を流用する。

`src/activities/reader/EpubReaderActivity.cpp:802-814` のセクションロード時
ルビフォント設定箇所で、`rubyDs.rubyEnabled == 0` なら `TextBlock::rubyFontId = 0`
を設定してearly return、それ以外は既存ロジックを維持。

この方式のメリット:
- TextBlock 側のコード変更不要（SETTINGS依存を追加しない）
- キャッシュ無効化不要（rubyTextsはシリアライズされ続ける）
- 設定変更後 `invalidateSectionPreservingPosition()` で即時反映

### 5. i18n

- `english.yaml`: `STR_RUBY_ENABLED: "Ruby (Furigana)"`
- `japanese.yaml`: `STR_RUBY_ENABLED: "ルビ表示"`
- 他言語は English にフォールバック（zrn-ns/crosspoint-jp は ENGLISH + JAPANESE のみ有効）

## 変更ファイル

| ファイル | 変更概要 |
|---------|---------|
| `src/CrossPointSettings.h` | DirectionSettings に rubyEnabled 追加 |
| `src/JsonSettingsIO.cpp` | save/load lambda に rubyEnabled 追加 |
| `src/activities/settings/DirectionSettingsActivity.cpp` | Toggle項目追加、reserve更新 |
| `src/activities/reader/EpubReaderActivity.cpp` | OFF時に rubyFontId=0 設定 |
| `lib/I18n/translations/english.yaml` | STR_RUBY_ENABLED 追加 |
| `lib/I18n/translations/japanese.yaml` | STR_RUBY_ENABLED 追加 |

## Done 判定基準

- [ ] 設定画面→横書き設定/縦書き設定 に「ルビ表示」トグルが表示される
- [ ] トグルOFFで開いた書籍にルビが描画されない（横書き/縦書き両方）
- [ ] トグルON/OFF切替後、リーダーに戻ると即時反映される
- [ ] 縦書き/横書きで設定が独立している（片方ONで他方OFFが可能）
- [ ] 設定がJSONに永続化され、再起動後も保持される

※ 必須ゲート（ビルド成功・既存機能・差分確認・シークレット）は常に適用

## 検証手順

1. `pio run` でビルド成功を確認（本実装では Flash 89.2%、RAM 31.9%）
2. デバイスにフラッシュし、ルビ入り日本語EPUBを開く
3. Settings → 横書き設定 → 「ルビ表示」をOFFに切替
4. リーダーに戻り、ルビが消えていることを確認
5. 縦書き書籍でも同様に確認
6. 再起動後も設定が保持されていることを確認
