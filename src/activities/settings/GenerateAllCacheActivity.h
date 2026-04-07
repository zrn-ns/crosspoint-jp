#pragma once

#include "activities/Activity.h"

class GenerateAllCacheActivity final : public Activity {
 public:
  explicit GenerateAllCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GenerateAllCache", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { CONFIRMING, GENERATING, SUCCESS, FAILED };

  State state = CONFIRMING;
  int processedCount = 0;
  int totalCount = 0;

  void goBack() { finish(); }
  void generateAllCaches();
};
