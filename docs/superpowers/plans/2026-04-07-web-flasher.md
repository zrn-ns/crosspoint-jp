# Web Flasher 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ESP Web Toolsベースの静的Webフラッシャーをリポジトリ内に追加し、GitHub Pagesでホスティングする

**Architecture:** `flasher/index.html` がESP Web Toolsコンポーネントを読み込み、GitHub Actions生成のマニフェストJSON経由でGitHub Releaseアセットのバイナリを書き込む。`deploy-flasher.yml` ワークフローがリリース後にマニフェスト生成→gh-pagesデプロイを自動実行する。

**Tech Stack:** ESP Web Tools (CDN), Vanilla HTML/CSS/JS, GitHub Actions, GitHub Pages

**Spec:** `docs/superpowers/specs/2026-04-07-web-flasher-design.md`

---

## ファイル構成

| 操作 | パス | 責務 |
|------|------|------|
| 作成 | `flasher/index.html` | フラッシャーUI（ESP Web Toolsコンポーネント + dev/stable切り替え） |
| 作成 | `.github/workflows/deploy-flasher.yml` | マニフェストJSON生成 + gh-pagesブランチへデプロイ |
| 変更 | `.github/workflows/release.yml:59-66` | リリースアセットにbootloader.bin, partitions.binを追加 |
| 変更 | `README.md:47-54` | インストールセクションにWeb Flasherリンクを追加 |

---

### Task 1: flasher/index.html を作成

**Files:**
- Create: `flasher/index.html`

- [ ] **Step 1: index.htmlを作成**

`flasher/index.html` を以下の内容で作成する。ESP Web ToolsはCDN（unpkg）から読み込む。

