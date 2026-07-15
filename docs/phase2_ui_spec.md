# Phase 2：INDUSTRIAL UI 仕様書

対象テーマ名: `INDUSTRIAL` / プロジェクト仮称: `X4 INDUSTRIAL READER`
本書はコードを変更しない設計文書。実装は `BaseTheme` を継承した `IndustrialTheme` と `IndustrialMetrics::values`（`constexpr ThemeMetrics`）で行う。
基準向き: **Portrait 480×800**（`GfxRenderer` 既定）。数値は 480/800 を直接使わず `getScreenWidth()/getScreenHeight()/getOrientedViewableTRBL()` から算出する。

---

## 1. デザイン原則

抽象原則のみを参照し、既存ブランドのロゴ・フォント・アイコン・固有名称・グラフィックは一切コピーしない。

1. **工業的**: 直角の罫線、モジュールを区切る水平/垂直線、切り欠き（`//`）による見出しマーカー。角丸は原則不使用（`cornerRadius = 0`）。
2. **技術的・情報密度の制御**: 画面識別コード（`HOME-01` 等）、セクション番号（`01 / CURRENT`）、状態コード（`READY/SYNC/READING/OFFLINE`）を規則的に配置。意味のない乱数コードは使わない。
3. **高コントラスト**: 前景=黒／背景=白を基本。グレーは進捗バーの空き部分など限定用途のみ。1px線が実機で欠ける懸念に配慮し、罫線は既定 `lineWidth = 1` だが主要区切りは 2px。
4. **モジュール化**: ヘッダー / コンテンツ / フッターの3帯構造。各帯は水平罫線で分離。
5. **精密な余白**: 8pxグリッド基調。全画面で余白・線幅・文字サイズを統一（各Activityに数値を重複させない＝トークン集約）。
6. **状態は最低2系統で表示**: 数値（%・P.128/412）＋図形（進捗バー）＋文字（状態コード）のうち2つ以上を併用。アイコン単独に依存しない。
7. **可読性優先**: 装飾より日本語本文/UIの可読性。窮屈になる場合は情報を削る。
8. **物理ボタン前提**: 全画面にフッターのボタンガイドを常設。迷いを生む隠しジェスチャーを作らない。
9. **E-Ink前提**: アニメーション不使用。全画面更新と部分更新の回数を意識。ゴースト対策維持。

---

## 2. デザイントークン（`IndustrialMetrics::values` に集約）

`ThemeMetrics` は既存フィールド（`BaseTheme.h`）を再利用し、不足分は**構造体末尾に追記**（designated initializer なので既存テーマは影響なし、新フィールドはデフォルト0）。実装時に確定するが、設計上の目標値は以下。

### 2-a. 既存 ThemeMetrics フィールドへの割当（Portrait 480幅基準）

| トークン | 既存フィールド | 目標値 | 備考 |
|---|---|---|---|
| 外側余白（左右） | `contentSidePadding` | 16 | 8pxグリッド×2 |
| 上余白 | `topPadding` | 8 | |
| セクション間余白 | `verticalSpacing` | 16 | |
| ヘッダー高さ | `headerHeight` | 48 | ステータスレール＋画面コード |
| フッター（ボタンガイド）高さ | `buttonHintsHeight` | 40 | |
| 行高（1行リスト） | `listRowHeight` | 40 | 日本語UIフォントが潰れない高さ |
| 行高（副題付） | `listWithSubtitleRowHeight` | 60 | |
| メニュー行高 | `menuRowHeight` | 48 | |
| タブバー高さ | `tabBarHeight` | 40 | 設定カテゴリ |
| スクロールバー幅 | `scrollBarWidth` | 4 | |
| 進捗バー高さ | `progressBarHeight` | 12 | 高コントラスト |
| バッテリー枠 | `batteryWidth/Height` | 16/12 | |

> `keyboardKeyCornerRadius` は `ThemeMetrics` の既存フィールドではなく、角丸値は各テーマ実装内の `constexpr` 値として管理されている（例: `LyraTheme.cpp` の `constexpr int cornerRadius = 6;`）。INDUSTRIALの角丸方針は実装時に `IndustrialTheme` 内で管理する。

### 2-b. 新規追記フィールド（末尾追加・後方互換）

| トークン | 追記フィールド名(案) | 目標値 | 用途 |
|---|---|---|---|
| 基本線幅 | `hairlineWidth` | 1 | 補助罫線 |
| 主要区切り線幅 | `ruleWidth` | 2 | 帯の境界 |
| 選択枠幅 | `selectionBorderWidth` | 2 | 太枠選択 |
| 左マーカー幅 | `selectionMarkerWidth` | 4 | 選択行の左端バー |
| 反転選択使用フラグ | `useInvertedSelection` | true | 反転＋左マーカー併用 |
| 切り欠きサイズ | `notchSize` | 8 | 見出しの `//` 相当 |
| 画面コード右余白 | `screenCodeRightPad` | 8 | 右上コード配置 |

### 2-c. 文字サイズ（既存フォントIDを流用、UIフォント追加はしない）

`src/fontIds.h` の既存IDのみ使用（Flash増やさない）:

| 用途 | フォントID | 実サイズ |
|---|---|---|
| 見出し/書名 | `UI_12_FONT_ID` | 12pt |
| 本文リスト/ラベル | `UI_10_FONT_ID` | 10pt |
| 補助/コード/状態 | `SMALL_FONT_ID` | 小 |

