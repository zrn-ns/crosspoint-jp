#include "AozoraActivity.h"

#include <ArduinoJson.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TlsHeapReclaim.h"

// --- 50音行定義 ---

struct KanaRow {
  StrId label;
  const char* apiParam;
};

static const KanaRow KANA_ROWS[] = {
    {StrId::STR_KANA_A, "ア"},  {StrId::STR_KANA_KA, "カ"}, {StrId::STR_KANA_SA, "サ"}, {StrId::STR_KANA_TA, "タ"},
    {StrId::STR_KANA_NA, "ナ"}, {StrId::STR_KANA_HA, "ハ"}, {StrId::STR_KANA_MA, "マ"}, {StrId::STR_KANA_YA, "ヤ"},
    {StrId::STR_KANA_RA, "ラ"}, {StrId::STR_KANA_WA, "ワ"},
};
static constexpr int KANA_ROW_COUNT = 10;

// 各行の個別文字（作品名検索の2段階目で使用）
static const char* KANA_CHARS[][5] = {
    {"あ", "い", "う", "え", "お"}, {"か", "き", "く", "け", "こ"}, {"さ", "し", "す", "せ", "そ"},
    {"た", "ち", "つ", "て", "と"}, {"な", "に", "ぬ", "ね", "の"}, {"は", "ひ", "ふ", "へ", "ほ"},
    {"ま", "み", "む", "め", "も"}, {"や", "ゆ", "よ", "", ""},     {"ら", "り", "る", "れ", "ろ"},
    {"わ", "を", "ん", "", ""},
};
// 各行の文字数
static const int KANA_CHAR_COUNTS[] = {5, 5, 5, 5, 5, 5, 5, 3, 5, 3};

// --- ジャンル定義 ---

struct GenreRow {
  StrId label;
  const char* ndc;
};

static const GenreRow GENRES[] = {
    {StrId::STR_GENRE_NOVEL, "913"}, {StrId::STR_GENRE_POETRY, "911"},     {StrId::STR_GENRE_ESSAY, "914"},
    {StrId::STR_GENRE_DRAMA, "912"}, {StrId::STR_GENRE_FAIRY_TALE, "388"},
};
static constexpr int GENRE_COUNT = 5;

// --- トップメニュー項目数 ---
static constexpr int TOP_MENU_COUNT = 6;

// --- Constructor ---

AozoraActivity::AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Aozora", renderer, mappedInput) {}

// --- Lifecycle ---

void AozoraActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void AozoraActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Reload font caches that were freed for TLS memory
  FontManager::getInstance().loadSettings();
}

void AozoraActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  reclaimHeapForTls(renderer, "AOZORA");

  // Load download index
  indexManager_.loadAndPurge();
  favoritesManager_.load();

  {
    RenderLock lock(*this);
    state_ = TOP_MENU;
    selectedIndex_ = 0;
  }
}

// --- State navigation ---

void AozoraActivity::pushState(State newState) {
  stateStack_.push_back(state_);
  selectedIndexStack_.push_back(selectedIndex_);
  state_ = newState;
  selectedIndex_ = 0;
}

void AozoraActivity::popState() {
  if (stateStack_.empty()) {
    finish();
    return;
  }
  state_ = stateStack_.back();
  selectedIndex_ = selectedIndexStack_.back();
  stateStack_.pop_back();
  selectedIndexStack_.pop_back();
}

// --- URL encoding helper ---

static void urlEncodeUtf8(const char* src, char* dest, size_t destSize) {
  size_t pos = 0;
  for (size_t i = 0; src[i] && pos < destSize - 4; i++) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      dest[pos++] = static_cast<char>(c);
    } else {
      pos += snprintf(dest + pos, destSize - pos, "%%%02X", c);
    }
  }
  dest[pos] = '\0';
}

// --- API calls (download JSON to SD temp file, then parse) ---

static constexpr const char* API_TMP_FILE = "/aozora_api.tmp";

static std::string lastApiError_;

static constexpr int API_MAX_RETRIES = 3;

// Build the one-line diagnostic shown on the error screen.
//
// blk = largest contiguous block. A TLS handshake needs a ~16.5KB contiguous
// block for the inbound record buffer plus ~4KB for the outbound one, so free
// heap alone cannot tell an out-of-memory failure from a fragmentation one.
// http is negative when it carries an esp_err_t: -28674 is ESP_ERR_HTTP_CONNECT,
// i.e. the connection never opened. tls is the mbedtls/esp-tls reason behind
// that (0 = the failure never reached TLS, so it was DNS or the TCP connect);
// -32512 is MBEDTLS_ERR_SSL_ALLOC_FAILED, which pins it on the heap.
static void formatNetworkError(char* buf, size_t bufSize, int result) {
  snprintf(buf, bufSize, "err=%d http=%d tls=%d/%X crt=%d/%X p=%d/%d f=%d/%d", result, HttpDownloader::lastHttpCode,
           HttpDownloader::lastTlsError, HttpDownloader::lastTlsFlags, HttpDownloader::lastCrtDiag,
           HttpDownloader::lastCrtErr, HttpDownloader::lastPreHeapKb, HttpDownloader::lastPreBlkKb,
           HttpDownloader::lastCrtHeapKb, HttpDownloader::lastCrtBlkKb);
}

