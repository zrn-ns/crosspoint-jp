# SDカードフォント: 単一バリアント+レンダリング時スケーリング設計

## 背景と動機

SDカードフォント（NotoSansCJK等）で複数サイズ×複数スタイルの.cpfontファイルをロードすると、prewarmデータがヒープを圧迫しメモリエラーが発生する。ESP32-C3の380KB RAMという制約下で安定動作させるため、1つの.cpfontファイルのみロードし、表示側で拡大縮小・太字合成を行う方式に変更する。

## 設計判断

| 項目 | 決定 | 理由 |
|------|------|------|
| ベースサイズ | 14pt | メモリ(~130KB prewarm)と品質のバランス。18ptは~200KBでヒープが厳しい |
| スケーリングレイヤー | GfxRenderer（描画時） | prewarmデータが1セットで済みメモリ最小 |
| スケーリングアルゴリズム | nearest-neighbor | E-inkのモノクロ/2bit表示では十分。実装がシンプルで高速 |
| Bold合成 | 水平1pxシフト+OR | シンプル・高速・CJKの太字として自然な見た目 |
| 固定小数点 | 8.8 (スケール用) | 256=1.0倍。整数演算のみでESP32-C3に最適 |

## アーキテクチャ

```
SDカード: NotoSansCJK_14.cpfont (Regular only)
            ↓ load()
     SdCardFont (1インスタンス, 14pt)
            ↓ register × 4 仮想fontId
     GfxRenderer:
       sdCardFonts_[fontId_12] → SdCardFont* (scale=219/256=0.857)
       sdCardFonts_[fontId_14] → SdCardFont* (scale=256/256=1.000)
       sdCardFonts_[fontId_16] → SdCardFont* (scale=293/256=1.143)
       sdCardFonts_[fontId_18] → SdCardFont* (scale=329/256=1.286)
            ↓
     renderChar(): ベース14ptグリフ × scale で描画
     getTextAdvanceX(): ベースadvance × scale で計算
     Bold要求時: 1px右シフト再描画で合成
```

### メモリ使用量比較

| 方式 | prewarmデータ | SdCardFontオブジェクト | 合計 |
|------|-------------|---------------------|------|
| 現行（4サイズ×2スタイル） | ~130KB × N | 8個 | メモリエラー発生 |
| 新方式（1ファイル） | ~130KB × 1 | 1個 | ~130KB + 余裕~94KB |

## コンポーネント別変更

### 1. SdCardFontManager::loadFamily()

**変更内容**: 全ファイルから1つだけロードし、4つの仮想fontIdで登録

- 優先順位: 14pt → 最も近いサイズ
- ロードしたSdCardFontを4つのfontId（12/14/16/18pt用）で`renderer.registerSdCardFont()`
- 各fontIdに対応するスケールファクターを`renderer.registerSdCardFontScale(fontId, scale)`で登録
- `loaded_`ベクタには1エントリのみ（二重free防止）
- fontMapにも4つの仮想fontIdで同じEpdFontFamilyを登録

### 2. GfxRenderer — スケールファクター管理

**新メンバ**:
```cpp
std::map<int, uint16_t> sdCardFontScales_;  // fontId → 8.8固定小数点スケール
```

**新メソッド**:
```cpp
void registerSdCardFontScale(int fontId, uint16_t scale);
void clearSdCardFontScales();
uint16_t getSdCardFontScale(int fontId) const;  // 見つからなければ256(=1.0)を返す
```

### 3. GfxRenderer::renderChar() — ビットマップスケーリング

SDカードフォント検出時（`sdCardFontScales_`にfontIdが存在）:

```cpp
uint16_t scale = getSdCardFontScale(fontId);
uint16_t targetW = (glyph->width * scale + 128) >> 8;
uint16_t targetH = (glyph->height * scale + 128) >> 8;
int16_t targetLeft = (glyph->left * scale + 128) >> 8;
int16_t targetTop = (glyph->top * scale + 128) >> 8;

for (uint16_t ty = 0; ty < targetH; ty++) {
    uint16_t sy = ty * 256 / scale;
    for (uint16_t tx = 0; tx < targetW; tx++) {
        uint16_t sx = tx * 256 / scale;
        // bitmap[sy * width + sx] のピクセルを描画位置に出力
    }
}
```

**Bold合成**:
- Boldスタイル要求時かつフォントにBoldスタイルがない場合:
  - 上記スケーリング描画を2回実行（オフセット0, +1px）
  - 2-bitモード: ピクセル値をOR（濃い方を採用）
  - advanceXに1px加算

### 4. GfxRenderer::getTextAdvanceX() — メトリクススケーリング

sdCardFonts_ fast-path内:
```cpp
auto sdIt = sdCardFonts_.find(fontId);
if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    uint16_t scale = getSdCardFontScale(fontId);
    int32_t widthFP = 0;
    while (uint32_t cp = utf8NextCodepoint(...)) {
        widthFP += sdIt->second->getAdvance(cp, styleIdx);
    }
    // スケール適用してからピクセル変換
    return fp4::toPixel(static_cast<int32_t>(widthFP * scale / 256));
}
```

### 5. スケール対象メトリクス一覧

| メトリクス | 適用箇所 | スケール方法 |
|-----------|---------|-------------|
| advanceX (レイアウト) | getTextAdvanceX, getSpaceAdvance | `advance * scale >> 8` |
| advanceY (行高さ) | getData()->advanceY参照箇所 | `advanceY * scale >> 8` |
| width, height (描画) | renderChar | `w * scale >> 8` |
| left, top (オフセット) | renderChar | `left * scale >> 8` |
| ascender, descender | getData()参照箇所 | `asc * scale >> 8` |
| kerning | getKerning | `kern * scale >> 8` |

### 6. SdCardFontManager::unloadAll()

- `loaded_`ベクタの1エントリをdelete（SdCardFontオブジェクト1つ）
- `renderer.clearSdCardFonts()` で4つのfontIdエントリを削除（ポインタ削除のみ、freeなし）
- `renderer.clearSdCardFontScales()` でスケールマップをクリア
- fontMapから4つの仮想fontIdを削除

## エッジケース

### 14ptが存在しない場合
`loadFamily()`で全サイズから最も近いサイズを選択。スケールファクターは実ベースサイズから再計算。

### Bold要求時のフォールバック
1. .cpfontにBoldスタイルあり → そのまま使用（スケーリングのみ適用）
2. Boldスタイルなし → 合成Bold（1px水平シフト+OR）

### 見出しフォント
h1: +2サイズステップ, h2: +1サイズステップ。仮想fontIdが異なるだけなので既存のgetHeadingFontId()は変更不要。

### 二重free防止
- `sdCardFonts_`に4つのfontIdが同一ポインタで登録
- `loaded_`ベクタは1エントリ → deleteは1回
- `clearSdCardFonts()`はマップクリアのみ（deleteしない）→ 既存動作と同じ

## スコープ外

- Italic合成（座標変換が複雑、Regularで代替）
- バイリニア補間（nearest-neighborで十分）
- .cpfontファイルフォーマットの変更
