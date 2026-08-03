#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TlsHeapReclaim.h"

// Retries per HTTPS request. Matches the Aozora endpoints; see the download
// loop for why a handshake can fail once and succeed a second later.
static constexpr int DOWNLOAD_MAX_RETRIES = 3;

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Reload font caches that were freed for TLS memory
  FontManager::getInstance().loadSettings();
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  reclaimHeapForTls(renderer, "FONT");

  if (!fetchAndParseManifest()) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // Download manifest to SD card temp file, then parse from file.
  // This avoids holding both TLS buffers and manifest data in RAM.
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  errorDetail_.clear();  // don't let a previous failure's diagnostics linger
  const size_t heapBefore = ESP.getFreeHeap();
  // LARGE buffers: the manifest lives on a GitHub release, which redirects to a
  // signed release-assets.githubusercontent.com URL ~860 bytes long. That request
  // line does not fit the default 512-byte TX buffer, so the reopen after the 302
  // fails with ESP_FAIL before any byte arrives — see HttpDownloader::BufferProfile.
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < DOWNLOAD_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      LOG_DBG("FONT", "Manifest retry %d/%d", attempt + 1, DOWNLOAD_MAX_RETRIES);
      delay(1000);
    }
    result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr, nullptr, "", "",
                                            HttpDownloader::BufferProfile::LARGE);
    if (result == HttpDownloader::OK) break;
    LOG_ERR("FONT", "Manifest attempt %d failed: err=%d http=%d tls=%d/%X blk=%d", attempt + 1,
            static_cast<int>(result), HttpDownloader::lastHttpCode, HttpDownloader::lastTlsError,
            HttpDownloader::lastTlsFlags, static_cast<int>(ESP.getMaxAllocHeap()));
    Storage.remove(MANIFEST_TMP);
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Manifest fetch failed (err=%d, http=%d, heap=%zu)", result, HttpDownloader::lastHttpCode,
            heapBefore);
    char buf[160];
    // blk = largest contiguous block; the TLS handshake needs a ~16.5KB one for
    // the inbound record buffer, so free heap alone cannot distinguish
    // exhaustion from fragmentation. tls is the mbedtls/esp-tls reason behind an
    // ESP_ERR_HTTP_CONNECT (0 = never reached TLS, so DNS or the TCP connect).
    snprintf(buf, sizeof(buf), "err=%d http=%d tls=%d/%X crt=%d/%X p=%d/%d f=%d/%d", static_cast<int>(result),
             HttpDownloader::lastHttpCode, HttpDownloader::lastTlsError, HttpDownloader::lastTlsFlags,
             HttpDownloader::lastCrtDiag, HttpDownloader::lastCrtErr, HttpDownloader::lastPreHeapKb,
             HttpDownloader::lastPreBlkKb, HttpDownloader::lastCrtHeapKb, HttpDownloader::lastCrtBlkKb);
    errorMessage_ = buf;
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  // TLS connection closed — buffers freed. Parse JSON from file.
  FsFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read manifest";
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  int version = doc["version"] | 0;
  if (version != 1) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = "Unsupported manifest version";
    return false;
  }

  snprintf(baseUrl_, sizeof(baseUrl_), "%s", doc["baseUrl"] | "");
  families_.clear();
  allFiles_.clear();

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  families_.reserve(familiesArr.size());

  // Count the files up front so allFiles_ is reserved exactly. Walking the
  // JsonArray twice is far cheaper than the reallocate-copy-free cycles a
  // growing vector would inflict on an already tight arena. The manifest's
  // "styles" array is deliberately ignored: it is empty for every family and
  // nothing reads it.
  size_t fileCountTotal = 0;
  for (JsonObject fObj : familiesArr) fileCountTotal += fObj["files"].as<JsonArray>().size();
  allFiles_.reserve(fileCountTotal);

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    snprintf(family.name, sizeof(family.name), "%s", fObj["name"] | "");
    snprintf(family.description, sizeof(family.description), "%s", fObj["description"] | "");

    family.fileStart = static_cast<uint16_t>(allFiles_.size());
    family.fileCount = 0;
    family.totalSize = 0;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
      if (family.fileCount == UINT8_MAX) {
        LOG_ERR("FONT", "Too many files in family %s; ignoring the rest", family.name);
        break;
      }
      ManifestFile file;
      snprintf(file.name, sizeof(file.name), "%s", fileObj["name"] | "");
      file.size = fileObj["size"] | 0u;
      family.totalSize += file.size;
      allFiles_.push_back(file);
      family.fileCount++;
    }

    family.installed = fontInstaller_.isFamilyInstalled(family.name);

    // Detect updates by comparing manifest file sizes with files on disk.
    // Not a checksum, but a size mismatch reliably indicates a rebuild in practice.
    if (family.installed) {
      for (uint8_t i = 0; i < family.fileCount; i++) {
        const auto& file = allFiles_[family.fileStart + i];
        char path[128];
        FontInstaller::buildFontPath(family.name, file.name, path, sizeof(path));
        FsFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != file.size) {
            family.hasUpdate = true;
            break;
          }
        } else {
          // File missing on disk but family dir exists — treat as update
          family.hasUpdate = true;
          break;
        }
      }
    }

    families_.push_back(std::move(family));
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed && !families_[i].hasUpdate) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

