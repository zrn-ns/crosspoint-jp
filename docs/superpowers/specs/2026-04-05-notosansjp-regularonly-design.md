# NotoSansJpRegularOnly フォントバリアント設計

## 背景

NotoSansCJK (Regular+Bold, 全CJK) は9.3MBあり、メモリ・ストレージの両面で重い。
日本語専用・Regular-only・14ptのみに絞ることで~2.4MBに軽量化する。

## 変更内容

### 1. fontconvert_sdcard.py — `--codepoints-file` オプション追加
- 指定時、intervalsとcodepoints fileの両方に含まれるコードポイントのみを出力
- 未指定時は従来通り全intervalsを出力

### 2. JIS X 0213 コードポイントファイル
- パス: `lib/EpdFont/scripts/codepoints/japanese_jis0213.txt`
- 内容: ASCII, Latin1, ひらがな, カタカナ, CJK句読点, 全角形, JIS X 0213漢字(~10,050字)
- 生成: Unihan データベースの kJis0213/kJis0208 プロパティから抽出するPythonスクリプト

### 3. sd-fonts.yaml — NotoSansJpRegularOnly 定義
```yaml
- name: NotoSansJpRegularOnly
  description: "Sans-serif Japanese (Regular only, JIS X 0213)"
  intervals: ascii,latin1,punctuation,cjk
  codepoints_file: codepoints/japanese_jis0213.txt
  sizes: [14]
  styles:
    regular: {url: "...NotoSansCJKjp-Regular.otf"}
```

### 4. build-sd-fonts.py — codepoints_file 対応
- sd-fonts.yaml の codepoints_file 設定を fontconvert_sdcard.py の --codepoints-file に渡す

## 推定サイズ
~10,700文字 × ~196 bytes/glyph (14pt, 2-bit) ≈ **~2.4MB**
