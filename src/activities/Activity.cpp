#include "Activity.h"

#include <HalPowerManager.h>

void Activity::renderTaskTrampoline(void* param) {
  auto* self = static_cast<Activity*>(param);
  self->renderTaskLoop();
}

void Activity::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      // RenderLock MUST be constructed before PowerLock so it is destroyed LAST.
      // C++ destroys in reverse order: ~PowerLock runs first (cleanup done),
      // then ~RenderLock releases the mutex. This prevents vTaskDelete from
      // killing this task while PowerLock cleanup is still pending.
      RenderLock lock(*this);
      HalPowerManager::Lock powerLock;
      render(std::move(lock));
    }
    // Log stack high water mark to detect near-overflow conditions
    const auto hwm = uxTaskGetStackHighWaterMark(nullptr);
    if (hwm < 1024) {
      LOG_ERR("ACT", "[%s] Stack dangerously low! HWM=%u bytes", name.c_str(), hwm * sizeof(StackType_t));
    }
  }
}

void Activity::onEnter() {
  xTaskCreate(&renderTaskTrampoline, name.c_str(),
              renderStackSize,       // Stack size (configurable per activity)
              this,                  // Parameters
              1,                     // Priority
              &renderTaskHandle      // Task handle
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

void Activity::onExit() {
  RenderLock lock(*this);  // Ensure we don't delete the task while it's rendering
  if (renderTaskHandle) {
    vTaskDelete(renderTaskHandle);
    renderTaskHandle = nullptr;
  }

  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
}

void Activity::requestUpdate() {
  // Using direct notification to signal the render task to update
  // Increment counter so multiple rapid calls won't be lost
  if (renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eIncrement);
  }
}

void Activity::requestUpdateAndWait() {
  // FIXME @ngxson : properly implement this using freeRTOS notification
  delay(100);
}

// RenderLock

Activity::RenderLock::RenderLock(Activity& activity) : activity(activity) {
  xSemaphoreTake(activity.renderingMutex, portMAX_DELAY);
}

Activity::RenderLock::~RenderLock() { xSemaphoreGive(activity.renderingMutex); }