static bool fetchApiJson(const char* url, JsonDocument& doc) {
  LOG_DBG("AOZORA", "API call: %s (heap=%d)", url, ESP.getFreeHeap());

  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < API_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      LOG_DBG("AOZORA", "Retry %d/%d", attempt + 1, API_MAX_RETRIES);
      delay(1000);
    }
    // HttpDownloader は esp_http_client の HTTP_TIMEOUT_MS=60000 を使うため、
    // 従来 fork にあった 30000 ms タイムアウト引数は不要 (本家 API に合わせて除去)。
    result = HttpDownloader::downloadToFile(url, API_TMP_FILE);
    if (result == HttpDownloader::OK) break;
    LOG_ERR("AOZORA", "API fetch attempt %d failed: err=%d http=%d heap=%d blk=%d", attempt + 1, result,
            HttpDownloader::lastHttpCode, static_cast<int>(ESP.getFreeHeap()), static_cast<int>(ESP.getMaxAllocHeap()));
    Storage.remove(API_TMP_FILE);
  }

  if (result != HttpDownloader::OK) {
    char buf[160];
    formatNetworkError(buf, sizeof(buf), static_cast<int>(result));
    lastApiError_ = buf;
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("AOZORA", API_TMP_FILE, file)) {
    LOG_ERR("AOZORA", "Failed to open temp file");
    Storage.remove(API_TMP_FILE);
    return false;
  }

  DeserializationError err = deserializeJson(doc, file);
  file.close();
  Storage.remove(API_TMP_FILE);

  if (err) {
    LOG_ERR("AOZORA", "JSON parse error: %s", err.c_str());
    return false;
  }

  return true;
}

bool AozoraActivity::fetchAuthors(const char* kanaPrefix) {
  // 検索キーを保存（再取得用）
  snprintf(lastAuthorsKanaPrefix_, sizeof(lastAuthorsKanaPrefix_), "%s", kanaPrefix);

  // 不要なバッファを解放してヒープ確保
  works_.clear();
  works_.shrink_to_fit();

  char encoded[32];
  urlEncodeUtf8(kanaPrefix, encoded, sizeof(encoded));

  char url[192];
  snprintf(url, sizeof(url), "%s/api/authors?kana_prefix=%s", API_BASE, encoded);

  JsonDocument doc;
  if (!fetchApiJson(url, doc)) {
    errorMessage_ = lastApiError_;
    return false;
  }

  return parseAuthorsJson(doc);
}

bool AozoraActivity::fetchWorks(const char* queryParam) {
  // 不要なバッファを解放してヒープ確保（TLSバッファ用）
  authors_.clear();
  authors_.shrink_to_fit();
  // ページ送りでの再取得時、旧ページの 30 件と JSON 一時バッファが同時に生存する
  // ピークを避けるため、works_ も取得前に返却しておく。
  works_.clear();
  works_.shrink_to_fit();

  // 新しいクエリの場合はオフセットをリセットし、クエリを保存
  if (queryParam) {
    snprintf(lastWorksQuery_, sizeof(lastWorksQuery_), "%s", queryParam);
    worksOffset_ = 0;
  }

  // URL構築（kana_prefix の値部分はURLエンコードが必要）
  char url[256];
  if (strncmp(lastWorksQuery_, "kana_prefix=", 12) == 0) {
    char encoded[32];
    urlEncodeUtf8(lastWorksQuery_ + 12, encoded, sizeof(encoded));
    snprintf(url, sizeof(url), "%s/api/works?kana_prefix=%s&offset=%d&limit=%d", API_BASE, encoded, worksOffset_,
             WORKS_PAGE_SIZE);
  } else {
    snprintf(url, sizeof(url), "%s/api/works?%s&offset=%d&limit=%d", API_BASE, lastWorksQuery_, worksOffset_,
             WORKS_PAGE_SIZE);
  }

  JsonDocument doc;
  if (!fetchApiJson(url, doc)) {
    errorMessage_ = lastApiError_;
    return false;
  }

  worksTotal_ = doc["total"] | 0;
  return parseWorksJson(doc);
}

