# Optimize EPUB機能のX3対応 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `src/network/html/FilesPage.html` の Optimize EPUB 機能をデバイス解像度（X4: 480×800 / X3: 528×792）に応じて動的に動作させる。

**Architecture:** `/api/status` が返す `screenWidth/screenHeight` を `MAX_WIDTH/MAX_HEIGHT` に反映。Optimize EPUB の内部ロジックは「縦向き読書時の長辺/短辺」を前提とするため、`Math.min/max` で縦向き相当に正規化。ハードコードされた `480`/`800` リテラルを変数参照へ置換。

**Tech Stack:** HTML/JavaScript (vanilla), PlatformIO build pipeline (`scripts/build_html.py` が `FilesPageHtml.generated.h` を再生成)

**Spec:** `docs/superpowers/specs/2026-04-29-optimize-epub-x3-design.md`

---

## File Structure

- **Modify:** `src/network/html/FilesPage.html`
  - line 1543: 静的ラベル `📏 Max 480×800px` に `id` 属性を付与
  - line 3320-3333: `fetchVersion()` に `updateOptimizerDimensions()` 呼び出しを追加
  - line 3333 直後: `updateOptimizerDimensions()` / `updateOptimizerLabel()` 関数を新規追加
  - line 2391, 2396-2397: `extractEpubImages` 内のリテラル置換
  - line 2635-2644: `renderImageGrid` 内のリテラル置換
  - line 4156-4157, 4206, 4281, 4283: `processImage` 内のコメントを実態に合わせて修正

- **Auto-regenerate:** `src/network/html/FilesPageHtml.generated.h` (gitignored、`pio run` で再生成)

---

## Task 1: 静的ラベルへの ID 付与

**Files:**
- Modify: `src/network/html/FilesPage.html:1543`

- [ ] **Step 1: ラベルに `id="optimizerResolutionInfo"` を付与**

`src/network/html/FilesPage.html:1543` を以下に変更:

Before:
```html
          <span>📏 Max 480×800px</span>
```

After:
```html
          <span id="optimizerResolutionInfo">📏 Max 480×800px</span>
```

- [ ] **Step 2: 変更が正しく入ったか確認**

Run: `grep -n 'optimizerResolutionInfo' src/network/html/FilesPage.html`
Expected: 1行マッチ（line 1543付近）

- [ ] **Step 3: コミット**

```bash
git add src/network/html/FilesPage.html
git commit -m "$(cat <<'EOF'
✨ Optimize EPUBラベルにIDを付与

X3対応のため、Optimize EPUBの解像度表示ラベルに
id="optimizerResolutionInfo" を追加し動的更新可能にする。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `updateOptimizerDimensions` / `updateOptimizerLabel` 関数の追加と `fetchVersion` への組み込み

**Files:**
- Modify: `src/network/html/FilesPage.html:3320-3333`

- [ ] **Step 1: `fetchVersion()` 内で `updateOptimizerDimensions()` を呼び出すよう変更**

`src/network/html/FilesPage.html:3320-3333` を以下に変更:

Before:
```js
// Fetch version and device info from API
async function fetchVersion() {
  try {
    const response = await fetch('/api/status');
    if (response.ok) {
      const data = await response.json();
      crosspointVersion = data.version || 'Unknown';
      if (data.screenWidth) deviceScreenWidth = data.screenWidth;
      if (data.screenHeight) deviceScreenHeight = data.screenHeight;
    }
  } catch (e) {
    console.error('Failed to fetch version:', e);
  }
}
```

After:
```js
// Fetch version and device info from API
async function fetchVersion() {
  try {
    const response = await fetch('/api/status');
    if (response.ok) {
      const data = await response.json();
      crosspointVersion = data.version || 'Unknown';
      if (data.screenWidth) deviceScreenWidth = data.screenWidth;
      if (data.screenHeight) deviceScreenHeight = data.screenHeight;
      updateOptimizerDimensions();
    }
  } catch (e) {
    console.error('Failed to fetch version:', e);
  }
}

