#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before esp_http_client (which includes lwip). Pin this
// order; clang-format would otherwise sort the local header last and break the
// build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_wifi.h>
// clang-format on

#include <cstring>
#include <string>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/zrn-ns/crosspoint-jp/releases/latest";

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

bool parseSemver3(const char* version, int* major, int* minor, int* patch) {
  if (!version || !major || !minor || !patch) {
    return false;
  }
  const char* p = version;
  if (*p == 'v' || *p == 'V') {
    p++;
  }
  return sscanf(p, "%d.%d.%d", major, minor, patch) == 3;
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}
} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  // Stream the release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. The old ArduinoJson + esp_http_client event_handler path
  // also silently dropped chunked-transfer responses. HttpDownloader::fetchUrl
  // handles verified-https GET, redirects, and User-Agent for us.
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: %s, asset size=%u", latestVersion.c_str(), static_cast<unsigned int>(otaSize));
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  if (!parseSemver3(latestVersion.c_str(), &latestMajor, &latestMinor, &latestPatch) ||
      !parseSemver3(currentVersion, &currentMajor, &currentMinor, &currentPatch)) {
    LOG_ERR("OTA", "Version parse failed: current=%s, latest=%s", currentVersion, latestVersion.c_str());
    return false;
  }

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate() {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;
  /* Signal for OtaUpdateActivity */
  render = false;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 15000,
      /* Default HTTP client buffer size 512 byte only
       * not sufficient to handle URL redirection cases or
       * parsing of large HTTP headers.
       */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    /* Sent signal to  OtaUpdateActivity */
    render = true;
    delay(100);  // TODO: should we replace this with something better?
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
