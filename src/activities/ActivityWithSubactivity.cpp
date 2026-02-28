#include "ActivityWithSubactivity.h"

#include <HalPowerManager.h>

#include "OrientationHelper.h"

void ActivityWithSubactivity::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      // RenderLock MUST be constructed before PowerLock (see Activity::renderTaskLoop)
      RenderLock lock(*this);
      HalPowerManager::Lock powerLock;
      if (!subActivity) {
        render(std::move(lock));
      }
      // If subActivity is set, consume the notification but skip parent render
      // Note: the sub-activity will call its render() from its own display task
    }
    // Log stack high water mark to detect near-overflow conditions
    const auto hwm = uxTaskGetStackHighWaterMark(nullptr);
    if (hwm < 1024) {
      LOG_ERR("ACT", "[%s] Stack dangerously low! HWM=%u bytes", name.c_str(), hwm * sizeof(StackType_t));
    }
  }
}

void ActivityWithSubactivity::exitActivity() {
  // No need to lock, since onExit() already acquires its own lock
  if (subActivity) {
    LOG_DBG("ACT", "Exiting subactivity...");
    subActivity->onExit();
    subActivity.reset();
    // Re-apply orientation for the parent activity. This handles two cases:
    // 1. Returning from a landscape reader to a portrait UI activity.
    // 2. Returning from settings where the user changed orientation.
    OrientationHelper::applyOrientation(renderer, mappedInput, this);
  }
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  // Acquire lock to avoid 2 activities rendering at the same time during transition
  RenderLock lock(*this);
  subActivity.reset(activity);
  OrientationHelper::applyOrientation(renderer, mappedInput, subActivity.get());
  subActivity->onEnter();
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    subActivity->loop();
  }
}

void ActivityWithSubactivity::requestUpdate() {
  if (!subActivity) {
    Activity::requestUpdate();
  }
  // Sub-activity should call their own requestUpdate() from their loop() function
}

void ActivityWithSubactivity::onExit() {
  // No need to lock, onExit() already acquires its own lock
  exitActivity();
  Activity::onExit();
}