// Optimize EPUBは常に縦向き読書を想定: 短辺=MAX_WIDTH, 長辺=MAX_HEIGHT
// デバイス向きに依存しないよう min/max で正規化する
function updateOptimizerDimensions() {
  MAX_WIDTH  = Math.min(deviceScreenWidth, deviceScreenHeight);
  MAX_HEIGHT = Math.max(deviceScreenWidth, deviceScreenHeight);
  updateOptimizerLabel();
}

function updateOptimizerLabel() {
  const el = document.getElementById('optimizerResolutionInfo');
  if (el) el.textContent = `📏 Max ${MAX_WIDTH}×${MAX_HEIGHT}px`;
}
```

- [ ] **Step 2: 変更が正しく入ったか確認**

Run: `grep -n 'updateOptimizerDimensions\|updateOptimizerLabel' src/network/html/FilesPage.html`
Expected:
- `fetchVersion` 内に `updateOptimizerDimensions()` 呼び出し1箇所
- `updateOptimizerDimensions` 関数定義1箇所
- `updateOptimizerLabel` 関数定義1箇所
- 関数内で `updateOptimizerLabel()` 呼び出し1箇所
- 関数内で `getElementById('optimizerResolutionInfo')` 1箇所

合計5～6行マッチ。

- [ ] **Step 3: コミット**

```bash
git add src/network/html/FilesPage.html
git commit -m "$(cat <<'EOF'
✨ Optimizer解像度をデバイスから動的取得

fetchVersion() 内で updateOptimizerDimensions() を呼び出し、
MAX_WIDTH/MAX_HEIGHT を deviceScreenWidth/Height から
min/max で正規化して反映する。ラベルも同時更新する。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `extractEpubImages` 内のリテラル置換

**Files:**
- Modify: `src/network/html/FilesPage.html:2391-2397`

- [ ] **Step 1: フィット判定とSplit可否判定をMAX_WIDTH/MAX_HEIGHTで置換**

`src/network/html/FilesPage.html:2390-2397` 付近を以下に変更:

Before:
```js
      // Images that fit screen can only rotate, not split
      const fitsScreen = (dims.width <= 480 && dims.height <= 800);
      
      // Split capability - no upscaling allowed
      // H-Split scales width to 800, so needs width >= 800
      // V-Split scales height to 800, so needs height >= 800
      const canHSplit = dims.width >= 800;
      const canVSplit = dims.height >= 800;
```

After:
```js
      // Images that fit screen can only rotate, not split
      const fitsScreen = (dims.width <= MAX_WIDTH && dims.height <= MAX_HEIGHT);
      
      // Split capability - no upscaling allowed
      // H-Split scales width to MAX_HEIGHT, so needs width >= MAX_HEIGHT
      // V-Split scales height to MAX_HEIGHT, so needs height >= MAX_HEIGHT
      const canHSplit = dims.width >= MAX_HEIGHT;
      const canVSplit = dims.height >= MAX_HEIGHT;
```

- [ ] **Step 2: 変更箇所に `480`/`800` リテラルが残っていないか確認**

Run: `sed -n '2385,2400p' src/network/html/FilesPage.html | grep -E '\b(480|800)\b'`
Expected: マッチなし（出力が空）

- [ ] **Step 3: コミット**

```bash
git add src/network/html/FilesPage.html
git commit -m "$(cat <<'EOF'
👍 extractEpubImagesでMAX_WIDTH/MAX_HEIGHTを参照

X3対応のため、フィット判定とSplit可否判定の
ハードコードされた480/800をMAX_WIDTH/MAX_HEIGHTに置換。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `renderImageGrid` 内のSplitプレビュー計算のリテラル置換

**Files:**
- Modify: `src/network/html/FilesPage.html:2632-2647`

- [ ] **Step 1: Splitプレビュー計算を MAX_WIDTH/MAX_HEIGHT で置換**

`src/network/html/FilesPage.html:2632-2647` 付近を以下に変更:

Before:
```js
        if (showSplitLines) {
          let finalWidth;
          if (state === 1) {
            // H-Split: scale width to 800, rotate, then check width
            const scaledH = Math.round(img.height * (800 / img.width));
            finalWidth = scaledH; // After rotation, height becomes width
          } else {
            // V-Split: scale height to 800, then check width
            finalWidth = Math.round(img.width * (800 / img.height));
          }
          if (finalWidth > 480) {
            const minOverlapPx = Math.round(480 * (OVERLAP_PERCENT / 100));
            const maxStep = 480 - minOverlapPx;
            numParts = Math.ceil((finalWidth - minOverlapPx) / maxStep);
            if (numParts < 2) numParts = 2;
          }
        }
