#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "BuildInfo.h"
#include "CrossPointSettings.h"
#include "ReleaseVersion.h"

// The channel lives in three places: the persisted setting, the comparison
// policy, and the settings-screen labels. Nothing but these asserts stops the
// first two from drifting — a value added to one only would compile, then get
// silently clamped back to STABLE at runtime with no test failure.
// The label count in SettingsList.h is a std::vector size and cannot be bound
// here; keep it in step with OTA_CHANNEL_COUNT by hand.
static_assert(static_cast<int>(releaseVersion::CHANNEL_STABLE) == CrossPointSettings::OTA_CHANNEL_STABLE,
              "otaChannel の永続値と releaseVersion::Channel がずれている");
static_assert(static_cast<int>(releaseVersion::CHANNEL_RC) == CrossPointSettings::OTA_CHANNEL_RC,
              "otaChannel の永続値と releaseVersion::Channel がずれている");
static_assert(static_cast<int>(releaseVersion::CHANNEL_DEV) == CrossPointSettings::OTA_CHANNEL_DEV,
              "otaChannel の永続値と releaseVersion::Channel がずれている");
static_assert(CrossPointSettings::OTA_CHANNEL_COUNT == static_cast<int>(releaseVersion::CHANNEL_DEV) + 1,
              "チャネルを追加したら SettingsList.h のラベルも増やすこと");

namespace {
// The stable channel asks GitHub for the release it marks as latest. Prereleases
// (RC and Dev Build) can never hold that flag, so this endpoint is exactly the
// stable line — no filtering needed, and it stays a single 8.7KB object.
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/zrn-ns/crosspoint-jp/releases/latest";

// The RC and DEV channels additionally scan the release list, which is where the
// prereleases live. The list is ordered by the tag commit's date, not by publish
// time, so "first entry" is not reliably the newest — every entry in the window
// is compared and the best one wins. 10 entries is ~90KB streamed; Dev Builds
// land about once a day, so the window covers well over a week of prereleases.
//
// The list alone is NOT enough: with a Dev Build a day, the window fills with
// dev tags within days and would hide a stable/RC release that sits just past
// it. That is why /releases/latest is always fetched too.
constexpr char releaseListUrl[] = "https://api.github.com/repos/zrn-ns/crosspoint-jp/releases?per_page=10";

// esp_err_to_name の "ESP_ERR_" prefix を削って画面幅に収まる短い名前にする。
const char* shortErrName(const char* name) {
  if (!name) return "?";
  if (strncmp(name, "ESP_ERR_", 8) == 0) return name + 8;
  return name;
}

}  // namespace

void OtaUpdater::sOnRelease(void* ctx, const char* tag, const char* fwUrl, size_t fwSize) {
  static_cast<OtaUpdater*>(ctx)->onRelease(tag, fwUrl, fwSize);
}

void OtaUpdater::onRelease(const char* tag, const char* fwUrl, size_t fwSize) {
  VersionKey key;
  if (!releaseVersion::parseCandidateTag(tag, &key)) {
    LOG_DBG("OTA", "Skipping release with unrecognised tag: %s", tag);
    return;
  }

  if (!releaseVersion::shouldReplaceWinner(key, winnerKey, deviceKey, CROSSPOINT_BUILD_TIME, channel)) return;

  winnerKey = key;
  latestVersion = tag;
  otaUrl = fwUrl;
  otaSize = fwSize;
  totalSize = fwSize;
  updateAvailable = true;
  LOG_DBG("OTA", "Candidate accepted: %s (%zu bytes)", tag, fwSize);
}