```html
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>CrossPoint JP Flasher</title>
  <script
    type="module"
    src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"
  ></script>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      max-width: 640px;
      margin: 0 auto;
      padding: 2rem 1rem;
      color: #1a1a1a;
      line-height: 1.6;
    }
    h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .subtitle { color: #666; margin-bottom: 2rem; font-size: 0.9rem; }
    .channel-select {
      background: #f5f5f5;
      border-radius: 8px;
      padding: 1rem;
      margin-bottom: 1.5rem;
    }
    .channel-select h2 { font-size: 1rem; margin-bottom: 0.75rem; }
    .channel-option {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      padding: 0.5rem 0;
    }
    .channel-option label { cursor: pointer; }
    .channel-option .desc { color: #666; font-size: 0.85rem; margin-left: 1.5rem; }
    .version-info {
      font-size: 0.85rem;
      color: #666;
      margin-left: 1.5rem;
      margin-top: 0.25rem;
    }
    .install-section { text-align: center; margin: 2rem 0; }
    .instructions {
      background: #f9f9f9;
      border-radius: 8px;
      padding: 1rem;
      margin-bottom: 1.5rem;
    }
    .instructions h2 { font-size: 1rem; margin-bottom: 0.75rem; }
    .instructions ol { padding-left: 1.5rem; }
    .instructions li { margin-bottom: 0.5rem; }
    .warning {
      background: #fff3cd;
      border: 1px solid #ffc107;
      border-radius: 8px;
      padding: 1rem;
      font-size: 0.85rem;
    }
    .warning h2 { font-size: 1rem; margin-bottom: 0.5rem; }
    .warning ul { padding-left: 1.5rem; }
    .warning li { margin-bottom: 0.25rem; }
    .no-serial {
      display: none;
      background: #f8d7da;
      border: 1px solid #f5c6cb;
      border-radius: 8px;
      padding: 1rem;
      margin-bottom: 1.5rem;
      font-size: 0.85rem;
    }
    .disabled { opacity: 0.5; pointer-events: none; }
  </style>
</head>
<body>
  <h1>CrossPoint JP Flasher</h1>
  <p class="subtitle">ブラウザからXteinkデバイスにファームウェアを書き込めます</p>

  <div class="no-serial" id="no-serial">
    <strong>お使いのブラウザはWeb Serial APIに対応していません。</strong><br>
    Chrome 89以降 または Edge 89以降をご利用ください。
  </div>

  <div class="channel-select">
    <h2>ファームウェアの選択</h2>
    <div class="channel-option">
      <input type="radio" id="ch-dev" name="channel" value="dev" checked>
      <label for="ch-dev"><strong>開発版 (Dev Build)</strong></label>
    </div>
    <div class="desc">最新のmaster変更を含むビルド。新機能をいち早く試せます。</div>
    <div class="version-info" id="version-dev"></div>

    <div class="channel-option" style="margin-top: 0.75rem;">
      <input type="radio" id="ch-stable" name="channel" value="stable">
      <label for="ch-stable"><strong>安定版 (Stable)</strong></label>
    </div>
    <div class="desc">テスト済みの安定リリース。</div>
    <div class="version-info" id="version-stable"></div>
  </div>

  <div class="install-section">
    <esp-web-install-button id="install-btn" manifest="./manifest_dev.json">
      <button slot="activate">インストール</button>
      <span slot="unsupported">
        お使いのブラウザは対応していません。Chrome または Edge をご利用ください。
      </span>
      <span slot="not-allowed">
        HTTPS接続が必要です。
      </span>
    </esp-web-install-button>
  </div>

  <div class="instructions">
    <h2>使い方</h2>
    <ol>
      <li>XteinkデバイスをUSB-Cケーブルでパソコンに接続</li>
      <li>上のファームウェアを選択し「インストール」をクリック</li>
      <li>シリアルポートの選択ダイアログでデバイスを選択</li>
      <li>書き込みが完了するまで待機（約2分）</li>
    </ol>
  </div>

  <div class="warning">
    <h2>注意事項</h2>
    <ul>
      <li>対応ブラウザ: Chrome 89+, Edge 89+（Safari, Firefoxは非対応）</li>
      <li>初回書き込み時は既存のデータが消去されます</li>
      <li>書き込み中はケーブルを抜かないでください</li>
      <li>書き込み後、デバイスは自動的に再起動します</li>
    </ul>
  </div>

  <script>
    // Web Serial API非対応ブラウザの検出
    if (!('serial' in navigator)) {
      document.getElementById('no-serial').style.display = 'block';
    }

    // チャンネル切り替え
    const installBtn = document.getElementById('install-btn');
    document.querySelectorAll('input[name="channel"]').forEach(function(radio) {
      radio.addEventListener('change', function(e) {
        installBtn.manifest = e.target.value === 'stable'
          ? './manifest_stable.json'
          : './manifest_dev.json';
      });
    });

    // マニフェストからバージョン情報を取得して表示
    function loadVersionInfo(manifestUrl, elementId) {
      fetch(manifestUrl)
        .then(function(r) { return r.ok ? r.json() : null; })
        .then(function(data) {
          if (data && data.version) {
            document.getElementById(elementId).textContent = 'バージョン: ' + data.version;
          }
        })
        .catch(function() {
          // stableが未リリースの場合など
          if (elementId === 'version-stable') {
            document.getElementById('ch-stable').disabled = true;
            document.getElementById(elementId).textContent = '（まだリリースされていません）';
            document.querySelector('label[for="ch-stable"]').parentElement.classList.add('disabled');
          }
        });
    }

    loadVersionInfo('./manifest_dev.json', 'version-dev');
    loadVersionInfo('./manifest_stable.json', 'version-stable');
  </script>
</body>
</html>
```

- [ ] **Step 2: ブラウザで開いて構造を確認**

```bash
open flasher/index.html
```

ページが表示されること、dev/stableラジオボタンが存在すること、「インストール」ボタンが表示されることを目視確認。マニフェストJSONがまだ無いためバージョン情報は表示されないが、UI構造が正しいことを確認する。

- [ ] **Step 3: コミット**

```bash
git add flasher/index.html
git commit -m "✨ Web Flasher UIを追加（Issue #9）"
```

---

### Task 2: release.yml にbootloader.bin, partitions.binを追加

**Files:**
- Modify: `.github/workflows/release.yml:59-66`