```

After:
```js
        if (showSplitLines) {
          let finalWidth;
          if (state === 1) {
            // H-Split: scale width to MAX_HEIGHT, rotate, then check width
            const scaledH = Math.round(img.height * (MAX_HEIGHT / img.width));
            finalWidth = scaledH; // After rotation, height becomes width
          } else {
            // V-Split: scale height to MAX_HEIGHT, then check width
            finalWidth = Math.round(img.width * (MAX_HEIGHT / img.height));
          }
          if (finalWidth > MAX_WIDTH) {
            const minOverlapPx = Math.round(MAX_WIDTH * (OVERLAP_PERCENT / 100));
            const maxStep = MAX_WIDTH - minOverlapPx;
            numParts = Math.ceil((finalWidth - minOverlapPx) / maxStep);
            if (numParts < 2) numParts = 2;
          }
        }
```

- [ ] **Step 2: 変更箇所に `480`/`800` リテラルが残っていないか確認**

Run: `sed -n '2630,2650p' src/network/html/FilesPage.html | grep -E '\b(480|800)\b'`
Expected: マッチなし（出力が空）

- [ ] **Step 3: コミット**

```bash
git add src/network/html/FilesPage.html
git commit -m "$(cat <<'EOF'
👍 SplitプレビューでMAX_WIDTH/MAX_HEIGHTを参照

X3対応のため、renderImageGrid内のSplitプレビュー計算の
ハードコードされた480/800をMAX_WIDTH/MAX_HEIGHTに置換。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `processImage` のコメント内 `480`/`800` 数値を実態に合わせて修正

**Files:**
- Modify: `src/network/html/FilesPage.html:4150-4451`

`processImage` 関数自体は既に `MAX_WIDTH/MAX_HEIGHT` を使用しているが、コメント内で `// 800` `// 480` と固定値が書かれている箇所をMAX_HEIGHT/MAX_WIDTH表現に変更する。実行ロジックは無変更。

- [ ] **Step 1: STATE 1 (H-Split) ヘッダコメントを修正**

`src/network/html/FilesPage.html:4149-4153` 付近を以下に変更:

Before:
```js
      // ========================================================================
      // STATE 1: H-Split (Rotate + Split) - EXACT COPY FROM index.html
      // Step 1: Scale WIDTH to 800px (keep aspect ratio)
      // Step 2: Rotate 90° CW or CCW based on HANDEDNESS
      // Step 3: If WIDTH > 480, split vertically with overlap
      // ========================================================================
```

After:
```js
      // ========================================================================
      // STATE 1: H-Split (Rotate + Split) - EXACT COPY FROM index.html
      // Step 1: Scale WIDTH to MAX_HEIGHT (keep aspect ratio)
      // Step 2: Rotate 90° CW or CCW based on HANDEDNESS
      // Step 3: If WIDTH > MAX_WIDTH, split vertically with overlap
      // ========================================================================
```

- [ ] **Step 2: STATE 1 内インラインコメントを修正**

`src/network/html/FilesPage.html:4155-4172` 付近を以下に変更:

Before:
```js
      if (imageState === 1) {
        // Step 1: Scale WIDTH to 800 (this is the key difference!)
        const scale = MAX_HEIGHT / origW;  // 800 / origW
        const scaledW = MAX_HEIGHT;  // 800
        const scaledH = Math.round(origH * scale);
```

