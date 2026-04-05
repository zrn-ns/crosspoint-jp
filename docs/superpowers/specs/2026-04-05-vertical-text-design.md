# 縦書き表示（Vertical Text）サポート設計

**Issue**: https://github.com/zrn-ns/crosspoint-reader/issues/15
**Date**: 2026-04-05
**Status**: Approved

## 目的

日本語の小説を自然に読めるレベルの縦書き（tategaki）表示を実現する。
EPUBのCSS `writing-mode: vertical-rl` を検出し、CJK文字を正立、ラテン文字を回転して
右から左へカラムレイアウトで表示する。

## スコープ

### 含むもの
- CSS `writing-mode: vertical-rl` の自動検出
- OPF `page-progression-direction="rtl"` の検出
- ユーザー設定による手動切り替え（Auto / Horizontal / Vertical）
- カラムベースの縦書きレイアウトエンジン
- CJK文字の正立表示
- ラテン文字・数字（3文字以上）の90°回転表示
- 縦中横（tate-chu-yoko）: 2桁以下の数字を正立横並び
- 句読点・括弧の縦書き位置補正
- ルビ（ふりがな）の縦書き対応（列の右側に配置）
- RTLページ送り方向
- セクションキャッシュの対応

### 含まないもの
- Landscape向きでの縦書き（Portraitのみ）
- フォント形式（EpdGlyph / .cpfont）の拡張（既存メトリクスを転用）
- `writing-mode: vertical-lr`（左→右の縦書き。モンゴル語等。需要が極めて低い）
- 横書き時のルビ表示（別Issue）

## 制約

- ESP32-C3: 380KB RAM、シングルコアRISC-V 160MHz
- フォント形式の変更なし: CJKの全角文字はadvanceXを縦方向に転用
- 既存の横書きパスは変更しない: 新メソッド/分岐で対応

## 設計

### 1. CSS検出 & 設定

#### CSSパーサー拡張
- **ファイル**: `lib/Epub/Epub/css/CssParser.cpp`, `CssStyle.h`
- `CssStyle` に `CssWritingMode` enum を追加:
  ```cpp
  enum class CssWritingMode : uint8_t { HorizontalTb, VerticalRl };
  ```
- `CssStyle` に `writingMode` フィールドを追加（デフォルト: `HorizontalTb`）
- CSSパーサーの `applyRule()` で `writing-mode` プロパティを解析:
  - `horizontal-tb` → `HorizontalTb`
  - `vertical-rl` → `VerticalRl`
  - `-epub-writing-mode`, `-webkit-writing-mode` もフォールバックとして認識

#### OPF検出
- **ファイル**: `lib/Epub/Epub/parsers/ContentOpfParser.cpp`
- `<spine page-progression-direction="rtl">` を検出し、Epubオブジェクトに保存
- CSS指定がない場合のヒントとして使用（RTL page progression + 言語がjaの場合は縦書きを推定）

#### ユーザー設定
- **ファイル**: `src/CrossPointSettings.h`, `src/activities/settings/SettingsActivity.cpp`
- `SETTINGS.writingMode` を追加（0=Auto, 1=Horizontal, 2=Vertical）
- Auto: CSS/OPF指定に従う。指定なければ横書き
- Horizontal / Vertical: CSS指定を上書き

#### 実効writingModeの決定ロジック
```
if (SETTINGS.writingMode == Horizontal) → 横書き
if (SETTINGS.writingMode == Vertical)   → 縦書き
if (SETTINGS.writingMode == Auto) {
  if (CSS writing-mode == vertical-rl)  → 縦書き
  if (OPF rtl && language == ja/zh)     → 縦書き
  otherwise                             → 横書き
}
```

### 2. レイアウトエンジン

#### ParsedText拡張
- **ファイル**: `lib/Epub/Epub/ParsedText.h`, `ParsedText.cpp`
- `layoutVerticalColumns()` を新設。既存の `layoutAndExtractLines()` は変更しない
- 縦書き時のワード配置:
  1. CJK文字: advanceXを縦方向の送り量として使用（正方形前提）
  2. 連続ラテン文字/数字（3文字以上）: 「横倒しブロック」としてグループ化。ブロック全体の高さ = getTextWidth()、幅 = lineHeight
  3. 2桁以下の数字: 「縦中横ブロック」として正立配置。幅 = getTextWidth()を列幅中央に配置
- 列の高さ（= viewportHeight）で折り返し
- 列はページ右端から左へ配置。列が左端を超えたら改ページ

#### TextBlock拡張
- **ファイル**: `lib/Epub/Epub/blocks/TextBlock.h`, `TextBlock.cpp`
- `std::vector<int16_t> wordYpos` を追加（縦書き用y座標）
- 既存の `wordXpos` は横書き用にそのまま残す
- シリアライズ/デシリアライズに `wordYpos` を追加（writingModeフラグで分岐）