bool AozoraActivity::parseAuthorsJson(JsonDocument& doc) {
  authors_.clear();
  JsonArray arr = doc["authors"].as<JsonArray>();
  if (arr.isNull()) {
    LOG_ERR("AOZORA", "No 'authors' array in response");
    errorMessage_ = "Invalid response";
    return false;
  }

  // No reserve(): deque grows one 512-byte node at a time, so there is no
  // reallocate-and-copy to pre-empt.
  for (JsonObject obj : arr) {
    AuthorEntry entry;
    entry.id = obj["id"] | 0;
    snprintf(entry.name, sizeof(entry.name), "%s", (obj["name"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (obj["kana"] | ""));
    entry.workCount = obj["work_count"] | 0;
    authors_.push_back(entry);
  }

  LOG_DBG("AOZORA", "Parsed %zu authors", authors_.size());
  return true;
}

bool AozoraActivity::parseWorksJson(JsonDocument& doc) {
  works_.clear();
  JsonArray arr = doc["works"].as<JsonArray>();
  if (arr.isNull()) {
    LOG_ERR("AOZORA", "No 'works' array in response");
    errorMessage_ = "Invalid response";
    return false;
  }

  // No reserve(): see parseAuthorsJson.
  for (JsonObject obj : arr) {
    WorkEntry entry;
    entry.id = obj["id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (obj["title"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (obj["kana"] | ""));
    snprintf(entry.ndc, sizeof(entry.ndc), "%s", (obj["ndc"] | ""));
    snprintf(entry.author, sizeof(entry.author), "%s", (obj["author"] | ""));
    snprintf(entry.subtitle, sizeof(entry.subtitle), "%s", (obj["subtitle"] | ""));
    snprintf(entry.variant, sizeof(entry.variant), "%s", (obj["variant"] | ""));
    works_.push_back(entry);
  }

  computeWorkDuplicateMasks();

  LOG_DBG("AOZORA", "Parsed %zu works", works_.size());
  return true;
}

void AozoraActivity::computeWorkDuplicateMasks() {
  dupTitleMask_ = 0;
  ambiguousMask_ = 0;

  // works_ は 1 ページ 30 件（WORKS_PAGE_SIZE）なので総当たりでも最大 435 回の strcmp。
  // パース時の 1 回だけなので描画コストには乗らない。
  const int n = static_cast<int>(std::min<size_t>(works_.size(), 32));
  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (strcmp(works_[i].title, works_[j].title) != 0) continue;
      dupTitleMask_ |= (1u << i) | (1u << j);
      // 副題・文字遣いも一致する場合は作品 ID でしか区別できない
      if (strcmp(works_[i].variant, works_[j].variant) == 0 && strcmp(works_[i].subtitle, works_[j].subtitle) == 0) {
        ambiguousMask_ |= (1u << i) | (1u << j);
      }
    }
  }
}

std::string AozoraActivity::buildWorkListSubtitle(int index) const {
  const auto& work = works_[index];

  // 作品名検索・ジャンル・新着では author が見えないと誰の作品か分からないため常に表示する
  std::string line = work.author[0] ? work.author : selectedAuthorName_;

  if (work.subtitle[0] != '\0') {
    line += "　";
    line += work.subtitle;
  }

  const bool inMaskRange = index >= 0 && index < 32;
  // 文字遣い（新字新仮名など）は同名が並ぶときだけ出す。常時表示はノイズになる。
  if (inMaskRange && (dupTitleMask_ & (1u << index)) != 0 && work.variant[0] != '\0') {
    line += "　(";
    line += work.variant;
    line += ")";
  }
  // 副題・文字遣いまで同一なら、残る識別子は作品 ID だけ
  if (inMaskRange && (ambiguousMask_ & (1u << index)) != 0) {
    char idBuf[16];
    snprintf(idBuf, sizeof(idBuf), "　#%d", work.id);
    line += idBuf;
  }

  return line;
}

std::string AozoraActivity::buildDownloadedListSubtitle(int index) const {
  const int localIdx = index - dlPageStart_;
  if (localIdx < 0 || localIdx >= dlPageCount_) return "";
  const auto& entry = dlPageCache_[localIdx];

  std::string line = entry.author;

  if (entry.subtitle[0] != '\0') {
    line += "　";
    line += entry.subtitle;
  }

  if ((dlDupTitleMask_ & (1u << localIdx)) != 0 && entry.variant[0] != '\0') {
    line += "　(";
    line += entry.variant;
    line += ")";
  }
  if ((dlAmbiguousMask_ & (1u << localIdx)) != 0) {
    char idBuf[16];
    snprintf(idBuf, sizeof(idBuf), "　#%d", static_cast<int>(entry.workId));
    line += idBuf;
  }

  return line;
}

bool AozoraActivity::downloadWithRetry(const char* url, const char* destPath) {
  // Same retry policy as fetchApiJson(): a TLS handshake needs a large
  // contiguous block, so on a fragmented heap esp_http_client_open() fails with
  // ESP_ERR_HTTP_CONNECT before a single byte moves — and the next attempt, a
  // second later, often succeeds. The listing calls have always retried; the
  // book download did not, which is why downloads failed far more visibly than
  // listings even though both hit the same endpoint.
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < API_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      LOG_DBG("AOZORA", "Download retry %d/%d", attempt + 1, API_MAX_RETRIES);
      delay(1000);
    }
    downloadProgress_ = 0;
    downloadTotal_ = 0;
    // downloadToFile() removes the partial file itself on failure.
    result = HttpDownloader::downloadToFile(url, destPath, [this](size_t downloaded, size_t total) {
      downloadProgress_ = downloaded;
      downloadTotal_ = total;
      requestUpdate(true);
    });
    if (result == HttpDownloader::OK) return true;
    LOG_ERR("AOZORA", "Download attempt %d failed: err=%d http=%d tls=%d/%X heap=%d blk=%d", attempt + 1, result,
            HttpDownloader::lastHttpCode, HttpDownloader::lastTlsError, HttpDownloader::lastTlsFlags,
            static_cast<int>(ESP.getFreeHeap()), static_cast<int>(ESP.getMaxAllocHeap()));
  }

  char buf[160];
  formatNetworkError(buf, sizeof(buf), static_cast<int>(result));
  errorMessage_ = buf;
  return false;
}

