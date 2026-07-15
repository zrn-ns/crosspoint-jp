#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Industrial theme metrics (zero runtime cost)
// B1: all pre-existing ThemeMetrics fields are identical to BaseMetrics so
// selecting INDUSTRIAL renders Classic-equivalent layout. Only the new
// tail fields (industrial-specific tokens) carry non-default values.
namespace IndustrialMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 65,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 30,
                                 .keyboardKeySpacing = 10,
                                 .keyboardBottomAligned = false,
                                 .keyboardCenteredText = false,
                                 .hairlineWidth = 1,
                                 .ruleWidth = 2,
                                 .selectionBorderWidth = 2,
                                 .selectionMarkerWidth = 4,
                                 .useInvertedSelection = true,
                                 .notchSize = 8,
                                 .screenCodeRightPad = 8};
}

// B1: skeleton registration only. Drawing is BaseTheme-equivalent;
// component overrides (header/footer/list/dialog) land in later builds.
class IndustrialTheme : public BaseTheme {};
