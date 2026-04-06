# CrossPoint JP Web Flasher 設計Spec

## 概要

ESP Web Toolsを使った静的Webページをcrosspoint-jpリポジトリ内に追加し、GitHub Pagesでホスティングする。ユーザーはブラウザからUSB接続でCrossPoint JPファームウェアを書き込める。

## 動機

現状ファームウェアの書き込みにはPlatformIO CLIやesptool.pyの知識が必要で、一般ユーザーにとって敷居が高い。Web Serial APIを使ったブラウザベースのフラッシュツールを提供することで、技術的知識なしにファームウェアをインストールできるようにする。

## 技術選定

- **ESP Web Tools** (https://esphome.github.io/esp-web-tools/): Espressif公式のWebコンポーネント
  - `<esp-web-install-button>` タグ1つでフラッシュUIを提供
  - Web Serial API対応ブラウザ（Chrome, Edge）で動作
  - マニフェストJSONでフラッシュ対象を定義
- **GitHub Pages**: 静的サイトホスティング（無料、設定簡単）

## ファイル構成

```
crosspoint-jp/
├── flasher/
│   └── index.html              … フラッシャーUI（静的HTML）
├── .github/workflows/
│   ├── dev-build.yml           … (既存) 変更なし
│   ├── release.yml             … (既存) 変更なし
│   └── deploy-flasher.yml      … マニフェスト生成 + gh-pagesデプロイ
```

## ESP Web Tools マニフェスト形式

ESP Web Toolsは以下形式のJSONマニフェストを要求する:

```json
{
  "name": "CrossPoint JP",
  "version": "dev-20260407-120000",
  "builds": [
    {
      "chipFamily": "ESP32-C3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
```

### フラッシュオフセット（ESP32-C3）

| パーティション | オフセット | 16進数 | 根拠 |
|---------------|-----------|--------|------|
| bootloader | 0 | 0x0 | ESP32-C3(RISC-V)のデフォルト |
| partitions | 32768 | 0x8000 | ESP-IDF標準 |
| app0 (firmware) | 65536 | 0x10000 | partitions.csv: `app0, app, ota_0, 0x10000` |

## UI設計

### index.html

シンプルな1ページ構成。外部フレームワーク不使用（Vanilla HTML/CSS/JS）。

#### 構成要素

1. **ヘッダー**: 「CrossPoint JP Flasher」タイトル
2. **ファームウェア選択**: ラジオボタン
   - 「最新安定版 (Stable)」→ `manifest_stable.json` を使用
   - 「開発版 (Dev Build)」→ `manifest_dev.json` を使用
3. **バージョン情報表示**: 選択中のファームウェアのタグ名・ビルド日時
4. **フラッシュボタン**: `<esp-web-install-button>` コンポーネント
5. **使い方説明**: 手順（USB接続 → ブラウザで開く → ファームウェア選択 → INSTALLクリック）
6. **注意事項**: 対応ブラウザ（Chrome/Edge）、初回書き込み時はデータが消去される旨

#### manifest属性の動的切り替え

```javascript
const button = document.querySelector('esp-web-install-button');
document.querySelectorAll('input[name="channel"]').forEach(radio => {
  radio.addEventListener('change', (e) => {
    button.manifest = e.target.value === 'stable'
      ? './manifest_stable.json'
      : './manifest_dev.json';
  });
});
```

## GitHub Actions ワークフロー

### release.yml の修正（前提条件）

現在の `release.yml` は `firmware.bin` のみリリースアセットに含めている。Web Flasherで安定版を書き込むには `bootloader.bin` と `partitions.bin` も必要なため、release.ymlのアセットに追加する。

```yaml
# 変更前
files: |
  firmware.bin

# 変更後
files: |
  firmware.bin
  bootloader.bin
  partitions.bin
```

### deploy-flasher.yml

**トリガー**:
- `workflow_run`: dev-build.yml または release.yml の完了後
- `workflow_dispatch`: 手動実行

**処理フロー**:

1. GitHub APIで crosspoint-jp の最新リリースを取得
   - **Stable**: `prerelease: false` の最新リリース（存在しない場合はスキップ）
   - **Dev**: `prerelease: true` の最新リリース（dev-build）
2. 各リリースのアセット（bootloader.bin, partitions.bin, firmware.bin）のダウンロードURLを取得
3. マニフェストJSON（`manifest_stable.json`, `manifest_dev.json`）を生成
   - `parts[].path` にはGitHub Releaseアセットの直接ダウンロードURLを指定
4. `flasher/index.html` + マニフェストJSON をまとめて `gh-pages` ブランチにデプロイ

### GitHub Pages設定

- ソース: `gh-pages` ブランチ
- デプロイ先のディレクトリ構造:

```
gh-pages branch root/
├── index.html
├── manifest_stable.json
├── manifest_dev.json
```

## CORSの考慮

ESP Web ToolsはマニフェストのJSON自体をfetchで取得し、各パーツのバイナリもfetchする。GitHub Releaseアセットの直接ダウンロードURL（`github.com/.../releases/download/...`）はCORSヘッダーを返すため、クロスオリジンfetchが可能。

## 制約事項

- **対応ブラウザ**: Web Serial API対応ブラウザのみ（Chrome 89+, Edge 89+）。Safari, Firefoxは非対応。
- **初回書き込み**: bootloader + partitions + firmwareの3ファイルを書き込むため、既存データは消去される。OTAアップデート（firmware.binのみ）は本ツールのスコープ外（デバイス本体のOTA機能を使用）。
- **安定版リリースが存在しない場合**: 安定版の選択肢をdisabledにし、dev-buildのみ選択可能とする。

## README更新

README.mdに「ファームウェアのインストール」セクションを追加し、Web Flasherへのリンクと簡単な手順を記載する。

## Done判定基準

- [ ] `flasher/index.html` が作成され、ESP Web Toolsコンポーネントが組み込まれている
- [ ] dev/stable切り替えUIが動作する
- [ ] `release.yml` にbootloader.bin, partitions.binが追加されている
- [ ] `deploy-flasher.yml` がdev-build/release完了後にマニフェストを生成・デプロイする
- [ ] GitHub Pagesでサイトが公開され、実際にアクセスできる
- [ ] README.mdにWeb Flasherへのリンクと手順が記載されている
- [ ] 実機でファームウェア書き込みが成功する（ユーザー検証）
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