bool AozoraActivity::downloadBook() {
  char url[256];
  snprintf(url, sizeof(url), "%s/api/convert?work_id=%d", API_BASE, selectedWorkId_);

  char relPath[160];
  if (!AozoraIndexManager::makeRelativePath(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath,
                                            sizeof(relPath))) {
    LOG_ERR("AOZORA", "Failed to make relative path");
    errorMessage_ = "Path error";
    return false;
  }
  char destPath[192];
  snprintf(destPath, sizeof(destPath), "%s/%s", AozoraIndexManager::AOZORA_DIR, relPath);

  if (!AozoraIndexManager::ensureDirectory() || !AozoraIndexManager::ensureAuthorDirectory(selectedWorkAuthor_)) {
    LOG_ERR("AOZORA", "Failed to create directory");
    errorMessage_ = "SD card error";
    return false;
  }

  if (!downloadWithRetry(url, destPath)) {
    Storage.remove(destPath);  // 念のため取り残しを掃除
    return false;
  }

  // Add to index
  if (!indexManager_.addEntry(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath, selectedWorkSubtitle_,
                              selectedWorkVariant_)) {
    LOG_ERR("AOZORA", "Failed to add index entry");
    // File is downloaded but index failed -- not critical
  } else {
    invalidateDownloadedPageCache();
  }

  LOG_DBG("AOZORA", "Downloaded: %s", destPath);
  return true;
}

bool AozoraActivity::updateBook() {
  char url[256];
  snprintf(url, sizeof(url), "%s/api/convert?work_id=%d", API_BASE, selectedWorkId_);

  char relPath[160];
  if (!AozoraIndexManager::makeRelativePath(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath,
                                            sizeof(relPath))) {
    LOG_ERR("AOZORA", "Failed to make relative path");
    errorMessage_ = "Path error";
    return false;
  }
  char destPath[192];
  char tmpPath[200];
  snprintf(destPath, sizeof(destPath), "%s/%s", AozoraIndexManager::AOZORA_DIR, relPath);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", destPath);

  if (!AozoraIndexManager::ensureDirectory() || !AozoraIndexManager::ensureAuthorDirectory(selectedWorkAuthor_)) {
    LOG_ERR("AOZORA", "Failed to create directory");
    errorMessage_ = "SD card error";
    return false;
  }

  // 既存のtmpファイルが残っていたら掃除
  Storage.remove(tmpPath);

  // 一時ファイルにダウンロード（既存ファイルはこの時点では無傷）
  if (!downloadWithRetry(url, tmpPath)) {
    Storage.remove(tmpPath);  // 不完全な一時ファイルを削除、旧ファイルは保持
    return false;
  }

  // Atomic swap: 旧ファイル削除 → .tmp を正式名にrename
  Storage.remove(destPath);
  if (!Storage.rename(tmpPath, destPath)) {
    LOG_ERR("AOZORA", "Rename failed: %s -> %s", tmpPath, destPath);
    Storage.remove(tmpPath);
    errorMessage_ = "Rename failed";
    return false;
  }

  LOG_DBG("AOZORA", "Updated: %s", destPath);
  return true;
}

void AozoraActivity::openSelectedWorkInReader() {
  // downloadBook() と同一のパス構築ロジックを使う。ファイルブラウザから開いた場合と
  // 同じ絶対パスになることで .crosspoint/ キャッシュのハッシュが一致し、読書進捗が共有される。
  char relPath[160];
  if (!AozoraIndexManager::makeRelativePath(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath,
                                            sizeof(relPath))) {
    LOG_ERR("AOZORA", "openSelectedWorkInReader: failed to make relative path");
    {
      RenderLock lock(*this);
      errorMessage_ = "Path error";
      state_ = ERROR;
    }
    requestUpdate();
    return;
  }
  char fullPath[192];
  snprintf(fullPath, sizeof(fullPath), "%s/%s", AozoraIndexManager::AOZORA_DIR, relPath);

  // インデックス上は存在しても実ファイルが消えている場合がある（SD を PC で編集した等）
  if (!Storage.exists(fullPath)) {
    LOG_ERR("AOZORA", "openSelectedWorkInReader: file missing: %s", fullPath);
    {
      RenderLock lock(*this);
      errorMessage_ = "File not found";
      state_ = ERROR;
    }
    requestUpdate();
    return;
  }

  // goToReader() は replaceActivity() の前に SD フォントをロードする。この時点では WiFi
  // スタックがまだ生きているため、先に一覧バッファを返してヒープの余裕を作っておく。
  works_.clear();
  works_.shrink_to_fit();
  authors_.clear();
  authors_.shrink_to_fit();

  // replaceActivity() は pendingActivity への登録のみで、実際の破棄は loop() が戻った後に
  // ActivityManager::loop() が行う（"delete this" 問題は起きない）。呼び出し後はメンバに触れないこと。
  onSelectBook(fullPath);
}

// --- Input handling ---