- [ ] **Step 1: release.ymlのリリースアセットを修正**

`.github/workflows/release.yml` の `Create Release` ステップの `files` フィールドに `bootloader.bin` と `partitions.bin` を追加する。

変更前（59-66行目付近）:
```yaml
      - name: Create Release
        uses: softprops/action-gh-release@v2
        with:
          name: CrossPoint Reader ${{ github.ref_name }}
          files: |
            firmware.bin
```

変更後:
```yaml
      - name: Create Release
        uses: softprops/action-gh-release@v2
        with:
          name: CrossPoint Reader ${{ github.ref_name }}
          files: |
            firmware.bin
            bootloader.bin
            partitions.bin
```

- [ ] **Step 2: コミット**

```bash
git add .github/workflows/release.yml
git commit -m "👍 release.ymlにbootloader/partitionsアセットを追加（Issue #9）"
```

---

### Task 3: deploy-flasher.yml ワークフローを作成

**Files:**
- Create: `.github/workflows/deploy-flasher.yml`

- [ ] **Step 1: deploy-flasher.ymlを作成**

`.github/workflows/deploy-flasher.yml` を以下の内容で作成する。

```yaml
name: Deploy Flasher

on:
  workflow_run:
    workflows: ["Dev Build", "Compile Release"]
    types: [completed]
  workflow_dispatch:

permissions:
  contents: write

concurrency:
  group: deploy-flasher
  cancel-in-progress: true

jobs:
  deploy:
    runs-on: ubuntu-latest
    if: ${{ github.event_name == 'workflow_dispatch' || github.event.workflow_run.conclusion == 'success' }}
    steps:
      - uses: actions/checkout@v6

      - name: Generate manifests
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          set -euo pipefail
          REPO="${{ github.repository }}"

          mkdir -p _site

          # Dev manifest (latest prerelease)
          DEV_TAG=$(gh api "repos/${REPO}/releases" --jq '[.[] | select(.prerelease == true)] | first | .tag_name // empty')
          if [ -n "${DEV_TAG}" ]; then
            DEV_BASE="https://github.com/${REPO}/releases/download/${DEV_TAG}"
            cat > _site/manifest_dev.json <<MANIFEST
          {
            "name": "CrossPoint JP (Dev)",
            "version": "${DEV_TAG}",
            "builds": [
              {
                "chipFamily": "ESP32-C3",
                "parts": [
                  { "path": "${DEV_BASE}/bootloader.bin", "offset": 0 },
                  { "path": "${DEV_BASE}/partitions.bin", "offset": 32768 },
                  { "path": "${DEV_BASE}/firmware.bin", "offset": 65536 }
                ]
              }
            ]
          }
          MANIFEST
            echo "Dev manifest generated: ${DEV_TAG}"
          else
            echo "No dev release found, skipping dev manifest"
          fi

          # Stable manifest (latest non-prerelease)
          STABLE_TAG=$(gh api "repos/${REPO}/releases" --jq '[.[] | select(.prerelease == false and .draft == false)] | first | .tag_name // empty')
          if [ -n "${STABLE_TAG}" ]; then
            STABLE_BASE="https://github.com/${REPO}/releases/download/${STABLE_TAG}"
            cat > _site/manifest_stable.json <<MANIFEST
          {
            "name": "CrossPoint JP (Stable)",
            "version": "${STABLE_TAG}",
            "builds": [
              {
                "chipFamily": "ESP32-C3",
                "parts": [
                  { "path": "${STABLE_BASE}/bootloader.bin", "offset": 0 },
                  { "path": "${STABLE_BASE}/partitions.bin", "offset": 32768 },
                  { "path": "${STABLE_BASE}/firmware.bin", "offset": 65536 }
                ]
              }
            ]
          }
          MANIFEST
            echo "Stable manifest generated: ${STABLE_TAG}"
          else
            echo "No stable release found, skipping stable manifest"
          fi

          # Copy index.html
          cp flasher/index.html _site/index.html

          echo "=== Site contents ==="
          ls -la _site/

      - name: Deploy to GitHub Pages
        uses: peaceiris/actions-gh-pages@v4
        with:
          github_token: ${{ secrets.GITHUB_TOKEN }}
          publish_dir: ./_site
          force_orphan: true
```

