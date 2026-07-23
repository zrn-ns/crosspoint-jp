#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps NetworkClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  /// Streaming body callback: called with each response chunk as it arrives.
  /// Return false to abort. Lets a streaming parser consume the response
  /// without buffering the whole body (TLS session + full-body buffer would
  /// otherwise contend for the same tight heap arena and OOM on ESP32-C3).
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /// Last HTTP status code from downloadToFile (-1 = connection failed, -11 = timeout, etc.)
  static int lastHttpCode;

  /**
   * Fetch text content from a URL with optional Basic auth credentials.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @param username Optional username for Basic auth
   * @param password Optional password for Basic auth
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   * Use when the response is large (>10KB) or when heap is tight during TLS
   * (OTA release JSON check, streaming JSON parse, etc.).
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Download a file to the SD card with optional Basic auth credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, int timeoutMs = 0,
                                      const std::string& username = "", const std::string& password = "");
};
