#include "DirectionSettingsActivity.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "FontSelectionActivity.h"
#include "LineSpacingSelectionActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"
#include "fontIds.h"

DirectionSettingsActivity::DirectionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     bool isVertical)
    : Activity("DirSettings", renderer, mappedInput), isVertical(isVertical) {}

void DirectionSettingsActivity::buildItems() {
  items.clear();
  items.reserve(12);

  // Font Family
  items.push_back({StrId::STR_FONT_FAMILY, Item::Type::FONT_FAMILY, nullptr, {}, {}});

  // Font Size
  items.push_back({StrId::STR_FONT_SIZE,
                   Item::Type::ENUM,
                   &DirectionSettings::fontSize,
                   {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE},
                   {}});

  // Line Spacing (slider)
  items.push_back(
      {StrId::STR_LINE_SPACING, Item::Type::LINE_SPACING, &DirectionSettings::lineSpacing, {}, {80, 250, 1}});

  // Character Spacing (vertical only — horizontal char spacing is not supported by renderer)
  if (isVertical) {
    items.push_back({StrId::STR_CHAR_SPACING, Item::Type::VALUE, &DirectionSettings::charSpacing, {}, {0, 50, 5}});
  }

  // Paragraph Alignment
  items.push_back(
      {StrId::STR_PARA_ALIGNMENT,
       Item::Type::ENUM,
       &DirectionSettings::paragraphAlignment,
       {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
       {}});

  // Extra Paragraph Spacing
  items.push_back({StrId::STR_EXTRA_SPACING, Item::Type::TOGGLE, &DirectionSettings::extraParagraphSpacing, {}, {}});

  // Hyphenation
  items.push_back({StrId::STR_HYPHENATION, Item::Type::TOGGLE, &DirectionSettings::hyphenationEnabled, {}, {}});

  // Screen Margin
  items.push_back({StrId::STR_SCREEN_MARGIN, Item::Type::VALUE, &DirectionSettings::screenMargin, {}, {5, 40, 5}});

  // First Line Indent
  items.push_back({StrId::STR_FIRST_LINE_INDENT, Item::Type::TOGGLE, &DirectionSettings::firstLineIndent, {}, {}});

  // Text Anti-Aliasing
  items.push_back({StrId::STR_TEXT_AA, Item::Type::TOGGLE, &DirectionSettings::textAntiAliasing, {}, {}});

  // Ruby (Furigana)
  items.push_back({StrId::STR_RUBY_ENABLED, Item::Type::TOGGLE, &DirectionSettings::rubyEnabled, {}, {}});
}

void DirectionSettingsActivity::onEnter() {
  Activity::onEnter();
  buildItems();
  requestUpdate();
}

void DirectionSettingsActivity::onExit() {
  SETTINGS.saveToFile();
  Activity::onExit();
}

void DirectionSettingsActivity::toggleCurrentItem() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) return;

  const auto& item = items[selectedIndex];

  switch (item.type) {
    case Item::Type::TOGGLE: {
      ds().*(item.valuePtr) = !(ds().*(item.valuePtr));
      SETTINGS.saveToFile();
      break;
    }
    case Item::Type::ENUM: {
      // Font Size: skip when external font is selected (fixed bitmap size)
      if (item.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
        return;
      }
      const uint8_t cur = ds().*(item.valuePtr);
      ds().*(item.valuePtr) = (cur + 1) % static_cast<uint8_t>(item.enumValues.size());
      SETTINGS.saveToFile();
      break;
    }
    case Item::Type::VALUE: {
      uint8_t& val = ds().*(item.valuePtr);
      if (val + item.valueRange.step > item.valueRange.max) {
        val = item.valueRange.min;
      } else {
        val = val + item.valueRange.step;
      }
      SETTINGS.saveToFile();
      break;
    }
    case Item::Type::LINE_SPACING: {
      uint8_t& target = ds().*(item.valuePtr);
      startActivityForResult(std::make_unique<LineSpacingSelectionActivity>(
                                 renderer, mappedInput, static_cast<int>(target),
                                 [this, &target](const int selectedValue) {
                                   target = static_cast<uint8_t>(selectedValue);
                                   SETTINGS.saveToFile();
                                   finish();
                                 },
                                 [this] { finish(); }),
                             [this](const ActivityResult&) {
                               skipNextButtonCheck = true;
                               requestUpdate();
                             });
      return;
    }
    case Item::Type::FONT_FAMILY: {
      startActivityForResult(
          std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry(), isVertical),
          [this](const ActivityResult&) {
            SETTINGS.saveToFile();
            skipNextButtonCheck = true;
            requestUpdate();
          });
      return;
    }
  }

  requestUpdate();
}

void DirectionSettingsActivity::loop() {
  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentItem();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void DirectionSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;

  // Header
  const char* title = isVertical ? tr(STR_VERTICAL_SETTINGS) : tr(STR_HORIZONTAL_SETTINGS);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding + hintGutterHeight, pageWidth, metrics.headerHeight}, title, "");

  const int itemCount = static_cast<int>(items.size());

  // List
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      itemCount, selectedIndex, [this](int index) { return std::string(I18N.get(items[index].nameId)); }, nullptr,
      nullptr,
      [this](int i) -> std::string {
        const auto& item = items[i];
        switch (item.type) {
          case Item::Type::TOGGLE:
            return ds().*(item.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
          case Item::Type::ENUM: {
            // Font Size: show actual pixel size when external font is active
            if (item.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
              const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
              return info ? (std::to_string(info->size) + "pt") : std::string("—");
            }
            const uint8_t val = ds().*(item.valuePtr);
            return std::string(I18N.get(item.enumValues[val]));
          }
          case Item::Type::VALUE:
            return std::to_string(ds().*(item.valuePtr));
          case Item::Type::LINE_SPACING: {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2fx", static_cast<float>(ds().*(item.valuePtr)) / 100.0f);
            return std::string(buf);
          }
          case Item::Type::FONT_FAMILY: {
            // Show current font name for this direction
            if (ds().sdFontFamilyName[0] != '\0') {
              return std::string(ds().sdFontFamilyName);
            }
            if (FontMgr.getSelectedIndex() >= 0) {
              const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
              return info ? std::string(info->name) : tr(STR_EXTERNAL_FONT);
            }
            switch (ds().fontFamily) {
              case CrossPointSettings::NOTOSERIF:
                return std::string(I18N.get(StrId::STR_NOTO_SERIF));
              case CrossPointSettings::NOTOSANS:
                return std::string(I18N.get(StrId::STR_NOTO_SANS));
              case CrossPointSettings::OPENDYSLEXIC:
                return std::string(I18N.get(StrId::STR_OPEN_DYSLEXIC));
              default:
                return std::string(I18N.get(StrId::STR_NOTO_SERIF));
            }
          }
        }
        return "";
      },
      true);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