void AozoraActivity::loop() {
  if (state_ == TOP_MENU) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < TOP_MENU_COUNT - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      switch (selectedIndex_) {
        case 0:  // お気に入り作家
        {
          RenderLock lock(*this);
          pushState(FAVORITE_AUTHORS);
        }
          requestUpdate();
          break;
        case 1:  // 作家から探す
          searchMode_ = SEARCH_AUTHOR;
          {
            RenderLock lock(*this);
            pushState(KANA_SELECT);
          }
          requestUpdate();
          break;
        case 2:  // 作品名から探す
          searchMode_ = SEARCH_TITLE;
          {
            RenderLock lock(*this);
            pushState(KANA_SELECT);
          }
          requestUpdate();
          break;
        case 3:  // ジャンルから探す
        {
          RenderLock lock(*this);
          pushState(GENRE_SELECT);
        }
          requestUpdate();
          break;
        case 4:  // 新着作品
        {
          {
            RenderLock lock(*this);
            pushState(LOADING);
          }
          requestUpdateAndWait();

          if (fetchWorks("sort=newest&limit=50")) {
            RenderLock lock(*this);
            state_ = WORK_LIST;
            selectedIndex_ = 0;
          } else {
            RenderLock lock(*this);
            state_ = ERROR;
          }
          requestUpdate();
        } break;
        case 5:  // ダウンロード済み
        {
          RenderLock lock(*this);
          pushState(DOWNLOADED_LIST);
        }
          requestUpdate();
          break;
      }
    }
  } else if (state_ == KANA_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < KANA_ROW_COUNT - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (searchMode_ == SEARCH_AUTHOR) {
        // 作家検索: 行全体でAPIを呼ぶ
        const char* kanaParam = KANA_ROWS[selectedIndex_].apiParam;
        {
          RenderLock lock(*this);
          pushState(LOADING);
        }
        requestUpdateAndWait();

        if (fetchAuthors(kanaParam)) {
          RenderLock lock(*this);
          state_ = AUTHOR_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      } else {
        // 作品名検索: 個別文字選択画面へ
        selectedKanaRowIndex_ = selectedIndex_;
        {
          RenderLock lock(*this);
          pushState(KANA_CHAR_SELECT);
        }
        requestUpdate();
      }
    }
  } else if (state_ == KANA_CHAR_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const int charCount = KANA_CHAR_COUNTS[selectedKanaRowIndex_];

    buttonNavigator_.onNextRelease([this, charCount] {
      if (selectedIndex_ < charCount - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const char* kanaChar = KANA_CHARS[selectedKanaRowIndex_][selectedIndex_];

      {
        RenderLock lock(*this);
        pushState(LOADING);
      }
      requestUpdateAndWait();

      char query[64];
      snprintf(query, sizeof(query), "kana_prefix=%s", kanaChar);
      if (fetchWorks(query)) {
        RenderLock lock(*this);
        state_ = WORK_LIST;
        selectedIndex_ = 0;
      } else {
        RenderLock lock(*this);
        state_ = ERROR;
      }
      requestUpdate();
    }
  } else if (state_ == GENRE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < GENRE_COUNT - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const char* ndc = GENRES[selectedIndex_].ndc;

      {
        RenderLock lock(*this);
        pushState(LOADING);
      }
      requestUpdateAndWait();

      char query[64];
      snprintf(query, sizeof(query), "ndc=%s", ndc);
      if (fetchWorks(query)) {
        RenderLock lock(*this);
        state_ = WORK_LIST;
        selectedIndex_ = 0;
      } else {
        RenderLock lock(*this);
        state_ = ERROR;
      }
      requestUpdate();
    }
  } else if (state_ == AUTHOR_LIST) {
    // 作品一覧でメモリ解放された場合、作家一覧を再取得
    if (authors_.empty() && lastAuthorsKanaPrefix_[0]) {
      {
        RenderLock lock(*this);
        state_ = LOADING;
      }
      requestUpdateAndWait();
      if (fetchAuthors(lastAuthorsKanaPrefix_)) {
        RenderLock lock(*this);
        state_ = AUTHOR_LIST;
      } else {
        RenderLock lock(*this);
        state_ = ERROR;
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < static_cast<int>(authors_.size()) - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!authors_.empty()) {
        const auto& author = authors_[selectedIndex_];
        selectedAuthorId_ = author.id;
        snprintf(selectedAuthorName_, sizeof(selectedAuthorName_), "%s", author.name);
        {
          RenderLock lock(*this);
          pushState(AUTHOR_ACTION);
          actionMenuIndex_ = 0;
        }
        requestUpdate();
      }
    }
  } else if (state_ == WORK_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    // カーソル移動は前面ボタン（↑/↓）のみに割り当てる。側面ボタンはページ送り
    // 専用なので、ButtonNavigator の onNext/onPrevious（側面 Up/Down を含む）は
    // ここでは使えない。使うと1回の押下でカーソル移動とページ取得が同時に走り、
    // ページ取得側が selectedIndex_ を 0 に戻すためカーソル移動が消える。#104
    buttonNavigator_.onRelease({MappedInputManager::Button::Right}, [this] {
      if (selectedIndex_ < static_cast<int>(works_.size()) - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onRelease({MappedInputManager::Button::Left}, [this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    // ページ送りは側面ボタンの物理的な並び順に従う（先頭側=前ページ、末尾側=次
    // ページ）。この一覧は横書きなので、読書方向の設定（sideButtonLayout）では
    // なく画面の向きに合わせる。X4 なら上=前/下=次、X3 なら左=前/右=次。
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (worksOffset_ + WORKS_PAGE_SIZE < worksTotal_) {
        worksOffset_ += WORKS_PAGE_SIZE;
        {
          RenderLock lock(*this);
          state_ = LOADING;
        }
        requestUpdateAndWait();
        if (fetchWorks(nullptr)) {  // nullptr = 前回のクエリを再利用
          RenderLock lock(*this);
          state_ = WORK_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (worksOffset_ > 0) {
        worksOffset_ = (worksOffset_ >= WORKS_PAGE_SIZE) ? worksOffset_ - WORKS_PAGE_SIZE : 0;
        {
          RenderLock lock(*this);
          state_ = LOADING;
        }
        requestUpdateAndWait();
        if (fetchWorks(nullptr)) {
          RenderLock lock(*this);
          state_ = WORK_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!works_.empty()) {
        const auto& work = works_[selectedIndex_];
        selectedWorkId_ = work.id;
        snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", work.title);
        snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s",
                 work.author[0] ? work.author : selectedAuthorName_);
        snprintf(selectedWorkSubtitle_, sizeof(selectedWorkSubtitle_), "%s", work.subtitle);
        snprintf(selectedWorkVariant_, sizeof(selectedWorkVariant_), "%s", work.variant);
        snprintf(selectedWorkNdc_, sizeof(selectedWorkNdc_), "%s", work.ndc);

        {
          RenderLock lock(*this);
          pushState(WORK_DETAIL);
        }
        requestUpdate();
      }
    }
  } else if (state_ == WORK_DETAIL) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const bool alreadyDownloaded = indexManager_.isDownloaded(selectedWorkId_);

    if (!alreadyDownloaded) {
      // 未ダウンロード: Right = 取得
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        {
          RenderLock lock(*this);
          state_ = DOWNLOADING;
          downloadProgress_ = 0;
          downloadTotal_ = 0;
        }
        requestUpdateAndWait();

        if (downloadBook()) {
          RenderLock lock(*this);
          state_ = WORK_DETAIL;  // Stay on detail, now showing "downloaded"
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    } else {
      // ダウンロード済み: Confirm = 読む, Left = 削除, Right = 更新
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        openSelectedWorkInReader();
        return;  // goToReader() 後は this が破棄され得るのでメンバに触れない
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        if (indexManager_.removeEntry(selectedWorkId_)) {
          invalidateDownloadedPageCache();
          // popState() は selectedIndex_ を selectedIndexStack_.back() で上書きするため、
          // 親 state が DOWNLOADED_LIST の場合は削除後の件数に合わせてスタック側を
          // クランプする。他 state（TOP_MENU など）の selectedIndex は履歴件数と無関係
          // なので触らない。
          if (!stateStack_.empty() && stateStack_.back() == DOWNLOADED_LIST && !selectedIndexStack_.empty()) {
            const int newTotal = static_cast<int>(indexManager_.activeCount());
            int& parentSel = selectedIndexStack_.back();
            if (newTotal == 0) {
              parentSel = 0;
            } else if (parentSel >= newTotal) {
              parentSel = newTotal - 1;
            }
          }
          {
            RenderLock lock(*this);
            popState();
          }
          requestUpdate();
        }
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        {
          RenderLock lock(*this);
          state_ = DOWNLOADING;
          downloadProgress_ = 0;
          downloadTotal_ = 0;
        }
        requestUpdateAndWait();

        if (updateBook()) {
          RenderLock lock(*this);
          state_ = WORK_DETAIL;  // 更新成功、詳細画面に戻る
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    }
  } else if (state_ == FAVORITE_AUTHORS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const auto& favEntries = favoritesManager_.entries();

    if (!favEntries.empty()) {
      buttonNavigator_.onNextRelease([this, &favEntries] {
        if (selectedIndex_ < static_cast<int>(favEntries.size()) - 1) {
          selectedIndex_++;
          requestUpdate();
        }
      });

      buttonNavigator_.onPreviousRelease([this] {
        if (selectedIndex_ > 0) {
          selectedIndex_--;
          requestUpdate();
        }
      });

      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        const auto& fav = favEntries[selectedIndex_];
        selectedAuthorId_ = fav.authorId;
        snprintf(selectedAuthorName_, sizeof(selectedAuthorName_), "%s", fav.name);
        {
          RenderLock lock(*this);
          pushState(AUTHOR_ACTION);
          actionMenuIndex_ = 0;
        }
        requestUpdate();
      }
    }

  } else if (state_ == AUTHOR_ACTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (actionMenuIndex_ < 1) {
        actionMenuIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (actionMenuIndex_ > 0) {
        actionMenuIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (actionMenuIndex_ == 0) {
        // 作品を見る
        {
          RenderLock lock(*this);
          state_ = LOADING;
        }
        requestUpdateAndWait();

        char query[64];
        snprintf(query, sizeof(query), "author_id=%d", selectedAuthorId_);
        if (fetchWorks(query)) {
          RenderLock lock(*this);
          state_ = WORK_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      } else {
        // お気に入り追加/削除
        bool wasFavorited = favoritesManager_.isFavorited(selectedAuthorId_);
        if (wasFavorited) {
          favoritesManager_.removeAuthor(selectedAuthorId_);
        } else {
          const char* kana = "";
          for (const auto& a : authors_) {
            if (a.id == selectedAuthorId_) {
              kana = a.kana;
              break;
            }
          }
          favoritesManager_.addAuthor(selectedAuthorId_, selectedAuthorName_, kana);
        }
        {
          RenderLock lock(*this);
          popState();
        }
        requestUpdate();
      }
    }

  } else if (state_ == DOWNLOADED_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const int total = static_cast<int>(indexManager_.activeCount());

    buttonNavigator_.onNextRelease([this, total] {
      if (selectedIndex_ < total - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (total > 0 && selectedIndex_ < total) {
        // 選択中のエントリがキャッシュ範囲外なら selectedIndex_ を中央に置いて再ロードする
        if (selectedIndex_ < dlPageStart_ || selectedIndex_ >= dlPageStart_ + dlPageCount_) {
          loadDownloadedPage(selectedIndex_ - DL_PAGE_SIZE / 2);
        }
        const int localIdx = selectedIndex_ - dlPageStart_;
        if (localIdx >= 0 && localIdx < dlPageCount_) {
          const auto& entry = dlPageCache_[localIdx];
          selectedWorkId_ = entry.workId;
          snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", entry.title);
          snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s", entry.author);
          snprintf(selectedWorkSubtitle_, sizeof(selectedWorkSubtitle_), "%s", entry.subtitle);
          snprintf(selectedWorkVariant_, sizeof(selectedWorkVariant_), "%s", entry.variant);
          // NDC はダウンロード履歴に保存していないため、直前に見た作品の値が残らないようクリアする
          selectedWorkNdc_[0] = '\0';

          {
            RenderLock lock(*this);
            pushState(WORK_DETAIL);
          }
          requestUpdate();
        }
      }
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
    }
  }
}

// --- Rendering ---

void AozoraActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AOZORA_BUNKO));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_LIST));

  } else if (state_ == TOP_MENU) {
    GUI.drawList(
        renderer,
        Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
        TOP_MENU_COUNT, selectedIndex_,
        [](int index) -> std::string {
          switch (index) {
            case 0:
              return tr(STR_FAVORITE_AUTHORS);
            case 1:
              return tr(STR_SEARCH_BY_AUTHOR);
            case 2:
              return tr(STR_SEARCH_BY_TITLE);
            case 3:
              return tr(STR_SEARCH_BY_GENRE);
            case 4:
              return tr(STR_NEWEST_WORKS);
            case 5:
              return tr(STR_DOWNLOADED_BOOKS);
            default:
              return "";
          }
        },
        nullptr, nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == KANA_SELECT) {
    GUI.drawList(
        renderer,
        Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
        KANA_ROW_COUNT, selectedIndex_, [](int index) -> std::string { return I18N.get(KANA_ROWS[index].label); },
        nullptr, nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == KANA_CHAR_SELECT) {
    const int charCount = KANA_CHAR_COUNTS[selectedKanaRowIndex_];
    GUI.drawList(
        renderer,
        Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
        charCount, selectedIndex_,
        [this](int index) -> std::string { return KANA_CHARS[selectedKanaRowIndex_][index]; }, nullptr, nullptr,
        nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == GENRE_SELECT) {
    GUI.drawList(
        renderer,
        Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
        GENRE_COUNT, selectedIndex_, [](int index) -> std::string { return I18N.get(GENRES[index].label); }, nullptr,
        nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == AUTHOR_LIST) {
    if (authors_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_RESULTS));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          static_cast<int>(authors_.size()), selectedIndex_,
          [this](int index) -> std::string { return authors_[index].name; }, nullptr, nullptr,
          [this](int index) -> std::string {
            char buf[24];
            if (favoritesManager_.isFavorited(authors_[index].id)) {
              snprintf(buf, sizeof(buf), "* %d", authors_[index].workCount);
            } else {
              snprintf(buf, sizeof(buf), "%d", authors_[index].workCount);
            }
            return buf;
          },
          false, nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == WORK_LIST) {
    if (works_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_RESULTS));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      // ページ情報をヘッダー下に表示
      if (worksTotal_ > WORKS_PAGE_SIZE) {
        char pageInfo[48];
        int currentPage = (worksOffset_ / WORKS_PAGE_SIZE) + 1;
        int totalPages = (worksTotal_ + WORKS_PAGE_SIZE - 1) / WORKS_PAGE_SIZE;
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d (%d)", currentPage, totalPages, worksTotal_);
        renderer.drawText(UI_10_FONT_ID, pageWidth - metrics.contentSidePadding - 80, contentTop, pageInfo);
      }

      const int listTop = (worksTotal_ > WORKS_PAGE_SIZE) ? contentTop + lineHeight + 4 : contentTop;
      GUI.drawList(
          renderer,
          Rect{0, listTop, pageWidth, pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          static_cast<int>(works_.size()), selectedIndex_,
          [this](int index) -> std::string { return works_[index].title; },
          [this](int index) -> std::string { return buildWorkListSubtitle(index); }, nullptr,
          [this](int index) -> std::string {
            if (indexManager_.isDownloaded(works_[index].id)) {
              return tr(STR_DOWNLOADED_BOOKS);
            }
            return "";
          },
          false, nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == WORK_DETAIL) {
    int y = contentTop;

    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, selectedWorkTitle_);
    y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

    if (selectedWorkSubtitle_[0] != '\0') {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, selectedWorkSubtitle_);
      y += lineHeight + metrics.verticalSpacing;
    }

    if (selectedWorkAuthor_[0] != '\0') {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, selectedWorkAuthor_);
      y += lineHeight + metrics.verticalSpacing;
    }

    // 文字遣い（新字新仮名など）と NDC 分類。同名作品の判別材料になる。
    if (selectedWorkVariant_[0] != '\0' || selectedWorkNdc_[0] != '\0') {
      char meta[48];
      if (selectedWorkVariant_[0] != '\0' && selectedWorkNdc_[0] != '\0') {
        snprintf(meta, sizeof(meta), "%s  NDC %s", selectedWorkVariant_, selectedWorkNdc_);
      } else if (selectedWorkVariant_[0] != '\0') {
        snprintf(meta, sizeof(meta), "%s", selectedWorkVariant_);
      } else {
        snprintf(meta, sizeof(meta), "NDC %s", selectedWorkNdc_);
      }
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, meta);
      y += lineHeight + metrics.verticalSpacing;
    }

    bool alreadyDownloaded = indexManager_.isDownloaded(selectedWorkId_);
    if (alreadyDownloaded) {
      y += metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, tr(STR_DOWNLOAD_COMPLETE));

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_AOZORA_READ), tr(STR_DELETE_CONFIRM), tr(STR_AOZORA_UPDATE));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", tr(STR_AOZORA_GET));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOADING_BOOK));

    float progress = 0;
    if (downloadTotal_ > 0) {
      progress = static_cast<float>(downloadProgress_) / static_cast<float>(downloadTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        downloadProgress_, downloadTotal_);

    int percentY = barY + metrics.progressBarHeight + metrics.verticalSpacing;
    char buf[32];
    if (downloadTotal_ > 0) {
      snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(progress * 100));
    } else if (downloadProgress_ > 0) {
      snprintf(buf, sizeof(buf), "%d KB", static_cast<int>(downloadProgress_ / 1024));
    } else {
      snprintf(buf, sizeof(buf), "...");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, percentY, buf);

  } else if (state_ == FAVORITE_AUTHORS) {
    const auto& favEntries = favoritesManager_.entries();

    if (favEntries.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FAVORITE_AUTHORS));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          static_cast<int>(favEntries.size()), selectedIndex_,
          [&favEntries](int index) -> std::string { return favEntries[index].name; }, nullptr, nullptr, nullptr, false,
          nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == AUTHOR_ACTION) {
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop, selectedAuthorName_);
    const int listTop = contentTop + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

    bool isFav = favoritesManager_.isFavorited(selectedAuthorId_);
    GUI.drawList(
        renderer,
        Rect{0, listTop, pageWidth, pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing}, 2,
        actionMenuIndex_,
        [isFav](int index) -> std::string {
          if (index == 0) return tr(STR_VIEW_WORKS);
          return isFav ? tr(STR_REMOVE_FROM_FAVORITES) : tr(STR_ADD_TO_FAVORITES);
        },
        nullptr, nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == DOWNLOADED_LIST) {
    const int total = static_cast<int>(indexManager_.activeCount());

    if (total == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_RESULTS));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      // drawList のページサイズは theme と画面高に依存し、DL_PAGE_SIZE と一致しないため、
      // selectedIndex_ がキャッシュ範囲外に出たら selectedIndex_ を中央に置いて再ロードする。
      // これにより drawList が要求する [selectedIdx / pageItems * pageItems, +pageItems) の
      // 範囲は pageItems <= DL_PAGE_SIZE / 2 の限り必ずキャッシュ内に収まる。
      if (selectedIndex_ < dlPageStart_ || selectedIndex_ >= dlPageStart_ + dlPageCount_) {
        loadDownloadedPage(selectedIndex_ - DL_PAGE_SIZE / 2);
      }

      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          total, selectedIndex_,
          [this](int index) -> std::string {
            const int localIdx = index - dlPageStart_;
            if (localIdx < 0 || localIdx >= dlPageCount_) return "";
            return dlPageCache_[localIdx].title;
          },
          // 著者は右端の value 列から 2 行目に移した。副題・文字遣いを併記して
          // 同名作品を区別できるようにするため。
          [this](int index) -> std::string { return buildDownloadedListSubtitle(index); }, nullptr, nullptr, false,
          nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_ERROR_MSG), true, EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void AozoraActivity::loadDownloadedPage(int desiredStart) {
  const int total = static_cast<int>(indexManager_.activeCount());
  dlDupTitleMask_ = 0;
  dlAmbiguousMask_ = 0;
  if (total == 0) {
    dlPageStart_ = 0;
    dlPageCount_ = 0;
    return;
  }

  // start をリストの末尾側でクランプしてキャッシュサイズを最大化する
  int start = desiredStart;
  if (start < 0) start = 0;
  if (start + DL_PAGE_SIZE > total) {
    start = std::max(0, total - DL_PAGE_SIZE);
  }

  dlPageStart_ = start;
  dlPageCount_ = 0;

  for (int i = 0; i < DL_PAGE_SIZE; ++i) {
    const int absIndex = start + i;
    if (absIndex >= total) break;
    if (!indexManager_.readEntryAt(absIndex, dlPageCache_[i])) {
      LOG_ERR("AOZORA", "loadDownloadedPage: readEntryAt(%d) failed", absIndex);
      break;
    }
    dlPageCount_++;
  }

  // 同名作品の識別マスクを計算する（WORK_LIST と同じルール）。ページロード時の
  // 1 回だけなので、最大 435 回の strcmp は描画コストに乗らない。
  for (int i = 0; i < dlPageCount_; ++i) {
    for (int j = i + 1; j < dlPageCount_; ++j) {
      if (strcmp(dlPageCache_[i].title, dlPageCache_[j].title) != 0) continue;
      dlDupTitleMask_ |= (1u << i) | (1u << j);
      if (strcmp(dlPageCache_[i].variant, dlPageCache_[j].variant) == 0 &&
          strcmp(dlPageCache_[i].subtitle, dlPageCache_[j].subtitle) == 0) {
        dlAmbiguousMask_ |= (1u << i) | (1u << j);
      }
    }
  }
}