- [ ] **Step 2: ワークフローの構文を検証**

```bash
# actionlintがあれば使用、なければyaml構文のみ確認
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/deploy-flasher.yml'))" && echo "YAML valid"
```

Expected: `YAML valid`

- [ ] **Step 3: コミット**

```bash
git add .github/workflows/deploy-flasher.yml
git commit -m "✨ Web Flasherデプロイワークフローを追加（Issue #9）"
```

---

### Task 4: README.mdを更新

**Files:**
- Modify: `README.md:47-54`

- [ ] **Step 1: READMEのインストールセクションを書き換え**

`README.md` の「## インストール」セクション（47-54行目）を以下に置き換える。

変更前:
```markdown
## インストール

1. [Releases ページ](https://github.com/zrn-ns/crosspoint-jp/releases)から最新の Dev Build を開く
2. `firmware.bin` をダウンロード
3. Xteink をUSB-Cでパソコンに接続
4. https://xteink.dve.al/ からOTAフラッシュで書き込み

初回書き込みの場合は `bootloader.bin` と `partitions.bin` も必要です。
```

変更後:
```markdown
## インストール

### Web Flasher（推奨）

**[CrossPoint JP Flasher](https://zrn-ns.github.io/crosspoint-jp/)** からブラウザ上で簡単にインストールできます。

1. XteinkデバイスをUSB-Cケーブルでパソコンに接続
2. Chrome または Edge で [CrossPoint JP Flasher](https://zrn-ns.github.io/crosspoint-jp/) を開く
3. ファームウェア（開発版 / 安定版）を選択し「インストール」をクリック
4. シリアルポート選択ダイアログでデバイスを選択

対応ブラウザ: Chrome 89+, Edge 89+

### 手動インストール

1. [Releases ページ](https://github.com/zrn-ns/crosspoint-jp/releases)から最新の Dev Build を開く
2. `firmware.bin`, `bootloader.bin`, `partitions.bin` をダウンロード
3. Xteink をUSB-Cでパソコンに接続
4. esptool.py または https://xteink.dve.al/ から書き込み
```

- [ ] **Step 2: コミット**

```bash
git add README.md
git commit -m "👍 READMEにWeb Flasherの案内を追加（Issue #9）"
```

---

### Task 5: 差分確認と最終検証

- [ ] **Step 1: 全変更の差分を確認**

```bash
git log --oneline -5
git diff HEAD~4..HEAD --stat
```

以下の4ファイルが変更されていること:
- `flasher/index.html` (新規作成)
- `.github/workflows/deploy-flasher.yml` (新規作成)
- `.github/workflows/release.yml` (修正)
- `README.md` (修正)

- [ ] **Step 2: GitHub Pagesの有効化手順を確認**

pushしてワークフローが初回実行された後、GitHub Pages の有効化が必要:

1. リポジトリ Settings > Pages
2. Source: `Deploy from a branch`
3. Branch: `gh-pages` / `/ (root)`
4. Save

これはユーザーが手動で行う必要がある（初回のみ）。

- [ ] **Step 3: pushしてワークフローの動作を確認**

```bash
git push origin master
```

push後、GitHub Actionsの `Deploy Flasher` ワークフローが手動実行可能であることを確認（`workflow_dispatch` トリガー）。最初のdev-buildが完了するまでは `workflow_run` トリガーは発火しない。

手動実行:
```bash
gh workflow run deploy-flasher.yml
```

ワークフロー完了後、`gh-pages` ブランチが作成され、マニフェストJSONが含まれていることを確認:
```bash
gh api repos/zrn-ns/crosspoint-jp/git/trees/gh-pages --jq '.tree[].path'
```

Expected output:
```
index.html
manifest_dev.json
```
（stableリリースが無い場合 `manifest_stable.json` は生成されない）
