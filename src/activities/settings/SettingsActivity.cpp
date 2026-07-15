#include "SettingsActivity.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>

#include "AozoraActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "DirectionSettingsActivity.h"
#include "FontDownloadActivity.h"
#include "FontSelectActivity.h"
#include "FontSelectionActivity.h"
#include "GenerateAllCacheActivity.h"
#include "HalGPIO.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "LineSpacingSelectionActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontGlobals.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[MAX_CATEGORIES] = {
    StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER, StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM, StrId::STR_CAT_RTC};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();
  rtcSettings.clear();

  for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_RTC) {
      // RTC無効時はマスタートグルのみ表示（カレンダーサブ設定を隠す）
      if (!SETTINGS.rtcEnabled && setting.nameId != StrId::STR_RTC_ENABLED) {
        continue;
      }
      rtcSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_GENERATE_ALL_CACHE, SettingAction::GenerateAllCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_DOWNLOAD_FONTS, SettingAction::DownloadFonts));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_AOZORA_BUNKO, SettingAction::AozoraBunko));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  // Direction-specific settings submenus at the top
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_HORIZONTAL_SETTINGS, SettingAction::HorizontalSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_VERTICAL_SETTINGS, SettingAction::VerticalSettings));
  // Insert "Download Fonts" right after the direction settings so users discover it naturally
  readerSettings.insert(readerSettings.begin() + 2,
                        SettingInfo::Action(StrId::STR_DOWNLOAD_FONTS, SettingAction::DownloadFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
    case 4:
      currentSettings = &rtcSettings;
      break;
    default:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // X4 にはDS3231がないためRTCタブを非表示
  categoryCount = gpio.deviceIsX4() ? 4 : MAX_CATEGORIES;

  // Initialize selection based on caller hint.
  if (initialCategoryIndex < 0 || initialCategoryIndex >= categoryCount) {
    selectedCategoryIndex = 0;
  } else {
    selectedCategoryIndex = initialCategoryIndex;
  }

  rebuildSettingsLists();

  if (initialSettingIndex < 0) {
    selectedSettingIndex = 0;
  } else if (initialSettingIndex > settingsCount) {
    selectedSettingIndex = settingsCount;
  } else {
    selectedSettingIndex = initialSettingIndex;
  }

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasPressed(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasPressed(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      if (onGoHome) {
        onGoHome();
      } else {
        finish();
      }
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
      case 4:
        currentSettings = &rtcSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
    // Apply invert images change immediately
    if (setting.nameId == StrId::STR_INVERT_IMAGES) {
      renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);
    }
    // RTCマスタートグル変更時はサブ設定の表示/非表示を更新
    if (setting.nameId == StrId::STR_RTC_ENABLED) {
      rebuildSettingsLists();
      // 選択位置をクランプ（サブ設定が消えた場合に備える）
      if (selectedSettingIndex > settingsCount) {
        selectedSettingIndex = settingsCount;
      }
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Calendar Position: skip when calendar is disabled
    if (setting.nameId == StrId::STR_SLEEP_CALENDAR_POSITION && SETTINGS.sleepCalendar == 0) {
      return;
    }
    // Font Size: skip when external font is selected (fixed bitmap size)
    if (setting.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
      return;
    }
    // Font Family: open FontSelectActivity (combined built-in + external fonts)
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      startActivityForResult(std::make_unique<FontSelectActivity>(
                                 renderer, mappedInput, FontSelectActivity::SelectMode::Reader, [this] { finish(); }),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());

    // Apply dark mode change immediately (renderer needs explicit notification)
    if (setting.nameId == StrId::STR_COLOR_MODE) {
      renderer.setDarkMode(SETTINGS.colorMode == CrossPointSettings::COLOR_MODE::DARK_MODE);
    }
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // Line spacing uses a slider activity (0.8x-2.5x) for finer control.
    // Note: Line spacing settings have moved to DirectionSettings and are no longer in the
    // main settings list. This code path is retained for backward compatibility.
    if (setting.nameId == StrId::STR_LINE_SPACING_HORIZONTAL || setting.nameId == StrId::STR_LINE_SPACING_VERTICAL) {
      const bool isVertical = (setting.nameId == StrId::STR_LINE_SPACING_VERTICAL);
      uint8_t& target = SETTINGS.getDirectionSettings(isVertical).lineSpacing;
      startActivityForResult(std::make_unique<LineSpacingSelectionActivity>(
                                 renderer, mappedInput, static_cast<int>(target),
                                 [this, &target](const int selectedValue) {
                                   target = static_cast<uint8_t>(selectedValue);
                                   SETTINGS.saveToFile();
                                   finish();
                                 },
                                 [this] { finish(); }),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    }

    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::GenerateAllCache:
        startActivityForResult(std::make_unique<GenerateAllCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::AozoraBunko:
        startActivityForResult(std::make_unique<AozoraActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { SETTINGS.saveToFile(); });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SelectUiFont:
        startActivityForResult(std::make_unique<FontSelectActivity>(
                                   renderer, mappedInput, FontSelectActivity::SelectMode::UI, [this] { finish(); }),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::HorizontalSettings:
        startActivityForResult(std::make_unique<DirectionSettingsActivity>(renderer, mappedInput, false),
                               [this](const ActivityResult&) {
                                 skipNextButtonCheck = true;
                                 rebuildSettingsLists();
                                 requestUpdate();
                               });
        break;
      case SettingAction::VerticalSettings:
        startActivityForResult(std::make_unique<DirectionSettingsActivity>(renderer, mappedInput, true),
                               [this](const ActivityResult&) {
                                 skipNextButtonCheck = true;
                                 rebuildSettingsLists();
                                 requestUpdate();
                               });
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    // Results will be handled in the result handler; also avoids concurrent SD card access.
    return;
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding + hintGutterHeight, pageWidth, metrics.headerHeight},
                 tr(STR_SETTINGS_TITLE), CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer,
                 Rect{0, metrics.topPadding + hintGutterHeight + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                 tabs, selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{
          0,
          metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing,
          pageWidth,
          pageHeight - (metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.tabBarHeight +
                        metrics.buttonHintsHeight + metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          // Font Family: show external font name when selected
          if (setting.nameId == StrId::STR_FONT_FAMILY && FontMgr.getSelectedIndex() >= 0) {
            const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
            valueText = info ? info->name : tr(STR_EXTERNAL_FONT);
            // Font Size: show actual pixel size when external font is active
          } else if (setting.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
            const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
            valueText = info ? (std::to_string(info->size) + "pt") : "—";
          } else {
            const uint8_t value = SETTINGS.*(setting.valuePtr);
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_LINE_SPACING) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2fx", static_cast<float>(SETTINGS.*(setting.valuePtr)) / 100.0f);
            valueText = buf;
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::ACTION && setting.nameId == StrId::STR_EXT_UI_FONT) {
          // Show current UI font name or "Built-in"
          if (FontMgr.isUiFontEnabled()) {
            const int idx = FontMgr.getUiSelectedIndex();
            const FontInfo* info = FontMgr.getFontInfo(idx);
            valueText = info ? info->name : tr(STR_EXTERNAL_FONT);
          } else {
            valueText = tr(STR_BUILTIN_DISABLED);
          }
        }
        return valueText;
      },
      true);

  // Draw help text
  const auto confirmLabel = (selectedSettingIndex == 0)
                                ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
                                : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