size_t FontDownloadActivity::totalUninstalledSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed || f.hasUpdate) total += f.totalSize;
  }
  return total;
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  {
    RenderLock lock(*this);
    errorDetail_.clear();  // don't let a previous failure's diagnostics linger
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = family.fileCount;
    fileProgress_ = 0;
    fileTotal_ = 0;
  }
  requestUpdateAndWait();

  if (!fontInstaller_.ensureFamilyDir(family.name)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  for (uint8_t i = 0; i < family.fileCount; i++) {
    const auto& file = allFiles_[family.fileStart + i];

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    // Reclaim before every file, not just once on entry: drawing the progress
    // screen between files re-fills the glyph caches this frees, so without it
    // each successive file starts from a more fragmented arena than the last.
    // Cheap insurance rather than a fix — the failures that motivated it were
    // caused by the record buffers being twice their configured size, which
    // scripts/patch_espidf_libcopy.py resolved.
    reclaimHeapForTls(renderer, "FONT");

    char destPath[128];
    FontInstaller::buildFontPath(family.name, file.name, destPath, sizeof(destPath));

    // Stack buffer, not std::string concatenation: this runs immediately before
    // the TLS handshake, where a transient heap allocation is exactly what the
    // contiguous-block budget cannot afford. baseUrl_ is 67 bytes, names reach 30.
    char url[192];
    snprintf(url, sizeof(url), "%s%s", baseUrl_, file.name);

    // Fire the render task only on whole-percent change. HttpDownloader reads in
    // 1KB chunks, so a per-chunk update would wake the render task ~400 times for
    // a single font file; that framebuffer work contends with TLS on the internal
    // arena and e-ink cannot repaint faster than a percent tick anyway. Same
    // reasoning as the OTA download — see OtaUpdater::installUpdate.
    // Retry like the Aozora endpoints do. Each file is a fresh TLS handshake,
    // and on a heap this tight a handshake can fail on contiguous space alone —
    // either on the 16.5KB record buffer or, later, on the small allocations
    // mbedtls needs to verify the chain (tls=12288). A second later, after the
    // previous connection's buffers have been returned, the same request
    // usually goes through. Without this, one unlucky file aborts the whole
    // run and the user has to start over.
    HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
    for (int attempt = 0; attempt < DOWNLOAD_MAX_RETRIES; attempt++) {
      if (attempt > 0) {
        LOG_DBG("FONT", "Retry %d/%d for %s (heap=%d blk=%d)", attempt + 1, DOWNLOAD_MAX_RETRIES, file.name,
                static_cast<int>(ESP.getFreeHeap()), static_cast<int>(ESP.getMaxAllocHeap()));
        delay(1000);
      }
      int lastReportedPct = -1;
      // LARGE buffers for the same reason as the manifest fetch: the signed
      // redirect target runs ~900 bytes for the longest font file name.
      result = HttpDownloader::downloadToFile(
          url, destPath,
          [this, &lastReportedPct](size_t downloaded, size_t total) {
            const int pct = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
            if (pct == lastReportedPct) return;
            lastReportedPct = pct;
            fileProgress_ = downloaded;
            fileTotal_ = total;
            requestUpdate(true);
          },
          nullptr, "", "", HttpDownloader::BufferProfile::LARGE);
      if (result == HttpDownloader::OK) break;
      LOG_ERR("FONT", "Attempt %d failed: %s err=%d http=%d tls=%d/%X heap=%d blk=%d got=%zu", attempt + 1, file.name,
              static_cast<int>(result), HttpDownloader::lastHttpCode, HttpDownloader::lastTlsError,
              HttpDownloader::lastTlsFlags, static_cast<int>(ESP.getFreeHeap()),
              static_cast<int>(ESP.getMaxAllocHeap()), fileProgress_);
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s err=%d http=%d heap=%d blk=%d got=%zu", file.name, static_cast<int>(result),
              HttpDownloader::lastHttpCode, static_cast<int>(ESP.getFreeHeap()),
              static_cast<int>(ESP.getMaxAllocHeap()), fileProgress_);
      char buf[160];
      // Same diagnostics as the manifest fetch: err is DownloadError, http is
      // either an HTTP status or a negated esp_err_t, tls is the TLS-layer
      // reason behind it, and blk is the largest contiguous block. got is how
      // far the transfer got before it died, rounded to the last whole percent.
      snprintf(buf, sizeof(buf), "err=%d http=%d crt=%d/%X a=%d@%dK/%d p=%d/%d got=%dKB", static_cast<int>(result),
               HttpDownloader::lastHttpCode, HttpDownloader::lastCrtDiag, HttpDownloader::lastCrtErr,
               HttpDownloader::lastFailSize, HttpDownloader::lastFailFreeKb, HttpDownloader::lastFailBlk,
               HttpDownloader::lastPreHeapKb, HttpDownloader::lastPreBlkKb, static_cast<int>(fileProgress_ / 1024));
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Download failed: ") + file.name;
      errorDetail_ = buf;
      return;
    }

    if (!fontInstaller_.validateCpfontFile(destPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", destPath);
      Storage.remove(destPath);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Invalid font file: ") + file.name;
      return;
    }
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < listItemCount() - 1) {
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

    // Every state transition below needs an explicit requestUpdate(): RenderLock
    // only guards the render task, it does not schedule a frame. Without it the
    // stale screen stays up and the next press looks like the first one being
    // swallowed — the confirmation screen never appeared, so the user had to
    // press Confirm twice to start a download.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        {
          RenderLock lock(*this);
          state_ = CONFIRM_DOWNLOAD;
        }
        requestUpdate();
      }
    }
  } else if (state_ == CONFIRM_DOWNLOAD) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (isDownloadAllSelected()) {
        downloadAll();
      } else {
        downloadFamily(families_[familyIndexFromList(selectedIndex_)]);
      }
      requestUpdate();
    }
  } else if (state_ == COMPLETE || state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_DOWNLOAD));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (index == 0) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalUninstalledSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          nullptr, nullptr,
          [this](int index) -> std::string {
            if (index == 0) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            return f.description;
          },
          true,
          [this](int index) -> bool {
            if (index == 0) return false;
            const auto& f = families_[familyIndexFromList(index)];
            // Dim installed fonts, but not those with updates available
            return f.installed && !f.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == CONFIRM_DOWNLOAD) {
    int y = contentTop;

    if (isDownloadAllSelected()) {
      std::string confirmText = std::string(tr(STR_DOWNLOAD_ALL)) + "?";
      renderer.drawCenteredText(UI_10_FONT_ID, y, confirmText.c_str());
      y += lineHeight + metrics.verticalSpacing;

      size_t totalFiles = 0;
      for (const auto& f : families_) {
        if (!f.installed) totalFiles += f.fileCount;
      }
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_FILES_LABEL)) + std::to_string(totalFiles)).c_str());
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_SIZE_LABEL)) + formatSize(totalUninstalledSize())).c_str());
    } else {
      const auto& family = families_[familyIndexFromList(selectedIndex_)];
      std::string confirmText = (family.installed ? std::string(tr(STR_REDOWNLOAD)) : std::string(tr(STR_DOWNLOAD))) +
                                " " + family.name + "?";
      renderer.drawCenteredText(UI_10_FONT_ID, y, confirmText.c_str());
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_FILES_LABEL)) + std::to_string(family.fileCount)).c_str());
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_SIZE_LABEL)) + formatSize(family.totalSize)).c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + family.name + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    int percentY = barY + metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, percentY,
                              (std::to_string(static_cast<int>(progress * 100)) + "%").c_str());
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    if (!errorDetail_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing + lineHeight, errorDetail_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // Live heap readout. Added to measure a reproduction that had resisted
  // guesswork — downloads succeeded right after boot and failed after a reading
  // session — and kept because comparing this number across states is the
  // fastest way to tell a heap regression from a network one.
  if (SETTINGS.debugDisplay) {
    char dbg[48];
    snprintf(dbg, sizeof(dbg), "heap=%dK blk=%dK", static_cast<int>(ESP.getFreeHeap() / 1024),
             static_cast<int>(ESP.getMaxAllocHeap() / 1024));
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, pageHeight - lineHeight * 2, dbg);
  }

  renderer.displayBuffer();
}