> UIフォントの新規変更は行わない（収録範囲・Flash・ライセンス確認が必要なため初期フェーズ対象外）。本文用フォント（SdCardFont/ExternalFont）とは混同しない。管理コード（`HOME-01` 等）は英数字なので既存UIフォントで表示可能。

---

## 3. 状態定義（コンポーネント共通）

| 状態 | 視覚表現（E-Inkで明確） | 実装 |
|---|---|---|
| 通常 | 黒文字/白背景、`hairlineWidth` 罫線 | 既定描画 |
| 選択 | **反転（黒地に白文字）＋左端に `selectionMarkerWidth` の黒バー**。補助で `selectionBorderWidth` 太枠も可 | `fillRect`＋反転`drawText`＋左`fillRect` |
| 無効（dimmed） | グレー階調文字、罫線は残す。左マーカー無し | `drawList` の `rowDimmed` を活用、ディザ描画 |
| エラー | 見出し左に `//!` マーカー＋状態コード `ERROR`＋短い日本語説明 | `drawPopup` 種別=ERROR |
| 処理中（BUSY） | 状態コード `SYNC/BUSY...` ＋進捗バー（不定時は区切りドット進行）。**E-Ink遅延中も無反応に見せない** | `fillPopupProgress` を一定間隔で更新 |

状態は必ず「文字/図形/数値」のうち**最低2種類**で伝える。

---

## 4. E-Ink 更新方針

`HalDisplay::RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH }`（`HalDisplay.h` L14–17）を使い分ける。

| 場面 | モード | 理由 |
|---|---|---|
| 画面遷移（Activity切替） | FULL_REFRESH | ゴースト除去 |
| 選択移動・小領域更新 | FAST_REFRESH | 応答性 |
| 連続ページ送り | 既存 `SETTINGS.refreshFrequency` に従い N回ごとFULL | ゴースト蓄積抑制 |
| ダイアログ進捗 | HALF/FAST 併用 | 無反応回避 |

- 新規アニメーションは実装しない。
- 部分更新と全画面更新の回数を計測対象とする（Phase 5）。
- ゴースト対策（既存 `fadingFix` 等）は維持。

---

## 5. 日本語と英数字の混在方針

- 管理コード・状態コード・数値（`HOME-01`, `READING`, `P.128/412`, `31%`）は**英数字**でUIフォント表示。
- 書名・著者・カテゴリ説明は**日本語**。設定は英日併記可（例 `01 / UI THEME　UIテーマ`）。
- 全角/半角混在で崩れないよう、行の実測幅は `getTextWidth()` で取得してから配置（固定文字数でレイアウトしない）。
- ユーザー向け文字列は全て `tr(STR_*)`。コード/数値の固定ラベルは `static constexpr char[]`＋`snprintf`。ループ内 String 連結禁止。

---

## 6. 長い書名・著者・フォルダ名の省略規則

- 既存の省略ロジックに合わせ、**末尾省略**（"…"）を基本とする。実幅は `getTextWidth()` で判定し、行幅（`getScreenWidth() - contentSidePadding*2 - 状態列幅`）を超えたら末尾からカット。
- 省略は**表示のみ**。選択・進捗などの内部データは完全な文字列/値を保持。
- パス表示（`PATH: /BOOKS/…/夏目漱石/`）は中間省略も可（先頭ルートと末尾フォルダを残す）。
- マルチバイト境界を壊さない（バイト単位でなく文字単位でカット、既存 UTF-8 ヘルパを使用）。

---

## 7. Portrait / Landscape 対応規則

- **基準は Portrait 480×800**。`getScreenWidth()` が幅、`getScreenHeight()` が高さを返し、向きに応じて自動で 480/800 が入替わる（`GfxRenderer.cpp` 実装確認済み）。**コードに 480/800 を直書きしない**。
- 帯構造（ヘッダー/コンテンツ/フッター）の高さはトークン固定、幅は `getScreenWidth()` 追従。
- コンテンツ行数は `UITheme::getNumberOfItemsPerPage()`（既存関数）で動的算出。
- 画面コードは右上、`getOrientedViewableTRBL()` の top/right を基準に配置（ベゼル余白内）。
- Landscape（800×480）では横幅が広がるため、ホーム/ライブラリは**2カラム化を許容**（左=リスト、右=詳細）。ただし初期実装は1カラムを両向き共通とし、2カラムは Phase 4 で検討。
- Portrait/PortraitInverted、Landscape CW/CCW の4モードで座標変換は `GfxRenderer` が担うため、テーマ側は論理座標のみ扱う。

---

## 8. アイコン方針

- モノクロ・小解像度で判別可能・線幅統一・幾何学的。既存ブランドを模倣しない。
- **可能な限り手続き描画**（`drawLine/drawRect/fillPolygon`）で生成し、Bitmap追加を避ける（Flash 85% のため）。
- どうしても要る記号のみ小型1bit（既存 `src/components/icons/*.h` と同形式）。
- アイコン単独で意味を伝えず、短い文字ラベルを併記（例: `[LIB] LIBRARY`）。

---

## 9. 非対象（初期フェーズで実装しないもの）

- 起動画面/ホーム画面の大規模再構成（最小ビルド成功後に分離）。
- 設定7カテゴリ再編（既存は DISPLAY/READER/CONTROLS/SYSTEM/RTC の5種。データモデル変更を伴うためPhase 4以降）。
- UIフォント差替、辞書・統計等の将来機能。
- 読書エンジン・OTA・パーティション・ブートローダーの変更。
