#include "EmptyDirCleanup.h"

namespace {
// 起動直後は未掃除とみなす。ESP32 はディープスリープからの復帰でも
// プログラムが先頭から動き直すため、復帰のたびに1回は掃除が走る。
bool cleanupRequested = true;
}  // namespace

void requestEmptyDirCleanup() { cleanupRequested = true; }

bool consumeEmptyDirCleanupRequest() {
  const bool requested = cleanupRequested;
  cleanupRequested = false;
  return requested;
}