OtaUpdater::OtaUpdaterError OtaUpdater::fetchReleases(const char* url, ReleaseJsonParser& releaseParser) {
  releaseParser.resetStream();
  const size_t seenBefore = releaseParser.releaseCount();

  // Stream the release JSON straight into the parser as it arrives. Buffering
  // the whole body in a std::string would add a growing allocation on top of the
  // TLS session's heap during the fetch; with -fno-exceptions an OOM there
  // aborts. fetchUrl handles the verified-https GET, redirects, and User-Agent
  // (see HttpDownloader).
  const bool ok = HttpDownloader::fetchUrl(url, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed: %s", url);
    char buf[64];
    snprintf(buf, sizeof(buf), "fetch fail http=%d heap=%dKB", HttpDownloader::lastHttpCode,
             static_cast<int>(ESP.getFreeHeap() / 1024));
    lastErrorDetail = buf;
    return HTTP_ERROR;
  }

  // Zero releases parsed out of a successful fetch means the body was not a
  // release payload. Releases that simply lost the comparison are counted here
  // too, so this cannot be confused with "nothing newer".
  if (releaseParser.releaseCount() == seenBefore) {
    LOG_ERR("OTA", "No release object in response: %s", url);
    char buf[64];
    snprintf(buf, sizeof(buf), "no release in json http=%d", HttpDownloader::lastHttpCode);
    lastErrorDetail = buf;
    return JSON_PARSE_ERROR;
  }
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  // A settings.json written by a newer firmware could carry a channel this
  // build does not know; fall back to the safest one rather than trusting it.
  channel = SETTINGS.otaChannel <= releaseVersion::CHANNEL_DEV
                ? static_cast<releaseVersion::Channel>(SETTINGS.otaChannel)
                : releaseVersion::CHANNEL_STABLE;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  lastErrorDetail.clear();
  winnerKey = VersionKey{};
  releasesScanned = 0;

  if (!releaseVersion::parseDeviceVersion(CROSSPOINT_VERSION, &deviceKey)) {
    // Only reachable when no v* tag was in reach at build time. Dev candidates
    // still compare fine (they use the build stamp), release candidates do not.
    LOG_ERR("OTA", "Version parse failed: current=%s", CROSSPOINT_VERSION);
  }
  LOG_DBG("OTA", "Checking for update (current: %s, built: %s, channel: %d)", CROSSPOINT_VERSION, CROSSPOINT_BUILD_TIME,
          static_cast<int>(channel));

  // ~1.7KB of buffers. The check runs on the loop task, whose stack this would
  // eat into, and it is a single short-lived allocation — the acceptable-malloc
  // case from CLAUDE.md.
  // nothrow: the build is -fno-exceptions, so a throwing new would abort
  // instead of letting the check fail gracefully.
  std::unique_ptr<ReleaseJsonParser> releaseParser(new (std::nothrow) ReleaseJsonParser());
  if (!releaseParser) {
    LOG_ERR("OTA", "Parser allocation failed");
    lastErrorDetail = "parser oom";
    return OOM_ERROR;
  }
  releaseParser->setHandler(&OtaUpdater::sOnRelease, this);

  OtaUpdaterError err = fetchReleases(latestReleaseUrl, *releaseParser);
  if (err != OK) return err;

  if (channel != releaseVersion::CHANNEL_STABLE) {
    err = fetchReleases(releaseListUrl, *releaseParser);
    // A stable candidate found by the first request is still worth offering, so
    // only report the list failure when it leaves us with nothing.
    if (err != OK && !updateAvailable) return err;
    if (err != OK) LOG_ERR("OTA", "Prerelease list unavailable; falling back to the stable candidate");
  }

  releasesScanned = releaseParser->releaseCount();

  if (!updateAvailable) {
    LOG_DBG("OTA", "No newer release in channel %d (%zu releases scanned)", static_cast<int>(channel), releasesScanned);
    return OK;  // not an error: the device is already on the newest build
  }

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
  lastErrorDetail.clear();

  // esp_https_ota opens its own esp_http_client with its own buffer sizes and
  // no visibility into the TLS diagnostics this fork captures. Drive the OTA
  // partition ourselves and stream the firmware through HttpDownloader instead,
  // reusing its redirect handling for the GitHub -> CDN hop and the LARGE TX
  // profile that hop's signed URL needs.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    lastErrorDetail = "no ota partition";
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    char buf[96];
    snprintf(buf, sizeof(buf), "begin:%s heap=%dKB", shortErrName(esp_err_to_name(esp_err)),
             static_cast<int>(ESP.getFreeHeap() / 1024));
    lastErrorDetail = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  esp_err_t writeErr = ESP_OK;
  // LARGE buffers: otaUrl points at the release asset, which redirects to a
  // signed release-assets.githubusercontent.com URL whose request line does not
  // fit the default 512-byte TX buffer. FontDownloadActivity needs them for the
  // same reason — see HttpDownloader::BufferProfile.
  const bool fetchOk = HttpDownloader::fetchUrl(
      otaUrl,
      [&](const uint8_t* data, size_t len) {
        writeErr = esp_ota_write(otaHandle, data, len);
        if (writeErr != ESP_OK) {
          flashOk = false;
          return false;  // abort the transfer
        }
        processedSize += len;
        // Fire the callback only on whole-percent change. Per-chunk updates wake the
        // render task, whose framebuffer work contends with TLS on the internal arena,
        // and e-ink can't repaint faster than a percent tick anyway.
        if (onProgress && totalSize > 0) {
          const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
          if (pct != lastReportedPct) {
            lastReportedPct = pct;
            onProgress(ctx);
          }
        }
        return true;
      },
      "", "", HttpDownloader::BufferProfile::LARGE);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    char buf[128];
    if (!flashOk) {
      snprintf(buf, sizeof(buf), "write:%s r=%d/%d|heap=%dKB blk=%dKB", shortErrName(esp_err_to_name(writeErr)),
               static_cast<int>(processedSize), static_cast<int>(totalSize), static_cast<int>(ESP.getFreeHeap() / 1024),
               static_cast<int>(ESP.getMaxAllocHeap() / 1024));
    } else {
      snprintf(buf, sizeof(buf), "fetch http=%d r=%d/%d|heap=%dKB blk=%dKB", HttpDownloader::lastHttpCode,
               static_cast<int>(processedSize), static_cast<int>(totalSize), static_cast<int>(ESP.getFreeHeap() / 1024),
               static_cast<int>(ESP.getMaxAllocHeap() / 1024));
    }
    lastErrorDetail = buf;
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    char buf[128];
    snprintf(buf, sizeof(buf), "end:%s r=%d/%d|heap=%dKB blk=%dKB", shortErrName(esp_err_to_name(esp_err)),
             static_cast<int>(processedSize), static_cast<int>(totalSize), static_cast<int>(ESP.getFreeHeap() / 1024),
             static_cast<int>(ESP.getMaxAllocHeap() / 1024));
    lastErrorDetail = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    char buf[96];
    snprintf(buf, sizeof(buf), "setboot:%s", shortErrName(esp_err_to_name(esp_err)));
    lastErrorDetail = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