After:
```js
      if (imageState === 1) {
        // Step 1: Scale WIDTH to MAX_HEIGHT (this is the key difference!)
        const scale = MAX_HEIGHT / origW;
        const scaledW = MAX_HEIGHT;
        const scaledH = Math.round(origH * scale);
```

`src/network/html/FilesPage.html:4170-4172` 付近 (rotH のコメント):

Before:
```js
        const rotH = scaledW;  // 800
```

After:
```js
        const rotH = scaledW;  // = MAX_HEIGHT
```

`src/network/html/FilesPage.html:4194-4206` 付近:

Before:
```js
        // Step 3: If WIDTH > 480, split vertically
        if (rotW <= MAX_WIDTH) {
```

After:
```js
        // Step 3: If WIDTH > MAX_WIDTH, split vertically
        if (rotW <= MAX_WIDTH) {
```

`src/network/html/FilesPage.html:4206` 付近:

Before:
```js
          const maxW = MAX_WIDTH;  // 480
```

After:
```js
          const maxW = MAX_WIDTH;
```

- [ ] **Step 3: STATE 2 (V-Split) のコメントを修正**

`src/network/html/FilesPage.html:4274-4283` 付近を以下に変更:

Before:
```js
      // ========================================================================
      // STATE 2: V-Split (Vertical Split, no rotation)
      // Step 1: Scale HEIGHT to 800px (up or down)
      // Step 2: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      else if (imageState === 2) {
        // ALWAYS scale height to 800 (up or down)
        const scale = MAX_HEIGHT / origH;  // 800 / origH
        const scaledW = Math.round(origW * scale);
        const scaledH = MAX_HEIGHT;  // Always 800
```

After:
```js
      // ========================================================================
      // STATE 2: V-Split (Vertical Split, no rotation)
      // Step 1: Scale HEIGHT to MAX_HEIGHT (up or down)
      // Step 2: If WIDTH > MAX_WIDTH, split vertically with overlap
      // ========================================================================
      else if (imageState === 2) {
        // ALWAYS scale height to MAX_HEIGHT (up or down)
        const scale = MAX_HEIGHT / origH;
        const scaledW = Math.round(origW * scale);
        const scaledH = MAX_HEIGHT;
```

- [ ] **Step 4: STATE 3 (Rotate & Fit) のコメントを修正**

`src/network/html/FilesPage.html:4362-4364` 付近を以下に変更:

Before:
```js
      // ========================================================================
      // STATE 3: Rotate & Fit (Rotate 90°, then scale to fit 480x800, no split)
      // ========================================================================
```

After:
```js
      // ========================================================================
      // STATE 3: Rotate & Fit (Rotate 90°, then scale to fit MAX_WIDTH x MAX_HEIGHT, no split)
      // ========================================================================
```

`src/network/html/FilesPage.html:4388` 付近 ("Step 2: Scale to fit 480x800 (if needed)"):

Before:
```js
        // Step 2: Scale to fit 480x800 (if needed)
```

After:
```js
        // Step 2: Scale to fit MAX_WIDTH x MAX_HEIGHT (if needed)
```

`src/network/html/FilesPage.html:4401` 付近 ("Scale to fit 480x800"):

Before:
```js
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / rotW, MAX_HEIGHT / rotH);
```

After:
```js
          // Scale to fit MAX_WIDTH x MAX_HEIGHT
          const scale = Math.min(MAX_WIDTH / rotW, MAX_HEIGHT / rotH);
```

`src/network/html/FilesPage.html:4450` 付近 (もう1箇所の "Scale to fit 480x800"):

Before:
```js
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
```

After:
```js
          // Scale to fit MAX_WIDTH x MAX_HEIGHT
          const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
```

- [ ] **Step 5: `processImage` 関数内に `// 800` や `// 480` のコメントが残っていないか確認**

Run: `sed -n '4130,4475p' src/network/html/FilesPage.html | grep -E '//.*\b(480|800)\b|\b480x800\b'`
Expected: マッチなし（出力が空）

- [ ] **Step 6: コミット**

