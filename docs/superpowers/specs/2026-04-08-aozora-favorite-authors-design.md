# 青空文庫 お気に入り作家管理機能 設計書

**Issue**: zrn-ns/crosspoint-jp#34
**日付**: 2026-04-08
**ステータス**: 承認済み

## 概要

青空文庫の作家をお気に入り登録し、TOP_MENUから素早くアクセスできる機能。操作は「Confirm＝アクションメニュー（選択）」パターンで、既存のAUTHOR_LISTフローと一貫した操作体系を提供する。

## 機能要件

1. 作家のお気に入り登録・削除ができる
2. お気に入り作家一覧から作品を検索できる
3. 通常の作家検索結果でお気に入り登録済み作家に★マークを表示する

## TOP_MENU（変更後）

| Index | メニュー項目 | 状態 |
|-------|-------------|------|
| 0 | お気に入り作家 | **新規** |
| 1 | 作家から探す | 既存（旧0） |
| 2 | 作品名から探す | 既存（旧1） |
| 3 | ジャンルから探す | 既存（旧2） |
| 4 | 新着作品 | 既存（旧3） |
| 5 | ダウンロード済み | 既存（旧4） |

## 状態遷移

### 新規状態

- `FAVORITE_AUTHORS` — お気に入り作家一覧（五十音順）
- `AUTHOR_ACTION` — 作家に対するアクションメニュー

### 遷移図

```
TOP_MENU
  └→ [0] お気に入り作家 → FAVORITE_AUTHORS（五十音順一覧）
       ├→ Confirm → AUTHOR_ACTION（「作品を見る」/「お気に入りから削除」）
       │    ├→ 作品を見る → fetchWorks(author_id) → WORK_LIST（既存フロー）
       │    └→ お気に入りから削除 → FAVORITE_AUTHORS（一覧に戻る）
       └→ Back → TOP_MENU

AUTHOR_LIST（既存の作家検索結果）:
  └→ Confirm → AUTHOR_ACTION（「作品を見る」/「お気に入りに追加 or 削除」）
       ├→ 作品を見る → fetchWorks(author_id) → WORK_LIST（既存フロー）
       └→ お気に入り追加/削除 → AUTHOR_LIST（一覧に戻る、★更新）
```

### AUTHOR_ACTION の動作

- 2項目のリストとして描画（Up/Downで選択、Confirmで実行、Backでキャンセル）
- 遷移元の状態（FAVORITE_AUTHORS or AUTHOR_LIST）を記憶し、Backやアクション完了後に正しく戻る
- お気に入り登録済みかどうかで2番目の項目が切り替わる:
  - 未登録: 「お気に入りに追加」
  - 登録済み: 「お気に入りから削除」

## データ永続化

### ファイル

- **パス**: `/Aozora/.favorite_authors.json`（SDカード上）
- **形式**: JSON配列

```json
[
  { "author_id": 23, "name": "芥川龍之介", "kana": "アクタガワリュウノスケ" },
  { "author_id": 148, "name": "夏目漱石", "kana": "ナツメソウセキ" }
]
```

### タイミング

- **ロード**: `AozoraActivity::onEnter()` 時（WiFi接続後、TOP_MENU表示前）
- **保存**: 追加/削除操作の都度（エントリ数は少量のため頻度問題なし）

## 管理クラス: FavoriteAuthorsManager

`AozoraIndexManager` と同パターンの永続化マネージャ。

### ファイル

- `src/FavoriteAuthorsManager.h`
- `src/FavoriteAuthorsManager.cpp`

### 構造体

```cpp
struct FavoriteAuthor {
  int authorId;     // API上の作家ID
  char name[48];    // 作家名（日本語）
  char kana[48];    // カナ読み（ソート用）
};
```

メモリ: 100バイト/件。50作家で約5KB — ESP32-C3のRAM制約下で問題なし。

### インターフェース

```cpp
class FavoriteAuthorsManager {
public:
  bool load();                                          // JSONファイルからロード
  bool save();                                          // JSONファイルへ書き込み
  void addAuthor(int id, const char* name, const char* kana);  // 追加（重複チェック付き）
  void removeAuthor(int id);                            // 削除
  bool isFavorited(int id) const;                       // 登録判定
  const std::vector<FavoriteAuthor>& entries() const;   // 五十音順ソート済み一覧
};
```

### ソート

- `entries()` は `kana` フィールドによる五十音順（`strcmp`による辞書順）
- 追加/削除時にソート済み状態を維持する（挿入ソート or 変更後に全ソート）

## UI表示

### FAVORITE_AUTHORS 画面

- 既存 `AUTHOR_LIST` の描画ロジックを再利用
- ヘッダー: 「お気に入り作家」
- リスト項目: 作家名（★は不要 — この画面の全作家がお気に入り）
- 空の場合: 画面中央に「お気に入り作家がありません」を表示
- ボタンヒント: Back / 選択 / ↑ / ↓

### AUTHOR_ACTION 画面

- ヘッダー: 選択した作家名
- 2項目のリスト:
  - 「作品を見る」
  - 「お気に入りに追加」 or 「お気に入りから削除」（状態依存）
- ボタンヒント: Back / 選択 / ↑ / ↓

### AUTHOR_LIST での★表示

- お気に入り登録済みの作家は、名前の先頭に「★ 」を付与して描画
- `FavoriteAuthorsManager::isFavorited(authorId)` で判定
- 例: `★ 芥川龍之介 (371)`

## i18n 追加キー

| キー | 日本語 | 英語 |
|------|--------|------|
| `STR_FAVORITE_AUTHORS` | お気に入り作家 | Favorite Authors |
| `STR_ADD_TO_FAVORITES` | お気に入りに追加 | Add to Favorites |
| `STR_REMOVE_FROM_FAVORITES` | お気に入りから削除 | Remove from Favorites |
| `STR_VIEW_WORKS` | 作品を見る | View Works |
| `STR_NO_FAVORITE_AUTHORS` | お気に入り作家がありません | No Favorite Authors |

## 変更対象ファイル

| ファイル | 変更内容 |
|---------|---------|
| **新規**: `src/FavoriteAuthorsManager.h` | お気に入り管理クラス定義 |
| **新規**: `src/FavoriteAuthorsManager.cpp` | お気に入り管理クラス実装 |
| `src/activities/settings/AozoraActivity.h` | FAVORITE_AUTHORS, AUTHOR_ACTION 状態追加、FavoriteAuthorsManager メンバー追加 |
| `src/activities/settings/AozoraActivity.cpp` | 状態遷移・描画・入力処理の追加、TOP_MENUインデックス調整 |
| `lib/I18n/translations/japanese.yaml` | 翻訳キー追加 |
| `lib/I18n/translations/english.yaml` | 翻訳キー追加 |
| その他の翻訳YAML | 同上（対応言語分） |

## Done 判定基準

- [ ] お気に入り作家の追加・削除ができる
- [ ] お気に入り一覧が五十音順で表示される
- [ ] AUTHOR_LISTで登録済み作家に★が表示される
- [ ] AUTHOR_ACTIONメニューが一貫して動作する（FAVORITE_AUTHORS / AUTHOR_LIST 両方から）
- [ ] 空のお気に入り一覧で適切なメッセージが表示される
- [ ] `/Aozora/.favorite_authors.json` への永続化が正しく動作する
- [ ] `pio run` でビルドが通る
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
