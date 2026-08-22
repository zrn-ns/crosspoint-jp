#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ReleaseVersion.h"

class OtaUpdater {
 public:
  using ProgressCallback = void (*)(void* ctx);

  // No "no update" value: a scan that finds nothing newer is a success. It
  // returns OK with isUpdateNewer() == false, because OtaUpdateActivity turns
  // any non-OK result into the FAILED screen.
  enum OtaUpdaterError {
    OK = 0,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
  };

  OtaUpdater() = default;

  size_t getOtaSize() const { return otaSize; }
  size_t getProcessedSize() const { return processedSize; }
  size_t getTotalSize() const { return totalSize; }
  const std::string& getLastErrorDetail() const { return lastErrorDetail; }
  const std::string& getLatestVersion() const;

  // 画面デバッグ用 (SETTINGS.debugDisplay)。シリアルが取れない環境で
  // チャネル判定と2フェッチ構成を確認するために公開している。
  releaseVersion::Channel getChannel() const { return channel; }
  size_t getReleasesScanned() const { return releasesScanned; }

  // True when checkForUpdate() selected a release that is newer than this
  // build. The comparison happens during the scan, so this is just the result.
  bool isUpdateNewer() const { return updateAvailable; }

  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);

 private:
  using VersionKey = ReleaseVersionKey;

  static void sOnRelease(void* ctx, const char* tag, const char* fwUrl, size_t fwSize);
  void onRelease(const char* tag, const char* fwUrl, size_t fwSize);
  OtaUpdaterError fetchReleases(const char* url, class ReleaseJsonParser& releaseParser);

  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  // 失敗ステップと esp_err_t の名前を短くまとめた診断文字列。
  // シリアルが取れない環境で FAILED 画面に表示するため。
  std::string lastErrorDetail;

  releaseVersion::Channel channel = releaseVersion::CHANNEL_STABLE;
  // 走査した release の件数。正式チャネルなら1、RC/Dev なら 1+per_page。
  size_t releasesScanned = 0;
  VersionKey deviceKey;
  VersionKey winnerKey;
};