#### ワードのグループ化（characterData内）
- **ファイル**: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- 縦書きモード時、`characterData()` 内でラテン文字の連続を検出:
  - 3文字以上のラテン/数字連続 → 1ワードとしてaddWord（回転フラグ付き）
  - 2桁以下の数字連続 → 1ワードとしてaddWord（縦中横フラグ付き）
- `ParsedText::addWord()` にオプションの `VerticalBehavior` enum を追加:
  ```cpp
  enum class VerticalBehavior : uint8_t { Normal, Sideways, TateChuYoko };
  ```

### 3. レンダリング

#### GfxRenderer拡張
- **ファイル**: `lib/GfxRenderer/GfxRenderer.h`, `GfxRenderer.cpp`
- `drawTextVertical()` を新設:
  - 文字ごとにcodepoint判定してdrawPixelの座標変換を行う
  - CJK文字: そのまま描画、yを下に送る（送り量 = fp4::toPixel(glyph->advanceX)）
  - ラテン文字（Sideways）: 既存の `drawTextRotated90CW` のピクセル変換ロジックを流用
  - 縦中横: 正立のまま列幅中央にオフセットして描画

#### 句読点・括弧のオフセットテーブル
- **ファイル**: `lib/GfxRenderer/VerticalPunctuationTable.h`（新規）
- `constexpr` の静的配列としてFlashに配置（RAMゼロ）
- codepoint → (dx, dy) オフセットのマッピング:

| 文字 | Unicode | dx | dy | 備考 |
|---|---|---|---|---|
| 。 | U+3002 | +charWidth/3 | -charHeight/3 | 右上にずらす |
| 、 | U+3001 | +charWidth/3 | -charHeight/3 | 同上 |
| 「 | U+300C | 0 | 0 | 回転不要（フォント依存で調整） |
| 」 | U+300D | 0 | 0 | 同上 |
| （ | U+FF08 | 0 | 0 | 全角括弧はフォント内で縦書き対応済みの場合あり |
| ー | U+30FC | 回転 | - | 長音記号は90°回転 |

- オフセット値はフォントサイズに対する比率で定義し、実行時にピクセル値を算出

#### ルビ（ふりがな）の縦書き対応
- 縦書き時、ルビは親文字列の**右側**に小さいフォントで描画
- ルビの各文字も縦に並べる（正立）
- ルビの幅分だけ列間のスペースを確保

### 4. キャッシュ・シリアライゼーション

#### セクションファイル
- **ファイル**: `lib/Epub/Epub/Section.cpp`, `Section.h`
- `SECTION_FILE_VERSION` を 25 → 26 にインクリメント
- セクションヘッダーに `writingMode` フラグ（1バイト）を追加
- 既存キャッシュはバージョン不一致で自動再構築（既存の仕組み）

#### TextBlockシリアライズ
- 縦書きモード時: `wordYpos` をシリアライズ（`wordXpos` の代わり、または追加）
- ヘッダーの `writingMode` フラグでデシリアライズ時に分岐

### 5. ページ送り方向

#### RTLページ送り
- **ファイル**: `src/activities/reader/EpubReaderActivity.cpp`
- 縦書きモード時、PageForward/PageBackの方向を反転:
  - PageForward → `section->currentPage--`（前のページ = 右のページ）
  - PageBack → `section->currentPage++`（次のページ = 左のページ）
- セクション（章）間の移動も同様に反転

#### ステータスバー
- 進捗バーの方向を右→左に反転（縦書きの自然な進行方向）

## メモリ影響

| 項目 | 追加メモリ | 備考 |
|---|---|---|
| CssWritingMode フィールド | 1バイト/CssStyle | 無視できる |
| wordYpos ベクタ（TextBlock） | 横書きと同等 | wordXposの代替使用 |
| 句読点オフセットテーブル | ~200バイト | Flash配置（constexpr） |
| VerticalBehavior ベクタ | 1バイト/ワード | ParsedText内、フラッシュ時に解放 |
| レイアウト計算 | 横書きと同等 | 新メソッドだが同規模の一時メモリ |

追加ヒープ消費は実質ゼロ。縦書きは横書きの「座標軸を入れ替えた」レイアウトであり、メモリ使用パターンは同等。

## テスト方針

- 青空文庫形式のEPUB（`writing-mode: vertical-rl` 付き）で基本動作確認
- テスト項目:
  - CJK文字の正立表示
  - ラテン文字の90°回転
  - 縦中横（2桁数字）
  - 句読点の位置
  - ルビの右側配置
  - ページ送り方向（RTL）
  - 設定切り替え（Auto/Horizontal/Vertical）
  - キャッシュ無効化と再構築
  - 長文セクション（31K文字級）でのOOM耐性