```bash
git add src/network/html/FilesPage.html
git commit -m "$(cat <<'EOF'
🎨 processImageコメントの固定値表記を変数表記に統一

X3対応のため、processImage内のコメントに残っていた
ハードコード値 // 800 / // 480 をMAX_HEIGHT/MAX_WIDTH表記に変更。
実行ロジックは無変更。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: ビルドと最終確認

- [ ] **Step 1: 全ファイルで残存する `480`/`800` リテラルを総点検**

Run:
```bash
grep -nE '\b(480|800)\b' src/network/html/FilesPage.html | grep -vE 'max-width|800px;|background-color|#e0a800'
```

Expected: 以下のみ残ること（許容される箇所）:
- `let DEFAULT_MAX_WIDTH = 480;` (line 3281付近)
- `let DEFAULT_MAX_HEIGHT = 800;` (line 3282付近)
- `let deviceScreenWidth = 480;` (line 3317付近)
- `let deviceScreenHeight = 800;` (line 3318付近)
- 静的初期表示 `📏 Max 480×800px` (Task1で id を付与済みの span 内、デフォルト値)

これら以外の `480`/`800` は許容しない。残っていれば該当タスクに戻って修正。

- [ ] **Step 2: PlatformIO ビルドで `FilesPageHtml.generated.h` を再生成し、コンパイルエラーがないことを確認**

Run: `pio run -e default`
Expected:
- 終了コード 0
- ビルド成功
- `src/network/html/FilesPageHtml.generated.h` が更新される（git statusでは `.gitignore` 済みなので変更扱いされない）

- [ ] **Step 3: 生成された `FilesPageHtml.generated.h` に新しい識別子が含まれているか確認**

Run: `grep -c 'optimizerResolutionInfo\|updateOptimizerDimensions' src/network/html/FilesPageHtml.generated.h`
Expected: 3 以上（HTML側のid、関数定義、関数呼び出し）

- [ ] **Step 4: `git status` で意図しないファイル変更がないか確認**

Run: `git status`
Expected:
- `modified:` の行に `src/network/html/FilesPage.html` を含まない（コミット済みのため）
- `?? .pio/` などのビルド成果物のみ untracked
- `FilesPageHtml.generated.h` は表示されない（gitignore）

- [ ] **Step 5: 仕様書のDone判定を満たすかセルフチェック**

仕様書 `docs/superpowers/specs/2026-04-29-optimize-epub-x3-design.md` のDone判定基準を1項目ずつ確認:
- [x] `MAX_WIDTH/MAX_HEIGHT` が API取得後にデバイス解像度から更新される（Task 2）
- [x] ラベル `📏 Max ...×...px` がデバイス解像度に応じて動的更新される（Task 1, 2）
- [x] `extractEpubImages` および `renderImageGrid` 内のすべての `480`/`800` リテラルが `MAX_WIDTH`/`MAX_HEIGHT` に置換されている（Task 3, 4, 6）
- [x] X4ビルド (`pio run`) が通り、`FilesPageHtml.generated.h` が再生成される（Task 6）

- [ ] **Step 6: 実機検証用の依頼メモを作成（ユーザに渡す）**

ユーザに以下を依頼することをコメントとして残す:

```
実機での動作確認をお願いします:

【X4実機】
1. WebUI開く → ラベルが「📏 Max 480×800px」のまま表示されること
2. EPUB Optimize ON でアップロード → 従来動作と一致

【X3実機】
1. WebUI開く → ラベルが「📏 Max 528×792px」に変わること
2. EPUB Optimize ON でアップロード → 画像が528幅にリサイズされる
3. H-Split可能な大きな画像で分割が正しく動作すること

【横向きで開いた場合（任意）】
- ラベルが縦向き相当値（X3: 528×792）になること
```

---

## 完了基準（再掲）

- [ ] X4で従来挙動と完全一致
- [ ] X3で 528×792 ベースで Optimize EPUB が機能
- [ ] PlatformIO ビルドが通る
- [ ] 不要な `480`/`800` リテラルが残っていない
- [ ] 全タスクのコミットが意味のある単位で分かれている
- ※ 必須ゲート（動作検証は実機ユーザ依頼／既存機能・差分確認・シークレット）
