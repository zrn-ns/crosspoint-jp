#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "BuildInfo.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down
}

// 「設定 → 本体 → デバッグ表示」がONのときだけ、チャネル判定の根拠を画面に出す。
// ESP32-C3 の USB Serial/JTAG は動作中に切れるため LOG_DBG が読めないことが多く、
// チャネル(0=正式/1=RC/2=Dev)・埋め込みビルド時刻・走査した release 件数が
// 分かればチャネル判定と2フェッチ構成をシリアル無しで検証できる。
// scan は正式チャネルなら1、RC/Dev なら 1+per_page(=11) になる。
void OtaUpdateActivity::drawChannelDiagnostics(const int y) {
  if (!SETTINGS.debugDisplay) return;
  char buf[64];
  snprintf(buf, sizeof(buf), "ch=%d built=%s scan=%u", static_cast<int>(updater.getChannel()), CROSSPOINT_BUILD_TIME,
           static_cast<unsigned>(updater.getReleasesScanned()));
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());
    drawChannelDiagnostics(top + height * 3 + metrics.verticalSpacing * 3);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y,
                              (std::to_string(static_cast<int>(updaterProgress * 100)) + "%").c_str());
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    drawChannelDiagnostics(top + height + metrics.verticalSpacing);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    // シリアルが取れない環境向け診断: '|' 区切りで2行に分割して描画 (画面幅に収まるよう)
    const auto& detail = updater.getLastErrorDetail();
    if (!detail.empty()) {
      const auto pipe = detail.find('|');
      const int row1 = top + height + metrics.verticalSpacing;
      if (pipe == std::string::npos) {
        renderer.drawCenteredText(UI_10_FONT_ID, row1, detail.c_str());
      } else {
        renderer.drawCenteredText(UI_10_FONT_ID, row1, detail.substr(0, pipe).c_str());
        renderer.drawCenteredText(UI_10_FONT_ID, row1 + height + metrics.verticalSpacing,
                                  detail.substr(pipe + 1).c_str());
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, tr(STR_RESTARTING_HINT));
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
      }
      requestUpdateAndWait();
      const auto res = updater.installUpdate(
          [](void* ctx) {
            // immediate=true notifies the render task directly. The default deferred path only
            // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
            // installUpdate() blocks this task.
            static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
          },
          this);

      if (res != OtaUpdater::OK) {
        LOG_DBG("OTA", "Update failed: %d", res);
        {
          RenderLock lock(*this);
          state = FAILED;
        }
        requestUpdate();
        return;
      }

      LOG_INF("OTA", "Update complete, restarting");
      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      // SdFirmwareUpdateActivity::performUpdate() と同じ手順。完了画面を確実に
      // 描き切ってから再起動する。requestUpdate() だけでは描画前に restart して
      // しまい、ユーザーに何も見えない。
      requestUpdateAndWait();
      delay(1500);
      ESP.restart();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }
}
