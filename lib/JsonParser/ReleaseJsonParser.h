#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

// Streams the GitHub releases API and emits one callback per release that
// carries a firmware.bin asset.
//
// Both response shapes are accepted:
//   /releases/latest       -> a single release object
//   /releases?per_page=N   -> an array of release objects
// The shape is detected from the first structural token, so the same parser
// serves both without configuration.
//
// The parser keeps no "best release" state: which release wins is a policy
// decision (channel, version comparison) that belongs to OtaUpdater. Keeping
// the winner out here is what lets one selection survive across the two
// requests the RC/DEV channels make.
class ReleaseJsonParser {
 public:
  // Invoked once per release that has a firmware.bin asset, at the point the
  // release object closes. tag/fwUrl are null-terminated and only valid for
  // the duration of the call.
  using ReleaseHandler = void (*)(void* ctx, const char* tag, const char* fwUrl, size_t fwSize);

  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void setHandler(ReleaseHandler handler, void* ctx);

  // Full reset, including the release counter.
  void reset();
  // Resets only the streaming state, so a second response can be fed through
  // the same instance while the caller's selection carries over.
  void resetStream();
  void feed(const char* data, size_t len);

  // Number of release objects seen across every response fed so far. Zero after
  // a successful fetch means the body was not a release payload, which is how
  // the caller tells "malformed" apart from "nothing newer".
  size_t releaseCount() const { return releasesSeen; }

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void beginRelease();
  void endRelease();
  void commitAsset();

  StreamingJsonParser parser;

  ReleaseHandler handler;
  void* handlerCtx;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;
  // Container depth at which a release object's own keys live: 1 for a bare
  // release object, 2 when the response is an array of releases. 0 until the
  // first structural token settles it.
  uint8_t keyDepth;
  size_t releasesSeen;

  // Release currently being parsed.
  char currentTag[32];
  char currentFwUrl[512];
  size_t currentFwSize;
  bool currentFwFound;

  // Asset currently being parsed, within the release's assets array.
  char currentAssetName[32];
  char currentAssetUrl[512];
  size_t currentAssetSize;
};
