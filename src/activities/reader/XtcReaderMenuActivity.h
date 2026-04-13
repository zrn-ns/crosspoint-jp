#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class XtcReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction {
    SELECT_CHAPTER,
    GO_TO_PERCENT,
    SCREENSHOT,
    GO_HOME,
    TILT_PAGE_TURN
  };

  explicit XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                 uint32_t currentPage, uint32_t totalPages, bool hasChapters);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasChapters);

  const std::vector<MenuItem> menuItems;
  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  bool skipNextButtonCheck = true;

  std::string getMenuItemValue(MenuAction action) const;
};
