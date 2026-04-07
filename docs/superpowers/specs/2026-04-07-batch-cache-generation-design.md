# 読書キャッシュ一括生成 設計Spec

## 概要

設定 > 本体メニューに「読書キャッシュを一括生成」を追加。SDカード上の全EPUBのセクション＋画像キャッシュを一括生成する。

## 実装

- `SettingAction::GenerateAllCache` を追加
- `systemSettings` の `ClearCache` の直後に配置
- 新Activity `GenerateAllCacheActivity` を作成
- SDカードを再帰スキャンして全EPUBを列挙
- 各EPUBに対して Epub::load() → 全セクション＋画像キャッシュを生成
- プログレスバー: (処理済みEPUB数 / 全EPUB数)
- 既にキャッシュ済みのセクションはスキップ（差分生成）

## i18n

- `STR_GENERATE_ALL_CACHE`: 「読書キャッシュを一括生成」
- `STR_GENERATING_ALL_CACHE`: 「全書籍のキャッシュを生成中...」
