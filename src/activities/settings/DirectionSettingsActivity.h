#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DirectionSettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  bool isVertical;
  int selectedIndex = 0;
  bool skipNextButtonCheck = true;

  struct Item {
    StrId nameId;
    enum class Type { TOGGLE, ENUM, VALUE, LINE_SPACING, FONT_FAMILY } type;
    uint8_t DirectionSettings::* valuePtr = nullptr;
    std::vector<StrId> enumValues;
    struct ValueRange {
      uint8_t min;
      uint8_t max;
      uint8_t step;
    };
    ValueRange valueRange = {};
  };

  std::vector<Item> items;
  void buildItems();
  void toggleCurrentItem();
  DirectionSettings& ds() { return SETTINGS.getDirectionSettings(isVertical); }
  const DirectionSettings& ds() const { return SETTINGS.getDirectionSettings(isVertical); }

 public:
  explicit DirectionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool isVertical);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
