# Dev Build 自動デプロイ設計

## 概要

masterブランチへのpush時に自動でファームウェアをビルドし、GitHub Releaseとして配布する。
正式リリース（タグ付き）とは別に、開発中のビルドをリリースページからダウンロード可能にする。

対応Issue: https://github.com/zrn-ns/crosspoint-reader/issues/3

## トリガー

- `master` ブランチへの `push` イベント

## ワークフロー: `.github/workflows/dev-build.yml`

### ビルドステップ

既存の `release.yml` と同じビルド手順を踏む:

1. `actions/checkout@v6`（submodules: recursive）
2. `actions/setup-python@v6`（Python 3.14）
3. `astral-sh/setup-uv@v7` でuv導入
4. PlatformIO Core インストール（pioarduino v6.1.19）
5. `pio run -e gh_release` でビルド
6. firmware.bin, bootloader.bin, partitions.bin を収集

### タグ・リリース作成

- **タグ形式**: `dev-YYYYMMDD-HHMMSS`（UTC）
  - 例: `dev-20260406-143025`
- **リリース名**: `Dev Build YYYY-MM-DD HH:MM:SS`
  - 例: `Dev Build 2026-04-06 14:30:25`
- **prereleaseフラグ**: `true`（正式リリースと区別）
- **make_latest**: `false`（正式リリースの「Latest」表示を上書きしない）

### リリースbody

以下の情報を含める:
- コミットSHA（短縮形 + フルSHAへのリンク）
- コミットメッセージ（1行目）
- ビルド環境: `gh_release`
- アセット一覧テーブル

### アセット

| ファイル | 用途 |
|---------|------|
| `firmware.bin` | OTAアップデート / フラッシュ書き込み |
| `bootloader.bin` | 初回書き込み / 復旧用 |
| `partitions.bin` | 初回書き込み / 復旧用 |

3ファイルすべてをGitHub Releaseのアセットとしてアップロードする。

## 既存ワークフローとの関係

| ワークフロー | ファイル | トリガー | 用途 | 変更 |
|---|---|---|---|---|
| CI | `ci.yml` | master push + PR | lint/ビルド検証 | なし |
| Release | `release.yml` | タグ push (`*`) | 正式リリース | なし |
| Release Candidate | `release_candidate.yml` | 手動dispatch | RC配布 | なし |
| Release Fonts | `release-fonts.yml` | 手動dispatch | フォント配布 | なし |
| **Dev Build（新規）** | `dev-build.yml` | master push | dev ビルド配布 | **新規追加** |

### 競合の回避

- `ci.yml` もmaster pushでトリガーされるが、目的が異なる（検証 vs 配布）ため共存させる
- `release.yml` はタグpushでトリガーされるため、master pushとは競合しない
- dev buildのタグは `dev-*` で始まるが、`release.yml` は全タグ（`*`）でトリガーされる。dev buildタグによる `release.yml` の二重実行を防ぐため、`release.yml` にタグ除外条件を追加するか、dev buildではタグをpushせずリリースAPIで直接作成する
  - **採用方針**: `softprops/action-gh-release` の `tag_name` パラメータでタグを指定しリリースを作成する。gitタグのpushは行わない。これにより `release.yml` のトリガー条件変更が不要

## リリース保持ポリシー

- 自動クリーンアップなし（無制限に保持）
- 不要なリリースは手動で削除

## permissions

```yaml
permissions:
  contents: write  # タグ作成 + リリース作成に必要
```

## 実装上の注意

- タイムスタンプはワークフロー実行時のUTC時刻を使用（`date -u +%Y%m%d-%H%M%S`）
- `softprops/action-gh-release@v2` を使用（既存release.ymlと同じ）
- `make_latest: false` を設定し、正式リリースの「Latest」バッジに影響しないようにする
